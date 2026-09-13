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
#include <stdlib.h>
#include <string.h>

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

	/* ---- the wire envelope ---------------------------------------
	 *
	 * [type][maplen4][map][histlen4][hist].  The three refusals below
	 * lived inline in cluster.c where nothing could reach them, and a
	 * control-state sync is what a promoted standby rebuilds the fleet
	 * from - a frame that parses loosely is worse here than most
	 * places.  Buffers are sized EXACTLY so ASan can see an overread. */
	{
		const unsigned char MAP[] = { 0xde, 0xad, 0xbe, 0xef, 0x11 };
		const unsigned char HIST[] = { 0x01, 0x02, 0x03 };
		const unsigned char *m, *h;
		uint32_t ml, hl, term, seq;
		unsigned char *e;
		size_t tot, i;

		tot = clsync_env_hist_at(sizeof MAP) + sizeof HIST;
		e = malloc(tot);
		memcpy(e + CLSYNC_ENV_MAP_AT, MAP, sizeof MAP);
		memcpy(e + clsync_env_hist_at(sizeof MAP), HIST, sizeof HIST);
		CHK(clsync_env_finish(e, tot, 21, sizeof MAP, sizeof HIST) == tot, "W1 the envelope frames to the expected length");
		CHK(e[0] == 21, "W2 the type byte is written");
		CHK(clsync_env_parse(e, tot, &m, &ml, &h, &hl), "W3 a whole envelope parses");
		CHK(ml == sizeof MAP && memcmp(m, MAP, ml) == 0, "W4 the map payload comes back intact");
		CHK(hl == sizeof HIST && memcmp(h, HIST, hl) == 0, "W5 the history payload comes back intact");

		/* every short length is refused, and nothing is read past the
		 * end - the buffer is cut to exactly the truncated size */
		for (i = 0; i < tot; i++) {
			unsigned char *t = malloc(i ? i : 1);

			memcpy(t, e, i);
			CHK(!clsync_env_parse(t, i, &m, &ml, &h, &hl), "W6 a truncated envelope is refused");
			free(t);
		}
		/* a TRAILING byte is a refusal too: the history must end
		 * exactly at the end, or the frame is not what it claims */
		{
			unsigned char *t = malloc(tot + 1);

			memcpy(t, e, tot);
			t[tot] = 0x5A;
			CHK(!clsync_env_parse(t, tot + 1, &m, &ml, &h, &hl), "W7 a trailing byte is refused, not ignored");
			free(t);
		}
		/* a map length that overruns the datagram */
		{
			unsigned char *t = malloc(tot);

			memcpy(t, e, tot);
			t[1] = 0xff; t[2] = 0xff;
			CHK(!clsync_env_parse(t, tot, &m, &ml, &h, &hl), "W8 a map length past the end is refused");
			free(t);
		}
		free(e);

		/* the ack */
		{
			unsigned char a[CLSYNC_ACK_LEN];

			CHK(clsync_ack_write(a, sizeof a, 22, 7, 99) ==
				CLSYNC_ACK_LEN, "W9 the ack is 9 bytes");
			CHK(clsync_ack_parse(a, sizeof a, &term, &seq) &&
				term == 7 && seq == 99, "W10 the ack round-trips");
			for (i = 0; i < CLSYNC_ACK_LEN; i++) {
				unsigned char *t = malloc(i ? i : 1);

				memcpy(t, a, i);
				CHK(!clsync_ack_parse(t, i, &term, &seq), "W11 a short ack is refused");
				free(t);
			}
		}
	}

	printf("clsynctest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
