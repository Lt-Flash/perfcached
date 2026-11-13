/* privmovetest.c - a privileged restore on a fleet handle whose member refuses it, privilege raised
 * by perfd_enable (api), an enable command (plain) or dropped before a moving set (disable). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perfd.h"

int main(int ac, char **av)
{
	const char *secs[2], *mode = ac > 6 ? av[6] : "api";
	char params[256];
	perfd_opts o;
	perfd_t *p;
	char *r;
	int before, ok;

	if (ac < 6 || ac > 7 || (strcmp(mode, "api") && strcmp(mode, "plain") && strcmp(mode, "disable"))) {
		fprintf(stderr, "usage: privmovetest <host> <port> <secret> <enable> <key> [api|plain|disable]\n");
		return 2;
	}
	secs[0] = av[3];
	secs[1] = NULL;
	perfd_opts_init(&o);
	o.secrets = secs;
	o.encrypt = 1;                         /* enable never crosses in the clear */
	p = perfd_connect(av[1], atoi(av[2]), &o);
	if (!p) {
		printf("RESULT connect-failed err=%s\n", perfd_error(NULL));
		return 1;
	}
	if (!strcmp(mode, "plain")) {
		snprintf(params, sizeof params, "{\"secret\":\"%s\"}", av[4]);
		r = perfd_command(p, "enable", params);
		ok = r && strstr(r, "\"privileged\":true");
		free(r);
	} else {
		ok = perfd_enable(p, av[4]) == 0;
	}
	if (!ok) {
		printf("RESULT enable-failed err=%s\n", perfd_error(p));
		perfd_free(p);
		return 1;
	}
	before = perfd_active_node(p);
	if (!strcmp(mode, "disable")) {
		r = perfd_command(p, "disable", NULL);
		free(r);
		snprintf(params, sizeof params, "{\"col\":\"c\",\"key\":\"%s-set\",\"value\":\"x\"}", av[5]);
		r = perfd_command(p, "set", params);
		printf("SET rc=%d node=%d\n", r ? 0 : -1, perfd_active_node(p));
		free(r);
	}
	snprintf(params, sizeof params, "{\"col\":\"c\",\"policy\":\"newer\",\"records\":"
		"[{\"k\":\"%s\",\"v\":\"moved\",\"ver\":1}]}", av[5]);
	r = perfd_command(p, "restore", params);
	printf("RESULT rc=%d before=%d after=%d failovers=%llu members=%d %s=%s\n", r ? 0 : -1,
		before, perfd_active_node(p), perfd_failovers(p), perfd_member_count(p),
		r ? "reply" : "err", r ? r : perfd_error(p));
	free(r);
	perfd_free(p);
	return 0;
}
