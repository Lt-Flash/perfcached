/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-request.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds the wire, outbound: the JSON edge (params text <-> trees, S317) and the
 * request encoders (enqueue_cmd, enqueue_bin).
 */
/* ---- the JSON edge (S317) ---------------------------------------------- */

/* params JSON text -> a request tree in a heap writer the caller frees.
 * NULL/empty JSON = no parameters (w left empty).  A value under a
 * DOC_KEYS key is kept as its JSON text in one bulk. */
static int json_to_tree(perfd_t *p, const char *json, struct pc_tw *w)
{
	const char *why = "malformed";

	pc_tw_init(w, NULL, 0);
	if (!json || !*json)
		return 0;
	if (pc_tree_from_json(json, strlen(json), w, DOC_KEYS, &why) != 0 ||
	        pc_tw_done(w) != 0) {
		pc_tw_free(w);
		return err(p, "bad params JSON: %s", why);
	}
	return 0;
}

/* a reply tree -> malloc'd NUL-terminated JSON text (*lenout = its length
 * without the terminator).  Bytes that are not UTF-8 render as U+FFFD -
 * lossy by design; a byte-exact caller takes the tree form. */
static char *tree_to_json(perfd_t *p, const unsigned char *tree, size_t n,
		size_t *lenout)
{
	struct pc_jw w;
	char *buf = malloc(32 + 6 * n), *fit;

	if (!buf) {
		err(p, "out of memory");
		return NULL;
	}
	pc_jw_init(&w, buf, 32 + 6 * n);
	if (pc_tree_json(&w, tree, n) != n || w.overflow) {
		free(buf);
		err(p, "bad reply");
		return NULL;
	}
	buf[w.len] = 0;
	if (lenout)
		*lenout = w.len;
	/* the bound above is six times the tree: give the slack back */
	fit = realloc(buf, w.len + 1);
	return fit ? fit : buf;
}

/* a reply tree indexed for reading; -1 with p->err when it is not a map */
/* S284: give the node array back to the handle for the next parse
 * (pc_tv_parse keeps and grows it) - in place of pc_tv_free */
static void tv_put(perfd_t *p, struct pc_tv *v)
{
	if (!p->tvn || p->tvcap < v->cap) {
		free(p->tvn);
		p->tvn = v->n;
		p->tvcap = v->cap;
	} else if (v->n != p->tvn) {
		free(v->n);
	}
	v->n = NULL;
	v->nn = v->cap = 0;
}

static int tv_root(perfd_t *p, struct pc_tv *v, const unsigned char *tree,
		size_t n, const char *what)
{
	memset(v, 0, sizeof *v);
	/* S284: parse into the handle's node array; tv_put returns it */
	v->n = p->tvn;
	v->cap = p->tvcap;
	p->tvn = NULL;
	p->tvcap = 0;
	if (pc_tv_parse(v, tree, n) != 0 || v->n[0].type != 'm') {
		tv_put(p, v);
		return err(p, "bad %s reply", what);
	}
	return 0;
}

/* node @k, a bulk, copied into a malloc'd buffer with a NUL after it (the
 * length is authoritative; the terminator is a courtesy to C callers) */
static int tv_bulk_dup(const struct pc_tv *v, int k, void **out, size_t *len)
{
	char *b;

	if (k < 0 || k >= v->nn || v->n[k].type != 'b')
		return -1;
	b = malloc((size_t)v->n[k].len + 1);
	if (!b)
		return -1;
	memcpy(b, v->n[k].p, v->n[k].len);
	b[v->n[k].len] = 0;
	*out = b;
	*len = v->n[k].len;
	return 0;
}

/* ---- request building -------------------------------------------------- */

static int push_id(perfd_t *p, unsigned long long id)
{
	if (p->nids == p->idcap) {
		int nc = p->idcap ? p->idcap * 2 : 64;
		unsigned long long *ni = realloc(p->ids,
			sizeof *ni * (size_t)nc);

		if (!ni)
			return err(p, "out of memory");
		p->ids = ni;
		p->idcap = nc;
	}
	p->ids[p->nids++] = id;
	return 0;
}

/* S317: frame + queue one CMD request: [BV_CMD][mlen u8][method] then
 * the parameters as ONE tree item (@tree, @tlen bytes), or none. */
static int enqueue_cmd(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen)
{
	size_t mlen = strlen(method), plen, need;
	unsigned char *at;

	if (p->poisoned)
		return err(p, "connection poisoned");
	if (!mlen || mlen > 255)
		return err(p, "bad method name");
	plen = 2 + mlen + tlen;
	if (plen > (1u << 20))
		return err(p, "request too large");
	need = BIN_HDR + plen;
	if (p->qlen + need > p->qcap) {
		while (p->qcap < p->qlen + need)
			p->qcap = p->qcap ? p->qcap * 2 : 65536;
		{
			void *nb = realloc(p->q, p->qcap);

			if (!nb) {
				p->poisoned = 1;
				return err(p, "out of memory");
			}
			p->q = nb;
		}
	}
	at = (unsigned char *)p->q + p->qlen;
	at[0] = BIN_MAGIC;
	at[1] = BIN_VER;
	at[2] = BIN_REQ;
	at[3] = 0;
	wle32(at + 4, (unsigned int)plen);
	wle64(at + 8, ++p->next_id);
	at += BIN_HDR;
	*at++ = BV_CMD;
	*at++ = (unsigned char)mlen;
	memcpy(at, method, mlen);
	if (tlen)
		memcpy(at + mlen, tree, tlen);
	/* the in-order FIFO belongs to the blocking collector; an async
	 * handle correlates by callback instead, and would otherwise grow
	 * this array for the life of the connection */
	if (!p->async && push_id(p, p->next_id) != 0)
		return -1;
	p->qlen += need;
	return 0;
}

/* frame + queue one binary data-verb request (layouts: src/proto.h).
 * @val doubles as the ping echo payload. */
static int enqueue_bin(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl)
{
	size_t cn = col ? strlen(col) : 0, kn = key ? strlen(key) : 0;
	size_t fixed = 0, plen, need;
	unsigned char *at;

	if (p->poisoned)
		return err(p, "connection poisoned");
	if (verb != BV_PING) {
		if (!cn || cn > 255)
			return err(p, "bad collection name");
		if (!kn || kn > KEY_MAX)
			return err(p, "bad key");
		fixed = 3;
		if (verb == BV_EXPIRE || verb == BV_SET || verb == BV_SETNX ||
		        verb == BV_SETXX)
			fixed += 8;
		else if (verb == BV_ADD || verb == BV_SUB || verb == BV_RLHIT)
			fixed += 16;
	}
	plen = 1 + fixed + cn + kn + vlen;
	need = BIN_HDR + plen;
	if (p->qlen + need > p->qcap) {
		while (p->qcap < p->qlen + need)
			p->qcap = p->qcap ? p->qcap * 2 : 65536;
		{
			void *nb = realloc(p->q, p->qcap);

			if (!nb) {
				p->poisoned = 1;
				return err(p, "out of memory");
			}
			p->q = nb;
		}
	}
	at = (unsigned char *)p->q + p->qlen;
	at[0] = BIN_MAGIC;
	at[1] = BIN_VER;
	at[2] = BIN_REQ;
	at[3] = p->want_hh ? BIN_F_HINT : 0;   /* S295 */
	wle32(at + 4, (unsigned int)plen);
	wle64(at + 8, ++p->next_id);
	at += BIN_HDR;
	*at++ = (unsigned char)verb;
	if (verb != BV_PING) {
		*at++ = (unsigned char)cn;
		wle16(at, (unsigned int)kn);
		at += 2;
		if (verb == BV_EXPIRE || verb == BV_SET || verb == BV_SETNX ||
		        verb == BV_SETXX) {
			wle64(at, (unsigned long long)ttl);
			at += 8;
		} else if (verb == BV_ADD || verb == BV_SUB ||
		        verb == BV_RLHIT) {
			wle64(at, (unsigned long long)by);
			at += 8;
			wle64(at, (unsigned long long)ttl);
			at += 8;
		}
		memcpy(at, col, cn);
		at += cn;
		memcpy(at, key, kn);
		at += kn;
	}
	if (vlen)
		memcpy(at, val, vlen);
	if (push_id(p, p->next_id) != 0)
		return -1;
	p->qlen += need;
	return 0;
}

