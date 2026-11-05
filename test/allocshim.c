/* allocshim.c - S283: an LD_PRELOAD shim that COUNTS heap allocations, so
 * a test can assert that a request path allocates nothing per request.
 *
 * PC_ALLOC_COUNT names a file; the shim maps its first page shared and
 * adds one to the 64-bit counter at offset 0 on every malloc, calloc and
 * realloc (frees are not counted: an allocation is the cost).  The test
 * reads the file whenever it likes - no signal, no polling, nothing in
 * the daemon changes.
 *
 * glibc only: the calls are forwarded to glibc's own __libc_malloc and
 * friends, because dlsym(RTLD_NEXT) needs a feature-test macro and itself
 * allocates.  On another libc the shim interposes nothing and the counter
 * stays 0 - the test's positive control (the daemon allocates at startup)
 * then reports that the shim is not in effect, rather than a green run.
 * The same holds under a sanitizer, whose runtime comes first in the
 * preload list and answers malloc itself.
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t *counter;

__attribute__((constructor)) static void allocshim_init(void)
{
	const char *p = getenv("PC_ALLOC_COUNT");
	void *m;
	int fd;

	if (!p || !*p)
		return;
	fd = open(p, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return;
	if (ftruncate(fd, 4096) != 0) {
		close(fd);
		return;
	}
	m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m != MAP_FAILED)
		counter = m;
}

#ifdef __GLIBC__
extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t k, size_t n);
extern void *__libc_realloc(void *p, size_t n);

static void bump(void)
{
	if (counter)
		__atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

void *malloc(size_t n)
{
	bump();
	return __libc_malloc(n);
}

void *calloc(size_t k, size_t n)
{
	bump();
	return __libc_calloc(k, n);
}

void *realloc(void *p, size_t n)
{
	bump();
	return __libc_realloc(p, n);
}
#endif
