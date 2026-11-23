/**
 * kaiman.c - Command line frontend for kaiman.
 * Summary: Manages saved AI services and the gateway listen address.
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
 * Print command usage.
 * @return void
 */
static void usage(void) {
    printf("kaiman - unified AI service manager\n\n");
    printf("Usage:\n");
    printf("  kaiman <command> [arguments]\n\n");
    printf("Commands:\n");
    printf("  service add NAME [key=value ...]\n");
    printf("  service list\n");
    printf("  service edit NAME [key=value ...]\n");
    printf("  service remove NAME\n");
    printf("  serve [--listen host:port]\n\n");
    printf("Options:\n");
    printf("  -h, --help                  Show this help\n");
    printf("  -v, --version               Show version\n\n");
    printf("Service keys: adapter, url, model, env, token-command,\n");
    printf("  scheme, user, timeout\n");
}

/**
 * Report the last detailed library error, or the status description.
 * @param kaiman Open manager, or NULL.
 * @param status Status returned by the library.
 * @return void
 */
static void report_error(const kc_kaiman_t *kaiman, int status) {
    const char *detail = kc_kaiman_error(kaiman);

    if (detail != NULL) fprintf(stderr, "kaiman: %s\n", detail);
    else fprintf(stderr, "kaiman: %s\n", kc_kaiman_strerror(status));
}

/**
 * Print one managed service in a stable, parseable form.
 * @param service Managed service.
 * @return void
 */
static void print_service(const kc_kaiman_service_t *service) {
    printf("%s\n", kc_kaiman_service_get(service, "name"));
    printf("  adapter: %s\n", kc_kaiman_service_get(service, "adapter"));
    printf("  url: %s\n", kc_kaiman_service_get(service, "url"));
    printf("  model: %s\n", kc_kaiman_service_get(service, "model"));
    printf("  env: %s\n", kc_kaiman_service_get(service, "env"));
    printf("  token-command: %s\n",
        kc_kaiman_service_get(service, "token-command"));
    printf("  scheme: %s\n", kc_kaiman_service_get(service, "scheme"));
    printf("  user: %s\n", kc_kaiman_service_get(service, "user"));
    printf("  timeout: %d\n", kc_kaiman_service_timeout(service));
}

/**
 * Apply key=value arguments to one managed service.
 * @param service Managed service.
 * @param arguments Argument array.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int apply_fields(kc_kaiman_t *kaiman, kc_kaiman_service_t *service,
    char **arguments, int count) {
    int index;

    for (index = 0; index < count; index++) {
        char *separator = strchr(arguments[index], '=');
        int status;
        if (separator == NULL) {
            fprintf(stderr, "kaiman: expected key=value: %s\n",
                arguments[index]);
            return 1;
        }
        *separator = '\0';
        status = kc_kaiman_service_set(service, arguments[index],
            separator + 1);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            return 1;
        }
    }
    return 0;
}

/**
 * Run the service subcommand.
 * @param arguments Arguments after "service".
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int service_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t **services = NULL;
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_descriptor_t descriptor;
    size_t service_count = 0U;
    size_t index;
    int status;
    int result = 1;

    if (count < 1) {
        usage();
        return 1;
    }
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    if (strcmp(arguments[0], "list") == 0 && count == 1) {
        status = kc_kaiman_get_services(kaiman, &services, &service_count);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
        for (index = 0U; index < service_count; index++) {
            print_service(services[index]);
        }
        result = 0;
    } else if (strcmp(arguments[0], "add") == 0 && count >= 2) {
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.name = arguments[1];
        status = kc_kaiman_add_service(kaiman, &descriptor, &service);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
        if (apply_fields(kaiman, service, arguments + 2, count - 2)) {
            goto done;
        }
        result = 0;
    } else if (strcmp(arguments[0], "edit") == 0 && count >= 2) {
        status = kc_kaiman_get_service(kaiman, arguments[1], &service);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
        if (apply_fields(kaiman, service, arguments + 2, count - 2)) {
            goto done;
        }
        result = 0;
    } else if (strcmp(arguments[0], "remove") == 0 && count == 2) {
        status = kc_kaiman_get_service(kaiman, arguments[1], &service);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
        status = kc_kaiman_service_remove(service);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
        result = 0;
    } else {
        usage();
    }

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the serve subcommand.
 * @param arguments Arguments after "serve".
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int serve_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    int status;
    int result = 1;
    int index;

    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    for (index = 0; index < count; index++) {
        char *separator = strchr(arguments[index], '=');
        if (separator != NULL) *separator = '\0';
        if (strcmp(arguments[index], "--listen") == 0) {
            const char *value = separator != NULL ? separator + 1 :
                (index + 1 < count ? arguments[++index] : NULL);
            status = kc_kaiman_listen(kaiman, value);
            if (status != KC_KAIMAN_OK) {
                report_error(kaiman, status);
                goto done;
            }
        } else {
            fprintf(stderr, "kaiman: unknown serve option: %s\n",
                arguments[index]);
            goto done;
        }
    }
    printf("gateway listen: %s\n", kc_kaiman_address(kaiman));
    printf("gateway transport is not enabled in this build\n");
    result = 0;

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the command line frontend.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 on success, 1 on failure.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("%s\n", kc_kaiman_version());
        return 0;
    }
    if (strcmp(argv[1], "service") == 0) {
        return service_command(argv + 2, argc - 2);
    }
    if (strcmp(argv[1], "serve") == 0) {
        return serve_command(argv + 2, argc - 2);
    }
    usage();
    return 1;
}
