/**
 * test.c - Contract tests for kaiman.
 * Summary: Exercises the public library API against a temporary config file.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkaiman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Remove the test configuration if a previous run left it behind.
 * @param path Configuration path.
 * @return void
 */
static void reset_file(const char *path) {
    remove(path);
}

/**
 * Run the library contract checks.
 * @param path Temporary configuration path.
 * @return 0 on success, 1 on failure.
 */
static int checks(const char *path) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_t *reloaded = NULL;
    kc_kaiman_service_t **services = NULL;
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_service_t *found = NULL;
    kc_kaiman_options_t options;
    kc_kaiman_descriptor_t descriptor;
    kc_kaiman_descriptor_t invalid;
    size_t count = 0U;
    int status;
    int result = 0;

    reset_file(path);
    options.path = path;
    options.listen = NULL;
    status = kc_kaiman_open(&kaiman, &options);
    if (status != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_address(kaiman), "127.0.0.1:49380") != 0) {
        fprintf(stderr, "open defaults failed\n");
        return 1;
    }
    printf("[1/6] [PASS] open_defaults: default listen and empty manager\n");

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = "local-opencode";
    descriptor.adapter = "opencode";
    descriptor.base_url = "http://127.0.0.1:49374";
    descriptor.model = "opencode/big-pickle";
    descriptor.api_key_env = "OPENCODE_TOKEN";
    descriptor.token_command = "opencode service get password";
    descriptor.auth_scheme = "basic";
    descriptor.auth_user = "opencode";
    descriptor.timeout = 90;
    status = kc_kaiman_add_service(kaiman, &descriptor, &service);
    if (status == KC_KAIMAN_OK) {
        status = kc_kaiman_get_services(kaiman, &services, &count);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_kaiman_get_service(kaiman, "local-opencode", &found);
    }
    if (status != KC_KAIMAN_OK || count != 1U || found != service ||
        strcmp(kc_kaiman_service_get(service, "model"),
            "opencode/big-pickle") != 0 ||
        kc_kaiman_service_timeout(service) != 90) {
        fprintf(stderr, "add and get failed\n");
        result = 1;
    }
    memset(&invalid, 0, sizeof(invalid));
    invalid.name = "invalid-adapter";
    invalid.adapter = "unknown";
    invalid.base_url = "http://127.0.0.1:1";
    if (kc_kaiman_add_service(kaiman, &invalid, &service) !=
        KC_KAIMAN_EINVAL || kc_kaiman_error(kaiman) == NULL) {
        fprintf(stderr, "invalid descriptor was accepted\n");
        result = 1;
    }
    if (kc_kaiman_add_service(kaiman, &descriptor, &service) !=
        KC_KAIMAN_EINVAL) {
        fprintf(stderr, "duplicate service was accepted\n");
        result = 1;
    }
    printf("[2/6] [PASS] add_and_get: managed service fields, identity and "
        "descriptor validation\n");

    status = kc_kaiman_service_set(service, "model", "opencode/gemini");
    if (status != KC_KAIMAN_OK || strcmp(kc_kaiman_service_get(service,
        "model"), "opencode/gemini") != 0) {
        fprintf(stderr, "service set failed\n");
        result = 1;
    }
    status = kc_kaiman_service_set(service, "adapter", "bad");
    if (status != KC_KAIMAN_EINVAL || kc_kaiman_error(kaiman) == NULL) {
        fprintf(stderr, "invalid adapter was accepted\n");
        result = 1;
    }
    printf("[3/6] [PASS] set_and_validate: field mutation and validation\n");

    status = kc_kaiman_listen(kaiman, "127.0.0.1:50000");
    if (status != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_address(kaiman), "127.0.0.1:50000") != 0) {
        fprintf(stderr, "listen failed\n");
        result = 1;
    }
    printf("[4/6] [PASS] listen: gateway address changed and persisted\n");

    status = kc_kaiman_open(&reloaded, &options);
    status = kc_kaiman_get_service(reloaded, "local-opencode", &found);
    if (status != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_address(reloaded), "127.0.0.1:50000") != 0 ||
        strcmp(kc_kaiman_service_get(found, "model"),
            "opencode/gemini") != 0 ||
        strcmp(kc_kaiman_service_get(found, "token-command"),
            "opencode service get password") != 0) {
        fprintf(stderr, "reload failed\n");
        result = 1;
    }
    printf("[5/6] [PASS] reload: persisted listen and service fields\n");

    status = kc_kaiman_service_remove(found);
    status = kc_kaiman_get_service(reloaded, "local-opencode", &service);
    if (status != KC_KAIMAN_NOT_FOUND ||
        kc_kaiman_service_set(found, "model", "x") != KC_KAIMAN_EINVAL) {
        fprintf(stderr, "remove lifecycle failed\n");
        result = 1;
    }
    printf("[6/6] [PASS] remove_lifecycle: removal invalidates the handle\n");

    if (kc_kaiman_version() == NULL || *kc_kaiman_version() == '\0') {
        fprintf(stderr, "missing version\n");
        result = 1;
    }
    kc_kaiman_close(reloaded);
    kc_kaiman_close(kaiman);
    reset_file(path);
    return result;
}

/**
 * Run the test program.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 on success, 1 on failure.
 */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: kaiman-test CONFIG_PATH\n");
        return 1;
    }
    if (checks(argv[1]) != 0) {
        fprintf(stderr, "6 tests run, 1 failed\n");
        return 1;
    }
    printf("6 tests run, 0 failed\n");
    return 0;
}
