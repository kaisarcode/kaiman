/**
 * libkaiman.h - Unified AI service management for kaiman.
 * Summary: Public API for managed services, gateway settings and persistence.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_H
#define KC_KAIMAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KC_KAIMAN_OK 0
#define KC_KAIMAN_NOT_FOUND 1
#define KC_KAIMAN_EINVAL -1
#define KC_KAIMAN_ENOMEM -2
#define KC_KAIMAN_EFORMAT -3
#define KC_KAIMAN_EIO -4

typedef struct kc_kaiman kc_kaiman_t;
typedef struct kc_kaiman_service kc_kaiman_service_t;

typedef struct {
    const char *path;
    const char *listen;
} kc_kaiman_options_t;

typedef struct {
    const char *name;
    const char *adapter;
    const char *base_url;
    const char *model;
    const char *api_key_env;
    const char *token_command;
    const char *auth_scheme;
    const char *auth_user;
    int timeout;
} kc_kaiman_descriptor_t;

/**
 * Open a service manager and load its saved configuration.
 * @param out Receives the manager, or NULL on failure.
 * @param options Optional path and listen override. Values are copied.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_open(kc_kaiman_t **out, const kc_kaiman_options_t *options);

/**
 * Close a manager and release every service it owns.
 * @param kaiman Manager to close, or NULL.
 * @return void
 */
void kc_kaiman_close(kc_kaiman_t *kaiman);

/**
 * Borrow all managed services in configuration order.
 * @param kaiman Open manager.
 * @param out Receives a borrowed array of service handles.
 * @param count Receives the number of services.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_get_services(const kc_kaiman_t *kaiman,
    kc_kaiman_service_t ***out, size_t *count);

/**
 * Find one managed service by name.
 * @param kaiman Open manager.
 * @param name Service name.
 * @param out Receives a borrowed service handle when found.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND, or an error status.
 */
int kc_kaiman_get_service(const kc_kaiman_t *kaiman, const char *name,
    kc_kaiman_service_t **out);

/**
 * Create a service from a descriptor and persist the configuration.
 * @param kaiman Open manager.
 * @param descriptor Service fields to copy.
 * @param out Receives a borrowed service handle.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_add_service(kc_kaiman_t *kaiman,
    const kc_kaiman_descriptor_t *descriptor, kc_kaiman_service_t **out);

/**
 * Reload services and gateway settings from the configured path.
 * @param kaiman Open manager.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_load(kc_kaiman_t *kaiman);

/**
 * Set the gateway listen address and persist the configuration.
 * @param kaiman Open manager.
 * @param listen Host and port address.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_listen(kc_kaiman_t *kaiman, const char *listen);

/**
 * Borrow the current gateway listen address.
 * @param kaiman Open manager.
 * @return NUL terminated address, or NULL for an invalid manager.
 */
const char *kc_kaiman_address(const kc_kaiman_t *kaiman);

/**
 * Borrow the resolved configuration path.
 * @param kaiman Open manager.
 * @return NUL terminated path, or NULL for an invalid manager.
 */
const char *kc_kaiman_path(const kc_kaiman_t *kaiman);

/**
 * Borrow the most recent detailed error message.
 * @param kaiman Open manager, or NULL.
 * @return NUL terminated message, or NULL when none is set.
 */
const char *kc_kaiman_error(const kc_kaiman_t *kaiman);

/**
 * Return a stable description for a status code.
 * @param status Status to describe.
 * @return NUL terminated description.
 */
const char *kc_kaiman_strerror(int status);

/**
 * Return the manifest version of the library.
 * @return NUL terminated semantic version.
 */
const char *kc_kaiman_version(void);

/**
 * Set one field of a service and persist the configuration.
 * @param service Managed service.
 * @param key Field name.
 * @param value New value.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_set(kc_kaiman_service_t *service, const char *key,
    const char *value);

/**
 * Borrow one field of a service.
 * @param service Managed service.
 * @param key Field name.
 * @return Borrowed value, or NULL when unset or unknown.
 */
const char *kc_kaiman_service_get(const kc_kaiman_service_t *service,
    const char *key);

/**
 * Borrow the configured request timeout of a service.
 * @param service Managed service.
 * @return Timeout in seconds, or zero for an invalid handle.
 */
int kc_kaiman_service_timeout(const kc_kaiman_service_t *service);

/**
 * Remove a service and persist the configuration.
 * The handle remains allocated but becomes invalid.
 * @param service Managed service.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_remove(kc_kaiman_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
