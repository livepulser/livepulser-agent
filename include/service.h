#ifndef LP_SERVICE_H
#define LP_SERVICE_H

#define LP_SERVICE_NAME "LivePulserAgent"

/* Runs fn under the Windows service control manager. Other systems use their own supervisors. */
int lp_service_run(int (*fn)(void *), void *arg);

#endif
