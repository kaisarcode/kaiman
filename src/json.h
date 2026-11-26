/**
 * json.h - Minimal JSON values for kaiman.
 * Summary: Public contracts for the internal JSON parser and string escaper.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_JSON_H
#define KC_KAIMAN_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KC_JSON_NULL = 0,
    KC_JSON_BOOL = 1,
    KC_JSON_NUMBER = 2,
    KC_JSON_STRING = 3,
    KC_JSON_ARRAY = 4,
    KC_JSON_OBJECT = 5
} kc_json_type_t;

typedef struct kc_json_value kc_json_value_t;

/**
 * Parse one complete JSON document.
 * @param text JSON bytes, which need not be NUL terminated.
 * @param size Byte count.
 * @param out Receives an owned value released with kc_json_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_parse(const char *text, size_t size, kc_json_value_t **out);

/**
 * Release one JSON value and everything it owns.
 * @param value Owned value, or NULL.
 * @return void
 */
void kc_json_free(kc_json_value_t *value);

/**
 * Return the type of one JSON value.
 * @param value Borrowed value.
 * @return JSON type, or KC_JSON_NULL for NULL.
 */
kc_json_type_t kc_json_type(const kc_json_value_t *value);

/**
 * Return the number of members in an object or items in an array.
 * @param value Borrowed JSON value.
 * @return Element count, or zero for other values.
 */
size_t kc_json_length(const kc_json_value_t *value);

/**
 * Find one object member by name.
 * @param value Borrowed object value.
 * @param name Member name.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_member(const kc_json_value_t *value,
    const char *name);

/**
 * Read one array item by index.
 * @param value Borrowed array value.
 * @param index Zero-based item index.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_item(const kc_json_value_t *value, size_t index);

/**
 * Read one object member by index.
 * @param value Borrowed object value.
 * @param index Zero-based member index.
 * @return Borrowed value, or NULL when absent.
 */
kc_json_value_t *kc_json_at(const kc_json_value_t *value, size_t index);

/**
 * Borrow one object member name by index.
 * @param value Borrowed object value.
 * @param index Zero-based member index.
 * @return Borrowed name, or NULL when absent.
 */
const char *kc_json_key(const kc_json_value_t *value, size_t index);

/**
 * Borrow the text of a JSON string.
 * @param value Borrowed JSON value.
 * @return NUL terminated text, or NULL for other values.
 */
const char *kc_json_text(const kc_json_value_t *value);

/**
 * Read a JSON number as double.
 * @param value Borrowed JSON value.
 * @return Number value, or zero for other values.
 */
double kc_json_number(const kc_json_value_t *value);

/**
 * Read a JSON boolean.
 * @param value Borrowed JSON value.
 * @return Boolean value, or zero for other values.
 */
int kc_json_boolean(const kc_json_value_t *value);

/**
 * Escape text for inclusion inside a JSON string.
 * @param text Source text.
 * @param out Receives owned escaped bytes released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_escape(const char *text, char **out);

/**
 * Serialize one JSON value to text.
 * @param value Borrowed value to serialize.
 * @param out Receives owned text released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_json_stringify(const kc_json_value_t *value, char **out);

#ifdef __cplusplus
}
#endif

#endif
