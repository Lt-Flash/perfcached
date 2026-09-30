/*
 * noisetest — S25' Noise core proof (the #4074 discipline: prove the
 * core in a standalone harness before anything depends on it).
 * Runs pc_noise_selftest(): RFC 5869 extract vector, a full NNpsk0
 * round-trip both directions, wrong-PSK rejection, tamper rejection.
 * The independent-implementation interop proof is test/noise_interop.py.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sodium.h>
#include "../src/pc_noise.h"

static int io_all(int fd, int wr, uint8_t *b, size_t n)
{
	while (n) {
		ssize_t r = wr ? write(fd, b, n) : read(fd, b, n);

		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

/* one handshake as <who> with a RAW key, framed as libperfd frames it.
 * A raw key lets a test present one that no secret derives - the
 * all-zero key an underived PSK slot holds.  0 established (msg2
 * verified), 3 refused (the responder closed or its msg2 does not
 * verify), 1 the dial itself failed. */
static int dial(const char *host, const char *port, const char *who,
		const char *keyhex)
{
	uint8_t psk[32] = { 0 }, ver = PC_CLIENT_VER, hdr[2];
	uint8_t prin = !strcmp(who, "cluster") ? PC_PRIN_CLUSTER : PC_PRIN_CLIENT;
	uint8_t msg[3 + PC_NOISE_MAXMSG], m2[PC_NOISE_MAXMSG];
	struct pc_handshake hs;
	struct pc_cipherstate tx, rx;
	struct sockaddr_in sa;
	struct timeval tv = { 5, 0 };
	size_t mlen = 0, plen = 0, cl, got = 0;
	int fd;

	if (strcmp(keyhex, "zero") != 0 &&
	        (sodium_hex2bin(psk, sizeof psk, keyhex, strlen(keyhex), NULL,
	            &got, NULL) != 0 || got != sizeof psk)) {
		printf("FAIL: key is not 64 hex digits or 'zero'\n");
		return 1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)atoi(port));
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		printf("FAIL: host must be an IPv4 address\n");
		return 1;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		printf("FAIL: connect\n");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	pc_hs_init_initiator(&hs, &prin, 1);
	if (pc_hs_write_msg1(&hs, psk, &ver, 1, msg + 3, &mlen) != 0) {
		printf("FAIL: msg1\n");
		return 1;
	}
	msg[0] = (uint8_t)(1 + mlen);
	msg[1] = (uint8_t)((1 + mlen) >> 8);
	msg[2] = prin;
	if (io_all(fd, 1, msg, 3 + mlen) != 0) {
		printf("refused\n");
		return 3;
	}
	if (io_all(fd, 0, hdr, 2) != 0) {
		printf("refused\n");
		return 3;
	}
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || io_all(fd, 0, m2, cl) != 0 ||
	        pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &tx, &rx) != 0) {
		printf("refused\n");
		return 3;
	}
	close(fd);
	printf("established\n");
	return 0;
}

/* `noisetest`            -> run the selftest
 * `noisetest psk C PWD`  -> print the authoritative PSK hex for principal
 *   C (client|cluster) and password PWD, from the SAME libsodium the
 *   daemon uses.  The interop test feeds this to its independent Python
 *   initiator, so PSK derivation never has to be reproduced elsewhere.
 * `noisetest dial HOST PORT client|cluster KEYHEX|zero`
 *   -> hand-shake with a live door as that principal with that raw key;
 *   prints established or refused. */
int main(int argc, char **argv)
{
	if (sodium_init() < 0) {
		printf("FAIL: sodium_init\n");
		return 1;
	}
	if (argc == 4 && !strcmp(argv[1], "psk")) {
		int prin = !strcmp(argv[2], "cluster") ? PC_PRIN_CLUSTER
			: PC_PRIN_CLIENT;
		uint8_t psk[32];
		char hex[65];

		if (pc_psk_derive(argv[3], strlen(argv[3]), prin, psk) != 0)
			return 1;
		sodium_bin2hex(hex, sizeof hex, psk, 32);
		printf("%s\n", hex);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "dial"))
		return dial(argv[2], argv[3], argv[4], argv[5]);
	if (pc_noise_selftest() != 0) {
		printf("FAIL: pc_noise_selftest\n");
		return 1;
	}
	printf("PASS: RFC5869 extract, NNpsk0 round-trip x2, wrong-PSK, tamper\n");
	return 0;
}
