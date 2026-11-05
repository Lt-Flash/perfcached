/*
 * clmaptest.c — the cluster map format (control plane, step 1).
 *
 * The map is the only thing placement may be computed from, so a map
 * that decodes differently on two nodes splits ownership and puts a key
 * in two places.  Everything here is about refusing input rather than
 * accepting it: a map that cannot be read identically twice is not a
 * map, and half-reading one is worse than having none.
 *
 * Build: cc -o clmaptest test/clmaptest.c src/clmap.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clmap.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void mknode(struct pc_clmap_node *n, int i)
{
	memset(n, 0, sizeof *n);
	memset(n->ident, 0xA0 + i, 16);
	n->node_id = (uint16_t)(100 + i);
	n->addr = 0x0100007Fu + (uint32_t)i;
	n->cluster_port = (uint16_t)(17000 + i);
	n->client_port = (uint16_t)(18000 + i);
	n->state = PC_CLMAP_ST_READY;
	n->master_pref = (uint8_t)(i == 0 ? 10 : 0);
	n->cap_weight = PC_CLMAP_W_NOMINAL;
	n->admin_weight = PC_CLMAP_W_UNSET;
}

static void mkmap(struct pc_clmap *m, int nodes)
{
	int i;

	memset(m, 0, sizeof *m);
	m->term = 7;
	m->seq = 42;
	m->master_id = 100;
	m->backup_id = 101;
	m->mode = PC_CLMAP_MODE_SHARD;
	m->eager = 1;
	m->config_digest = 0x0123456789ABCDEFull;
	m->nnodes = (uint16_t)nodes;
	for (i = 0; i < nodes; i++)
		mknode(&m->node[i], i);
}

int main(void)
{
	unsigned char buf[PC_CLMAP_MAXBYTES + 64];
	struct pc_clmap m, back;
	const char *why;
	long n;

	/* ---- 1. round trip, every field ---------------------------- */
	mkmap(&m, 3);
	n = pc_clmap_encode(&m, buf, sizeof buf);
	CHK(n == PC_CLMAP_HDR + 3 * PC_CLMAP_NODESZ,
		"encoded %ld bytes, expected %d", n,
		PC_CLMAP_HDR + 3 * PC_CLMAP_NODESZ);
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) == 0,
		"decode of a good map failed: %s", why);
	CHK(back.term == 7 && back.seq == 42, "epoch did not survive");
	CHK(back.master_id == 100 && back.backup_id == 101,
		"master/backup did not survive");
	CHK(back.mode == PC_CLMAP_MODE_SHARD && back.eager == 1,
		"mode/eager lost");
	CHK(back.config_digest == 0x0123456789ABCDEFull,
		"config digest lost");
	CHK(back.nnodes == 3, "node count lost");
	CHK(!memcmp(&back.node[1], &m.node[1], sizeof back.node[1]),
		"node 1 did not round trip byte-for-byte");

	/* ---- 2. a maximum fleet still fits one datagram ------------- */
	mkmap(&m, PC_CLMAP_MAXNODE);
	{
		int i;
		for (i = 0; i < PC_CLMAP_MAXNODE; i++) {
			m.node[i].node_id = (uint16_t)(i + 1);
			memset(m.node[i].ident, 0, 16);
			memcpy(m.node[i].ident, &i, sizeof i);
		}
	}
	n = pc_clmap_encode(&m, buf, sizeof buf);
	CHK(n > 0 && n <= PC_CLMAP_MAXBYTES,
		"a full fleet encodes to %ld, over the %d ceiling", n,
		PC_CLMAP_MAXBYTES);
	CHK(n < 65000, "a full fleet map (%ld) does not fit a datagram", n);
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) == 0,
		"full fleet failed to decode: %s", why);

	/* ---- 3. every refusal, and each for its own reason ---------- */
	mkmap(&m, 2);
	n = pc_clmap_encode(&m, buf, sizeof buf);

	buf[0] ^= 0xFF;
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "magic"), "bad magic was accepted (why=%s)", why);
	buf[0] ^= 0xFF;

	buf[4] = 99;
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "version"),
		"an unparseable version was accepted (why=%s)", why);
	buf[4] = PC_CLMAP_VER;

	CHK(pc_clmap_decode(buf, (size_t)n - 1, &back, &why) < 0 &&
		!strcmp(why, "trunc"),
		"a truncated map was accepted (why=%s)", why);
	CHK(pc_clmap_decode(buf, (size_t)n + 1, &back, &why) < 0 &&
		!strcmp(why, "trunc"),
		"trailing bytes were accepted (why=%s)", why);
	CHK(pc_clmap_decode(buf, 8, &back, &why) < 0,
		"a runt was accepted");

	/* the count lives at the end of the header, wherever that is */
	buf[PC_CLMAP_HDR - 2] = 0xFF; buf[PC_CLMAP_HDR - 1] = 0xFF;
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "count"),
		"an impossible node count was accepted (why=%s)", why);
	buf[PC_CLMAP_HDR - 2] = 2; buf[PC_CLMAP_HDR - 1] = 0;

	/* a duplicate id makes find() and placement depend on iteration
	 * order - two nodes could disagree about who owns a key */
	memcpy(buf + PC_CLMAP_HDR + PC_CLMAP_NODESZ + 16,
	       buf + PC_CLMAP_HDR + 16, 2);
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "dup"),
		"a duplicate node id was accepted (why=%s)", why);

	mkmap(&m, 2);
	memcpy(m.node[1].ident, m.node[0].ident, 16);
	n = pc_clmap_encode(&m, buf, sizeof buf);
	CHK(pc_clmap_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "dup"),
		"a duplicate identity was accepted (why=%s)", why);

	/* ---- 4. epoch ordering: TERM WINS FIRST -------------------- */
	CHK(pc_clmap_epoch_cmp(1, 500, 2, 1) < 0,
		"a lower term with a huge seq did not lose - this is the "
		"deposed-master case the term exists for");
	CHK(pc_clmap_epoch_cmp(2, 1, 1, 500) > 0, "term did not win");
	CHK(pc_clmap_epoch_cmp(3, 9, 3, 10) < 0, "seq ordering broken");
	CHK(pc_clmap_epoch_cmp(3, 10, 3, 10) == 0, "equal epochs differ");

	/* ---- 5. weights -------------------------------------------- */
	{
		struct pc_clmap_node w;

		mknode(&w, 0);
		CHK(pc_clmap_weight(&w) == PC_CLMAP_W_NOMINAL,
			"an unset override does not fall through to capacity (%u)",
			pc_clmap_weight(&w));

		w.cap_weight = 250;
		CHK(pc_clmap_weight(&w) == 250,
			"capacity is not used when the operator is silent (%u)",
			pc_clmap_weight(&w));

		w.admin_weight = 0;
		CHK(pc_clmap_weight(&w) == 0,
			"admin 0 does not mean 'owns nothing'");
		CHK(!pc_clmap_placeable(&w),
			"a zero-weight node is still placeable - the drain "
			"primitive does not drain");

		/* THE case a multiplier could not express: capacity says this
		 * node is nearly full, the operator says load it anyway */
		w.cap_weight = 10;
		w.admin_weight = 4 * PC_CLMAP_W_NOMINAL;
		CHK(pc_clmap_weight(&w) == 4 * PC_CLMAP_W_NOMINAL,
			"an operator cannot override capacity into overload (%u)",
			pc_clmap_weight(&w));

		w.admin_weight = PC_CLMAP_W_UNSET;
		CHK(pc_clmap_weight(&w) == 10,
			"clearing the override does not restore capacity (%u)",
			pc_clmap_weight(&w));

		mknode(&w, 0);
		w.state = PC_CLMAP_ST_RECOVERING;
		CHK(!pc_clmap_placeable(&w),
			"a recovering node is placeable");
		w.state = PC_CLMAP_ST_STARTING;
		CHK(!pc_clmap_placeable(&w), "a starting node is placeable");
		w.state = PC_CLMAP_ST_DRAINING;
		CHK(!pc_clmap_placeable(&w), "a draining node is placeable");
		w.state = PC_CLMAP_ST_READY;
		CHK(pc_clmap_placeable(&w), "a ready node is not placeable");
	}

	/* ---- 6. lookup --------------------------------------------- */
	mkmap(&m, 4);
	CHK(pc_clmap_find(&m, 102) && pc_clmap_find(&m, 102)->node_id == 102,
		"find missed a present node");
	CHK(pc_clmap_find(&m, 999) == NULL, "find invented an absent node");
	CHK(pc_clmap_find(&m, 0) == NULL, "id 0 resolved to something");

	printf("clmaptest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
