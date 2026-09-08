#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>
#include <stddef.h>

size_t remove_ansi_escape_codes(char *buf);
void json_escape(const char* in, char* out, size_t out_size);
bool parse_int_fast(const char* s, const char* end, uint32_t *out);

#endif // _UTIL_H