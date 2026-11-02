/* S150 step B: per-thread quiescent state - see quiesce.h */
#include "quiesce.h"

struct qs_line {
	unsigned long long epoch;      /* 0 = outside; else the sequence
	                                * read on entry */
} __attribute__((aligned(64)));

static struct qs_line lines[PC_QS_LINES];
static unsigned int nlines;
static unsigned long long seq = 1; /* never 0: 0 is "outside" */
static __thread int me = -1;

void pc_qs_attach(void)
{
	unsigned int i;

	if (me >= 0)
		return;
	i = __atomic_fetch_add(&nlines, 1, __ATOMIC_ACQ_REL);
	if (i >= PC_QS_LINES) {
		__atomic_fetch_sub(&nlines, 1, __ATOMIC_ACQ_REL);
		return;                    /* uncounted: never blocks, never
		                            * protected - the daemon has far
		                            * fewer threads than lines */
	}
	me = (int)i;
}

/* The store and the fence together make this a Dekker pair with the
 * retirer's stamp: either our epoch is visible to a retirer that
 * stamped after we entered, or its unpublish is visible to us and the
 * pointer we take is the new one. */
void pc_qs_enter(void)
{
	if (me < 0)
		return;
	__atomic_store_n(&lines[me].epoch,
		__atomic_load_n(&seq, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void pc_qs_exit(void)
{
	if (me < 0)
		return;
	__atomic_store_n(&lines[me].epoch, 0, __ATOMIC_RELEASE);
}

int pc_qs_inside(void)
{
	return me >= 0 &&
		__atomic_load_n(&lines[me].epoch, __ATOMIC_RELAXED) != 0;
}

unsigned long long pc_qs_stamp(void)
{
	unsigned long long s = __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return s;
}

int pc_qs_clear(unsigned long long stamp)
{
	unsigned int i, n = __atomic_load_n(&nlines, __ATOMIC_ACQUIRE);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < n && i < PC_QS_LINES; i++) {
		unsigned long long e =
			__atomic_load_n(&lines[i].epoch, __ATOMIC_ACQUIRE);

		if (e && e < stamp)
			return 0;              /* inside since before the stamp */
	}
	return 1;
}

void pc_qs_figures(unsigned int *nl, unsigned int *inside)
{
	unsigned int i, n = __atomic_load_n(&nlines, __ATOMIC_ACQUIRE), in = 0;

	for (i = 0; i < n && i < PC_QS_LINES; i++)
		if (__atomic_load_n(&lines[i].epoch, __ATOMIC_RELAXED))
			in++;
	*nl = n > PC_QS_LINES ? PC_QS_LINES : n;
	*inside = in;
}
