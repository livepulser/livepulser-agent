#include "agent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LP_TESTING
int64_t lp_time_divisor = 1;
#endif

void lp_wipe(void *p, size_t n)
{
	volatile unsigned char *b = p;

	while (n--)
		*b++ = 0;
}

int64_t lp_jitter_ms(int64_t max_ms)
{
	uint64_t r;

	if (max_ms <= 0 || lp_random_bytes(&r, sizeof r) != 0)
		return 0;
	return (int64_t)(r % (uint64_t)(max_ms + 1));
}

int lp_uuid4(char out[LP_UUID_LEN + 1])
{
	unsigned char b[16];

	if (lp_random_bytes(b, sizeof b) != 0)
		return -1;
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	snprintf(out, LP_UUID_LEN + 1,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return 0;
}

bool lp_valid_uuid(const char *s)
{
	for (int i = 0; i < LP_UUID_LEN; i++) {
		bool dash = i == 8 || i == 13 || i == 18 || i == 23;
		if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
			return false;
	}
	return s[LP_UUID_LEN] == '\0';
}

static int64_t days_from_civil(int64_t y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* Parses the IMF-fixdate form required by RFC 9110, e.g. "Sun, 06 Nov 1994 08:49:37 GMT". */
int64_t lp_parse_http_date(const char *s)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4] = "";
	int day, year, h, mi, sec, n = 0;
	const char *found;

	if (sscanf(s, "%*3s, %2d %3s %4d %2d:%2d:%2d GMT%n", &day, mon, &year, &h, &mi, &sec, &n) != 6 || s[n] != '\0')
		return -1;
	found = strlen(mon) == 3 ? strstr(months, mon) : NULL;
	if (!found || (found - months) % 3 != 0 || day < 1 || day > 31 || year < 1970 || h > 23 || mi > 59 || sec > 60)
		return -1;
	return days_from_civil(year, (int)((found - months) / 3) + 1, day) * 86400 + h * 3600 + mi * 60 + sec;
}

int64_t lp_parse_retry_after(const char *value, int64_t now_unix)
{
	char *end;
	long long secs;
	int64_t when;

	while (*value == ' ')
		value++;
	if (*value >= '0' && *value <= '9') {
		secs = strtoll(value, &end, 10);
		while (*end == ' ')
			end++;
		if (*end)
			return -1;
		return secs > 86400 ? 86400 : secs;
	}
	when = lp_parse_http_date(value);
	if (when < 0)
		return -1;
	if (when <= now_unix)
		return 0;
	return when - now_unix > 86400 ? 86400 : when - now_unix;
}
