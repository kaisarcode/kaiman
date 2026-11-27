/**
 * upstream.c - Outbound HTTP requests for kaiman adapters.
 * Summary: Implements bounded HTTP and HTTPS exchanges with streamed bodies.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "upstream.h"

#include "libhttp.h"
#include "libkaiman.h"
#include "libtls.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define UPSTREAM_MAX_BODY (16U * 1024U * 1024U)
#define UPSTREAM_MAX_HEADERS (64U * 1024U)

#ifdef _WIN32
typedef SOCKET kc_socket_t;
#define KC_SOCKET_INVALID INVALID_SOCKET
#define kc_socket_close(value) closesocket(value)
#else
typedef int kc_socket_t;
#define KC_SOCKET_INVALID (-1)
#define kc_socket_close(value) close(value)
#endif

typedef struct {
    char *host;
    char *host_header;
    char *port;
    char *target;
    int secure;
} kc_upstream_url_t;

typedef struct {
    kc_socket_t fd;
    kc_tls_t *tls;
    int secure;
    int headers_done;
    int complete;
    int failure;
    int chunked;
    int chunk_gap;
    size_t remaining;
    size_t chunk_need;
    double deadline;
    char *content_type;
    char *location;
    int stream_only;
    kc_buffer_t *raw;
    kc_buffer_t *header;
    kc_buffer_t *body;
    kc_upstream_chunk_fn chunk;
    kc_upstream_status_fn status_fn;
    void *userdata;
    int status;
} kc_upstream_state_t;

static int upstream_wait(kc_socket_t fd, int writable, double deadline);

/**
 * Remove a prefix from one growable buffer.
 * @param buffer Buffer to modify.
 * @param count Number of bytes to remove.
 * @return void
 */
static void upstream_consume(kc_buffer_t *buffer, size_t count) {
    if (buffer == NULL || count == 0U) return;
    if (count >= buffer->size) {
        buffer->size = 0U;
        if (buffer->data != NULL) buffer->data[0] = '\0';
        return;
    }
    memmove(buffer->data, buffer->data + count, buffer->size - count);
    buffer->size -= count;
    buffer->data[buffer->size] = '\0';
}

/**
 * Deliver body bytes to the consumer and retain them for the caller.
 * @param state Exchange state.
 * @param data Borrowed body bytes.
 * @param size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_emit(kc_upstream_state_t *state,
    const void *data, size_t size) {
    int status;

    if (state == NULL || size == 0U) return KC_KAIMAN_OK;
    if (!state->stream_only) {
        if (size > UPSTREAM_MAX_BODY - state->body->size) {
            state->failure = KC_KAIMAN_EHTTP;
            return state->failure;
        }
        status = kc_buffer_append(state->body, data, size);
        if (status != KC_KAIMAN_OK) {
            state->failure = status;
            return status;
        }
    }
    if (state->chunk != NULL) {
        status = state->chunk(data, size, state->userdata);
        if (status < 0) {
            state->failure = KC_KAIMAN_EHTTP;
            return state->failure;
        }
    }
    return KC_KAIMAN_OK;
}

/**
 * Find a NUL terminated marker inside one growable buffer.
 * @param buffer Buffer to search.
 * @param marker Marker text.
 * @return Offset, or SIZE_MAX when absent.
 */
static size_t upstream_find(const kc_buffer_t *buffer, const char *marker) {
    const char *found;

    if (buffer == NULL || buffer->data == NULL || marker == NULL) {
        return (size_t)-1;
    }
    found = strstr(buffer->data, marker);
    if (found == NULL) return (size_t)-1;
    return (size_t)(found - buffer->data);
}

/**
 * Compare one header name without regard to case.
 * @param name Header name.
 * @param size Name length.
 * @param expected Expected name.
 * @return 1 on match, 0 otherwise.
 */
static int upstream_name(const char *name, size_t size, const char *expected) {
    size_t index;

    if (name == NULL || expected == NULL) return 0;
    if (strlen(expected) != size) return 0;
    for (index = 0U; index < size; index++) {
        if (tolower((unsigned char)name[index]) !=
            tolower((unsigned char)expected[index])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Find the last occurrence of one byte in a bounded region.
 * @param data Region to search.
 * @param value Byte to find.
 * @param size Region length.
 * @return Pointer to the match, or NULL when absent.
 */
static const void *upstream_last_byte(const void *data, int value,
    size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    unsigned char wanted = (unsigned char)value;

    if (bytes == NULL) return NULL;
    while (size > 0U) {
        size--;
        if (bytes[size] == wanted) return bytes + size;
    }
    return NULL;
}

/**
 * Parse one response status line and its headers.
 * @param state Exchange state.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_headers(kc_upstream_state_t *state) {
    const char *data = kc_buffer_value(state->header);
    const char *line;
    const char *end;
    int status;

    if (data == NULL) return KC_KAIMAN_EHTTP;
    end = strstr(data, "\r\n");
    if (end == NULL) return KC_KAIMAN_EHTTP;
    if (sscanf(data, "HTTP/%*15[0-9.] %d", &status) != 1 ||
        status < 100 || status > 599) {
        return KC_KAIMAN_EHTTP;
    }
    state->status = status;
    line = end + 2U;

    while (*line != '\0') {
        const char *separator = strstr(line, "\r\n");
        const char *colon;
        size_t length;
        char *value;

        if (separator == NULL) break;
        length = (size_t)(separator - line);
        colon = memchr(line, ':', length);
        if (colon == NULL) {
            line = separator + 2U;
            continue;
        }
        value = (char *)colon + 1;
        while (value < separator && isspace((unsigned char)*value)) value++;
        if (upstream_name(line, (size_t)(colon - line), "content-type")) {
            size_t count = (size_t)(separator - value);
            while (count > 0U && isspace((unsigned char)value[count - 1U])) {
                count--;
            }
            free(state->content_type);
            state->content_type = (char *)malloc(count + 1U);
            if (state->content_type == NULL) return KC_KAIMAN_ENOMEM;
            memcpy(state->content_type, value, count);
            state->content_type[count] = '\0';
        } else if (upstream_name(line, (size_t)(colon - line),
            "location")) {
            size_t count = (size_t)(separator - value);

            while (count > 0U && isspace((unsigned char)value[count - 1U])) {
                count--;
            }
            free(state->location);
            state->location = (char *)malloc(count + 1U);
            if (state->location == NULL) return KC_KAIMAN_ENOMEM;
            memcpy(state->location, value, count);
            state->location[count] = '\0';
        } else if (upstream_name(line, (size_t)(colon - line),
            "transfer-encoding")) {
            if (strstr(value, "chunked") != NULL) state->chunked = 1;
        } else if (upstream_name(line, (size_t)(colon - line),
            "content-length")) {
            char *stop = NULL;
            long parsed = strtol(value, &stop, 10);
            if (stop != value && parsed >= 0 && !state->chunked) {
                state->remaining = (size_t)parsed;
            }
        }
        line = separator + 2U;
    }
    if (state->status_fn != NULL) {
        state->status_fn(status,
            state->content_type != NULL ? state->content_type : "",
            state->userdata);
    }
    if (status == 204 || status == 304 || (status >= 100 && status < 200)) {
        state->complete = 1;
    } else if (!state->chunked && state->remaining == 0U) {
        state->remaining = (size_t)-1;
    }
    return KC_KAIMAN_OK;
}

/**
 * Process all complete body bytes currently buffered.
 * @param state Exchange state.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_process(kc_upstream_state_t *state) {
    int status;

    if (state->failure != KC_KAIMAN_OK) return state->failure;
    for (;;) {
        if (!state->headers_done) {
            size_t position = upstream_find(state->raw, "\r\n\r\n");
            size_t length;
            if (position == (size_t)-1) {
                if (state->raw->size > UPSTREAM_MAX_HEADERS) {
                    state->failure = KC_KAIMAN_EHTTP;
                    return state->failure;
                }
                return KC_KAIMAN_OK;
            }
            length = position + 4U;
            status = kc_buffer_append(state->header, state->raw->data, length);
            if (status != KC_KAIMAN_OK) {
                state->failure = status;
                return status;
            }
            upstream_consume(state->raw, length);
            status = upstream_headers(state);
            if (status != KC_KAIMAN_OK) {
                state->failure = status;
                return status;
            }
            state->headers_done = 1;
        }
        if (state->complete) return KC_KAIMAN_OK;

        if (state->chunked) {
            if (state->chunk_need == 0U && !state->chunk_gap) {
                size_t position = upstream_find(state->raw, "\r\n");
                char *stop = NULL;
                unsigned long parsed;
                if (position == (size_t)-1) return KC_KAIMAN_OK;
                if (position == 0U) {
                    state->failure = KC_KAIMAN_EHTTP;
                    return state->failure;
                }
                {
                    char size_text[64];
                    size_t count = position < sizeof(size_text) - 1U
                        ? position : sizeof(size_text) - 1U;
                    memcpy(size_text, state->raw->data, count);
                    size_text[count] = '\0';
                    stop = NULL;
                    parsed = strtoul(size_text, &stop, 16);
                    if (stop == size_text || *stop != '\0' ||
                        parsed > (unsigned long)(size_t)-1) {
                        state->failure = KC_KAIMAN_EHTTP;
                        return state->failure;
                    }
                }
                upstream_consume(state->raw, position + 2U);
                if (parsed == 0UL) {
                    state->complete = 1;
                    return KC_KAIMAN_OK;
                }
                state->chunk_need = (size_t)parsed;
            }
            if (state->chunk_need > 0U) {
                size_t count = state->raw->size < state->chunk_need
                    ? state->raw->size : state->chunk_need;
                if (count == 0U) return KC_KAIMAN_OK;
                status = upstream_emit(state, state->raw->data, count);
                if (status != KC_KAIMAN_OK) return status;
                upstream_consume(state->raw, count);
                state->chunk_need -= count;
                if (state->chunk_need == 0U) state->chunk_gap = 1;
                continue;
            }
            if (state->chunk_gap) {
                if (state->raw->size < 2U) return KC_KAIMAN_OK;
                if (state->raw->data[0] != '\r' || state->raw->data[1] != '\n') {
                    state->failure = KC_KAIMAN_EHTTP;
                    return state->failure;
                }
                upstream_consume(state->raw, 2U);
                state->chunk_gap = 0;
                continue;
            }
            return KC_KAIMAN_OK;
        }

        if (state->remaining != (size_t)-1) {
            size_t count = state->raw->size < state->remaining
                ? state->raw->size : state->remaining;
            if (count == 0U) return KC_KAIMAN_OK;
            status = upstream_emit(state, state->raw->data, count);
            if (status != KC_KAIMAN_OK) return status;
            upstream_consume(state->raw, count);
            state->remaining -= count;
            if (state->remaining == 0U) state->complete = 1;
            continue;
        }

        if (state->raw->size > 0U) {
            status = upstream_emit(state, state->raw->data, state->raw->size);
            if (status != KC_KAIMAN_OK) return status;
            upstream_consume(state->raw, state->raw->size);
        }
        return KC_KAIMAN_OK;
    }
}

/**
 * Feed TLS plaintext or plain socket bytes into the response parser.
 * @param state Exchange state.
 * @param data Borrowed bytes.
 * @param size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_feed(kc_upstream_state_t *state,
    const void *data, size_t size) {
    int status;

    if (state == NULL || (data == NULL && size != 0U)) {
        return KC_KAIMAN_EINVAL;
    }
    if (size == 0U) return KC_KAIMAN_OK;
    status = kc_buffer_append(state->raw, data, size);
    if (status != KC_KAIMAN_OK) {
        state->failure = status;
        return status;
    }
    return upstream_process(state);
}

/**
 * Supply plaintext from one TLS session to the exchange.
 * @param data Borrowed plaintext.
 * @param size Byte count.
 * @param userdata Exchange state.
 * @return void
 */
static void upstream_tls_plain(const void *data, size_t size, void *userdata) {
    (void)upstream_feed((kc_upstream_state_t *)userdata, data, size);
}

/**
 * Send TLS wire bytes through the connected socket.
 * @param data Borrowed ciphertext.
 * @param size Byte count.
 * @param userdata Exchange state.
 * @return void
 */
static void upstream_tls_wire(const void *data, size_t size, void *userdata) {
    kc_upstream_state_t *state = (kc_upstream_state_t *)userdata;
    const char *bytes = (const char *)data;
    size_t offset = 0U;

    while (offset < size && state->failure == KC_KAIMAN_OK) {
        int sent = send(state->fd, bytes + offset,
            (int)(size - offset > 0x7fffffffU ? 0x7fffffffU : size - offset), 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
            if (upstream_wait(state->fd, 1, state->deadline) != KC_KAIMAN_OK) {
                state->failure = KC_KAIMAN_EIO;
            }
            continue;
        }
        state->failure = KC_KAIMAN_EIO;
    }
}

/**
 * Return the remaining wait time in milliseconds.
 * @param deadline Monotonic deadline.
 * @param status Receives a timeout status when expired.
 * @return Milliseconds to wait.
 */
static int upstream_wait_ms(double deadline, int *status) {
    double remaining = (deadline - kc_util_seconds()) * 1000.0;
    if (remaining <= 0.0) {
        *status = KC_KAIMAN_ETIMEOUT;
        return 0;
    }
    if (remaining > 60000.0) remaining = 60000.0;
    *status = KC_KAIMAN_OK;
    return (int)remaining;
}

/**
 * Wait until one socket is readable or writable.
 * @param fd Connected socket.
 * @param writable Wait for writability instead of readability.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_wait(kc_socket_t fd, int writable, double deadline) {
    int status;
    int milliseconds = upstream_wait_ms(deadline, &status);
    if (status != KC_KAIMAN_OK) return status;
#ifdef _WIN32
    WSAPOLLFD item;
    item.fd = fd;
    item.events = writable ? POLLWRNORM : POLLRDNORM;
    item.revents = 0;
    status = WSAPoll(&item, 1, milliseconds);
    if (status == 0) return KC_KAIMAN_ETIMEOUT;
    if (status < 0) return KC_KAIMAN_EIO;
    if ((item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return KC_KAIMAN_EIO;
    }
#else
    struct pollfd item;
    item.fd = fd;
    item.events = writable ? POLLOUT : POLLIN;
    item.revents = 0;
    status = poll(&item, 1, milliseconds);
    if (status == 0) return KC_KAIMAN_ETIMEOUT;
    if (status < 0) return errno == EINTR ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
    if ((item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return KC_KAIMAN_EIO;
    }
#endif
    return KC_KAIMAN_OK;
}

/**
 * Send all request bytes through the connected transport.
 * @param state Exchange state.
 * @param data Bytes to send.
 * @param size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_send_bytes(kc_upstream_state_t *state,
    const void *data, size_t size) {
    const char *bytes = (const char *)data;
    size_t offset = 0U;
    int status;

    if (state->secure) {
        status = kc_tls_send(state->tls, data, size);
        if (status != KC_TLS_OK) return KC_KAIMAN_EIO;
        return state->failure;
    }
    while (offset < size) {
        int sent = send(state->fd, bytes + offset,
            (int)(size - offset > 0x7fffffffU ? 0x7fffffffU : size - offset), 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK) return KC_KAIMAN_EIO;
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return KC_KAIMAN_EIO;
        }
#endif
        status = upstream_wait(state->fd, 1, state->deadline);
        if (status != KC_KAIMAN_OK) return status;
    }
    return KC_KAIMAN_OK;
}

/**
 * Receive one chunk and feed it into the response parser.
 * @param state Exchange state.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND at EOF, or an error status.
 */
static int upstream_receive(kc_upstream_state_t *state) {
    char bytes[8192];
    int received;
    int status;

    status = upstream_wait(state->fd, 0, state->deadline);
    if (status != KC_KAIMAN_OK) return status;
    received = recv(state->fd, bytes, (int)sizeof(bytes), 0);
    if (received == 0) return KC_KAIMAN_NOT_FOUND;
    if (received < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) return KC_KAIMAN_OK;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return KC_KAIMAN_OK;
        }
#endif
        return KC_KAIMAN_EIO;
    }
    if (state->secure) {
        status = kc_tls_receive(state->tls, bytes, (size_t)received);
        if (status != KC_TLS_OK && status != KC_TLS_CLOSED) {
            return KC_KAIMAN_EIO;
        }
        return state->failure;
    }
    return upstream_feed(state, bytes, (size_t)received);
}

/**
 * Parse one absolute HTTP or HTTPS URL.
 * @param value URL text.
 * @param out Receives the parsed URL.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_url(const char *value, kc_upstream_url_t *out) {
    const char *authority;
    const char *end;
    const char *host = NULL;
    const char *port = NULL;
    size_t host_length = 0U;
    size_t port_length = 0U;
    size_t authority_length;
    const char *userinfo;
    kc_buffer_t *target = NULL;
    int status;

    if (value == NULL || out == NULL) return KC_KAIMAN_EINVAL;
    memset(out, 0, sizeof(*out));
    if (strncmp(value, "https://", 8U) == 0) {
        authority = value + 8U;
        out->secure = 1;
        port = "443";
        port_length = 3U;
    } else if (strncmp(value, "http://", 7U) == 0) {
        authority = value + 7U;
        port = "80";
        port_length = 2U;
    } else {
        return KC_KAIMAN_EINVAL;
    }
    end = authority + strcspn(authority, "/?#");
    authority_length = (size_t)(end - authority);
    if (authority_length == 0U) return KC_KAIMAN_EINVAL;
    userinfo = (const char *)upstream_last_byte(authority, '@',
        authority_length);
    if (userinfo != NULL) {
        size_t remove = (size_t)(userinfo + 1 - authority);
        authority = userinfo + 1;
        authority_length -= remove;
    }
    if (authority_length == 0U) return KC_KAIMAN_EINVAL;

    if (*authority == '[') {
        const char *close = memchr(authority, ']', authority_length);
        if (close == NULL || close + 1 >= authority + authority_length) {
            return KC_KAIMAN_EINVAL;
        }
        host = authority + 1;
        host_length = (size_t)(close - host);
        if (close[1] == ':') {
            port = close + 2;
            port_length = (size_t)(authority + authority_length - port);
            if (port_length == 0U) return KC_KAIMAN_EINVAL;
        }
    } else {
        const char *colon = (const char *)upstream_last_byte(authority, ':',
            authority_length);
        if (colon != NULL && colon[1] != '\0' &&
            (size_t)(authority + authority_length - (colon + 1)) > 0U) {
            const char *cursor;
            int digits = 1;
            for (cursor = colon + 1; cursor < authority + authority_length;
                cursor++) {
                if (!isdigit((unsigned char)*cursor)) digits = 0;
            }
            if (digits) {
                host = authority;
                host_length = (size_t)(colon - authority);
                port = colon + 1;
                port_length = (size_t)(authority + authority_length - port);
            }
        }
        if (host == NULL) {
            host = authority;
            host_length = authority_length;
        }
    }
    if (host_length == 0U) return KC_KAIMAN_EINVAL;
    out->host = (char *)malloc(host_length + 1U);
    if (out->host == NULL) return KC_KAIMAN_ENOMEM;
    memcpy(out->host, host, host_length);
    out->host[host_length] = '\0';

    status = kc_buffer_open(&target);
    if (status != KC_KAIMAN_OK) {
        free(out->host);
        out->host = NULL;
        return status;
    }
    if (*end == '/' || *end == '?') {
        const char *fragment = strchr(end, '#');
        size_t length = fragment != NULL ? (size_t)(fragment - end) : strlen(end);
        if (*end == '?') status = kc_buffer_text(target, "/");
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_append(target, end, length);
        }
    } else {
        status = kc_buffer_text(target, "/");
    }
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(target);
        free(out->host);
        out->host = NULL;
        return status;
    }
    out->target = kc_util_strdup(kc_buffer_value(target));
    kc_buffer_close(target);

    {
        int bracketed = authority[0] == '[';
        kc_buffer_t *header = NULL;
        status = kc_buffer_open(&header);
        if (status == KC_KAIMAN_OK) {
            if (bracketed) status = kc_buffer_text(header, "[");
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_append(header, out->host, host_length);
            }
            if (status == KC_KAIMAN_OK && bracketed) {
                status = kc_buffer_text(header, "]");
            }
            if (status == KC_KAIMAN_OK) status = kc_buffer_text(header, ":");
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_append(header, port, port_length);
            }
        }
        if (status != KC_KAIMAN_OK || header == NULL) {
            kc_buffer_close(header);
            free(out->host);
            out->host = NULL;
            free(out->target);
            out->target = NULL;
            return status != KC_KAIMAN_OK ? status : KC_KAIMAN_ENOMEM;
        }
        out->host_header = kc_util_strdup(kc_buffer_value(header));
        kc_buffer_close(header);
        if (out->host_header == NULL) {
            free(out->host);
            free(out->target);
            out->host = NULL;
            out->target = NULL;
            return KC_KAIMAN_ENOMEM;
        }
    }
    out->port = (char *)malloc(port_length + 1U);
    if (out->port == NULL) {
        free(out->host);
        free(out->host_header);
        free(out->target);
        out->host = NULL;
        out->host_header = NULL;
        out->target = NULL;
        return KC_KAIMAN_ENOMEM;
    }
    memcpy(out->port, port, port_length);
    out->port[port_length] = '\0';
    return KC_KAIMAN_OK;
}

/**
 * Release one parsed URL.
 * @param url URL to clear.
 * @return void
 */
static void upstream_url_free(kc_upstream_url_t *url) {
    if (url == NULL) return;
    free(url->host);
    free(url->host_header);
    free(url->port);
    free(url->target);
    memset(url, 0, sizeof(*url));
}

/**
 * Open a connected nonblocking TCP socket.
 * @param url Parsed destination.
 * @param deadline Monotonic deadline.
 * @param out Receives the socket.
 * @return KC_KAIMAN_OK or an error status.
 */
static int upstream_connect(const kc_upstream_url_t *url, double deadline,
    kc_socket_t *out) {
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    struct addrinfo *item;
    kc_socket_t fd = KC_SOCKET_INVALID;
    int status = KC_KAIMAN_EIO;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(url->host, url->port, &hints, &list) != 0) {
        return KC_KAIMAN_EIO;
    }
    for (item = list; item != NULL; item = item->ai_next) {
#ifdef _WIN32
        u_long enabled = 1UL;
#else
        int flags;
#endif
        int error = 0;
        int result;

        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd == KC_SOCKET_INVALID) continue;
#ifdef _WIN32
        (void)ioctlsocket(fd, FIONBIO, &enabled);
        result = connect(fd, item->ai_addr, (int)item->ai_addrlen);
        if (result != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            status = upstream_wait(fd, 1, deadline);
            if (status == KC_KAIMAN_OK) {
                int length = (int)sizeof(error);
                (void)getsockopt(fd, SOL_SOCKET, SO_ERROR,
                    (char *)&error, &length);
                status = error == 0 ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
            }
        } else if (result == 0) {
            status = KC_KAIMAN_OK;
        }
#else
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        result = connect(fd, item->ai_addr, (socklen_t)item->ai_addrlen);
        if (result != 0 && (errno == EINPROGRESS || errno == EALREADY)) {
            status = upstream_wait(fd, 1, deadline);
            if (status == KC_KAIMAN_OK) {
                socklen_t length = (socklen_t)sizeof(error);
                (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
                status = error == 0 ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
            }
        } else if (result == 0) {
            status = KC_KAIMAN_OK;
        }
#endif
        if (status == KC_KAIMAN_OK) break;
        kc_socket_close(fd);
        fd = KC_SOCKET_INVALID;
    }
    freeaddrinfo(list);
    if (status != KC_KAIMAN_OK) return status;
    *out = fd;
    return KC_KAIMAN_OK;
}

/**
 * Release one owned upstream response.
 * @param response Response to clear, or NULL.
 * @return void
 */
void kc_upstream_response_free(kc_upstream_response_t *response) {
    if (response == NULL) return;
    free(response->content_type);
    free(response->location);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

/**
 * Perform one outbound HTTP or HTTPS request.
 * @param request Request description.
 * @param response Receives the owned response.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_upstream_send(const kc_upstream_request_t *request,
    kc_upstream_response_t *response) {
    kc_upstream_url_t url;
    kc_upstream_state_t state;
    kc_http_field_t *fields = NULL;
    kc_http_request_build_t built;
    kc_socket_t fd = KC_SOCKET_INVALID;
    void *wire = NULL;
    size_t wire_size = 0U;
    size_t field_count = 0U;
    size_t index;
    int status;
#ifdef _WIN32
    WSADATA data;
#endif

    if (request == NULL || response == NULL || request->url == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    memset(response, 0, sizeof(*response));
    memset(&state, 0, sizeof(state));
    status = upstream_url(request->url, &url);
    if (status != KC_KAIMAN_OK) return status;

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        upstream_url_free(&url);
        return KC_KAIMAN_EIO;
    }
#endif
    state.deadline = kc_util_seconds() +
        (double)(request->timeout > 0 ? request->timeout : 60);
    status = upstream_connect(&url, state.deadline, &fd);
    if (status != KC_KAIMAN_OK) goto done;
    state.fd = fd;

    fields = (kc_http_field_t *)calloc(
        request->header_count + 4U, sizeof(*fields));
    if (fields == NULL) {
        status = KC_KAIMAN_ENOMEM;
        goto done;
    }
    fields[field_count].name = "Host";
    fields[field_count++].value = url.host_header;
    fields[field_count].name = "User-Agent";
    fields[field_count++].value = "kaiman";
    fields[field_count].name = "Connection";
    fields[field_count++].value = "close";
    if (request->body != NULL && request->body_size > 0U) {
        fields[field_count].name = "Content-Type";
        fields[field_count++].value = "application/json";
    }
    for (index = 0U; index < request->header_count; index++) {
        if (request->headers[index].name == NULL ||
            request->headers[index].value == NULL) {
            continue;
        }
        fields[field_count].name = request->headers[index].name;
        fields[field_count++].value = request->headers[index].value;
    }
    memset(&built, 0, sizeof(built));
    built.method = request->method != NULL ? request->method : "GET";
    built.target = url.target;
    built.headers = fields;
    built.header_count = field_count;
    built.body = request->body;
    built.body_size = request->body_size;
    status = kc_http_request(&built, &wire, &wire_size);
    if (status != KC_HTTP_OK) {
        status = KC_KAIMAN_EINVAL;
        goto done;
    }

    status = kc_buffer_open(&state.raw);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&state.header);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&state.body);
    if (status != KC_KAIMAN_OK) goto done;
    state.stream_only = request->stream_only;
    state.chunk = request->chunk;
    state.status_fn = request->status;
    state.userdata = request->userdata;
    state.secure = url.secure;

    if (url.secure) {
        kc_tls_options_t options;
        memset(&options, 0, sizeof(options));
        options.role = KC_TLS_CLIENT;
        options.host = url.host;
        status = kc_tls_open(&state.tls, &options,
            upstream_tls_plain, upstream_tls_wire, &state);
        if (status != KC_TLS_OK) {
            status = KC_KAIMAN_EPROTO;
            goto done;
        }
    }
    status = upstream_send_bytes(&state, wire, wire_size);
    if (status != KC_KAIMAN_OK) goto done;

    while (!state.complete && state.failure == KC_KAIMAN_OK) {
        status = upstream_receive(&state);
        if (status == KC_KAIMAN_NOT_FOUND) {
            if (state.headers_done) state.complete = 1;
            else status = KC_KAIMAN_EHTTP;
            if (state.headers_done) {
                status = KC_KAIMAN_OK;
                break;
            }
        }
        if (status != KC_KAIMAN_OK) goto done;
        status = state.failure;
        if (status != KC_KAIMAN_OK) goto done;
    }
    if (state.failure != KC_KAIMAN_OK) {
        status = state.failure;
        goto done;
    }
    if (!state.complete) {
        status = KC_KAIMAN_ETIMEOUT;
        goto done;
    }
    response->status = state.status;
    response->content_type = kc_util_strdup(state.content_type != NULL
        ? state.content_type : "");
    response->location = state.location;
    state.location = NULL;
    response->body = kc_util_strdup(kc_buffer_value(state.body));
    if (response->content_type == NULL || response->body == NULL) {
        kc_upstream_response_free(response);
        status = KC_KAIMAN_ENOMEM;
        goto done;
    }
    response->body_size = state.body->size;
    status = KC_KAIMAN_OK;

done:
    kc_tls_close(state.tls);
    if (fd != KC_SOCKET_INVALID) kc_socket_close(fd);
    kc_buffer_close(state.raw);
    kc_buffer_close(state.header);
    kc_buffer_close(state.body);
    free(state.content_type);
    free(state.location);
    kc_http_free(wire);
    free(fields);
    upstream_url_free(&url);
#ifdef _WIN32
    WSACleanup();
#endif
    return status;
}

/**
 * Return a stable description for an upstream status.
 * @param status Status to describe.
 * @return NUL terminated description.
 */
const char *kc_upstream_strerror(int status) {
    switch (status) {
        case KC_KAIMAN_OK: return "success";
        case KC_KAIMAN_EINVAL: return "invalid upstream request";
        case KC_KAIMAN_ENOMEM: return "out of memory";
        case KC_KAIMAN_EHTTP: return "invalid upstream response";
        case KC_KAIMAN_EIO: return "upstream input/output error";
        case KC_KAIMAN_EPROTO: return "upstream protocol error";
        case KC_KAIMAN_ETIMEOUT: return "upstream request timed out";
        default: return "unknown upstream error";
    }
}
