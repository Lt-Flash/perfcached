/* quiesce.h - per-thread quiescent state for retiring an index table read lock-free: a table
 * unpublished before a stamp is clear once every line is OUTSIDE or entered after the stamp. */
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

/* this thread holds a line and is inside a turn right now - a reader that
 * is not may not rely on the retire/clear protocol and must lock instead */
int pc_qs_inside(void);

/* attached lines, and how many are inside right now */
void pc_qs_figures(unsigned int *lines, unsigned int *inside);

#endif
