/*
 * clsynctest.c — staging a map change past the backup (step 3).
 *
 * Every test is a way "the backup is never behind the fleet" could stop
 * being true:
 *   - publishing something the backup never committed to
 *   - a late ack for an abandoned change committing its replacement
 *   - a reused sequence putting two different maps at one epoch
 *   - a promoted backup silently dropping a change it had already acked
 *   - a lost standby blocking the cluster instead of degrading it
 *
 * Build: cc -o clsynctest test/clsynctest.c src/clsync.o
 */
#include <stdio.h>

#include "../src/clsync.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(void)
{
	struct pc_clsync s;
	uint32_t a, b, t, q;

	/* ---- 1. the happy path, in order --------------------------- */
	pc_clsync_init(&s, 4, 100);
	pc_clsync_set_backup(&s, 1);
	CHK(!pc_clsync_publishable(&s), "an idle machine is publishable");
	CHK(pc_clsync_stage(&s, &a) == 0, "staging failed");
	CHK(a == 101, "staged at seq %u, expected 101", a);
	CHK(!pc_clsync_publishable(&s),
		"STAGED is publishable before any ack - the fleet could get a "
		"change the backup never committed to");
	CHK(pc_clsync_ack(&s, 4, 101) == 0, "a matching ack was refused");
	CHK(pc_clsync_publishable(&s), "an acked change is not publishable");
	CHK(pc_clsync_published(&s) == 0, "publish failed");
	CHK(s.published_seq == 101, "published_seq is %u", s.published_seq);
	CHK(!pc_clsync_publishable(&s), "still publishable after publishing");

	/* ---- 2. one change in flight at a time --------------------- */
	CHK(pc_clsync_stage(&s, &a) == 0, "second stage failed");
	CHK(pc_clsync_stage(&s, &b) == -1,
		"two changes staged at once - an ack could not say which one "
		"it meant");

	/* ---- 3. an ack must match the change in flight ------------- */
	CHK(pc_clsync_ack(&s, 4, a + 7) == -1,
		"an ack for the wrong sequence was accepted");
	CHK(pc_clsync_ack(&s, 9, a) == -1,
		"an ack from the wrong TERM was accepted - it came from a "
		"master that no longer exists");
	CHK(!pc_clsync_publishable(&s),
		"a mismatched ack made the change publishable");
	CHK(pc_clsync_ack(&s, 4, a) == 0, "the matching ack was refused");

	/* ---- 4. THE rule: an aborted sequence is never reused ------ */
	pc_clsync_init(&s, 4, 200);
	pc_clsync_set_backup(&s, 1);
	pc_clsync_stage(&s, &a);
	CHK(a == 201, "staged at %u", a);
	CHK(pc_clsync_abort(&s) == 0, "abort failed");
	pc_clsync_stage(&s, &b);
	CHK(b == 202,
		"a re-staged change reused seq %u. The backup may have acked "
		"the abandoned one with the ack lost - two DIFFERENT maps "
		"would then exist at the same epoch, which is exactly what "
		"the epoch exists to prevent", b);
	CHK(pc_clsync_ack(&s, 4, 201) == -1,
		"a late ack for the ABANDONED change committed its "
		"replacement");
	CHK(!pc_clsync_publishable(&s),
		"the replacement became publishable on the abandoned "
		"change's ack");

	/* ---- 5. no backup degrades, never blocks ------------------- */
	pc_clsync_init(&s, 4, 300);
	pc_clsync_set_backup(&s, 0);
	CHK(pc_clsync_stage(&s, &a) == 0, "staging with no backup failed");
	CHK(pc_clsync_publishable(&s),
		"with no standby the cluster cannot publish - a node could "
		"not join precisely when the cluster is already short a "
		"controller");
	CHK(s.staged_unsynced, "an unsynced publish was not marked as one");
	CHK(pc_clsync_published(&s) == 0, "unsynced publish failed");
	CHK(s.unsynced_n == 1, "unsynced publishes are not counted");

	/* a standby appearing mid-flight must not retroactively require an
	 * ack for something already committed */
	pc_clsync_init(&s, 4, 400);
	pc_clsync_set_backup(&s, 0);
	pc_clsync_stage(&s, &a);
	pc_clsync_set_backup(&s, 1);
	CHK(pc_clsync_publishable(&s),
		"a standby appearing mid-flight un-committed a change that "
		"was already publishable");

	/* losing the standby mid-flight must NOT abort: it may already have
	 * acked, with the ack in flight */
	pc_clsync_init(&s, 4, 500);
	pc_clsync_set_backup(&s, 1);
	pc_clsync_stage(&s, &a);
	pc_clsync_set_backup(&s, 0);
	CHK(s.state == PC_CLSYNC_STAGED,
		"losing the standby abandoned a change it may already have "
		"acked");
	CHK(pc_clsync_ack(&s, 4, a) == 0,
		"an ack arriving after the standby was marked gone was "
		"refused - it was in flight the whole time");

	/* ---- 6. backup side: what a promotion owes the fleet ------- */
	pc_clsync_init(&s, 7, 900);
	CHK(!pc_clsync_owed(&s, &t, &q), "a fresh backup owes something");

	pc_clsync_hold(&s, 7, 901);        /* we acked it */
	CHK(pc_clsync_owed(&s, &t, &q) && t == 7 && q == 901,
		"an acked change is not owed - promoting would silently drop "
		"a change the master had already committed");

	pc_clsync_saw(&s, 7, 900);          /* the fleet is still behind */
	CHK(pc_clsync_owed(&s, &t, &q),
		"seeing an OLDER epoch cleared the debt");

	pc_clsync_saw(&s, 7, 901);          /* now it has it */
	CHK(!pc_clsync_owed(&s, &t, &q),
		"seeing it published did not clear the debt - it would be "
		"republished for no reason");

	/* a later TERM supersedes: whatever we held was for a master that
	 * no longer exists, and someone else has since spoken */
	pc_clsync_hold(&s, 7, 950);
	pc_clsync_saw(&s, 8, 1);
	CHK(!pc_clsync_owed(&s, &t, &q),
		"a change held for a superseded term survived a later term - "
		"term must win over sequence here too");

	/* and an OLDER term must not clear it */
	pc_clsync_hold(&s, 7, 960);
	pc_clsync_saw(&s, 6, 99999);
	CHK(pc_clsync_owed(&s, &t, &q),
		"a huge sequence in an OLDER term cleared the debt");

	printf("clsynctest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
