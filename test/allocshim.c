/* allocshim.c - LD_PRELOAD shim counting malloc/calloc/realloc into the 64-bit word at offset 0 of
 * the file PC_ALLOC_COUNT names.  glibc only: on another libc or a sanitizer the count stays 0. */
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
