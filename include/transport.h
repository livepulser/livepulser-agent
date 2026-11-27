#ifndef LP_TRANSPORT_H
#define LP_TRANSPORT_H

#include "agent.h"
#include "options.h"

#define LP_ETAG_SIZE 130

struct lp_response {
	long status;
	bool transport_error;
	bool tls_error;
	char error[256];
	char etag[LP_ETAG_SIZE];
	int64_t retry_after_ms;
	int64_t server_date;
	int64_t config_version;
	char conflict[16];
	const char *body;
	size_t body_len;
	bool body_overflow;
};

struct lp_transport;

struct lp_transport *lp_transport_new(const struct lp_options *o, const char *api_key, char *err, size_t errlen);
void lp_transport_set_key(struct lp_transport *t, const char *api_key);
int lp_fetch_config(struct lp_transport *t, const char *etag, struct lp_response *r);
int lp_send_report(struct lp_transport *t, const char *body, size_t len, struct lp_response *r);
void lp_transport_free(struct lp_transport *t);

#endif
