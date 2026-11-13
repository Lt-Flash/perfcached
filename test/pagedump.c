/* pagedump - the status page's bytes on stdout for the tests that read its text: the daemon does
 * not serve it, and this links the same object perfcached-dashboard does. */
#include <stdio.h>
#include "../src/statuspage.h"

int main(void)
{
	return fwrite(pc_status_page, 1, pc_status_page_len, stdout)
		== pc_status_page_len ? 0 : 1;
}
