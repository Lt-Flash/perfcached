/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* hostlink.c - S465: the physical link under the node's address - a VLAN
 * followed to its bond, an active-backup bond to its active slave - and its
 * kernel counters, read once a second on pc-maint and kept a minute */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "hostlink.h"

struct hostlink HL = { .speed_mbps = -1, .rx_pct = -1, .tx_pct = -1,
	.rx_peak_pct = -1, .tx_peak_pct = -1 };

/* PERFCACHED_HOSTLINK_ROOT: a fake /sys and /proc for the tests */
static char root[256];
static int fd_rx = -1, fd_tx = -1, fd_rxd = -1, fd_txd = -1, fd_miss = -1;
static char open_phys[64];
static long long resolved_ms;

static int rd_line(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");

	out[0] = 0;
	if (!f)
		return -1;
	if (!fgets(out, (int)n, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\n")] = 0;
	return out[0] ? 0 : -1;
}

static unsigned long long rd_fd(int fd)
{
	char b[32];
	ssize_t n;

	if (fd < 0)
		return 0;
	n = pread(fd, b, sizeof b - 1, 0);
	if (n <= 0)
		return 0;
	b[n] = 0;
	return strtoull(b, NULL, 10);
}

static int net_path(char *p, size_t n, const char *dev, const char *leaf)
{
	return snprintf(p, n, "%s/sys/class/net/%s/%s", root, dev, leaf) < (int)n ? 0 : -1;
}

/* a VLAN's (or macvlan's) one lower device; 0 = none, or more than one */
static int one_lower(const char *dev, char *low, size_t n)
{
	char p[384];
	struct dirent *e;
	DIR *d;
	int found = 0;

	if (net_path(p, sizeof p, dev, "") != 0 || !(d = opendir(p)))
		return 0;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, "lower_", 6) && found++ == 0)
			snprintf(low, n, "%s", e->d_name + 6);
	closedir(d);
	return found == 1;
}

/* @dev down to the device whose counters carry its traffic */
static int resolve(const char *dev, char *phys, size_t pn, char *path, size_t ptn,
		int *speed)
{
	char cur[64], nxt[64], p[384], s[64];
	size_t l;
	int hops;

	snprintf(cur, sizeof cur, "%s", dev);
	snprintf(path, ptn, "%s", dev);
	for (hops = 0; hops < 4; hops++) {
		l = strlen(path);
		if (net_path(p, sizeof p, cur, "bonding/mode") == 0 &&
		    rd_line(p, s, sizeof s) == 0) {
			s[strcspn(s, " ")] = 0;
			if (strcmp(s, "active-backup")) {
				snprintf(path + l, ptn - l, " (%s)", s);
				break;         /* it spreads: the bond's own counters */
			}
			if (net_path(p, sizeof p, cur, "bonding/active_slave") != 0 ||
			    rd_line(p, nxt, sizeof nxt) != 0) {
				snprintf(path + l, ptn - l, " (active-backup, no active slave)");
				break;
			}
			snprintf(path + l, ptn - l, " (active-backup) -> %s", nxt);
		} else if (one_lower(cur, nxt, sizeof nxt))
			snprintf(path + l, ptn - l, " -> %s", nxt);
		else
			break;
		snprintf(cur, sizeof cur, "%s", nxt);
	}
	snprintf(phys, pn, "%s", cur);
	*speed = -1;
	if (net_path(p, sizeof p, cur, "speed") == 0 && rd_line(p, s, sizeof s) == 0)
		*speed = atoi(s) > 0 ? atoi(s) : -1;
	return net_path(p, sizeof p, cur, "statistics/rx_bytes") == 0 &&
		access(p, R_OK) == 0 ? 0 : -1;
}

static void fds_close(void)
{
	int *f[] = { &fd_rx, &fd_tx, &fd_rxd, &fd_txd, &fd_miss };
	size_t i;

	for (i = 0; i < sizeof f / sizeof f[0]; i++) {
		if (*f[i] >= 0)
			close(*f[i]);
		*f[i] = -1;
	}
	open_phys[0] = 0;
}

static void fds_open(const char *phys)
{
	static const char *const leaf[] = { "statistics/rx_bytes", "statistics/tx_bytes",
		"statistics/rx_dropped", "statistics/tx_dropped",
		"statistics/rx_missed_errors" };
	int *f[] = { &fd_rx, &fd_tx, &fd_rxd, &fd_txd, &fd_miss };
	char p[384];
	size_t i;

	fds_close();
	for (i = 0; i < sizeof f / sizeof f[0]; i++)
		if (net_path(p, sizeof p, phys, leaf[i]) == 0)
			*f[i] = open(p, O_RDONLY | O_CLOEXEC);
	snprintf(open_phys, sizeof open_phys, "%s", phys);
}

/* the host's UDP receive-buffer drops (/proc/net/snmp Udp: RcvbufErrors) */
static unsigned long long udp_rcvbuf_errors(void)
{
	char p[300], hdr[1024], val[1024], *ht, *vt, *hs = NULL, *vs = NULL;
	unsigned long long v = 0;
	FILE *f;

	snprintf(p, sizeof p, "%s/proc/net/snmp", root);
	if (!(f = fopen(p, "r")))
		return 0;
	while (fgets(hdr, sizeof hdr, f))
		if (!strncmp(hdr, "Udp:", 4) && fgets(val, sizeof val, f)) {
			for (ht = strtok_r(hdr, " \n", &hs), vt = strtok_r(val, " \n", &vs);
			     ht && vt; ht = strtok_r(NULL, " \n", &hs), vt = strtok_r(NULL, " \n", &vs))
				if (!strcmp(ht, "RcvbufErrors"))
					v = strtoull(vt, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

/* the device holding @addr, else the default route's */
static int auto_dev(const char *addr, char *dev, size_t n)
{
	struct ifaddrs *ifa, *i;
	unsigned char want[16];
	char p[300], line[256], ifn[64];
	unsigned dst;
	int fam = 0;
	FILE *f;

	dev[0] = 0;
	if (addr && inet_pton(AF_INET, addr, want) == 1)
		fam = AF_INET;
	else if (addr && inet_pton(AF_INET6, addr, want) == 1)
		fam = AF_INET6;
	if (fam && getifaddrs(&ifa) == 0) {
		for (i = ifa; i && !dev[0]; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != fam)
				continue;
			if (fam == AF_INET ? !memcmp(&((struct sockaddr_in *)
			        (void *)i->ifa_addr)->sin_addr, want, 4)
			    : !memcmp(&((struct sockaddr_in6 *)(void *)i->ifa_addr)->sin6_addr,
			        want, 16))
				snprintf(dev, n, "%s", i->ifa_name);
		}
		freeifaddrs(ifa);
	}
	if (dev[0])
		return 0;
	snprintf(p, sizeof p, "%s/proc/net/route", root);
	if (!(f = fopen(p, "r")))
		return -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%63s %x", ifn, &dst) == 2 && dst == 0) {
			snprintf(dev, n, "%s", ifn);
			break;
		}
	fclose(f);
	return dev[0] ? 0 : -1;
}

void hl_init(const char *mode, int speed_mbps, const char *addr)
{
	const char *r = getenv("PERFCACHED_HOSTLINK_ROOT");
	int sp;

	snprintf(root, sizeof root, "%s", r ? r : "");
	fds_close();
	HL.on = HL.have = HL.i = 0;
	HL.rx_pct = HL.tx_pct = HL.rx_peak_pct = HL.tx_peak_pct = HL.speed_mbps = -1;
	HL.rx_bps = HL.tx_bps = HL.drops_ps = HL.udperr_ps = 0;
	HL.speed_cfg = speed_mbps > 0 ? speed_mbps : 0;
	HL.why[0] = HL.path[0] = HL.phys[0] = 0;
	snprintf(HL.want, sizeof HL.want, "%s", mode && strcasecmp(mode, "auto") ? mode : "");
	if (mode && !strcasecmp(mode, "off")) {
		snprintf(HL.why, sizeof HL.why, "link_monitor = off");
		return;
	}
	if (HL.want[0])
		snprintf(HL.dev, sizeof HL.dev, "%s", HL.want);
	else if (auto_dev(addr, HL.dev, sizeof HL.dev) != 0) {
		snprintf(HL.why, sizeof HL.why, "no device holds %.64s and there is no "
			"default route - set [daemon] link_monitor = <device>",
			addr ? addr : "the node's address");
		return;
	}
	if (resolve(HL.dev, HL.phys, sizeof HL.phys, HL.path, sizeof HL.path, &sp) != 0) {
		snprintf(HL.why, sizeof HL.why, "%.60s: no counters for %.60s", HL.dev,
			HL.phys);
		return;
	}
	HL.speed_mbps = HL.speed_cfg ? HL.speed_cfg : sp;
	fds_open(HL.phys);
	HL.on = 1;
}

void hl_read(struct hl_obs *o, long long now)
{
	memset(o, 0, sizeof *o);
	o->now = now;
	if (!HL.on)
		return;
	if (now - resolved_ms >= HL_RESOLVE_MS) {
		resolved_ms = now;
		if (resolve(HL.dev, o->phys, sizeof o->phys, o->path, sizeof o->path,
		        &o->speed) == 0 && strcmp(o->phys, open_phys)) {
			fds_open(o->phys);
			o->moved = 1;
		}
		if (!o->moved)
			o->phys[0] = o->path[0] = 0;
	} else
		o->speed = -2;                 /* not read this second */
	o->rx = rd_fd(fd_rx);
	o->tx = rd_fd(fd_tx);
	o->drops = rd_fd(fd_rxd) + rd_fd(fd_txd) + rd_fd(fd_miss);
	o->udperr = udp_rcvbuf_errors();
	o->ok = fd_rx >= 0 && fd_tx >= 0;
}

static int pct_of(long long bps, int mbps)
{
	long long p;

	if (mbps <= 0)
		return -1;
	p = bps * 8 * 100 / ((long long)mbps * 1000000);
	return p > 100 ? 100 : (int)p;
}

void hl_apply(const struct hl_obs *o, int fast)
{
	int cur, old, win, k, j, back;
	long long dt;

	if (!HL.on)
		return;
	if (o->moved) {
		snprintf(HL.phys, sizeof HL.phys, "%s", o->phys);
		snprintf(HL.path, sizeof HL.path, "%s", o->path);
		HL.have = 0;                   /* another device's counters */
	}
	if (o->speed != -2)
		HL.speed_mbps = HL.speed_cfg ? HL.speed_cfg : o->speed;
	if (!o->ok)
		return;
	cur = HL.i;
	HL.rx[cur] = o->rx;
	HL.tx[cur] = o->tx;
	HL.drops[cur] = o->drops;
	HL.udperr[cur] = o->udperr;
	HL.ms[cur] = o->now;
	HL.i = (cur + 1) % HM_N;
	if (HL.have < HM_N)
		HL.have++;
	win = fast > 1 ? (HM_N - 1) / fast + 1 : HM_N - 1;
	if (win > HL.have - 1)
		win = HL.have - 1;
	if (win < 1)
		return;
	old = (cur + HM_N - win) % HM_N;
	dt = HL.ms[cur] - HL.ms[old];
	if (dt <= 0 || HL.rx[cur] < HL.rx[old] || HL.tx[cur] < HL.tx[old]) {
		HL.have = 1;                   /* a counter reset */
		return;
	}
	HL.rx_bps = (long long)((HL.rx[cur] - HL.rx[old]) * 1000 / (unsigned long long)dt);
	HL.tx_bps = (long long)((HL.tx[cur] - HL.tx[old]) * 1000 / (unsigned long long)dt);
	HL.drops_ps = HL.drops[cur] >= HL.drops[old] ? (long long)((HL.drops[cur] -
		HL.drops[old]) * 1000 / (unsigned long long)dt) : 0;
	HL.udperr_ps = HL.udperr[cur] >= HL.udperr[old] ? (long long)((HL.udperr[cur] -
		HL.udperr[old]) * 1000 / (unsigned long long)dt) : 0;
	HL.rx_pct = pct_of(HL.rx_bps, HL.speed_mbps);
	HL.tx_pct = pct_of(HL.tx_bps, HL.speed_mbps);
	HL.rx_peak_pct = HL.tx_peak_pct = -1;
	back = HL_PEAK_S < win ? HL_PEAK_S : win;
	for (k = 0; k < back && HL.speed_mbps > 0; k++) {
		long long d;
		int a, b;

		j = (cur + HM_N - k) % HM_N;
		a = (j + HM_N - 1) % HM_N;
		d = HL.ms[j] - HL.ms[a];
		if (d <= 0 || HL.rx[j] < HL.rx[a] || HL.tx[j] < HL.tx[a])
			continue;              /* a reset between these two */
		b = pct_of((long long)((HL.rx[j] - HL.rx[a]) * 1000 / (unsigned long long)d),
			HL.speed_mbps);
		if (b > HL.rx_peak_pct)
			HL.rx_peak_pct = b;
		b = pct_of((long long)((HL.tx[j] - HL.tx[a]) * 1000 / (unsigned long long)d),
			HL.speed_mbps);
		if (b > HL.tx_peak_pct)
			HL.tx_peak_pct = b;
	}
}

int hl_pct(void)
{
	if (!HL.on || HL.rx_pct < 0)
		return -1;
	return HL.rx_pct > HL.tx_pct ? HL.rx_pct : HL.tx_pct;
}
