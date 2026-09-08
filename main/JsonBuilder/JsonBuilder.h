#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"

class JsonBuilder {
public:
    enum CtxType : uint8_t { CTX_OBJECT, CTX_ARRAY };

    JsonBuilder(char* buf, size_t capacity);
    void reset();

    bool beginObject();
    bool endObject();
    bool beginArray();
    bool endArray();
    bool beginObject(const char* key);
    bool beginArray(const char* key);

    bool add(const char* key, const char* value);
    bool addRaw(const char* key, const char* rawJson);
    bool add(const char* key, esp_log_level_t v);
    bool add(const char* key, int8_t v);
    bool add(const char* key, int16_t v);
    bool add(const char* key, int32_t v);
    bool add(const char* key, uint8_t v);
    bool add(const char* key, uint16_t v);
    bool add(const char* key, uint32_t v);
    bool add(const char* key, double v, int prec = 6);
    bool add(const char* key, bool v);
    bool addNull(const char* key);

    bool push(const char* value);
    bool pushRaw(const char* rawJson);
    bool push(esp_log_level_t v);
    bool push(int8_t v);
    bool push(int16_t v);
    bool push(int32_t v);
    bool push(uint8_t v);
    bool push(uint16_t v);
    bool push(uint32_t v);
    bool push(double v, int prec = 6);
    bool push(bool v);
    bool pushNull();

    bool finalize();

    const char* c_str() const;
    size_t size() const;
    size_t capacity() const;
    bool ok() const;

private:
    struct Ctx { CtxType t; bool first; };
    static constexpr uint8_t MAX_DEPTH = 16;

    char*  buf_;
    size_t cap_;
    size_t len_;
    bool   error_;
    Ctx    stack_[MAX_DEPTH];
    uint8_t depth_;

    bool ensure_(size_t need);
    bool commitPrintf_(int wrote);
    bool putChar_(char c);
    bool writeStr_(const char* s);
    bool writeRaw_(const char* raw);
    bool writeEscaped_(const char* s);
    bool writeString_(const char* s);
    bool writeInt_(int32_t v);
    bool writeUInt_(uint32_t v);
    bool writeDouble_(double v, int prec);
    bool writeBool_(bool v);

    bool beginCtx(CtxType t, const char* key);
    bool endCtx(CtxType t);
    bool addKey_(const char* key);
    bool addValue_();
    bool maybeComma_();
    bool checkIn_(CtxType t);
};
