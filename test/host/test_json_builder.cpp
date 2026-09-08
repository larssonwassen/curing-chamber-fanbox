// Host tests for JsonBuilder, built with ASan/UBSan.
//
// The builder writes into a caller-supplied fixed buffer and reports the result
// through size()/c_str(), which the MQTT publish calls pass straight to the
// client. Anything that lets len_ exceed the buffer therefore hands the client
// a length longer than the allocation.

#include "JsonBuilder.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool ok, const char* what) {
	printf("%-54s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

int main(void) {
	{
		char buf[128];
		JsonBuilder jb(buf, sizeof(buf));
		jb.beginObject();
		jb.add("a", (int32_t)-5);
		jb.add("b", true);
		jb.add("s", "x\"y");
		jb.endObject();
		check(jb.finalize() && strcmp(jb.c_str(), "{\"a\":-5,\"b\":true,\"s\":\"x\\\"y\"}") == 0,
			  "object with int, bool and escaped string");
		check(jb.size() == strlen(jb.c_str()), "  size() matches the string length");
	}

	{
		// 1e300 at 15 decimals is over 300 characters. The reservation before
		// the snprintf is only 32, so the overflow has to be caught after it.
		char buf[64];
		JsonBuilder jb(buf, sizeof(buf));
		jb.beginObject();
		jb.add("big", 1e300, 15);
		jb.endObject();
		check(!jb.finalize(), "oversized double is rejected");
		check(jb.size() <= sizeof(buf), "  size() stays within the buffer");
		check(strlen(buf) < sizeof(buf), "  buffer stays null-terminated in range");
	}

	{
		char buf[16];
		JsonBuilder jb(buf, sizeof(buf));
		jb.beginObject();
		for (int i = 0; i < 20; i++) {
			jb.add("key", "value");
		}
		jb.endObject();
		check(!jb.finalize(), "overflowing a small buffer is rejected");
		check(jb.size() < sizeof(buf), "  size() stays within the buffer");
	}

	{
		char buf[64];
		JsonBuilder jb(buf, sizeof(buf));
		jb.beginObject();
		jb.add("c", "a\x01\x1f" "b");
		jb.endObject();
		check(jb.finalize() && strcmp(jb.c_str(), "{\"c\":\"a\\u0001\\u001Fb\"}") == 0,
			  "control characters escape to \\u");
	}

	{
		char buf[64];
		JsonBuilder jb(buf, sizeof(buf));
		jb.beginArray();
		jb.push((uint32_t)4294967295u);
		jb.push(1.5, 2);
		jb.endArray();
		check(jb.finalize() && strcmp(jb.c_str(), "[4294967295,1.50]") == 0,
			  "array with uint32 max and a fixed-precision double");
	}

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures != 0;
}
