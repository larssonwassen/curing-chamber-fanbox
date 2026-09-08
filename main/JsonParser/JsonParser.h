#pragma once
#include <stddef.h>
#include <stdint.h>

class JsonParser {
public:
    enum Type : uint8_t { T_NULL, T_BOOL, T_NUMBER, T_STRING, T_ARRAY, T_OBJECT };

    struct Node {
        Type     type;
        int16_t  parent;        // index of parent or -1
        int16_t  first_child;   // head of singly linked child list or -1
        int16_t  next_sibling;  // next sibling index or -1

        // For OBJECT members only
        const char* key;        // null-terminated key inside buffer (or nullptr)

        // Value payload (depending on type)
        union {
            const char* s;      // STRING: null-terminated in buffer
            double      num;    // NUMBER
            uint8_t     b;      // BOOL (0/1)
        } as;

        // Meta for numbers
        uint8_t number_is_int;  // 1 if parsed as 64-bit signed int fits
        int64_t i64;            // valid if number_is_int==1
    };

    // Construct with your own node pool
    JsonParser(Node* pool, int pool_cap);

    // Parse a JSON document in-place. 'json' must be mutable and null-terminated
    // (len optional; if 0, it will be computed with strlen()).
    // Returns index of root node (>=0) on success, or -1 on error.
    int parse(char* json, size_t len = 0);

    // Access
    const Node& node(int idx) const { return pool_[idx]; }
    int         root()        const { return root_; }
    const char* last_error()  const { return error_; }

    // Helpers
    static bool is_object(const Node& n) { return n.type == T_OBJECT; }
    static bool is_array (const Node& n) { return n.type == T_ARRAY;  }

    // Find child (object only). Returns index or -1.
    int find(int obj_idx, const char* key) const;

private:
    // ---- internal state ----
    Node*  pool_;
    int    cap_;
    int    used_;
    int    root_;
    const char* error_;

    // ---- scanning state ----
    char* p_;     // current
    char* end_;   // end (one past)
    char* base_;  // buffer start

    // ---- core parsing ----
    void  set_error_(const char* msg);
    void  skip_ws_();
    bool  consume_(char c);
    bool  expect_(char c);
    int   parse_value_(int parent);
    int   parse_object_(int parent);
    int   parse_array_ (int parent);
    int   parse_string_(char** out_str);     // returns 0 on error
    bool  unescape_in_place_(char* start, char* stop, char** out_end);
    bool  parse_number_(double* out, int64_t* i64, uint8_t* is_int);
    bool  parse_bool_(uint8_t* out);
    bool  parse_null_();

    int   new_node_(Type t, int parent);
};
