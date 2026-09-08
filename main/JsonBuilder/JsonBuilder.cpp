#include "JsonBuilder.h"
#include <stdio.h>
#include <string.h>

JsonBuilder::JsonBuilder(char* buf, size_t capacity)
	: buf_(buf), cap_(capacity), len_(0), error_(false), depth_(0)
{
	if (!buf_ || cap_ == 0) { error_ = true; return; }
	buf_[0] = '\0';
}

void JsonBuilder::reset() {
	len_ = 0;
	error_ = false;
	depth_ = 0;
	buf_[0] = '\0';
}

bool JsonBuilder::ensure_(size_t need) {
	if (error_) {
		return false;
	}
	if (len_ + need >= cap_) {
		error_ = true;
		return false;
	}
	return true;
}

bool JsonBuilder::putChar_(char c) {
	if (!ensure_(1)) {
		return false;
	}
	buf_[len_++] = c;
	buf_[len_] = '\0';
	return true;
}

bool JsonBuilder::writeStr_(const char* s) {
	size_t n = strlen(s);
	if (!ensure_(n)) {
		return false;
	}
	memcpy(buf_ + len_, s, n);
	len_ += n;
	buf_[len_] = '\0';
	return true;
}

bool JsonBuilder::writeRaw_(const char* raw) {
	return writeStr_(raw);
}

bool JsonBuilder::writeEscaped_(const char* s) {
	if (!putChar_('\"')) {
		return false;
	}
	for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
		unsigned char c = *p;
		switch (c) {
			case '\"': 
				if (!writeStr_("\\\"")) {
					return false;
				}
				break;
			case '\\': 
				if (!writeStr_("\\\\")) {
					return false;
				}
				break;
			case '\b': 
				if (!writeStr_("\\b")) {
					return false;
				}
				break;
			case '\f': 
				if (!writeStr_("\\f")) {
					return false;
				}
				break;
			case '\n': 
				if (!writeStr_("\\n")) {
					return false;
				}
				break;
			case '\r': 
				if (!writeStr_("\\r")) {
					return false;
				}
				break;
			case '\t': 
				if (!writeStr_("\\t")) {
					return false;
				}
				break;
			default:
				if (c < 0x20) {
					if (!ensure_(6)) {
						error_ = true;
						return false;
					}
					int wrote = snprintf(buf_ + len_, cap_ - len_, "\\u%04X", (unsigned)c);
					if (wrote <= 0) {
						error_ = true;
						return false;
					}
					len_ += (size_t)wrote;
					buf_[len_] = '\0';
				} else {
					if (!putChar_((char)c)) {
						return false;
					}
				}
		}
	}
	return putChar_('\"');
}

bool JsonBuilder::writeString_(const char* s) {
	return writeEscaped_(s);
}

bool JsonBuilder::writeInt_(int32_t v) {
	if (!ensure_(11)) {
		return false;
	}
	int wrote = snprintf(buf_ + len_, cap_ - len_, "%ld", (long)v);
	if (wrote <= 0) {
		error_ = true;
		return false;
	}
	len_ += (size_t)wrote;
	return true;
}

bool JsonBuilder::writeUInt_(uint32_t v) {
	if (!ensure_(11)) {
		error_ = true;
		return false;
	}
	int wrote = snprintf(buf_ + len_, cap_ - len_, "%lu", (unsigned long)v);
	if (wrote <= 0) {
		error_ = true;
		return false;
	}
	len_ += (size_t)wrote;
	return true;
}

bool JsonBuilder::writeDouble_(double v, int prec) {
	if (prec < 0) {
		prec = 0;
	}
	if (prec > 15) {
		prec = 15;
	}
	if (!ensure_(32)) {
		error_ = true;
		return false;
	}
	int wrote = snprintf(buf_ + len_, cap_ - len_, "%.*f", prec, v);
	if (wrote <= 0) {
		error_ = true;
		return false;
	}
	len_ += (size_t)wrote;
	return true;
}

bool JsonBuilder::writeBool_(bool v) {
	return writeStr_(v ? "true" : "false");
}

bool JsonBuilder::beginCtx(CtxType t, const char* key) {
	if (key) {
		if (!addKey_(key)) {
			return false;
		}
		if (!putChar_(t == CTX_OBJECT ? '{' : '[')) {
			return false;
		}
		if (depth_ >= MAX_DEPTH) {
			error_ = true;
			return false;
		}
		stack_[depth_++] = Ctx{t, true};
		return true;
	} else {
		if (!addValue_()) {
			return false;
		}
		if (!putChar_(t == CTX_OBJECT ? '{' : '[')) {
			return false;
		}
		if (depth_ >= MAX_DEPTH) {
			error_ = true;
			return false;
		}
		stack_[depth_++] = Ctx{t, true};
		return true;
	}
}

bool JsonBuilder::endCtx(CtxType t) {
	if (error_) {
		return false;
	}
	if (depth_ == 0 || stack_[depth_-1].t != t) {
		error_ = true;
		return false;
	}
	--depth_;
	if (!putChar_(t == CTX_OBJECT ? '}' : ']')) {
		return false;
	}
	if (depth_ > 0) {
		stack_[depth_-1].first = false;
	}
	return true;
}

bool JsonBuilder::addKey_(const char* key) {
	if (!checkIn_(CTX_OBJECT)) {
		return false;
	}
	if (!maybeComma_()) {
		return false;
	}
	if (!writeEscaped_(key)) {
		return false;
	}
	if (!putChar_(':')) {
		return false;
	}
	return true;
}

bool JsonBuilder::addValue_() {
	if (depth_ == 0) {
		if (len_ != 0) {
			error_ = true;
			return false;
		}
		return true;
	}
	return maybeComma_();
}

bool JsonBuilder::maybeComma_() {
	if (depth_ == 0) {
		return (len_ == 0);
	}
	Ctx &c = stack_[depth_-1];
	if (c.first) { c.first = false; return true; }
	return putChar_(',');
}

bool JsonBuilder::checkIn_(CtxType t) {
	if (depth_ == 0) { error_ = true; return false; }
	if (stack_[depth_-1].t != t) { error_ = true; return false; }
	return true;
}

bool JsonBuilder::beginObject() {
	return beginCtx(CTX_OBJECT, nullptr);
}
bool JsonBuilder::endObject() {
	return endCtx(CTX_OBJECT);
}
bool JsonBuilder::beginArray() {
	return beginCtx(CTX_ARRAY,  nullptr);
}
bool JsonBuilder::endArray() {
	return endCtx(CTX_ARRAY); 
}

bool JsonBuilder::beginObject(const char* key) {
	return beginCtx(CTX_OBJECT, key);
}
bool JsonBuilder::beginArray (const char* key) {
	return beginCtx(CTX_ARRAY,  key);
}

bool JsonBuilder::add(const char* key, const char* value) {
	return addKey_(key) && writeString_(value);
}
bool JsonBuilder::addRaw(const char* key, const char* rawJson) {
	return addKey_(key) && writeRaw_(rawJson);
}
bool JsonBuilder::add(const char* key, esp_log_level_t v) {
	return addKey_(key) && writeInt_(v);
}
bool JsonBuilder::add(const char* key, int8_t v) {
	return addKey_(key) && writeInt_(v);
}
bool JsonBuilder::add(const char* key, int16_t v) {
	return addKey_(key) && writeInt_(v);
}
bool JsonBuilder::add(const char* key, int32_t v) {
	return addKey_(key) && writeInt_(v);
}
bool JsonBuilder::add(const char* key, uint8_t v) {
	return addKey_(key) && writeUInt_(v);
}
bool JsonBuilder::add(const char* key, uint16_t v) {
	return addKey_(key) && writeUInt_(v);
}
bool JsonBuilder::add(const char* key, uint32_t v) {
	return addKey_(key) && writeUInt_(v);
}
bool JsonBuilder::add(const char* key, double v, int prec) {
	return addKey_(key) && writeDouble_(v, prec);
}
bool JsonBuilder::add(const char* key, bool v) {
	return addKey_(key) && writeBool_(v);
}
bool JsonBuilder::addNull(const char* key) {
	return addKey_(key) && writeStr_("null");
}

bool JsonBuilder::push(const char* value) {
	return addValue_() && writeString_(value);
}
bool JsonBuilder::pushRaw(const char* rawJson) {
	return addValue_() && writeRaw_(rawJson);
}
bool JsonBuilder::push(esp_log_level_t v) {
	return addValue_() && writeInt_(v);
}
bool JsonBuilder::push(int8_t v) {
	return addValue_() && writeInt_(v);
}
bool JsonBuilder::push(int16_t v) {
	return addValue_() && writeInt_(v);
}
bool JsonBuilder::push(int32_t v) {
	return addValue_() && writeInt_(v);
}
bool JsonBuilder::push(uint8_t v) {
	return addValue_() && writeUInt_(v);
}
bool JsonBuilder::push(uint16_t v) {
	return addValue_() && writeUInt_(v);
}
bool JsonBuilder::push(uint32_t v) {
	return addValue_() && writeUInt_(v);
}
bool JsonBuilder::push(double v, int prec) {
	return addValue_() && writeDouble_(v, prec);
}
bool JsonBuilder::push(bool v) {
	return addValue_() && writeBool_(v);
}
bool JsonBuilder::pushNull() {
	return addValue_() && writeStr_("null");
}

bool JsonBuilder::finalize() {
	if (depth_ != 0) {
		return false;
	}
	return !error_;
}

const char* JsonBuilder::c_str() const {
	return buf_;
}
size_t JsonBuilder::size() const {
	return len_;
}
size_t JsonBuilder::capacity() const {
	return cap_;
}
bool JsonBuilder::ok() const {
	return !error_;
}