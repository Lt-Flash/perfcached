/* hostlinktest.c - S465's link monitor (hostlink.c) against a fake /sys and
 * /proc (PERFCACHED_HOSTLINK_ROOT), on seconds made here.
 *
 *   A  the path: a VLAN to its active-backup bond to the active slave, its
 *      speed; an 802.3ad bond read as itself; off; a device with no counters
 *   B  the minute: known bytes a second give the share of the speed, the
 *      10-s peak, drops and the host's UDP RcvbufErrors a second; a speed of
 *      -1 counts bytes and gives no share; link_speed_mbps stands for it
 *   C  a failover: the active slave moves, the next resolution follows it
 *      and the minute starts again on the new device's counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hostlink.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static char R[256];

static void put(const char *rel, const char *fmt, unsigned long long v, const char *s)
{
	char p[512], d[512], *sl;
	FILE *f;

	snprintf(p, sizeof p, "%s/%s", R, rel);
	snprintf(d, sizeof d, "%s", p);
	for (sl = strchr(d + strlen(R) + 1, '/'); sl; sl = strchr(sl + 1, '/')) {
		*sl = 0;
		mkdir(d, 0755);
		*sl = '/';
	}
	if (!(f = fopen(p, "w")))
		return;
	if (s)
		fprintf(f, "%s\n", s);
	else
		fprintf(f, fmt, v);
	fclose(f);
}

static void ctr(const char *dev, const char *leaf, unsigned long long v)
{
	char rel[256];

	snprintf(rel, sizeof rel, "sys/class/net/%s/statistics/%s", dev, leaf);
	put(rel, "%llu\n", v, NULL);
}

static void lower(const char *dev, const char *low)
{
	char p[512], t[64];

	snprintf(p, sizeof p, "%s/sys/class/net/%s/lower_%s", R, dev, low);
	snprintf(t, sizeof t, "../%s", low);
	if (symlink(t, p) != 0)
		perror("symlink");
}

static void nic(const char *dev, int speed)
{
	char rel[256];
	const char *l[] = { "rx_bytes", "tx_bytes", "rx_dropped", "tx_dropped",
		"rx_missed_errors" };
	size_t i;

	for (i = 0; i < 5; i++)
		ctr(dev, l[i], 0);
	snprintf(rel, sizeof rel, "sys/class/net/%s/speed", dev);
	put(rel, "%llu\n", 0, speed < 0 ? "-1" : NULL);
	if (speed >= 0)
		put(rel, "%llu\n", (unsigned long long)speed, NULL);
}

static void snmp(unsigned long long rcvbuf)
{
	char v[256];

	snprintf(v, sizeof v, "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors\n"
		"Udp: 10 0 0 10 %llu 0", rcvbuf);
	put("proc/net/snmp", "", 0, v);
}

/* @n seconds from @t0 (ms), each adding @rx/@tx bytes and @dr drops on @dev */
static long long run(const char *dev, long long t0, int n, unsigned long long *rx,
		unsigned long long *tx, unsigned long long *dr, unsigned long long *ue,
		unsigned long long drx, unsigned long long dtx, unsigned long long ddr)
{
	struct hl_obs o;
	int k;

	for (k = 0; k < n; k++) {
		*rx += drx;
		*tx += dtx;
		*dr += ddr;
		*ue += 3;
		ctr(dev, "rx_bytes", *rx);
		ctr(dev, "tx_bytes", *tx);
		ctr(dev, "rx_dropped", *dr);
		snmp(*ue);
		hl_read(&o, t0 + k * 1000LL);
		hl_apply(&o, 0);
	}
	return t0 + n * 1000LL;
}

int main(void)
{
	unsigned long long rx = 0, tx = 0, dr = 0, ue = 0;
	long long t;
	char cmd[300];

	snprintf(R, sizeof R, "/tmp/hostlinktest.%d", (int)getpid());
	mkdir(R, 0755);
	setenv("PERFCACHED_HOSTLINK_ROOT", R, 1);
	nic("eno1", 10000);
	nic("eno2", 10000);
	put("sys/class/net/bond0/bonding/mode", "", 0, "active-backup 1");
	put("sys/class/net/bond0/bonding/active_slave", "", 0, "eno1");
	nic("bond0", 10000);
	lower("bond0", "eno1");
	lower("bond0", "eno2");
	nic("bond0.20", 10000);
	lower("bond0.20", "bond0");
	put("sys/class/net/bond1/bonding/mode", "", 0, "802.3ad 4");
	nic("bond1", 20000);
	nic("virt0", -1);
	snmp(0);

	/* A */
	hl_init("bond0.20", 0, NULL);
	CHK(HL.on && !strcmp(HL.path, "bond0.20 -> bond0 (active-backup) -> eno1") &&
	    !strcmp(HL.phys, "eno1") && HL.speed_mbps == 10000,
		"A: a VLAN to its active-backup bond to the active slave - '%s', %d Mb/s",
		HL.path, HL.speed_mbps);
	hl_init("bond1", 0, NULL);
	CHK(HL.on && !strcmp(HL.path, "bond1 (802.3ad)") && !strcmp(HL.phys, "bond1") &&
	    HL.speed_mbps == 20000, "A: an 802.3ad bond - its own counters, '%s'", HL.path);
	hl_init("off", 0, NULL);
	CHK(!HL.on && strstr(HL.why, "off"), "A: off - not watched, '%s'", HL.why);
	hl_init("nosuch9", 0, NULL);
	CHK(!HL.on && strstr(HL.why, "no counters"), "A: a device with no counters - '%s'", HL.why);

	/* B */
	hl_init("bond0.20", 0, NULL);
	t = run("eno1", 100000, 1, &rx, &tx, &dr, &ue, 0, 0, 0);
	CHK(HL.rx_pct < 0 && HL.have == 1, "B: one sample - no share yet");
	/* 625 MB/s in = 5 Gbit/s = 50%, 125 MB/s out = 10%, 7 drops a second */
	t = run("eno1", t, 60, &rx, &tx, &dr, &ue, 625000000ULL, 125000000ULL, 7);
	CHK(HL.rx_bps == 625000000LL && HL.rx_pct == 50 && HL.tx_pct == 10 &&
	    hl_pct() == 50, "B: 625 MB/s in, 125 MB/s out on 10 Gb/s - rx %d%%, tx %d%%",
	    HL.rx_pct, HL.tx_pct);
	CHK(HL.drops_ps == 7 && HL.udperr_ps == 3, "B: drops %lld/s, RcvbufErrors %lld/s",
		HL.drops_ps, HL.udperr_ps);
	t = run("eno1", t, 1, &rx, &tx, &dr, &ue, 1240000000ULL, 125000000ULL, 7);
	CHK(HL.rx_peak_pct == 99 && HL.rx_pct < 60,
		"B: one second at 1.24 GB/s - the 10-s peak %d%%, the minute %d%%",
		HL.rx_peak_pct, HL.rx_pct);
	hl_init("virt0", 0, NULL);
	rx = tx = dr = 0;
	t = run("virt0", t, 3, &rx, &tx, &dr, &ue, 1000000, 2000000, 0);
	CHK(HL.speed_mbps == -1 && HL.rx_bps == 1000000 && HL.rx_pct == -1 &&
	    hl_pct() == -1, "B: speed -1 - bytes counted (%lld/s), no share", HL.rx_bps);
	hl_init("virt0", 100, NULL);
	t = run("virt0", t, 3, &rx, &tx, &dr, &ue, 1000000, 2000000, 0);
	CHK(HL.speed_mbps == 100 && HL.tx_pct == 16,
		"B: link_speed_mbps = 100 stands for it - tx %d%%", HL.tx_pct);

	/* C */
	hl_init("bond0.20", 0, NULL);
	rx = tx = dr = 0;
	ctr("eno1", "rx_bytes", 0);
	t = run("eno1", t + HL_RESOLVE_MS, 5, &rx, &tx, &dr, &ue, 100000000, 0, 0);
	put("sys/class/net/bond0/bonding/active_slave", "", 0, "eno2");
	ctr("eno2", "rx_bytes", 5000000000000ULL);
	{
		unsigned long long rx2 = 5000000000000ULL, tx2 = 0, dr2 = 0;

		t = run("eno2", t + HL_RESOLVE_MS, 1, &rx2, &tx2, &dr2, &ue, 0, 0, 0);
		CHK(!strcmp(HL.phys, "eno2") && !strcmp(HL.path,
		    "bond0.20 -> bond0 (active-backup) -> eno2") && HL.have == 1,
			"C: the active slave moved - followed ('%s'), the minute restarted",
			HL.path);
		t = run("eno2", t, 3, &rx2, &tx2, &dr2, &ue, 250000000, 0, 0);
		CHK(HL.rx_pct == 20, "C: and eno2's own counters read - rx %d%%", HL.rx_pct);
	}

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", R);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", R);
	printf("hostlinktest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
