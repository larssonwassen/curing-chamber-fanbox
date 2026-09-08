// Host tests for the util.cpp string helpers, built with ASan/UBSan.
//
// json_escape and parse_int_fast both feed the MQTT log stream, where their
// inputs are arbitrary log text. The interesting cases are the ones where the
// output does not fit: truncation must still leave a valid JSON string.

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// util.cpp logs through UART_LOGV; the tests do not need the real streamer.
// Declared in log_streamer.h with C++ linkage, so the stub must match.
int log_no_stream(const char*, const char*, const char*, ...) { return 0; }

static int failures = 0;

static void check(bool ok, const char* what) {
	printf("%-54s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

int main(void) {
	{
		char out[32];
		json_escape("plain", out, sizeof(out));
		check(strcmp(out, "plain") == 0, "json_escape passes plain text through");
	}

	{
		char out[32];
		json_escape("a\"b\\c\nd\te", out, sizeof(out));
		check(strcmp(out, "a\\\"b\\\\c\\nd\\te") == 0, "json_escape escapes quote/backslash/nl/tab");
	}

	{
		char out[8];
		json_escape("\x01\x02", out, sizeof(out));
		check(strcmp(out, "\\u0001") == 0, "json_escape emits \\u for control chars");
	}

	{
		// Six quotes need twelve bytes; only four fit. The result must stop on a
		// character boundary rather than emitting a lone backslash or dropping
		// an escape and continuing.
		char out[6];
		json_escape("\"\"\"\"\"\"", out, sizeof(out));
		check(strcmp(out, "\\\"\\\"") == 0 && strlen(out) % 2 == 0,
			  "json_escape truncates on a character boundary");
	}

	{
		char out[4] = { 'x', 'x', 'x', 'x' };
		json_escape("abc", out, 0);
		check(out[0] == 'x', "json_escape with out_size 0 writes nothing");
	}

	{
		char buf[64];
		strcpy(buf, "\x1b[0;32mI (56) boot: hello\x1b[0m");
		size_t len = remove_ansi_escape_codes(buf);
		check(strcmp(buf, "I (56) boot: hello") == 0 && len == strlen(buf),
			  "remove_ansi_escape_codes strips CSI sequences");
	}

	{
		uint32_t v = 12345;
		const char* s = "4711";
		check(parse_int_fast(s, s + 4, &v) && v == 4711, "parse_int_fast parses digits");
	}

	{
		uint32_t v = 12345;
		const char* s = "";
		check(!parse_int_fast(s, s, &v), "parse_int_fast rejects an empty range");
	}

	{
		uint32_t v = 0;
		const char* s = "12a4";
		check(!parse_int_fast(s, s + 4, &v), "parse_int_fast rejects a non-digit");
	}

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures != 0;
}
