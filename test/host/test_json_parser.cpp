// Host tests for JsonParser, built with ASan/UBSan.
//
// Every document is parsed from an exact-length heap allocation with no
// trailing null, which is how the MQTT path delivers payloads: a slice of the
// client's receive buffer. Any read or write past json[len-1] is then a real
// out-of-bounds access that ASan reports rather than a silent one that happens
// to land on a null terminator.

#include "JsonParser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JsonParser::Node pool[64];
static int failures = 0;

static void check(bool ok, const char* what) {
	printf("%-54s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

// Copies the literal into an allocation sized exactly to its length.
static char* exact(const char* lit, size_t* out_len) {
	size_t n = strlen(lit);
	char* buf = (char*)malloc(n);
	memcpy(buf, lit, n);
	*out_len = n;
	return buf;
}

int main(void) {
	size_t n;

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"shared\":{\"humidity_setpoint\":75.5}}", &n);
		int r = jp.parse(b, n);
		check(r >= 0, "object with a trailing float");
		int s = jp.find(r, "shared");
		int h = jp.find(s, "humidity_setpoint");
		check(h >= 0 && jp.node(h).as.num == 75.5, "  nested value reads back as 75.5");
		free(b);
	}

	{
		// The number is the last token, so terminating it in place wrote at
		// json[len] -- one byte past the buffer.
		JsonParser jp(pool, 64);
		char* b = exact("{\"a\":123}", &n);
		int r = jp.parse(b, n);
		int a = jp.find(r, "a");
		check(r >= 0 && jp.node(a).number_is_int && jp.node(a).i64 == 123,
			  "trailing integer in an exact-length buffer");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("-1.5e3", &n);
		int r = jp.parse(b, n);
		check(r >= 0 && jp.node(r).as.num == -1500.0, "bare number ending at the buffer end");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"n\":1111111111111111111111111111111111111111111111111111111111111111111}", &n);
		check(jp.parse(b, n) < 0, "over-long number token is rejected, not overrun");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"s\":\"a\\\"b\\n\\u00e5\"}", &n);
		int r = jp.parse(b, n);
		int s = jp.find(r, "s");
		check(r >= 0 && strcmp(jp.node(s).as.s, "a\"b\n\xc3\xa5") == 0,
			  "string unescaping, including \\u to UTF-8");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"a\":tru", &n);
		check(jp.parse(b, n) < 0, "literal truncated at the buffer end is rejected");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"a\":1", &n);
		check(jp.parse(b, n) < 0, "object truncated at the buffer end is rejected");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("{\"s\":\"unterminated", &n);
		check(jp.parse(b, n) < 0, "unterminated string is rejected");
		free(b);
	}

	{
		JsonParser jp(pool, 64);
		char* b = exact("[1,2,3]", &n);
		int r = jp.parse(b, n);
		check(r >= 0 && JsonParser::is_array(jp.node(r)), "array");
		free(b);
	}

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures != 0;
}
