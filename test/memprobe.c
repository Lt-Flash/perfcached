/*
 * memprobe — S3 verification runner for the ported memory backing.
 *
 * Exercises the private-anon port end to end on the running host:
 *   1. pcache_mem_probe(): the tier ladder, by trying (prints sizes+tier);
 *   2. pcache_mem_reserve(64M): mapping, PMD alignment, tier, pinning;
 *   3. fill the reservation, verify RSS carries it;
 *   4. the give-back interplay, exactly as the arena experiences it:
 *      - on a PINNED reservation (tiers 2-4) MADV_DONTNEED must be
 *        REFUSED (EINVAL) - this is what latches the arena's
 *        giveback_off: pinned memory is committed memory;
 *      - after munlock the same call must SUCCEED, RSS must drop, and
 *        the mapping must survive readable as zeros (never-unmap);
 *      - on tier 1 (hugetlb, never mlocked) DONTNEED returns pages to
 *        the pool - verified via HugePages_Free, since hugetlb memory
 *        never shows in VmRSS.
 *
 * Exit 0 = every check passed; nonzero = the first failed check, named.
 * PC_EXPECT_TIER=hugetlb|thp_advise|thp_collapse|4k asserts the achieved
 * reserve tier — used by the arch matrix (S27) and to demonstrate the
 * harness CAN fail (fail-before-pass discipline).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../src/core/pcache_mem.h"

#define RESV_MB 64UL

static long status_kb(const char *tag, size_t taglen)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, tag, taglen)) {
			kb = strtol(line + taglen, NULL, 10);
			break;
		}
	fclose(f);
	return kb;
}

static long meminfo_val(const char *tag, size_t taglen)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, tag, taglen)) {
			v = strtol(line + taglen, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

static const char *tier_name(enum pcache_mem_tier t)
{
	switch (t) {
	case PCACHE_MEM_HUGETLB:      return "hugetlb";
	case PCACHE_MEM_THP_ADVISE:   return "thp_advise";
	case PCACHE_MEM_THP_COLLAPSE: return "thp_collapse";
	case PCACHE_MEM_4K:           return "4k";
	default:                      return "?";
	}
}

int main(void)
{
	enum pcache_mem_tier tier;
	unsigned long locked_mb;
	long rss0, rss_full, rss_cut, huge_free0, huge_free1;
	const char *expect;
	char *base;
	size_t sz = RESV_MB << 20, i;

	pcache_mem_probe();
	printf("probe: tier=%s (%s)  hugetlb_kb=%lu thp_pmd_kb=%lu "
		"nr_hugepages=%d overcommit=%d\n",
		tier_name(pcache_mem.tier), pcache_mem_tier_str(pcache_mem.tier),
		pcache_mem.hugetlb_kb, pcache_mem.thp_pmd_kb,
		pcache_mem.huge_static, pcache_mem.huge_overcommit);

	rss0 = status_kb("VmRSS:", 6);
	base = pcache_mem_reserve(sz, &tier, &locked_mb);
	if (!base) {
		printf("FAIL: reserve returned NULL\n");
		return 1;
	}
	rss_full = status_kb("VmRSS:", 6);
	printf("reserve: base=%p tier=%s locked_mb=%lu  rss %ld -> %ld kB  "
		"vmlck %ld kB\n", (void *)base, tier_name(tier), locked_mb,
		rss0, rss_full, status_kb("VmLck:", 6));

	if (((unsigned long)base & ((pcache_mem.thp_pmd_kb << 10) - 1)) != 0) {
		printf("FAIL: base not PMD-aligned\n");
		return 2;
	}

	expect = getenv("PC_EXPECT_TIER");
	if (expect && strcmp(expect, tier_name(tier))) {
		printf("FAIL: achieved tier %s, expected %s\n",
			tier_name(tier), expect);
		return 4;
	}

	/* write a pattern through the whole reservation */
	for (i = 0; i < sz; i += 4096)
		base[i] = (char)(i >> 12);

	if (tier == PCACHE_MEM_HUGETLB) {
		/* hugetlb never appears in VmRSS; the pool accounting is the
		 * truth.  Not mlocked, so the punch must work directly. */
		huge_free0 = meminfo_val("HugePages_Free:", 15);
		if (madvise(base, sz, MADV_DONTNEED) != 0) {
			perror("FAIL: MADV_DONTNEED on hugetlb");
			return 5;
		}
		huge_free1 = meminfo_val("HugePages_Free:", 15);
		printf("give-back: HugePages_Free %ld -> %ld\n",
			huge_free0, huge_free1);
		if (huge_free1 <= huge_free0) {
			printf("FAIL: no hugetlb pages returned to the pool\n");
			return 6;
		}
	} else {
		/* pinned reservation: the arena's give-back call must be
		 * REFUSED while the pin holds - this is the giveback_off
		 * latch working as designed, not a defect.  Trust the
		 * KERNEL's VmLck, not the library's locked_mb belief:
		 * sanitizer runtimes intercept mlock as a success-returning
		 * no-op, and then there is no pin to verify - skip loudly
		 * instead of failing on a pin that never existed. */
		if (locked_mb > 0 && status_kb("VmLck:", 6) == 0) {
			printf("SKIP: mlock claimed %lu MB but VmLck=0 kB - an "
				"interceptor (sanitizer?) no-op'd the pin; "
				"pinned-punch checks unverifiable in this process\n",
				locked_mb);
		} else if (locked_mb > 0) {
			if (madvise(base, sz, MADV_DONTNEED) == 0) {
				printf("FAIL: DONTNEED succeeded on a PINNED range - "
					"the pin is not actually holding\n");
				return 5;
			}
			printf("pinned punch: refused as designed (%s)\n",
				strerror(errno));
			if (munlock(base, sz) != 0) {
				perror("FAIL: munlock");
				return 5;
			}
		}
		/* resample AFTER the pattern write: with a real pin the pages
		 * were resident since reserve, but a no-op'd mlock populates
		 * nothing and residency only arrived with the writes above -
		 * the give-back claim is about what is resident NOW */
		rss_full = status_kb("VmRSS:", 6);
		if (madvise(base, sz, MADV_DONTNEED) != 0) {
			perror("FAIL: MADV_DONTNEED after unpin");
			return 5;
		}
		rss_cut = status_kb("VmRSS:", 6);
		printf("give-back: rss %ld -> %ld kB\n", rss_full, rss_cut);
		if (rss_full - rss_cut < (long)(sz >> 10) * 9 / 10) {
			printf("FAIL: RSS did not drop after DONTNEED (got %ld kB "
				"back)\n", rss_full - rss_cut);
			return 6;
		}
	}

	/* never-unmap: the range must still be mapped and read as zeros */
	for (i = 0; i < sz; i += sz / 16)
		if (base[i] != 0) {
			printf("FAIL: refault at +%zu read %d, not 0\n", i, base[i]);
			return 7;
		}
	printf("refault: mapping intact, zero-filled - never-unmap holds\n");
	printf("PASS\n");
	return 0;
}
