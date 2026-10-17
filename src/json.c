#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "json.h"

// Nesting bound so hostile or corrupt input cannot exhaust the stack.
#define JSON_MAX_DEPTH 128

struct json_value {
    json_kind kind;
    union {
        bool boolean;
        char *number; // raw literal, already validated against the JSON grammar
        char *string;
        struct {
            json_value **items;
            char **keys; // NULL for arrays
            size_t count;
        } container;
    } as;
};

typedef struct {
    const char *pos;
    const char *end;
    int depth;
} parser;

static json_value *parse_value(parser *p);

static void skip_whitespace(parser *p) {
    while (p->pos < p->end && (*p->pos == ' ' || *p->pos == '\t' || *p->pos == '\n' || *p->pos == '\r'))
        p->pos++;
}

static bool consume(parser *p, char c) {
    if (p->pos < p->end && *p->pos == c) {
        p->pos++;
        return true;
    }
    return false;
}

static bool consume_literal(parser *p, const char *literal) {
    size_t len = strlen(literal);
    if ((size_t)(p->end - p->pos) < len || memcmp(p->pos, literal, len) != 0)
        return false;
    p->pos += len;
    return true;
}

static json_value *new_value(json_kind kind) {
    json_value *value = calloc(1, sizeof(*value));
    if (value)
        value->kind = kind;
    return value;
}

// Growable byte buffer for decoding strings.
typedef struct {
    char *data;
    size_t len, cap;
} byte_buffer;

static bool buffer_push(byte_buffer *b, const char *bytes, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 32;
        while (b->len + n + 1 > cap)
            cap *= 2;
        char *data = realloc(b->data, cap);
        if (!data)
            return false;
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool buffer_push_utf8(byte_buffer *b, uint32_t cp) {
    char out[4];
    size_t n;
    if (cp < 0x80) {
        out[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return buffer_push(b, out, n);
}

static bool parse_hex4(parser *p, uint32_t *out) {
    if (p->end - p->pos < 4)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->pos[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    p->pos += 4;
    *out = v;
    return true;
}

static bool parse_escape(parser *p, byte_buffer *b) {
    if (p->pos >= p->end)
        return false;
    char c = *p->pos++;
    switch (c) {
        case '"':  return buffer_push(b, "\"", 1);
        case '\\': return buffer_push(b, "\\", 1);
        case '/':  return buffer_push(b, "/", 1);
        case 'b':  return buffer_push(b, "\b", 1);
        case 'f':  return buffer_push(b, "\f", 1);
        case 'n':  return buffer_push(b, "\n", 1);
        case 'r':  return buffer_push(b, "\r", 1);
        case 't':  return buffer_push(b, "\t", 1);
        case 'u': {
            uint32_t cp;
            if (!parse_hex4(p, &cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t low;
                if (!consume_literal(p, "\\u") || !parse_hex4(p, &low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return false;
            }
            return buffer_push_utf8(b, cp);
        }
        default:
            return false;
    }
}

// Decodes a string token into a freshly allocated NUL-terminated buffer.
static char *parse_string_contents(parser *p) {
    if (!consume(p, '"'))
        return NULL;
    byte_buffer b = {0};
    if (!buffer_push(&b, "", 0))
        return NULL;
    while (p->pos < p->end) {
        const char *run = p->pos;
        while (p->pos < p->end && *p->pos != '"' && *p->pos != '\\' && (unsigned char)*p->pos >= 0x20)
            p->pos++;
        if (!buffer_push(&b, run, (size_t)(p->pos - run)))
            break;
        if (p->pos >= p->end || (unsigned char)*p->pos < 0x20)
            break;
        if (*p->pos == '"') {
            p->pos++;
            return b.data;
        }
        p->pos++; // backslash
        if (!parse_escape(p, &b))
            break;
    }
    free(b.data);
    return NULL;
}

static bool skip_digits(parser *p) {
    const char *start = p->pos;
    while (p->pos < p->end && *p->pos >= '0' && *p->pos <= '9')
        p->pos++;
    return p->pos > start;
}

static json_value *parse_number(parser *p) {
    const char *start = p->pos;
    consume(p, '-');
    if (consume(p, '0')) {
        // A leading zero may not be followed by more digits
    } else if (!skip_digits(p)) {
        return NULL;
    }
    if (consume(p, '.') && !skip_digits(p))
        return NULL;
    if (consume(p, 'e') || consume(p, 'E')) {
        if (!consume(p, '+'))
            consume(p, '-');
        if (!skip_digits(p))
            return NULL;
    }

    json_value *value = new_value(JSON_NUMBER);
    if (!value)
        return NULL;
    value->as.number = strndup(start, (size_t)(p->pos - start));
    if (!value->as.number) {
        free(value);
        return NULL;
    }
    return value;
}

static bool container_push(json_value *container, char *key, json_value *item) {
    size_t n = container->as.container.count + 1;
    json_value **items = realloc(container->as.container.items, n * sizeof(*items));
    if (!items)
        return false;
    container->as.container.items = items;
    if (container->kind == JSON_OBJECT) {
        char **keys = realloc(container->as.container.keys, n * sizeof(*keys));
        if (!keys)
            return false;
        container->as.container.keys = keys;
        keys[n - 1] = key;
    }
    items[n - 1] = item;
    container->as.container.count = n;
    return true;
}

// Parses an array or object body after its opening bracket.
static json_value *parse_container(parser *p, json_kind kind, char close) {
    if (++p->depth > JSON_MAX_DEPTH)
        return NULL;
    json_value *container = new_value(kind);
    if (!container)
        return NULL;

    skip_whitespace(p);
    if (consume(p, close)) {
        p->depth--;
        return container;
    }

    for (;;) {
        char *key = NULL;
        skip_whitespace(p);
        if (kind == JSON_OBJECT) {
            key = parse_string_contents(p);
            skip_whitespace(p);
            if (!key || !consume(p, ':')) {
                free(key);
                break;
            }
        }
        json_value *item = parse_value(p);
        if (!item || !container_push(container, key, item)) {
            free(key);
            json_free(item);
            break;
        }
        skip_whitespace(p);
        if (consume(p, close)) {
            p->depth--;
            return container;
        }
        if (!consume(p, ','))
            break;
    }
    json_free(container);
    return NULL;
}

static json_value *parse_value(parser *p) {
    skip_whitespace(p);
    if (p->pos >= p->end)
        return NULL;

    json_value *value;
    switch (*p->pos) {
        case '{':
            p->pos++;
            return parse_container(p, JSON_OBJECT, '}');
        case '[':
            p->pos++;
            return parse_container(p, JSON_ARRAY, ']');
        case '"': {
            char *s = parse_string_contents(p);
            if (!s || !(value = new_value(JSON_STRING))) {
                free(s);
                return NULL;
            }
            value->as.string = s;
            return value;
        }
        case 't':
        case 'f': {
            bool b = *p->pos == 't';
            if (!consume_literal(p, b ? "true" : "false") || !(value = new_value(JSON_BOOL)))
                return NULL;
            value->as.boolean = b;
            return value;
        }
        case 'n':
            return consume_literal(p, "null") ? new_value(JSON_NULL) : NULL;
        default:
            return parse_number(p);
    }
}

json_value *json_parse(const char *text, size_t len) {
    parser p = {.pos = text, .end = text + len, .depth = 0};
    json_value *value = parse_value(&p);
    skip_whitespace(&p);
    if (value && p.pos != p.end) {
        json_free(value);
        return NULL;
    }
    return value;
}

void json_free(json_value *value) {
    if (!value)
        return;
    switch (value->kind) {
        case JSON_NUMBER:
            free(value->as.number);
            break;
        case JSON_STRING:
            free(value->as.string);
            break;
        case JSON_ARRAY:
        case JSON_OBJECT:
            for (size_t i = 0; i < value->as.container.count; i++) {
                json_free(value->as.container.items[i]);
                if (value->as.container.keys)
                    free(value->as.container.keys[i]);
            }
            free(value->as.container.items);
            free(value->as.container.keys);
            break;
        default:
            break;
    }
    free(value);
}

bool json_has_kind(const json_value *value, json_kind kind) {
    return value && value->kind == kind;
}

const json_value *json_get(const json_value *object, const char *key) {
    if (!json_has_kind(object, JSON_OBJECT))
        return NULL;
    for (size_t i = 0; i < object->as.container.count; i++) {
        if (strcmp(object->as.container.keys[i], key) == 0)
            return object->as.container.items[i];
    }
    return NULL;
}

size_t json_count(const json_value *value) {
    if (json_has_kind(value, JSON_ARRAY) || json_has_kind(value, JSON_OBJECT))
        return value->as.container.count;
    return 0;
}

const json_value *json_at(const json_value *value, size_t index) {
    return index < json_count(value) ? value->as.container.items[index] : NULL;
}

const char *json_key_at(const json_value *object, size_t index) {
    if (!json_has_kind(object, JSON_OBJECT) || index >= object->as.container.count)
        return NULL;
    return object->as.container.keys[index];
}

bool json_as_bool(const json_value *value, bool *out) {
    if (!json_has_kind(value, JSON_BOOL))
        return false;
    *out = value->as.boolean;
    return true;
}

bool json_as_u64(const json_value *value, uint64_t *out) {
    if (!json_has_kind(value, JSON_NUMBER))
        return false;
    const char *text = value->as.number;
    // Integer literals only: digits, no sign, fraction or exponent
    if (text[strspn(text, "0123456789")] != '\0')
        return false;
    errno = 0;
    unsigned long long v = strtoull(text, NULL, 10);
    if (errno == ERANGE || v > UINT64_MAX)
        return false;
    *out = (uint64_t)v;
    return true;
}

bool json_as_double(const json_value *value, double *out) {
    if (!json_has_kind(value, JSON_NUMBER))
        return false;
    // g_ascii_strtod ignores the C locale's decimal separator
    *out = g_ascii_strtod(value->as.number, NULL);
    return true;
}

const char *json_as_string(const json_value *value) {
    return json_has_kind(value, JSON_STRING) ? value->as.string : NULL;
}
