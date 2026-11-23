/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-fleetinfo.inc - part of perfd.c, #included in order (one translation unit): a handle
 * described in one line, and a count of the changes to the fleet it learned. */

/* every site that changes the member list or a member's ready, draining or down calls this; a
 * fingerprint, so a site that rewrites the same list counts nothing */
static void fleet_seen(perfd_t *p)
{
	unsigned long long h = 1469598103934665603ULL;
	int i;

	if (p->is_child)
		return;
#define FP_MIX(v) (h = (h ^ (unsigned long long)(v)) * 1099511628211ULL)
	FP_MIX(p->nmem);
	for (i = 0; i < p->nmem; i++) {
		const struct pd_member *m = &p->mem[i];
		const char *a;

		FP_MIX(m->node);
		FP_MIX(m->port);
		FP_MIX(m->ready | (m->draining << 1) | (m->down << 2));
		for (a = m->addr; *a; a++)
			FP_MIX((unsigned char)*a);
		FP_MIX(0x100);
	}
#undef FP_MIX
	if (h != p->fleet_fp) {
		p->fleet_fp = h;
		p->fleet_changes++;
	}
}

unsigned long long perfd_fleet_changes(const perfd_t *p)
{
	return p ? p->fleet_changes : 0;
}

int perfd_describe(const perfd_t *p, char *buf, size_t cap)
{
	int n;

	if (!p)
		return -1;
	n = snprintf(buf, buf ? cap : 0, "perfcached %s, %d member%s, routing %s, %d standby%s",
		p->srv_version[0] ? p->srv_version : "(version unknown)", p->nmem,
		p->nmem == 1 ? "" : "s", p->route_ok ? "on" : "off", p->nspare,
		p->nspare == 1 ? "" : "s");
	return n;
}
