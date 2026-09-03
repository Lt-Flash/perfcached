/*
 * cltermtest.c — the mastership term (control plane, step 2).
 *
 * The term exists to order maps issued by different masters.  Every
 * test here is a way that ordering could be lost:
 *   - a promoting node claiming from its own history instead of the
 *     fleet's, and so reissuing a term
 *   - a term acted on before it was persisted, and reissued after a
 *     restart with different content
 *   - a master ignoring a higher term and staying up, which is split
 *     brain by definition
 *   - one bad value taking the term somewhere nothing can out-term,
 *     after which the fleet can never elect a master again
 *
 * Build: cc -o cltermtest test/cltermtest.c src/clterm.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/clterm.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char dir[256];

static void rm_state(void)
{
	char p[512];

	snprintf(p, sizeof p, "%s/node-term", dir);
	unlink(p);
}

int main(void)
{
	uint32_t t;

	snprintf(dir, sizeof dir, "/var/tmp/pcterm.%d", (int)getpid());
	if (mkdir(dir, 0700) != 0) {
		printf("cannot create %s\n", dir);
		return 1;
	}

	/* ---- 1. a fresh node starts at zero and can persist ---------- */
	CHK(pc_term_init(dir) == 0, "init on an empty dir failed");
	CHK(pc_term_current() == 0, "a fresh node did not start at term 0");
	CHK(pc_term_durable(),
		"a writable state dir did not report durable - a read-only "
		"dir must be caught here, not at the moment of a promotion");

	/* ---- 2. claiming persists BEFORE returning ------------------ */
	t = pc_term_claim();
	CHK(t == 1, "first claim gave %u, expected 1", t);
	CHK(pc_term_init(dir) == 0 && pc_term_current() == 1,
		"a claimed term did not survive a reload - it could be "
		"reissued with different content");

	/* ---- 3. observing a peer moves us, and persists -------------- */
	CHK(pc_term_observe(5) == 1, "a higher peer term was not adopted");
	CHK(pc_term_current() == 5, "term is %u after observing 5",
		pc_term_current());
	CHK(pc_term_observe(3) == 0, "a LOWER peer term moved us");
	CHK(pc_term_current() == 5, "a lower term rewound us to %u",
		pc_term_current());
	CHK(pc_term_init(dir) == 0 && pc_term_current() == 5,
		"an observed term did not survive a reload");

	/* ---- 4. a promoting node claims above the FLEET, not itself -- */
	/* the case this rule exists for: we were away, the fleet moved on,
	 * we hear term 9, and must promote above 9 - not above our own 5 */
	pc_term_observe(9);
	t = pc_term_claim();
	CHK(t == 10, "claimed %u after seeing 9 - a term someone else may "
		"already have used", t);

	/* ---- 5. the poison guard ------------------------------------ */
	{
		unsigned long long before = pc_term_rejected;

		CHK(pc_term_observe(0xFFFFFFFFu) == -1,
			"an absurd term was accepted - nothing could ever "
			"out-term it and the fleet could not elect again");
		CHK(pc_term_rejected == before + 1,
			"a refused term was not counted");
		CHK(pc_term_current() == 10,
			"a refused term still moved us to %u",
			pc_term_current());
		CHK(pc_term_observe(10 + PC_TERM_MAX_JUMP) == 1,
			"a term at exactly the bound was refused - the guard "
			"is for nonsense, not for a long absence");
	}

	/* ---- 6. the three rules, as pure decisions ------------------- */
	CHK(pc_term_cmp(5, 4) == PC_TERM_STALE, "an older term is not stale");
	CHK(pc_term_cmp(5, 5) == PC_TERM_SAME, "equal terms differ");
	CHK(pc_term_cmp(5, 6) == PC_TERM_AHEAD, "a newer term is not ahead");

	CHK(pc_term_must_stepdown(1, 5, 6),
		"a master seeing a HIGHER term did not step down - this is "
		"split brain, by definition");
	CHK(!pc_term_must_stepdown(1, 5, 5),
		"a master stepped down for an EQUAL term");
	CHK(!pc_term_must_stepdown(1, 5, 4),
		"a master stepped down for an OLDER term - a deposed peer "
		"could unseat the legitimate master");
	CHK(!pc_term_must_stepdown(0, 5, 99),
		"a non-master 'stepped down'");

	/* ---- 7. a node with nowhere to persist ---------------------- */
	CHK(pc_term_init(NULL) == 0, "init with no state dir failed");
	CHK(pc_term_current() == 0, "no-state-dir node did not start at 0");
	CHK(!pc_term_durable(), "a node with no state dir claims durability");
	t = pc_term_claim();
	CHK(t == 1, "a node with no state dir could not claim (%u) - it has "
		"no old map to contradict, so 0 is correct for it", t);

	/* ---- 8. a corrupt term file refuses startup ----------------- */
	{
		char p[512];
		FILE *f;

		snprintf(p, sizeof p, "%s/node-term", dir);
		f = fopen(p, "wb");
		if (f) { fwrite("garbage!", 1, 8, f); fclose(f); }
		CHK(pc_term_init(dir) == -1,
			"a corrupt term file was accepted - a node that "
			"silently forgets its term can reissue one");
	}

	rm_state();
	rmdir(dir);
	printf("cltermtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
