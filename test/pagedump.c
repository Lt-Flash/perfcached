/* pagedump - the status page's bytes on stdout (S407 part 3, 0.7.0).
 * The daemon no longer serves the page; the tests that read its text
 * (the elements it carries, no external assets, the script pagejstest
 * runs) take it from here, the same object perfcached-dashboard links. */
#include <stdio.h>
#include "../src/statuspage.h"

int main(void)
{
	return fwrite(pc_status_page, 1, pc_status_page_len, stdout)
		== pc_status_page_len ? 0 : 1;
}
