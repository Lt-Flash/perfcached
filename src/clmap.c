/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmap.c — encode, decode and validate the cluster map.
 *
 * Deliberately free of cluster.c: this parses bytes that arrived over a
 * network and is the one place a malformed map can be caught, so it is
 * kept small enough to unit-test on its own.
 */
#include <string.h>

#include "clmap.h"

/* ---- LE accessors (local: cluster.c's are static, and this file is
 * built into the test binary too) ---------------------------------- */

static void w16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}
static void w32(unsigned char *p, uint32_t v)
{
	w16(p, (uint16_t)v);
	w16(p + 2, (uint16_t)(v >> 16));
}
static void w64(unsigned char *p, uint64_t v)
{
	w32(p, (uint32_t)v);
	w32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t r16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t r32(const unsigned char *p)
{
	return (uint32_t)r16(p) | ((uint32_t)r16(p + 2) << 16);
}
static uint64_t r64(const unsigned char *p)
{
	return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

/* ---- encode ------------------------------------------------------- */

long pc_clmap_encode(const struct pc_clmap *m, unsigned char *buf, size_t cap)
{
	size_t need, i;
	unsigned char *p;

	if (!m || !buf || m->nnodes > PC_CLMAP_MAXNODE)
		return -1;
	need = PC_CLMAP_HDR + (size_t)m->nnodes * PC_CLMAP_NODESZ;
	if (cap < need)
		return -1;

	w32(buf, PC_CLMAP_MAGIC);
	w16(buf + 4, PC_CLMAP_VER);
	w32(buf + 6, m->term);
	w32(buf + 10, m->seq);
	w16(buf + 14, m->master_id);
	w16(buf + 16, m->backup_id);
	buf[18] = m->mode;
	buf[19] = m->eager ? 1 : 0;
	w64(buf + 20, m->config_digest);
	w16(buf + 28, m->nnodes);

	p = buf + PC_CLMAP_HDR;
	for (i = 0; i < m->nnodes; i++) {
		const struct pc_clmap_node *n = &m->node[i];

		memcpy(p, n->ident, 16);
		w16(p + 16, n->node_id);
		w32(p + 18, n->addr);
		w16(p + 22, n->cluster_port);
		w16(p + 24, n->client_port);
		p[26] = n->state;
		p[27] = n->master_pref;
		w16(p + 28, n->cap_weight);
		w16(p + 30, n->admin_weight);
		p += PC_CLMAP_NODESZ;
	}
	return (long)need;
}

/* ---- decode ------------------------------------------------------- */

int pc_clmap_decode(const unsigned char *buf, size_t n, struct pc_clmap *out,
		const char **why)
{
	const char *dummy;
	const unsigned char *p;
	unsigned int i, j, cnt;

	if (!why)
		why = &dummy;
	*why = "short";
	if (!buf || !out || n < PC_CLMAP_HDR)
		return -1;
	if (r32(buf) != PC_CLMAP_MAGIC) {
		*why = "magic";
		return -1;
	}
	if (r16(buf + 4) != PC_CLMAP_VER) {
		/* A map from a build we cannot parse is REFUSED, never
		 * half-read: placement derived from a misread map splits
		 * ownership, which is worse than having no map at all. */
		*why = "version";
		return -1;
	}
	cnt = r16(buf + 28);
	if (cnt > PC_CLMAP_MAXNODE) {
		*why = "count";
		return -1;
	}
	if (n != PC_CLMAP_HDR + (size_t)cnt * PC_CLMAP_NODESZ) {
		/* exact, not "at least": trailing bytes mean the sender and
		 * this reader disagree about the format, whatever the header
		 * claims */
		*why = "trunc";
		return -1;
	}

	memset(out, 0, sizeof *out);
	out->term = r32(buf + 6);
	out->seq = r32(buf + 10);
	out->master_id = r16(buf + 14);
	out->backup_id = r16(buf + 16);
	out->mode = buf[18];
	out->eager = buf[19];
	out->config_digest = r64(buf + 20);
	out->nnodes = (uint16_t)cnt;

	p = buf + PC_CLMAP_HDR;
	for (i = 0; i < cnt; i++) {
		struct pc_clmap_node *nd = &out->node[i];

		memcpy(nd->ident, p, 16);
		nd->node_id = r16(p + 16);
		nd->addr = r32(p + 18);
		nd->cluster_port = r16(p + 22);
		nd->client_port = r16(p + 24);
		nd->state = p[26];
		nd->master_pref = p[27];
		nd->cap_weight = r16(p + 28);
		nd->admin_weight = r16(p + 30);
		p += PC_CLMAP_NODESZ;
	}

	/* A duplicate id or identity would make pc_clmap_find() and every
	 * placement decision depend on iteration order.  Refuse: a map that
	 * cannot be read the same way twice is not a map.  O(n^2) over at
	 * most 257 entries, on a control-plane message, is free. */
	for (i = 0; i < cnt; i++) {
		for (j = i + 1; j < cnt; j++) {
			if (out->node[i].node_id == out->node[j].node_id ||
			        !memcmp(out->node[i].ident,
			                out->node[j].ident, 16)) {
				*why = "dup";
				return -1;
			}
		}
	}

	*why = "ok";
	return 0;
}

/* ---- ordering and weights ----------------------------------------- */

int pc_clmap_epoch_cmp(uint32_t a_term, uint32_t a_seq,
		uint32_t b_term, uint32_t b_seq)
{
	if (a_term != b_term)
		return a_term < b_term ? -1 : 1;
	if (a_seq != b_seq)
		return a_seq < b_seq ? -1 : 1;
	return 0;
}

uint32_t pc_clmap_weight(const struct pc_clmap_node *n)
{
	if (!n)
		return 0;
	/* an override stands even when capacity disagrees - including into
	 * overload, which is the operator's call to make */
	if (n->admin_weight != PC_CLMAP_W_UNSET)
		return n->admin_weight;
	return n->cap_weight;
}

int pc_clmap_placeable(const struct pc_clmap_node *n)
{
	return n && n->state == PC_CLMAP_ST_READY && pc_clmap_weight(n) > 0;
}

int pc_clmap_placeable_count(const struct pc_clmap *m)
{
	unsigned int i;
	int n = 0;

	if (!m)
		return 0;
	for (i = 0; i < m->nnodes; i++)
		if (pc_clmap_placeable(&m->node[i]))
			n++;
	return n;
}

const struct pc_clmap_node *pc_clmap_find(const struct pc_clmap *m, uint16_t id)
{
	unsigned int i;

	if (!m)
		return NULL;
	for (i = 0; i < m->nnodes; i++)
		if (m->node[i].node_id == id)
			return &m->node[i];
	return NULL;
}
