/* S150 B: the quiescent-state primitive a table retirement waits on.
 * Readers are threads scripted from main: attach, enter, exit on
 * command, so every ordering below is exact, not raced.  A stamp is
 * taken AFTER an unpublish; it is clear once no line is inside since
 * before it - a thread that entered later, or parked since, cannot hold
 * what was unpublished. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "../src/quiesce.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

enum { CMD_NONE, CMD_ENTER, CMD_EXIT, CMD_QUIT };
struct rd {
	pthread_t t;
	volatile int cmd, done;
};

static void *reader(void *p)
{
	struct rd *r = p;

	pc_qs_attach();
	__atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
	for (;;) {
		int c;

		while (!(c = __atomic_load_n(&r->cmd, __ATOMIC_ACQUIRE)))
			usleep(500);
		r->cmd = 0;
		if (c == CMD_QUIT)
			break;
		if (c == CMD_ENTER)
			pc_qs_enter();
		else
			pc_qs_exit();
		__atomic_fetch_add(&r->done, 1, __ATOMIC_ACQ_REL);
	}
	return NULL;
}

static void tell(struct rd *r, int cmd)
{
	int d = __atomic_load_n(&r->done, __ATOMIC_ACQUIRE);

	__atomic_store_n(&r->cmd, cmd, __ATOMIC_RELEASE);
	if (cmd == CMD_QUIT)
		return;
	while (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) == d)
		usleep(500);
}

int main(void)
{
	struct rd a = { 0 }, b = { 0 };
	unsigned long long s0, s1, s2, s3, s4;
	unsigned int lines, inside;

	pthread_create(&a.t, NULL, reader, &a);
	pthread_create(&b.t, NULL, reader, &b);
	while (!__atomic_load_n(&a.done, __ATOMIC_ACQUIRE) ||
	       !__atomic_load_n(&b.done, __ATOMIC_ACQUIRE))
		usleep(500);
	pc_qs_figures(&lines, &inside);
	CHK(lines == 2 && inside == 0, "two lines attached, none inside (%u/%u)",
		lines, inside);

	s0 = pc_qs_stamp();
	CHK(pc_qs_clear(s0), "nobody inside: a stamp is clear at once");

	tell(&a, CMD_ENTER);
	s1 = pc_qs_stamp();
	CHK(!pc_qs_clear(s1), "A inside since before the stamp: it holds s1");
	CHK(pc_qs_clear(s0), "A entered after s0: it cannot hold what s0 unpublished");

	tell(&b, CMD_ENTER);                   /* after s1 */
	CHK(!pc_qs_clear(s1), "B entered after s1 and does not hold it; A still does");
	pc_qs_figures(&lines, &inside);
	CHK(inside == 2, "both inside now (%u)", inside);

	tell(&a, CMD_EXIT);
	CHK(pc_qs_clear(s1), "A parked: s1 is clear although B is still inside");

	s2 = pc_qs_stamp();
	CHK(!pc_qs_clear(s2), "B inside since before s2: it holds s2");
	tell(&b, CMD_EXIT);
	CHK(pc_qs_clear(s2), "B parked: s2 clear");

	/* leave and come back: the return is after the stamp, so it is not
	 * a hold on it */
	tell(&a, CMD_ENTER);
	s3 = pc_qs_stamp();
	tell(&a, CMD_EXIT);
	tell(&a, CMD_ENTER);
	CHK(pc_qs_clear(s3), "A left and re-entered after s3: clear, A still inside");
	pc_qs_figures(&lines, &inside);
	CHK(inside == 1, "one inside (%u)", inside);

	/* a thread that never attached (this one) is a no-op on both sides */
	pc_qs_enter();
	s4 = pc_qs_stamp();
	tell(&a, CMD_EXIT);
	CHK(pc_qs_clear(s4), "an unattached thread's enter is not a hold");
	pc_qs_exit();

	tell(&a, CMD_QUIT);
	tell(&b, CMD_QUIT);
	pthread_join(a.t, NULL);
	pthread_join(b.t, NULL);
	printf("quiesctest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
