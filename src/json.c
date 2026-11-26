/**
 * json.c - Minimal JSON values for kaiman.
 * Summary: Parses JSON documents and escapes text for JSON strings.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "json.h"

#include "libkaiman.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64U

struct kc_json_value {
    kc_json_type_t type;
    union {
        int boolean;
        double number;
        char *text;
        struct {
            kc_json_value_t **items;
            size_t count;
        } array;
        struct {
            char **keys;
            kc_json_value_t **values;
            size_t count;
        } object;
    } data;
};

typedef struct {
    const char *text;
    size_t size;
    size_t offset;
} json_input_t;

/**
 * Allocate a JSON value of one type.
 * @param type Value type.
 * @return Owned zeroed value, or NULL on allocation failure.
 */
static kc_json_value_t *json_new(kc_json_type_t type) {
    kc_json_value_t *value = (kc_json_value_t *)calloc(1, sizeof(*value));
    if (value != NULL) value->type = type;
    return value;
}

/**
 * Skip JSON whitespace.
 * @param input Parser input.
 * @return void
 */
static void json_space(json_input_t *input) {
    while (input->offset < input->size) {
        unsigned char value = (unsigned char)input->text[input->offset];
        if (value != ' ' && value != '\t' && value != '\n' &&
            value != '\r') {
            break;
        }
        input->offset++;
    }
}

/**
 * Append one byte to a growing text buffer.
 * @param buffer Destination buffer.
 * @param size Current byte count.
 * @param capacity Current capacity.
 * @param value Byte to append.
 * @return 1 on success, or 0 on allocation failure.
 */
static int json_append(char **buffer, size_t *size, size_t *capacity,
    char value) {
    char *larger;

    if (*size + 2U > *capacity) {
        size_t next = *capacity == 0U ? 32U : *capacity * 2U;
        larger = (char *)realloc(*buffer, next);
        if (larger == NULL) return 0;
        *buffer = larger;
        *capacity = next;
    }
    (*buffer)[(*size)++] = value;
    (*buffer)[*size] = '\0';
    return 1;
}

/**
 * Append one Unicode code point as UTF-8.
 * @param buffer Destination buffer.
 * @param size Current byte count.
 * @param capacity Current capacity.
 * @param code Unicode code point.
 * @return 1 on success, or 0 on allocation failure.
 */
static int json_append_code(char **buffer, size_t *size, size_t *capacity,
    unsigned long code) {
    if (code <= 0x7fUL) {
        return json_append(buffer, size, capacity, (char)code);
    }
    if (code <= 0x7ffUL) {
        return json_append(buffer, size, capacity,
                (char)(0xc0UL | (code >> 6))) &&
            json_append(buffer, size, capacity,
                (char)(0x80UL | (code & 0x3fUL)));
    }
    if (code <= 0xffffUL) {
        return json_append(buffer, size, capacity,
                (char)(0xe0UL | (code >> 12))) &&
            json_append(buffer, size, capacity,
                (char)(0x80UL | ((code >> 6) & 0x3fUL))) &&
            json_append(buffer, size, capacity,
                (char)(0x80UL | (code & 0x3fUL)));
    }
    return json_append(buffer, size, capacity,
            (char)(0xf0UL | (code >> 18))) &&
        json_append(buffer, size, capacity,
            (char)(0x80UL | ((code >> 12) & 0x3fUL))) &&
        json_append(buffer, size, capacity,
            (char)(0x80UL | ((code >> 6) & 0x3fUL))) &&
        json_append(buffer, size, capacity,
            (char)(0x80UL | (code & 0x3fUL)));
}

/**
 * Read four hexadecimal digits from parser input.
 * @param input Parser input.
 * @param out Receives the decoded value.
 * @return 1 on success, or 0 on invalid input.
 */
static int json_hex4(json_input_t *input, unsigned long *out) {
    unsigned long value = 0UL;
    int index;

    if (input->size - input->offset < 4U) return 0;
    for (index = 0; index < 4; index++) {
        unsigned char value_byte =
            (unsigned char)input->text[input->offset++];
        value <<= 4;
        if (value_byte >= '0' && value_byte <= '9') {
            value += (unsigned long)(value_byte - '0');
        } else if (value_byte >= 'a' && value_byte <= 'f') {
            value += (unsigned long)(value_byte - 'a' + 10);
        } else if (value_byte >= 'A' && value_byte <= 'F') {
            value += (unsigned long)(value_byte - 'A' + 10);
        } else {
            return 0;
        }
    }
    *out = value;
    return 1;
}

/**
 * Read one escaped Unicode sequence, including surrogate pairs.
 * @param input Parser input.
 * @param buffer Destination buffer.
 * @param size Current byte count.
 * @param capacity Current capacity.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_unicode(json_input_t *input, char **buffer, size_t *size,
    size_t *capacity) {
    unsigned long first;
    unsigned long second;

    if (!json_hex4(input, &first)) return 0;
    if (first >= 0xd800UL && first <= 0xdbffUL) {
        if (input->size - input->offset < 6U ||
            input->text[input->offset] != '\\' ||
            input->text[input->offset + 1U] != 'u') {
            return 0;
        }
        input->offset += 2U;
        if (!json_hex4(input, &second) || second < 0xdc00UL ||
            second > 0xdfffUL) {
            return 0;
        }
        first = 0x10000UL + ((first - 0xd800UL) << 10) +
            (second - 0xdc00UL);
    } else if (first >= 0xdc00UL && first <= 0xdfffUL) {
        return 0;
    }
    return json_append_code(buffer, size, capacity, first);
}

/**
 * Parse one JSON string.
 * @param input Parser input.
 * @param out Receives an owned NUL terminated string.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_parse_string(json_input_t *input, char **out) {
    char *buffer = NULL;
    size_t size = 0U;
    size_t capacity = 0U;

    *out = NULL;
    if (input->offset >= input->size ||
        input->text[input->offset] != '"') {
        return 0;
    }
    input->offset++;
    while (input->offset < input->size) {
        unsigned char value = (unsigned char)input->text[input->offset++];
        if (value == '"') {
            if (buffer == NULL) {
                buffer = (char *)malloc(1U);
                if (buffer == NULL) return 0;
                buffer[0] = '\0';
            }
            *out = buffer;
            return 1;
        }
        if (value == '\\') {
            if (input->offset >= input->size) break;
            value = (unsigned char)input->text[input->offset++];
            if (value == 'u') {
                if (!json_unicode(input, &buffer, &size, &capacity)) break;
                continue;
            }
            if (value == 'b') value = '\b';
            else if (value == 'f') value = '\f';
            else if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
            else if (value != '"' && value != '\\' && value != '/') break;
        } else if (value < 0x20U) {
            break;
        }
        if (!json_append(&buffer, &size, &capacity, (char)value)) break;
    }
    free(buffer);
    return 0;
}

/**
 * Parse one JSON number.
 * @param input Parser input.
 * @param out Receives the parsed number.
 * @return 1 on success, or 0 on invalid input.
 */
static int json_parse_number(json_input_t *input, double *out) {
    size_t start = input->offset;
    char *copy;
    char *end;
    size_t length;
    int status;

    if (input->offset < input->size &&
        input->text[input->offset] == '-') {
        input->offset++;
    }
    if (input->offset >= input->size) return 0;
    if (input->text[input->offset] == '0') {
        input->offset++;
    } else if (input->text[input->offset] >= '1' &&
        input->text[input->offset] <= '9') {
        while (input->offset < input->size &&
            isdigit((unsigned char)input->text[input->offset])) {
            input->offset++;
        }
    } else {
        return 0;
    }
    if (input->offset < input->size &&
        input->text[input->offset] == '.') {
        input->offset++;
        if (input->offset >= input->size ||
            !isdigit((unsigned char)input->text[input->offset])) {
            return 0;
        }
        while (input->offset < input->size &&
            isdigit((unsigned char)input->text[input->offset])) {
            input->offset++;
        }
    }
    if (input->offset < input->size &&
        (input->text[input->offset] == 'e' ||
        input->text[input->offset] == 'E')) {
        input->offset++;
        if (input->offset < input->size &&
            (input->text[input->offset] == '+' ||
            input->text[input->offset] == '-')) {
            input->offset++;
        }
        if (input->offset >= input->size ||
            !isdigit((unsigned char)input->text[input->offset])) {
            return 0;
        }
        while (input->offset < input->size &&
            isdigit((unsigned char)input->text[input->offset])) {
            input->offset++;
        }
    }
    length = input->offset - start;
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) return 0;
    memcpy(copy, input->text + start, length);
    copy[length] = '\0';
    *out = strtod(copy, &end);
    status = end != NULL && *end == '\0' && isfinite(*out);
    free(copy);
    return status;
}

/**
 * Parse one JSON value at the current input position.
 * @param input Parser input.
 * @param depth Current nesting depth.
 * @param out Receives an owned value.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_parse_value(json_input_t *input, unsigned int depth,
    kc_json_value_t **out);

/**
 * Parse one JSON object.
 * @param input Parser input.
 * @param depth Current nesting depth.
 * @param out Receives an owned object value.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_parse_object(json_input_t *input, unsigned int depth,
    kc_json_value_t **out) {
    kc_json_value_t *object = json_new(KC_JSON_OBJECT);
    char **keys = NULL;
    kc_json_value_t **values = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t index;

    if (object == NULL) return 0;
    input->offset++;
    json_space(input);
    if (input->offset < input->size &&
        input->text[input->offset] == '}') {
        input->offset++;
        *out = object;
        return 1;
    }
    while (input->offset < input->size) {
        char *key = NULL;
        kc_json_value_t *value = NULL;
        char **more_keys;
        kc_json_value_t **more_values;

        if (!json_parse_string(input, &key)) break;
        json_space(input);
        if (input->offset >= input->size ||
            input->text[input->offset] != ':') {
            free(key);
            break;
        }
        input->offset++;
        json_space(input);
        if (!json_parse_value(input, depth + 1U, &value)) {
            free(key);
            break;
        }
        if (count == capacity) {
            size_t next = capacity == 0U ? 8U : capacity * 2U;
            more_keys = (char **)realloc(keys, next * sizeof(*keys));
            if (more_keys == NULL) {
                free(key);
                kc_json_free(value);
                break;
            }
            keys = more_keys;
            more_values = (kc_json_value_t **)realloc(values,
                next * sizeof(*values));
            if (more_values == NULL) {
                free(key);
                kc_json_free(value);
                break;
            }
            values = more_values;
            capacity = next;
        }
        keys[count] = key;
        values[count] = value;
        count++;
        json_space(input);
        if (input->offset < input->size &&
            input->text[input->offset] == ',') {
            input->offset++;
            json_space(input);
            continue;
        }
        if (input->offset < input->size &&
            input->text[input->offset] == '}') {
            input->offset++;
            object->data.object.keys = keys;
            object->data.object.values = values;
            object->data.object.count = count;
            *out = object;
            return 1;
        }
        kc_json_free(value);
        free(key);
        count--;
        break;
    }
    for (index = 0U; index < count; index++) {
        kc_json_free(values[index]);
        free(keys[index]);
    }
    free(keys);
    free(values);
    free(object);
    return 0;
}

/**
 * Parse one JSON array.
 * @param input Parser input.
 * @param depth Current nesting depth.
 * @param out Receives an owned array value.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_parse_array(json_input_t *input, unsigned int depth,
    kc_json_value_t **out) {
    kc_json_value_t *array = json_new(KC_JSON_ARRAY);
    kc_json_value_t **items = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t index;

    if (array == NULL) return 0;
    input->offset++;
    json_space(input);
    if (input->offset < input->size &&
        input->text[input->offset] == ']') {
        input->offset++;
        *out = array;
        return 1;
    }
    while (input->offset < input->size) {
        kc_json_value_t *value = NULL;
        kc_json_value_t **more_items;

        if (!json_parse_value(input, depth + 1U, &value)) break;
        if (count == capacity) {
            size_t next = capacity == 0U ? 8U : capacity * 2U;
            more_items = (kc_json_value_t **)realloc(items,
                next * sizeof(*items));
            if (more_items == NULL) {
                kc_json_free(value);
                break;
            }
            items = more_items;
            capacity = next;
        }
        items[count++] = value;
        json_space(input);
        if (input->offset < input->size &&
            input->text[input->offset] == ',') {
            input->offset++;
            json_space(input);
            continue;
        }
        if (input->offset < input->size &&
            input->text[input->offset] == ']') {
            input->offset++;
            array->data.array.items = items;
            array->data.array.count = count;
            *out = array;
            return 1;
        }
        kc_json_free(value);
        count--;
        break;
    }
    for (index = 0U; index < count; index++) kc_json_free(items[index]);
    free(items);
    free(array);
    return 0;
}

/**
 * Parse one JSON literal by name.
 * @param input Parser input.
 * @param word Expected literal.
 * @return 1 when the literal was consumed, or 0 otherwise.
 */
static int json_literal(json_input_t *input, const char *word) {
    size_t length = strlen(word);

    if (input->size - input->offset < length ||
        memcmp(input->text + input->offset, word, length) != 0) {
        return 0;
    }
    input->offset += length;
    return 1;
}

/**
 * Parse one JSON value at the current input position.
 * @param input Parser input.
 * @param depth Current nesting depth.
 * @param out Receives an owned value.
 * @return 1 on success, or 0 on invalid input or allocation failure.
 */
static int json_parse_value(json_input_t *input, unsigned int depth,
    kc_json_value_t **out) {
    kc_json_value_t *value;

    *out = NULL;
    if (depth > JSON_MAX_DEPTH || input->offset >= input->size) return 0;
    json_space(input);
    if (input->offset >= input->size) return 0;
    if (input->text[input->offset] == '{') {
        return json_parse_object(input, depth, out);
    }
    if (input->text[input->offset] == '[') {
        return json_parse_array(input, depth, out);
    }
    if (input->text[input->offset] == '"') {
        char *text = NULL;
        if (!json_parse_string(input, &text)) return 0;
        value = json_new(KC_JSON_STRING);
        if (value == NULL) {
            free(text);
            return 0;
        }
        value->data.text = text;
        *out = value;
        return 1;
    }
    if (input->text[input->offset] == 't' ||
        input->text[input->offset] == 'f') {
        value = json_new(KC_JSON_BOOL);
        if (value == NULL) return 0;
        if (input->text[input->offset] == 't' &&
            json_literal(input, "true")) {
            value->data.boolean = 1;
        } else if (input->text[input->offset] == 'f' &&
            json_literal(input, "false")) {
            value->data.boolean = 0;
        } else {
            free(value);
            return 0;
        }
        *out = value;
        return 1;
    }
    if (input->text[input->offset] == 'n') {
        if (!json_literal(input, "null")) return 0;
        value = json_new(KC_JSON_NULL);
        if (value == NULL) return 0;
        *out = value;
        return 1;
    }
    value = json_new(KC_JSON_NUMBER);
    if (value == NULL) return 0;
    if (!json_parse_number(input, &value->data.number)) {
        free(value);
        return 0;
    }
    *out = value;
    return 1;
}

/**
 * Parse one complete JSON document.
 * @param text JSON bytes, which need not be NUL terminated.
 * @param size Byte count.
 * @param out Receives an owned value released with kc_json_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_parse(const char *text, size_t size, kc_json_value_t **out) {
    json_input_t input;
    kc_json_value_t *value = NULL;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (text == NULL) return KC_KAIMAN_EINVAL;
    input.text = text;
    input.size = size;
    input.offset = 0U;
    if (!json_parse_value(&input, 0U, &value)) return KC_KAIMAN_EFORMAT;
    json_space(&input);
    if (input.offset != input.size) {
        kc_json_free(value);
        return KC_KAIMAN_EFORMAT;
    }
    *out = value;
    return KC_KAIMAN_OK;
}

/**
 * Release one JSON value and everything it owns.
 * @param value Owned value, or NULL.
 * @return void
 */
void kc_json_free(kc_json_value_t *value) {
    size_t index;

    if (value == NULL) return;
    if (value->type == KC_JSON_STRING) {
        free(value->data.text);
    } else if (value->type == KC_JSON_ARRAY) {
        for (index = 0U; index < value->data.array.count; index++) {
            kc_json_free(value->data.array.items[index]);
        }
        free(value->data.array.items);
    } else if (value->type == KC_JSON_OBJECT) {
        for (index = 0U; index < value->data.object.count; index++) {
            free(value->data.object.keys[index]);
            kc_json_free(value->data.object.values[index]);
        }
        free(value->data.object.keys);
        free(value->data.object.values);
    }
    free(value);
}

/**
 * Return the type of one JSON value.
 * @param value Borrowed value.
 * @return JSON type, or KC_JSON_NULL for NULL.
 */
kc_json_type_t kc_json_type(const kc_json_value_t *value) {
    return value != NULL ? value->type : KC_JSON_NULL;
}

/**
 * Return the number of members in an object or items in an array.
 * @param value Borrowed JSON value.
 * @return Element count, or zero for other values.
 */
size_t kc_json_length(const kc_json_value_t *value) {
    if (value == NULL) return 0U;
    if (value->type == KC_JSON_ARRAY) return value->data.array.count;
    if (value->type == KC_JSON_OBJECT) return value->data.object.count;
    return 0U;
}

/**
 * Find one object member by name.
 * @param value Borrowed object value.
 * @param name Member name.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_member(const kc_json_value_t *value,
    const char *name) {
    size_t index;

    if (value == NULL || value->type != KC_JSON_OBJECT || name == NULL) {
        return NULL;
    }
    for (index = 0U; index < value->data.object.count; index++) {
        if (strcmp(value->data.object.keys[index], name) == 0) {
            return value->data.object.values[index];
        }
    }
    return NULL;
}

/**
 * Read one array item by index.
 * @param value Borrowed array value.
 * @param index Zero-based item index.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_item(const kc_json_value_t *value, size_t index) {
    if (value == NULL || value->type != KC_JSON_ARRAY ||
        index >= value->data.array.count) {
        return NULL;
    }
    return value->data.array.items[index];
}

/**
 * Read one object member by index.
 * @param value Borrowed object value.
 * @param index Zero-based member index.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_at(const kc_json_value_t *value, size_t index) {
    if (value == NULL || value->type != KC_JSON_OBJECT ||
        index >= value->data.object.count) {
        return NULL;
    }
    return value->data.object.values[index];
}

/**
 * Borrow one object member name by index.
 * @param value Borrowed object value.
 * @param index Zero-based member index.
 * @return Borrowed name, or NULL when absent.
 */
const char *kc_json_key(const kc_json_value_t *value, size_t index) {
    if (value == NULL || value->type != KC_JSON_OBJECT ||
        index >= value->data.object.count) {
        return NULL;
    }
    return value->data.object.keys[index];
}

/**
 * Borrow the text of a JSON string.
 * @param value Borrowed JSON value.
 * @return NUL terminated text, or NULL for other values.
 */
const char *kc_json_text(const kc_json_value_t *value) {
    if (value == NULL || value->type != KC_JSON_STRING) return NULL;
    return value->data.text;
}

/**
 * Read a JSON number as double.
 * @param value Borrowed JSON value.
 * @return Number value, or zero for other values.
 */
double kc_json_number(const kc_json_value_t *value) {
    if (value == NULL || value->type != KC_JSON_NUMBER) return 0.0;
    return value->data.number;
}

/**
 * Read a JSON boolean.
 * @param value Borrowed JSON value.
 * @return Boolean value, or zero for other values.
 */
int kc_json_boolean(const kc_json_value_t *value) {
    if (value == NULL || value->type != KC_JSON_BOOL) return 0;
    return value->data.boolean;
}

/**
 * Escape text for inclusion inside a JSON string.
 * @param text Source text.
 * @param out Receives owned escaped bytes released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_escape(const char *text, char **out) {
    char *buffer = NULL;
    size_t size = 0U;
    size_t capacity = 0U;
    const unsigned char *cursor;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (text == NULL) return KC_KAIMAN_EINVAL;
    for (cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
        char sequence[8];
        const char *replacement = NULL;

        if (*cursor == '"') replacement = "\\\"";
        else if (*cursor == '\\') replacement = "\\\\";
        else if (*cursor == '\b') replacement = "\\b";
        else if (*cursor == '\f') replacement = "\\f";
        else if (*cursor == '\n') replacement = "\\n";
        else if (*cursor == '\r') replacement = "\\r";
        else if (*cursor == '\t') replacement = "\\t";
        if (replacement != NULL) {
            for (; *replacement != '\0'; replacement++) {
                if (!json_append(&buffer, &size, &capacity,
                    *replacement)) {
                    free(buffer);
                    return KC_KAIMAN_ENOMEM;
                }
            }
            continue;
        }
        if (*cursor < 0x20U) {
            snprintf(sequence, sizeof(sequence), "\\u%04x",
                (unsigned int)*cursor);
            replacement = sequence;
            while (*replacement != '\0') {
                if (!json_append(&buffer, &size, &capacity,
                    *replacement)) {
                    free(buffer);
                    return KC_KAIMAN_ENOMEM;
                }
                replacement++;
            }
            continue;
        }
        if (!json_append(&buffer, &size, &capacity, (char)*cursor)) {
            free(buffer);
            return KC_KAIMAN_ENOMEM;
        }
    }
    if (buffer == NULL) buffer = (char *)malloc(1U);
    if (buffer == NULL) return KC_KAIMAN_ENOMEM;
    buffer[size] = '\0';
    *out = buffer;
    return KC_KAIMAN_OK;
}

/**
 * Append one escaped JSON string to a serialization buffer.
 * @param buffer Destination buffer.
 * @param text Source text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int json_write_string(kc_buffer_t *buffer, const char *text) {
    char *escaped = NULL;
    int status;

    status = kc_json_escape(text != NULL ? text : "", &escaped);
    if (status != KC_KAIMAN_OK) return status;
    if (kc_buffer_text(buffer, "\"") != KC_KAIMAN_OK) status = KC_KAIMAN_ENOMEM;
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, escaped);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, "\"");
    }
    kc_kaiman_free(escaped);
    return status;
}

/**
 * Append one JSON value to a serialization buffer.
 * @param buffer Destination buffer.
 * @param value Value to append.
 * @param depth Current nesting depth.
 * @return KC_KAIMAN_OK or an error status.
 */
static int json_write_value(kc_buffer_t *buffer,
    const kc_json_value_t *value, unsigned int depth) {
    size_t index;
    int status;

    if (buffer == NULL || value == NULL || depth > JSON_MAX_DEPTH) {
        return KC_KAIMAN_EINVAL;
    }
    if (value->type == KC_JSON_NULL) return kc_buffer_text(buffer, "null");
    if (value->type == KC_JSON_BOOL) {
        return kc_buffer_text(buffer, value->data.boolean ? "true" : "false");
    }
    if (value->type == KC_JSON_NUMBER) {
        return kc_buffer_format(buffer, "%.17g", value->data.number);
    }
    if (value->type == KC_JSON_STRING) {
        return json_write_string(buffer, value->data.text);
    }
    if (value->type == KC_JSON_ARRAY) {
        if (kc_buffer_text(buffer, "[") != KC_KAIMAN_OK) {
            return KC_KAIMAN_ENOMEM;
        }
        for (index = 0U; index < value->data.array.count; index++) {
            if (index != 0U && kc_buffer_text(buffer, ",") != KC_KAIMAN_OK) {
                return KC_KAIMAN_ENOMEM;
            }
            status = json_write_value(buffer,
                value->data.array.items[index], depth + 1U);
            if (status != KC_KAIMAN_OK) return status;
        }
        return kc_buffer_text(buffer, "]");
    }
    if (value->type == KC_JSON_OBJECT) {
        if (kc_buffer_text(buffer, "{") != KC_KAIMAN_OK) {
            return KC_KAIMAN_ENOMEM;
        }
        for (index = 0U; index < value->data.object.count; index++) {
            if (index != 0U && kc_buffer_text(buffer, ",") != KC_KAIMAN_OK) {
                return KC_KAIMAN_ENOMEM;
            }
            status = json_write_string(buffer,
                value->data.object.keys[index]);
            if (status != KC_KAIMAN_OK) return status;
            if (kc_buffer_text(buffer, ":") != KC_KAIMAN_OK) {
                return KC_KAIMAN_ENOMEM;
            }
            status = json_write_value(buffer,
                value->data.object.values[index], depth + 1U);
            if (status != KC_KAIMAN_OK) return status;
        }
        return kc_buffer_text(buffer, "}");
    }
    return KC_KAIMAN_EFORMAT;
}

/**
 * Serialize one JSON value to text.
 * @param value Borrowed value to serialize.
 * @param out Receives owned text released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_stringify(const kc_json_value_t *value, char **out) {
    kc_buffer_t *buffer = NULL;
    char *result;
    int status;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (value == NULL) return KC_KAIMAN_EINVAL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = json_write_value(buffer, value, 0U);
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return status;
    }
    result = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    if (result == NULL) return KC_KAIMAN_ENOMEM;
    *out = result;
    return KC_KAIMAN_OK;
}
