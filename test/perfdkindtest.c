/* SPDX-License-Identifier: GPL-2.0-or-later */
/* perfdkindtest.c - libperfd's verdicts: server_kind() tells refusals (nothing applied, "forward
 * failed" too) from server errors; parse_members() routes by key at wire epoch 4 only. */
#include <stdio.h>
#include <stdlib.h>
#include "perfd.c"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* the routing verdict of a one-member shard reply at @epoch: 1, 0, or -1 if it did not parse */
static int routes_at(int epoch)
{
	unsigned char buf[512];
	struct pc_tw w;
	perfd_t *p = calloc(1, sizeof *p);
	int ok = -1;

	pc_tw_init(&w, buf, sizeof buf);
	pc_tw_map(&w);
	pc_tw_key(&w, "members");
	pc_tw_arr(&w);
	pc_tw_map(&w);
	pc_tw_key(&w, "addr");
	pc_tw_str(&w, "127.0.0.1");
	pc_tw_key(&w, "port");
	pc_tw_i64(&w, 24941);
	pc_tw_key(&w, "node");
	pc_tw_i64(&w, 7);
	pc_tw_key(&w, "self");
	pc_tw_bool(&w, 1);
	pc_tw_end(&w);
	pc_tw_end(&w);
	pc_tw_key(&w, "routing");
	pc_tw_map(&w);
	pc_tw_key(&w, "epoch");
	pc_tw_i64(&w, epoch);
	pc_tw_key(&w, "algo");
	pc_tw_str(&w, PERFD_ROUTE_ALGO);
	pc_tw_key(&w, "mode");
	pc_tw_str(&w, "shard");
	pc_tw_key(&w, "cport");
	pc_tw_i64(&w, 24943);
	pc_tw_end(&w);
	pc_tw_end(&w);
	if (p) {
		p->route_want = 1;
		if (pc_tw_done(&w) == 0 && parse_members(p, w.b, w.n) == 1)
			ok = p->route_ok;
		free(p);
	}
	return ok;
}

int main(void)
{
	static const char *const refused[] = {
		"max number of clients reached",
		"node is not READY (recovering or connecting) - retry, or use another member",
		"collection c refuses writes",
		"cluster busy",
		"parked-request table full - retry",
		"node is FAILED - writes refused, reads served",
		"node is HEALING - writes refused",
		"forward failed",
		NULL };
	static const char *const server[] = {
		"WRONGTYPE Operation against a key holding the wrong kind of value",
		"value is not an integer",
		"this operation needs a privileged connection - call enable with the [secrets] enable value first",
		NULL };
	int i;

	printf("--- the refusals\n");
	for (i = 0; refused[i]; i++)
		CHK(server_kind(refused[i]) == PERFD_E_REFUSED, "REFUSED: %s", refused[i]);
	printf("--- the server errors\n");
	for (i = 0; server[i]; i++)
		CHK(server_kind(server[i]) == PERFD_E_SERVER, "SERVER: %s", server[i]);
	printf("--- routing by wire epoch\n");
	for (i = 1; i <= 5; i++) {
		int want = i == 4, got = routes_at(i);

		CHK(got == want, "epoch %d: routes %d (want %d)", i, got, want);
	}
	printf("perfdkindtest: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
