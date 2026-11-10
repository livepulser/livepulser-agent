#ifndef LP_OPTIONS_H
#define LP_OPTIONS_H

#include "agent.h"

struct lp_options {
	char endpoint[LP_ENDPOINT_SIZE];
	char monitor_id[LP_UUID_LEN + 1];
	char api_key_file[LP_FILE_PATH_SIZE];
	char state_dir[LP_FILE_PATH_SIZE];
	char ca_file[LP_FILE_PATH_SIZE];
	char proxy[LP_ENDPOINT_SIZE];
};

int lp_options_parse(const char *text, size_t len, struct lp_options *o, char *err, size_t errlen);
int lp_options_load(const char *path, struct lp_options *o, char *err, size_t errlen);
int lp_api_key_load(const char *path, char key[LP_API_KEY_LEN + 1], char *err, size_t errlen);

#endif
