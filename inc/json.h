#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal JSON reader producing an immutable tree.
//
// Every accessor accepts NULL and reports failure for it, so lookups can be
// chained without intermediate checks:
//
//     double w;
//     if (json_as_double(json_at(json_get(json_get(win, "layout"), "tile_size"), 0), &w)) ...
//
// A missing key and a value of the wrong type look the same to the caller,
// which is what tolerant protocol readers want.

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_kind;

typedef struct json_value json_value;

// Parses exactly one JSON document from text[0..len), allowing surrounding
// whitespace. Returns NULL on any syntax error.
json_value *json_parse(const char *text, size_t len);
void json_free(json_value *value);

// False for a NULL (absent) value.
bool json_has_kind(const json_value *value, json_kind kind);

// Object member by key, or NULL when absent or value is not an object.
const json_value *json_get(const json_value *object, const char *key);

// Element count of an array or member count of an object; 0 otherwise.
size_t json_count(const json_value *value);
// Array element or object member value at index; NULL when out of range.
const json_value *json_at(const json_value *value, size_t index);
// Object member key at index; NULL when out of range or not an object.
const char *json_key_at(const json_value *object, size_t index);

bool json_as_bool(const json_value *value, bool *out);
// Succeeds only for non-negative integer literals that fit in 64 bits.
bool json_as_u64(const json_value *value, uint64_t *out);
bool json_as_double(const json_value *value, double *out);
// NUL-terminated string contents, or NULL when not a string.
const char *json_as_string(const json_value *value);

#endif // JSON_H
