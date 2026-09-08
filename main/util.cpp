#include "util.h"
#include "log_streamer.h"
extern "C" {
	#include <string.h>
	#include <stdio.h>
}

static int is_csi_final(unsigned char c) { return c >= 0x40 && c <= 0x7E; }
static int is_esc_intermediate(unsigned char c) { return c >= 0x20 && c <= 0x2F; }
static int is_esc_final(unsigned char c) { return c >= 0x30 && c <= 0x7E; }

/**
 * @brief
 * 
 * @param buf 
 * @return size_t new length of the buffer
 */
size_t remove_ansi_escape_codes(char *buf) {
	unsigned char *src = (unsigned char *)buf;
	unsigned char *dst = (unsigned char *)buf;

	while (*src) {
		if (*src == 0x1B) { // ESC (0x1B)
			unsigned char *p = src + 1;
			if (!*p) break;

			if (*p == '[') {
				// CSI: ESC [ ... (final 0x40–0x7E)
				p++;
				while (*p && !is_csi_final(*p)) p++;
				if (*p) p++; // consume final
				src = p;
				continue;
			} else if (*p == ']') {
				// OSC: ESC ] ... BEL(0x07) or ST (ESC \)
				p++;
				for (;;) {
					if (!*p) break;
					if (*p == 0x07) { p++; break; }               // BEL
					if (*p == 0x1B && p[1] == '\\') { p += 2; break; } // ST
					p++;
				}
				src = p;
				continue;
			} else if (*p == 'P' || *p == 'X' || *p == '^' || *p == '_') {
				// DCS/ SOS/ PM/ APC: ESC P|X|^|_ ... ST
				p++;
				for (;;) {
					if (!*p) break;
					if (*p == 0x1B && p[1] == '\\') { p += 2; break; } // ST
					p++;
				}
				src = p;
				continue;
			} else {
				// 2-byte Fe escape (ESC followed by 0x40–0x5F), or general ESC sequence
				if (is_esc_final(*p)) {
					// Consume intermediates + final (per ESC Fe/Fs form)
					do { p++; } while (*p && is_esc_intermediate(*p));
					if (*p && is_esc_final(*p)) p++;
				} else {
					// Unknown/partial; drop ESC only
					p++;
				}
				src = p;
				continue;
			}
		} else if (*src == 0x9B) {
			// 8-bit CSI
			unsigned char *p = src + 1;
			while (*p && !is_csi_final(*p)) p++;
			if (*p) p++;
			src = p;
			continue;
		} else if (*src == 0x9D) {
			// 8-bit OSC: ... ST(0x9C)
			unsigned char *p = src + 1;
			while (*p && *p != 0x9C) p++;
			if (*p) p++;
			src = p;
			continue;
		} else if (*src == 0x90) {
			// 8-bit DCS: ... ST(0x9C)
			unsigned char *p = src + 1;
			while (*p && *p != 0x9C) p++;
			if (*p) p++;
			src = p;
			continue;
		} else {
			// Normal byte
			*dst++ = *src++;
		}
	}
	*dst = '\0';
	return (size_t)(dst - (const unsigned char*)buf);
}

// Simple escape helper
void json_escape(const char* in, char* out, size_t out_size) {
	size_t in_len = strlen(in);
	size_t out_pos = 0;
	
	for (size_t i = 0; i < in_len && out_pos < out_size - 1; i++) {
		char c = in[i];
		switch (c) {
			case '\"': 
				if (out_pos < out_size - 2) {
					out[out_pos++] = '\\';
					out[out_pos++] = '\"';
				}
				break;
			case '\\': 
				if (out_pos < out_size - 2) {
					out[out_pos++] = '\\';
					out[out_pos++] = '\\';
				}
				break;
			case '\n': 
				if (out_pos < out_size - 2) {
					out[out_pos++] = '\\';
					out[out_pos++] = 'n';
				}
				break;
			case '\r': 
				if (out_pos < out_size - 2) {
					out[out_pos++] = '\\';
					out[out_pos++] = 'r';
				}
				break;
			case '\t': 
				if (out_pos < out_size - 2) {
					out[out_pos++] = '\\';
					out[out_pos++] = 't';
				}
				break;
			default:
				if ((unsigned char)c < 0x20) {
					// For control characters, use \u format
					if (out_pos < out_size - 6) {
						snprintf(out + out_pos, out_size - out_pos, "\\u%04x", (unsigned char)c);
						out_pos += 6;
					}
				} else {
					out[out_pos++] = c;
				}
				break;
		}
	}
	out[out_pos] = '\0';
}

bool parse_int_fast(const char* s, const char* end, uint32_t *out) {
	// Convert string into integer
	// Overflow is not checked and is OK if that happens.
	*out = 0;
	for (const char* p = s; p < end; ++p) {
		if (*p < '0' || *p > '9') {
			UART_LOGV("util", "Could not parse int from '%.*s', char: '%c' is not a digit.", (int)(end - s), s, *p);
			return false;
		}
		*out = (*out * 10) + (*p - '0');
	}
	return true;
}
