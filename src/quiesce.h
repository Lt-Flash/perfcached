/* S150 step B: per-thread quiescent state, for retiring an index table.
 *
 * The read path is lock-free: a thread takes a table pointer from the
 * registry and reads through it with nothing held.  A table the registry
 * no longer publishes (a resize swapped it out, a drop emptied it) may
 * therefore still be under some thread's feet.  Every thread that can
 * hold such a pointer marks itself INSIDE for the span it may hold one -
 * a loop turn, a walk - and OUTSIDE when it parks in its wait.  A
 * retirer takes a stamp AFTER it unpublished the table; the table is
 * clear once every line is outside or entered after the stamp.
 *
 * Two stores per turn on the thread's own line, nothing on a fetch. */
#ifndef PC_QUIESCE_H
#define PC_QUIESCE_H

#define PC_QS_LINES 1024

/* this thread takes a line; a thread that never attached is never
 * counted, and its enter/exit are no-ops */
void pc_qs_attach(void);
void pc_qs_enter(void);
void pc_qs_exit(void);

/* a stamp, taken after the unpublish; clear when no attached thread can
 * still hold what was unpublished before it */
unsigned long long pc_qs_stamp(void);
int pc_qs_clear(unsigned long long stamp);

/* attached lines, and how many are inside right now */
void pc_qs_figures(unsigned int *lines, unsigned int *inside);

#endif
