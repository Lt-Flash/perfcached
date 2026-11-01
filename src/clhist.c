/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clhist.c — the identity history: bounded, LRU, and cheap to be wrong.
 */
#include <stddef.h>
#include <string.h>

#include "clhist.h"

static void w16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void w32(unsigned char *p, uint32_t v)
{
	w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t r16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t r32(const unsigned char *p)
{
	return (uint32_t)r16(p) | ((uint32_t)r16(p + 2) << 16);
}

void pc_clhist_init(struct pc_clhist *h)
{
	if (h)
		memset(h, 0, sizeof *h);
}

static int find(const struct pc_clhist *h, const unsigned char ident[16])
{
	unsigned int i;

	for (i = 0; i < h->n; i++)
		if (!memcmp(h->ent[i].ident, ident, 16))
			return (int)i;
	return -1;
}

int pc_clhist_note(struct pc_clhist *h, const unsigned char ident[16],
		uint16_t node_id)
{
	int i;
	unsigned int j, oldest = 0;

	if (!h || !ident)
		return 0;
	h->clock++;
	i = find(h, ident);
	if (i >= 0) {
		h->ent[i].seen = h->clock;
		h->ent[i].node_id = node_id;
		return 1;                  /* known: this node is RECOVERING */
	}
	if (h->n < PC_CLHIST_MAX) {
		i = h->n++;
	} else {
		/* full: drop the least recently seen.  Costs a redundant
		 * backfill if that node ever comes back, never its data. */
		for (j = 1; j < h->n; j++)
			if (h->ent[j].seen < h->ent[oldest].seen)
				oldest = j;
		i = (int)oldest;
	}
	memcpy(h->ent[i].ident, ident, 16);
	h->ent[i].node_id = node_id;
	h->ent[i]._pad = 0;
	h->ent[i].seen = h->clock;
	return 0;                          /* first sight: STARTING */
}

int pc_clhist_seen(const struct pc_clhist *h, const unsigned char ident[16])
{
	return h && ident && find(h, ident) >= 0;
}

uint16_t pc_clhist_last_id(const struct pc_clhist *h,
		const unsigned char ident[16])
{
	int i;

	if (!h || !ident)
		return 0;
	i = find(h, ident);
	return i < 0 ? 0 : h->ent[i].node_id;
}

long pc_clhist_encode(const struct pc_clhist *h, unsigned char *buf, size_t cap)
{
	size_t need, i;
	unsigned char *p;

	if (!h || !buf || h->n > PC_CLHIST_MAX)
		return -1;
	need = 2 + (size_t)h->n * PC_CLHIST_ENTSZ;
	if (cap < need)
		return -1;
	w16(buf, h->n);
	p = buf + 2;
	for (i = 0; i < h->n; i++) {
		memcpy(p, h->ent[i].ident, 16);
		w16(p + 16, h->ent[i].node_id);
		w16(p + 18, 0);
		w32(p + 20, h->ent[i].seen);
		p += PC_CLHIST_ENTSZ;
	}
	return (long)need;
}

int pc_clhist_decode(const unsigned char *buf, size_t n, struct pc_clhist *out,
		const char **why)
{
	const char *dummy;
	const unsigned char *p;
	unsigned int i, cnt;

	if (!why)
		why = &dummy;
	*why = "short";
	if (!buf || !out || n < 2)
		return -1;
	cnt = r16(buf);
	if (cnt > PC_CLHIST_MAX) {
		*why = "count";
		return -1;
	}
	if (n != 2 + (size_t)cnt * PC_CLHIST_ENTSZ) {
		*why = "trunc";
		return -1;
	}
	pc_clhist_init(out);
	out->n = (uint16_t)cnt;
	p = buf + 2;
	for (i = 0; i < cnt; i++) {
		memcpy(out->ent[i].ident, p, 16);
		out->ent[i].node_id = r16(p + 16);
		out->ent[i]._pad = 0;
		out->ent[i].seen = r32(p + 20);
		if (out->ent[i].seen > out->clock)
			out->clock = out->ent[i].seen;
		p += PC_CLHIST_ENTSZ;
	}
	/* A duplicate identity would make find() - and so the whole
	 * STARTING/RECOVERING decision - depend on iteration order. */
	{
		unsigned int j;

		for (i = 0; i < cnt; i++)
			for (j = i + 1; j < cnt; j++)
				if (!memcmp(out->ent[i].ident,
				            out->ent[j].ident, 16)) {
					*why = "dup";
					return -1;
				}
	}
	*why = "ok";
	return 0;
}
