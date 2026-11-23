/* respcltest.c - the RESP door's CLUSTER commands without a fleet: the members, the placement and
 * verbs.c's helpers are stubbed, and rh_cluster answers into a buffer. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "cluster.h"
#include "pc_slot.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: what respcl.c reaches outside itself -------------------- */
unsigned int pc_resp_slots_hits, pc_resp_slots_builds;
static struct pc_member fleet[3];
static int fleet_n, alternate, unk_calls;

struct pc_member *members_scratch(void)
{
	static struct pc_member m[PC_CL_MAXMEMBERS];

	return m;
}

int pc_cluster_members(struct pc_member *out, int max)
{
	int n = fleet_n < max ? fleet_n : max;

	memcpy(out, fleet, sizeof *out * (size_t)n);
	return n;
}

int resp_cluster_on(void)
{
	return fleet_n > 0;
}

void unk_sub(const struct resp_ctx *c)
{
	(void)c;
	unk_calls++;
}

/* this node (0) owns the lower half, node 2 the upper - or every other
 * slot when @alternate */
int pc_shard_owner_slot(unsigned slot)
{
	if (alternate)
		return slot % 2 ? 2 : 0;
	return slot < 8192 ? 0 : 2;
}

/* ---- the harness ------------------------------------------------------- */
static char outbuf[1 << 20], scratch[8192];
static struct pc_jw out;

static void cluster(const char *a1, const char *a2)
{
	char *argv[3] = { "CLUSTER", (char *)a1, (char *)a2 };
	size_t argl[3] = { 7, a1 ? strlen(a1) : 0, a2 ? strlen(a2) : 0 };
	struct resp_ctx c;

	memset(&c, 0, sizeof c);
	c.argv = argv;
	c.argl = argl;
	c.nargs = a2 ? 3 : a1 ? 2 : 1;
	c.out = &out;
	c.scratch = scratch;
	c.scratch_cap = sizeof scratch;
	pc_jw_init(&out, outbuf, sizeof outbuf);
	rh_cluster(&c);
}

static int has(const char *needle)
{
	return memmem(out.buf, out.len, needle, strlen(needle)) != NULL;
}

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

int main(void)
{
	int i;

	/* A: KEYSLOT - Redis's own examples, hash tags keep keys together, the arity refused */
	cluster("KEYSLOT", "somekey");
	CHK(is(":11058\r\n"), "A: KEYSLOT somekey = 11058, as Redis");
	cluster("KEYSLOT", "foo{hash_tag}");
	CHK(is(":2515\r\n"), "A: KEYSLOT foo{hash_tag} = 2515, as Redis");
	{
		char a[16];

		cluster("KEYSLOT", "{user1000}.following");
		memcpy(a, out.buf, out.len < sizeof a ? out.len : sizeof a - 1);
		a[out.len < sizeof a ? out.len : sizeof a - 1] = 0;
		cluster("KEYSLOT", "{user1000}.followers");
		CHK(is(a), "A: a hash tag keeps two keys on one slot (%s)", strtok(a, "\r"));
	}
	cluster("KEYSLOT", NULL);
	CHK(has("wrong number of arguments") && out.buf[0] == '-', "A: KEYSLOT without a key refused");

	/* B: standalone - INFO says disabled and fail, MYID and NODES refuse */
	fleet_n = 0;
	cluster("INFO", NULL);
	CHK(has("cluster_enabled:0") && has("cluster_state:fail") &&
		has("cluster_known_nodes:0"), "B: standalone INFO - disabled, fail, no nodes");
	cluster("MYID", NULL);
	CHK(is("-ERR this node is not in a cluster\r\n"), "B: MYID refused");
	cluster("NODES", NULL);
	CHK(is("-ERR This instance has cluster support disabled\r\n"), "B: NODES refused");

	/* C: two members - INFO, MYID, SLOTS byte for byte, cached, 16384 ranges when alternating */
	memset(fleet, 0, sizeof fleet);
	fleet[0].node = 1;
	fleet[0].is_self = 1;
	inet_pton(AF_INET, "127.0.0.1", &fleet[0].addr);
	fleet[0].client_port = 6480;
	fleet[0].resp_port = 6379;
	fleet[0].has_ident = 1;
	for (i = 0; i < 16; i++)
		fleet[0].ident[i] = (unsigned char)(0x11 * i);
	fleet[0].incarn = 0x12345678u;
	fleet[1].node = 2;
	inet_pton(AF_INET, "127.0.0.2", &fleet[1].addr);
	fleet[1].client_port = 6481;               /* no RESP door gossiped */
	fleet[0].gone_s = fleet[1].gone_s = -1;    /* both present */
	fleet[0].live = fleet[1].live = 1;
	fleet_n = 2;
	cluster("INFO", NULL);
	CHK(has("cluster_enabled:1") && has("cluster_state:ok") &&
		has("cluster_slots_assigned:16384") && has("cluster_known_nodes:2"),
		"C: INFO - enabled, ok, every slot, two nodes");
	cluster("MYID", NULL);
	CHK(is("$40\r\n00112233445566778899aabbccddeeff12345678\r\n"),
		"C: MYID - the persisted identity, then the incarnation");
	cluster("SLOTS", NULL);
	CHK(is("*2\r\n"
		"*3\r\n:0\r\n:8191\r\n"
		"*3\r\n$9\r\n127.0.0.1\r\n:6379\r\n$40\r\n00112233445566778899aabbccddeeff12345678\r\n"
		"*3\r\n:8192\r\n:16383\r\n"
		"*3\r\n$9\r\n127.0.0.2\r\n:6481\r\n$40\r\n0000000000000000000000000000000000000002\r\n"),
		"C: SLOTS byte for byte - two ranges, the RESP port (or the native one), the ids");
	CHK(pc_resp_slots_builds == 1 && pc_resp_slots_hits == 0, "C: built once");
	cluster("SLOTS", NULL);
	CHK(pc_resp_slots_hits == 1 && has(":8192\r\n:16383\r\n"), "C: the same reply again, from the cache (S75)");
	alternate = 1;
	fleet[1].state = 1;                        /* a new snapshot: a new key */
	cluster("SLOTS", NULL);
	CHK(!strncmp(out.buf, "*16384\r\n", 8) && pc_resp_slots_builds == 2,
		"C: every slot alternating owners - 16384 ranges, built afresh");

	/* D: an unknown subcommand is counted and refused */
	unk_calls = 0;
	cluster("BOGUS", NULL);
	CHK(unk_calls == 1 && is("-ERR unsupported CLUSTER subcommand\r\n"),
		"D: an unknown subcommand counted (S165) and refused");

	/* E: a member that left keeps its card while its id is reserved (ports 0, gone_s >= 0):
	 * no command lists it - no client may dial <ip>:0 or count it as a node */
	alternate = 0;
	memset(&fleet[2], 0, sizeof fleet[2]);
	fleet[2].node = 3;
	inet_pton(AF_INET, "127.0.0.3", &fleet[2].addr);
	fleet[2].gone_s = 5;
	fleet_n = 3;
	cluster("NODES", NULL);
	CHK(!has("127.0.0.3") && !has(":0@0"), "E: NODES leaves out the member that left");
	cluster("SHARDS", NULL);
	CHK(!strncmp(out.buf, "*2\r\n", 4) && !has("127.0.0.3"), "E: SHARDS - two shards, not three");
	cluster("SLOTS", NULL);
	CHK(!has("127.0.0.3") && !has(":0\r\n$40"), "E: SLOTS - nor here");
	cluster("INFO", NULL);
	CHK(has("cluster_known_nodes:2") && has("cluster_size:2"), "E: INFO counts the two present");
	fleet[2].gone_s = -1;                      /* present, but no door to dial */
	fleet[2].live = 1;
	cluster("NODES", NULL);
	CHK(!has("127.0.0.3"), "E: a member with no port to dial is not listed either");
	fleet[2].client_port = 6482;
	cluster("NODES", NULL);
	CHK(has("127.0.0.3:6482@6482 master"), "E: once it has one, it is");

	printf("respcltest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
