#include "JsonParser.h"
#include <string.h>
#include <stdlib.h>   // strtod
#include <stdio.h>
#include <ctype.h>
#include <climits>


JsonParser::JsonParser(Node* pool, int pool_cap):
	pool_(pool),
	cap_(pool_cap),
	used_(0),
	root_(-1),
	error_(nullptr),
	p_(nullptr),
	end_(nullptr),
	base_(nullptr) {
}

void JsonParser::set_error_(const char* msg) {
	if (!error_) {
		error_ = msg;
	}
}

void JsonParser::skip_ws_() {
	while (p_ < end_) {
		char c = *p_;
		if (c==' '||c=='\t'||c=='\r'||c=='\n') { ++p_; continue; }
		break;
	}
}

bool JsonParser::consume_(char c) {
	skip_ws_();
	if (p_ < end_ && *p_ == c) { ++p_; return true; }
	return false;
}

bool JsonParser::expect_(char c) {
	skip_ws_();
	if (p_ < end_ && *p_ == c) { ++p_; return true; }
	set_error_("expected char");
	return false;
}

int JsonParser::new_node_(Type t, int parent) {
	if (used_ >= cap_) { set_error_("node pool exhausted"); return -1; }
	int idx = used_++;
	Node& n = pool_[idx];
	n.type = t;
	n.parent = (int16_t)parent;
	n.first_child = -1;
	n.next_sibling = -1;
	n.key = nullptr;
	n.as.s = nullptr;
	n.number_is_int = 0;
	n.i64 = 0;
	// link into parent's child list
	if (parent >= 0) {
		Node& p = pool_[parent];
		if (p.first_child == -1) {
			p.first_child = idx;
		} else {
			// append at end
			int it = p.first_child;
			while (pool_[it].next_sibling != -1) {
				it = pool_[it].next_sibling;
			}
			pool_[it].next_sibling = idx;
		}
	}
	return idx;
}

bool JsonParser::unescape_in_place_(char* start, char* stop, char** out_end) {
	// Converts escape sequences inside start..stop-1.
	// Returns true and sets *out_end to the new end (where '\0' should be placed).
	char* w = start;          // write head
	for (char* r = start; r < stop; ++r) {
		char c = *r;
		if (c != '\\') { *w++ = c; continue; }
		// escape
		if (++r >= stop) { set_error_("bad escape"); return false; }
		char e = *r;
		switch (e) {
			case '\"': *w++ = '\"'; break;
			case '\\': *w++ = '\\'; break;
			case '/':  *w++ = '/';  break;
			case 'b':  *w++ = '\b'; break;
			case 'f':  *w++ = '\f'; break;
			case 'n':  *w++ = '\n'; break;
			case 'r':  *w++ = '\r'; break;
			case 't':  *w++ = '\t'; break;
			case 'u': {
				if (r + 4 >= stop) { set_error_("short \\u"); return false; }
				auto hex = [](char ch)->int {
					if (ch>='0'&&ch<='9') return ch-'0';
					if (ch>='a'&&ch<='f') return ch-'a'+10;
					if (ch>='A'&&ch<='F') return ch-'A'+10;
					return -1;
				};
				int v=0;
				for (int i=0;i<4;i++) {
					int h = hex(*(++r));
					if (h<0) {
						set_error_("bad \\u hex");
						return false;
					}
					// Check for overflow before shifting
					if (v > 0x0FFFFFFF) {
						set_error_("unicode value too large");
						return false;
					}
					v = (v<<4)|h;
				}
				// Basic BMP to UTF-8 (no surrogate pair combining to keep tiny)
				if (v <= 0x7F) {
					*w++ = (char)v;
				} else if (v <= 0x7FF) {
					*w++ = (char)(0xC0 | (v>>6));
					*w++ = (char)(0x80 | (v&0x3F));
				} else {
					*w++ = (char)(0xE0 | (v>>12));
					*w++ = (char)(0x80 | ((v>>6)&0x3F));
					*w++ = (char)(0x80 | (v&0x3F));
				}
			} break;
			default: set_error_("unknown escape"); return false;
		}
	}
	*out_end = w;
	return true;
}

int JsonParser::parse_string_(char** out_str) {
	// p_ is at the opening '"'
	if (p_ >= end_ || *p_ != '\"') { set_error_("string expected"); return 0; }
	++p_;
	char* start = p_;
	while (p_ < end_) {
		char c = *p_;
		if (c == '\"') {
			char* raw_end = p_;
			// unescape in place (may shrink)
			char* new_end = nullptr;
			if (!unescape_in_place_(start, raw_end, &new_end)) {
				return 0;
			}
			*new_end = '\0';
			*out_str = start;
			++p_; // skip closing "
			return 1;
		} else if (c == '\\') {
			++p_; // skip, handled in unescape
			if (p_ >= end_) {
				set_error_("bad string escape");
				return 0;
			}
		} else if ((unsigned char)c < 0x20) {
			set_error_("control in string");
			return 0;
		}
		++p_;
	}
	set_error_("unterminated string");
	return 0;
}

bool JsonParser::parse_number_(double* out, int64_t* i64, uint8_t* is_int) {
	skip_ws_();
	char* s = p_;
	if (s >= end_) {
		return false;
	}

	// sign
	if (*p_ == '-') {
		++p_;
	}
	if (p_ >= end_) {
		return false;
	}

	// int part
	if (*p_ == '0') {
		++p_;
	} else {
		if (!isdigit((unsigned char)*p_)) {
			return false;
		}
		while (p_ < end_ && isdigit((unsigned char)*p_)) {
			++p_;
		}
	}
	// frac
	if (p_ < end_ && *p_ == '.') {
		++p_;
		if (p_ >= end_ || !isdigit((unsigned char)*p_)) {
			return false;
		}
		while (p_ < end_ && isdigit((unsigned char)*p_)) {
			++p_;
		}
	}
	// exp
	if (p_ < end_ && (*p_=='e' || *p_=='E')) {
		++p_;
		if (p_ < end_ && (*p_=='+'||*p_=='-')) {
			++p_;
		}
		if (p_ >= end_ || !isdigit((unsigned char)*p_)) {
			return false;
		}
		while (p_ < end_ && isdigit((unsigned char)*p_)) {
			++p_;
		}
	}

	// The scan above has already validated the token and left p_ one past it.
	// Copy it out to null-terminate for strtod. Writing the terminator into the
	// document instead (`char saved = *p_; *p_ = '\0';`) is a one-byte write at
	// json[len] whenever the number runs to the end of the buffer -- and the
	// MQTT path hands us a slice of the driver's receive buffer, where that byte
	// is not ours.
	char tok[64];
	size_t tok_len = (size_t)(p_ - s);
	if (tok_len == 0 || tok_len >= sizeof(tok)) {
		set_error_("number token too long");
		return false;
	}
	memcpy(tok, s, tok_len);
	tok[tok_len] = '\0';

	char* tok_endptr = nullptr;
	double dv = strtod(tok, &tok_endptr);
	if (!tok_endptr || tok_endptr == tok) {
		return false;
	}
	// Map the copy's end back onto the document so the offsets below, and p_,
	// stay expressed in terms of the original buffer.
	const char* endptr = s + (size_t)(tok_endptr - tok);

	// Try 64-bit integer exact fit
	int isint = 1;
	const char* t = s;
	if (*t=='-') {
		++t;
	}
	for (const char* q=t; q<endptr; ++q) {
		if (*q=='.'||*q=='e'||*q=='E') {
			isint = 0;
			break;
		}
	}
	int64_t iv = 0;
	if (isint) {
		// parse manually to avoid overflow undefined behavior
		int neg = (*s=='-');
		const char* q = s + neg;
		if (*q=='0' && (q+1)!=endptr) {
			/* leading zero */
		}
		int64_t acc = 0;
		while (q < endptr) {
			if (!isdigit((unsigned char)*q)) {
				isint=0;
				break;
			}
			int d = *q - '0';
			if (acc > (INT64_MAX - d) / 10) {
				isint = 0;
				break;
			}
			acc = acc*10 + d;
			++q;
		}
		if (isint) {
			iv = neg ? -acc : acc;
		}
	}

	*out = dv;
	*i64 = iv;
	*is_int = (uint8_t)isint;
	p_ = (char*)endptr;
	return true;
}

bool JsonParser::parse_bool_(uint8_t* out) {
	skip_ws_();
	if (end_ - p_ >= 4 && !strncmp(p_, "true", 4)) { *out = 1; p_ += 4; return true; }
	if (end_ - p_ >= 5 && !strncmp(p_, "false", 5)) { *out = 0; p_ += 5; return true; }
	return false;
}

bool JsonParser::parse_null_() {
	skip_ws_();
	if (end_ - p_ >= 4 && !strncmp(p_, "null", 4)) { p_ += 4; return true; }
	return false;
}

int JsonParser::parse_array_(int parent) {
	if (!expect_('[')) {
		return -1;
	}
	int idx = new_node_(T_ARRAY, parent);
	if (idx < 0) {
		return -1;
	}

	skip_ws_();
	if (consume_(']')) {
		return idx; // empty
	}

	while (true) {
		int vi = parse_value_(idx);
		if (vi < 0) {
			return -1;
		}
		skip_ws_();
		if (consume_(']')) {
			break;
		}
		if (!expect_(',')) {
			return -1;
		}
	}
	return idx;
}

int JsonParser::parse_object_(int parent) {
	if (!expect_('{')) {
		return -1;
	}
	int idx = new_node_(T_OBJECT, parent);
	if (idx < 0) {
		return -1;
	}

	skip_ws_();
	if (consume_('}')) {
		return idx; // empty
	}

	while (true) {
		skip_ws_();
		if (p_ >= end_ || *p_ != '\"') {
			set_error_("object key not string");
			return -1;
		}
		char* key = nullptr;
		if (!parse_string_(&key)) {
			return -1;
		}

		if (!expect_(':')) {
			return -1;
		}

		int vi = parse_value_(idx);
		if (vi < 0) {
			return -1;
		}
		// attach key to the just-added child
		// (the new node is the last child of idx)
		int child = pool_[idx].first_child;
		while (pool_[child].next_sibling != -1) {
			child = pool_[child].next_sibling;
		}
		pool_[child].key = key;

		skip_ws_();
		if (consume_('}')) {
			break;
		}
		if (!expect_(',')) {
			return -1;
		}
	}
	return idx;
}

int JsonParser::parse_value_(int parent) {
	skip_ws_();
	if (p_ >= end_) { set_error_("unexpected end"); return -1; }
	char c = *p_;
	if (c == '\"') {
		char* s = nullptr;
		if (!parse_string_(&s)) {
			return -1;
		}
		int idx = new_node_(T_STRING, parent);
		if (idx < 0) {
			return -1;
		}
		pool_[idx].as.s = s;
		return idx;
	}
	if (c == '{') {
		return parse_object_(parent);
	}
	if (c == '[') {
		return parse_array_(parent);
	}
	if (c == 't' || c == 'f') {
		uint8_t b = 0;
		if (!parse_bool_(&b)) {
			set_error_("bad bool");
			return -1;
		}
		int idx = new_node_(T_BOOL, parent);
		if (idx < 0) {
			return -1;
		}
		pool_[idx].as.b = b;
		return idx;
	}
	if (c == 'n') {
		if (!parse_null_()) {
			set_error_("bad null");
			return -1;
		}
		int idx = new_node_(T_NULL, parent);
		return idx;
	}
	// number
	double dv; int64_t iv; uint8_t isint;
	if (parse_number_(&dv, &iv, &isint)) {
		int idx = new_node_(T_NUMBER, parent);
		if (idx < 0) {
			return -1;
		}
		pool_[idx].as.num = dv;
		pool_[idx].i64 = iv;
		pool_[idx].number_is_int = isint;
		return idx;
	}
	set_error_("value expected");
	return -1;
}

int JsonParser::parse(char* json, size_t len) {
	error_ = nullptr;
	used_ = 0;
	root_ = -1;
	base_ = json;
	if (!json) {
		set_error_("null buffer");
		return -1;
	}
	if (len == 0) {
		len = strlen(json);
	}
	p_ = json;
	end_ = json + len;

	skip_ws_();
	int r = parse_value_(-1);
	if (r < 0) {
		return -1;
	}
	skip_ws_();
	if (p_ != end_) {
		set_error_("trailing data");
		return -1;
	}
	root_ = r;
	return root_;
}

int JsonParser::find(int obj_idx, const char* key) const {
	if (obj_idx < 0 || obj_idx >= used_) {
		return -1;
	}
	const Node& o = pool_[obj_idx];
	if (o.type != T_OBJECT) {
		return -1;
	}
	for (int c = o.first_child; c != -1; c = pool_[c].next_sibling) {
		if (c < 0 || c >= used_) {
			break; // Invalid index, stop searching
		}
		const Node& n = pool_[c];
		if (n.key && key && strcmp(n.key, key) == 0) {
			return c;
		}
	}
	return -1;
}
