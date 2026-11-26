/**
 * upstream.h - Outbound HTTP requests for kaiman adapters.
 * Summary: Internal client contract with body streaming and timeouts.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_UPSTREAM_H
#define KC_KAIMAN_UPSTREAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} kc_upstream_field_t;

/**
 * Receives streamed response body bytes.
 * Returning a negative value stops the exchange.
 * @param data Borrowed body bytes.
 * @param size Byte count.
 * @param userdata Consumer context.
 * @return 0 to continue, or a negative value to stop.
 */
typedef int (*kc_upstream_chunk_fn)(const void *data, size_t size,
    void *userdata);

/**
 * Receives the upstream response status and content type before its body.
 * @param status HTTP status code.
 * @param content_type Borrowed content type, or an empty string.
 * @param userdata Consumer context.
 * @return void
 */
typedef void (*kc_upstream_status_fn)(int status, const char *content_type,
    void *userdata);

typedef struct {
    const char *method;
    const char *url;
    const kc_upstream_field_t *headers;
    size_t header_count;
    const void *body;
    size_t body_size;
    int timeout;
    kc_upstream_chunk_fn chunk;
    kc_upstream_status_fn status;
    void *userdata;
} kc_upstream_request_t;

typedef struct {
    int status;
    char *content_type;
    char *body;
    size_t body_size;
} kc_upstream_response_t;

/**
 * Perform one outbound HTTP or HTTPS request.
 * @param request Request description.
 * @param response Receives the owned response.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_upstream_send(const kc_upstream_request_t *request,
    kc_upstream_response_t *response);

/**
 * Release one owned upstream response.
 * @param response Response to clear, or NULL.
 * @return void
 */
void kc_upstream_response_free(kc_upstream_response_t *response);

/**
 * Return a stable description for an upstream status.
 * @param status Status to describe.
 * @return NUL terminated description.
 */
const char *kc_upstream_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
