/*
 * noisetest — S25' Noise core proof (the #4074 discipline: prove the
 * core in a standalone harness before anything depends on it).
 * Runs pc_noise_selftest(): RFC 5869 extract vector, a full NNpsk0
 * round-trip both directions, wrong-PSK rejection, tamper rejection.
 * The independent-implementation interop proof is test/noise_interop.py.
 */
#include <stdio.h>
#include <string.h>
#include <sodium.h>
#include "../src/pc_noise.h"

/* `noisetest`            -> run the selftest
 * `noisetest psk C PWD`  -> print the authoritative PSK hex for principal
 *   C (client|cluster) and password PWD, from the SAME libsodium the
 *   daemon uses.  The interop test feeds this to its independent Python
 *   initiator, so PSK derivation never has to be reproduced elsewhere. */
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
	if (pc_noise_selftest() != 0) {
		printf("FAIL: pc_noise_selftest\n");
		return 1;
	}
	printf("PASS: RFC5869 extract, NNpsk0 round-trip x2, wrong-PSK, tamper\n");
	return 0;
}
