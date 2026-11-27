/* ackstatetest.c - the watermark file's save (ackstate.c): a close that fails is not repeated on
 * the same number, the temp file goes, and a clean save still lands. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../src/ackstate.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* every close the code under test makes, by number; the armed one closes but reports EIO */
static int closed[64], nclosed, fail_next;

int close(int fd)
{
	int rc = (int)syscall(SYS_close, fd);

	if (nclosed < 64)
		closed[nclosed++] = fd;
	if (fail_next) {
		fail_next = 0;
		errno = EIO;
		return -1;
	}
	return rc;
}

static int closed_twice(void)
{
	int i, j;

	for (i = 0; i < nclosed; i++)
		for (j = i + 1; j < nclosed; j++)
			if (closed[i] == closed[j])
				return 1;
	return 0;
}

int main(void)
{
	char dir[] = "/tmp/ackstatetest.XXXXXX", path[600];
	struct stat st;
	int rc;

	if (!mkdtemp(dir) || pc_ack_init(0)) {
		printf("  FAIL setup\n");
		return 1;
	}
	pc_ack_applied(0x1111, 7, 1, 1);

	/* ---- A: the temp file's close fails: the save fails, nothing is closed twice */
	pc_ack_capture();
	nclosed = 0;
	fail_next = 1;
	rc = pc_ack_save(dir, 5);
	CHK(rc == -1, "A: a failed close fails the save (rc %d)", rc);
	CHK(nclosed >= 1 && !closed_twice(), "A: %d close(s), no number closed twice", nclosed);
	snprintf(path, sizeof path, "%s/" PC_ACK_FILE ".tmp.%d", dir, (int)getpid());
	CHK(stat(path, &st) != 0, "A: the temp file is removed");
	snprintf(path, sizeof path, "%s/" PC_ACK_FILE, dir);
	CHK(stat(path, &st) != 0, "A: no watermark file was put in place");

	/* ---- B: a clean save lands the file */
	pc_ack_capture();
	nclosed = 0;
	rc = pc_ack_save(dir, 6);
	CHK(rc == 0, "B: a clean save succeeds (rc %d)", rc);
	CHK(stat(path, &st) == 0 && st.st_size > 0, "B: the watermark file is there");
	CHK(!closed_twice(), "B: no number closed twice");

	unlink(path);
	rmdir(dir);
	printf("%s\n", fails ? "ackstatetest: FAIL" : "ackstatetest: all ok");
	return fails ? 1 : 0;
}
