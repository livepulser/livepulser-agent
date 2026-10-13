#include "agent.h"

#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

#ifdef LP_TESTING
int64_t lp_time_divisor = 1;
#endif

void lp_wipe(void *p, size_t n)
{
	volatile unsigned char *b = p;

	while (n--)
		*b++ = 0;
}

int lp_random_bytes(void *buf, size_t n)
{
	unsigned char *p = buf;

	while (n > 0) {
		size_t chunk = n > 256 ? 256 : n;
		if (getentropy(p, chunk) != 0)
			return -1;
		p += chunk;
		n -= chunk;
	}
	return 0;
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
