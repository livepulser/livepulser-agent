#include <stdio.h>

#include "agent.h"
#include "service.h"

int lp_service_run(int (*fn)(void *), void *arg)
{
	(void)fn;
	(void)arg;
	fprintf(stderr, "--service is only used on Windows; use systemd or launchd instead\n");
	return LP_EXIT_CONFIG;
}
