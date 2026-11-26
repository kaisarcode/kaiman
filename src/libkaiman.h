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
#define KC_KAIMAN_EBUSY -5
#define KC_KAIMAN_EPROTO -6
#define KC_KAIMAN_EHTTP -7
#define KC_KAIMAN_ETIMEOUT -8

typedef struct kc_kaiman kc_kaiman_t;
typedef struct kc_kaiman_service kc_kaiman_service_t;
typedef struct kc_kaiman_gateway kc_kaiman_gateway_t;

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

typedef struct {
    const char *executable;
} kc_kaiman_process_options_t;

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
 * Reload services and gateway settings from the configured path.
 * @param kaiman Open manager.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_load(kc_kaiman_t *kaiman);

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
 * Set authentication and optionally store a new secret in the system keyring.
 * A NULL secret preserves the existing source for an unchanged scheme.
 * The none scheme removes the existing credential and references.
 * @param service Managed service.
 * @param scheme Authentication scheme: none, bearer or basic.
 * @param user Username for basic authentication, otherwise NULL or empty.
 * @param secret New credential, or NULL to keep the current credential.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_authenticate(kc_kaiman_service_t *service,
    const char *scheme, const char *user, const char *secret);

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

/**
 * Open an HTTP gateway bound to an existing service manager.
 * The manager must outlive the gateway. All borrowed service handles are
 * invalidated by configuration reloads performed inside the gateway.
 * @param out Receives the gateway, or NULL on failure.
 * @param kaiman Open service manager.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_gateway_open(kc_kaiman_gateway_t **out, kc_kaiman_t *kaiman);

/**
 * Close a gateway and stop accepting requests.
 * @param gateway Gateway to close, or NULL.
 * @return void
 */
void kc_kaiman_gateway_close(kc_kaiman_gateway_t *gateway);

/**
 * Serve requests until the gateway is stopped.
 * @param gateway Open gateway.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_gateway_run(kc_kaiman_gateway_t *gateway);

/**
 * Request that a running gateway stop after active work completes.
 * @param gateway Open gateway, or NULL.
 * @return void
 */
void kc_kaiman_gateway_stop(kc_kaiman_gateway_t *gateway);

/**
 * Borrow the address on which the gateway listens.
 * @param gateway Open gateway.
 * @return NUL terminated address, or NULL for an invalid gateway.
 */
const char *kc_kaiman_gateway_address(const kc_kaiman_gateway_t *gateway);

/**
 * Borrow the most recent gateway error message.
 * @param gateway Open gateway, or NULL.
 * @return NUL terminated message, or NULL when none is set.
 */
const char *kc_kaiman_gateway_error(const kc_kaiman_gateway_t *gateway);

/**
 * Start the gateway as an independent process.
 * @param options Process options containing the executable path.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_start(const kc_kaiman_process_options_t *options);

/**
 * Stop the gateway process started by kc_kaiman_start().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_stop(void);

/**
 * Describe the gateway process and endpoint.
 * @param out Receives an owned description released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_status(char **out);

/**
 * Release memory returned by kc_kaiman_status().
 * @param ptr Owned pointer, or NULL.
 * @return void
 */
void kc_kaiman_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
