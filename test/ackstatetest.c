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

/* every close the code under test makes; the armed one closes but reports EIO.  A number is
 * reused by the next open, so a second close shows as a close of a number not open (EBADF) */
static int nclosed, nbad, fail_next;

int close(int fd)
{
	int rc = (int)syscall(SYS_close, fd);

	nclosed++;
	if (rc != 0 && errno == EBADF)
		nbad++;
	if (fail_next) {
		fail_next = 0;
		errno = EIO;
		return -1;
	}
	return rc;
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
	nclosed = nbad = 0;
	fail_next = 1;
	rc = pc_ack_save(dir, 5);
	CHK(rc == -1, "A: a failed close fails the save (rc %d)", rc);
	CHK(nclosed >= 1 && !nbad, "A: %d close(s), none of a number not open", nclosed);
	snprintf(path, sizeof path, "%s/" PC_ACK_FILE ".tmp.%d", dir, (int)getpid());
	CHK(stat(path, &st) != 0, "A: the temp file is removed");
	snprintf(path, sizeof path, "%s/" PC_ACK_FILE, dir);
	CHK(stat(path, &st) != 0, "A: no watermark file was put in place");

	/* ---- B: a clean save lands the file */
	pc_ack_capture();
	nclosed = nbad = 0;
	rc = pc_ack_save(dir, 6);
	CHK(rc == 0, "B: a clean save succeeds (rc %d)", rc);
	CHK(stat(path, &st) == 0 && st.st_size > 0, "B: the watermark file is there");
	CHK(nclosed >= 2 && !nbad, "B: %d close(s), none of a number not open", nclosed);

	unlink(path);
	rmdir(dir);
	printf("%s\n", fails ? "ackstatetest: FAIL" : "ackstatetest: all ok");
	return fails ? 1 : 0;
}
