/**
 * test.c - Contract tests for kaiman.
 * Summary: Exercises the public API, gateway and CLI process on every target.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "json.h"
#include "libkaiman.h"
#include "libnetl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define KC_TEST_TOTAL "10"
#define TEST_GATEWAY_PORT 49381
#define TEST_UPSTREAM_PORT 49382
#define TEST_CLI_PORT 49383
#ifdef _WIN32
#define TEST_CLI "kaiman.exe"
#else
#define TEST_CLI "./kaiman"
#endif
#define TEST_CLI_OUTPUT "kaiman-cli-output.txt"

#ifdef _WIN32
typedef SOCKET test_socket_t;
typedef HANDLE test_thread_t;
typedef unsigned (__stdcall *test_thread_fn)(void *);
#define TEST_SOCKET_INVALID INVALID_SOCKET
#else
typedef int test_socket_t;
typedef pthread_t test_thread_t;
typedef void *(*test_thread_fn)(void *);
#define TEST_SOCKET_INVALID (-1)
#endif

/**
 * Remove one test file.
 * @param path File path.
 * @return void
 */
static void reset_file(const char *path) {
    remove(path);
}

/**
 * Sleep for a bounded interval.
 * @param milliseconds Milliseconds to wait.
 * @return void
 */
static void test_sleep(int milliseconds) {
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    usleep((useconds_t)milliseconds * 1000U);
#endif
}

/**
 * Create one test directory.
 * @param path Directory path.
 * @return 1 on success, 0 on failure.
 */
static int test_directory(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) return 1;
#else
    if (mkdir(path, 0700) == 0) return 1;
#endif
    return errno == EEXIST;
}

/**
 * Set one environment value for the current test process.
 * @param name Variable name.
 * @param value Variable value.
 * @return void
 */
static void test_env_set(const char *name, const char *value) {
#ifdef _WIN32
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

/**
 * Initialize the platform socket layer.
 * @return void
 */
static void test_socket_start(void) {
#ifdef _WIN32
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

/**
 * Release the platform socket layer.
 * @return void
 */
static void test_socket_stop(void) {
#ifdef _WIN32
    (void)WSACleanup();
#endif
}

/**
 * Close one test socket.
 * @param fd Socket handle.
 * @return 1 on success, 0 on failure.
 */
static int test_socket_close(test_socket_t fd) {
#ifdef _WIN32
    return closesocket(fd) == 0;
#else
    return close(fd) == 0;
#endif
}

/**
 * Send every byte of one buffer through a connected socket.
 * @param fd Connected socket.
 * @param data Bytes to send.
 * @param size Byte count.
 * @return 1 on success, 0 on failure.
 */
static int test_send_all(test_socket_t fd, const void *data, size_t size) {
    const char *bytes = (const char *)data;
    size_t offset = 0U;

    while (offset < size) {
#ifdef _WIN32
        int sent = send(fd, bytes + offset, (int)(size - offset), 0);
#else
        ssize_t sent = send(fd, bytes + offset, size - offset, 0);
#endif
        if (sent <= 0) return 0;
        offset += (size_t)sent;
    }
    return 1;
}

/**
 * Perform one HTTP request against the test gateway.
 * @param method HTTP method.
 * @param path Request target.
 * @param body Request body, or NULL.
 * @param size Receives the owned response byte count.
 * @return Owned NUL terminated response, or NULL on failure.
 */
static char *test_request(const char *method, const char *path,
    const char *body, size_t *size) {
    struct sockaddr_in address;
    char request[4096];
    char *response;
    test_socket_t fd;
#ifdef _WIN32
    int timeout = 5000;
#else
    struct timeval timeout;
#endif
    int length;
    int ok = 0;

    if (size == NULL) return NULL;
    *size = 0U;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == TEST_SOCKET_INVALID) return NULL;
#ifdef _WIN32
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
        (const char *)&timeout, sizeof(timeout));
#else
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(TEST_GATEWAY_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)test_socket_close(fd);
        return NULL;
    }
    if (body != NULL) {
        length = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
            "application/json\r\nContent-Length: %zu\r\nConnection: "
            "close\r\n\r\n%s",
            method, path, strlen(body), body);
    } else {
        length = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n",
            method, path);
    }
    if (length < 0 || (size_t)length >= sizeof(request) ||
        !test_send_all(fd, request, (size_t)length)) {
        (void)test_socket_close(fd);
        return NULL;
    }
    response = (char *)malloc(65536U);
    if (response == NULL) {
        (void)test_socket_close(fd);
        return NULL;
    }
#ifdef _WIN32
    {
        int received;
        while ((received = recv(fd, response + *size,
            (int)(65535U - *size), 0)) > 0) {
            *size += (size_t)received;
            if (*size >= 65535U) break;
        }
    }
#else
    {
        ssize_t received;
        while ((received = recv(fd, response + *size,
            65535U - *size, 0)) > 0) {
            *size += (size_t)received;
            if (*size >= 65535U) break;
        }
    }
#endif
    ok = test_socket_close(fd);
    response[*size] = '\0';
    if (!ok) {
        free(response);
        *size = 0U;
        return NULL;
    }
    return response;
}

/**
 * Wait for the test gateway to accept requests.
 * @return 1 when the gateway is ready, or 0 on timeout.
 */
static int test_wait_gateway(void) {
    int index;

    for (index = 0; index < 100; index++) {
        size_t size = 0U;
        char *response = test_request("GET", "/health", NULL, &size);
        if (response != NULL &&
            strstr(response, "\"status\":\"ok\"") != NULL) {
            free(response);
            return 1;
        }
        free(response);
        test_sleep(100);
    }
    return 0;
}

/**
 * Reply to one deterministic upstream request.
 * @param peer Borrowed requesting peer.
 * @param data Borrowed request bytes.
 * @param size Request byte count.
 * @param userdata Unused callback context.
 * @return void
 */
static void test_mock_receive(kc_netl_peer_t *peer, const void *data,
    size_t size, void *userdata) {
    static const char completion[] =
        "{\"id\":\"upstream-1\",\"object\":\"chat.completion\","
        "\"created\":1,\"model\":\"mock/model\",\"choices\":[{\"index\":0,"
        "\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},"
        "\"finish_reason\":\"stop\"}]}";
    static const char events[] =
        "data: {\"id\":\"upstream-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"mock/model\",\"choices\":[{\"index\":0,\"delta\":"
        "{\"content\":\"hello\"},\"finish_reason\":null}]}\n\n"
        "data: [DONE]\n\n";
    static unsigned int turns;
    char request[8192];
    char response[8192];
    const char *payload;
    const char *content_type;
    int length;

    (void)userdata;
    if (size >= sizeof(request)) size = sizeof(request) - 1U;
    memcpy(request, data, size);
    request[size] = '\0';
    if (strstr(request, "GET /opencode/api/session/active") != NULL) {
        payload = "{\"data\":{\"ses_busy\":{}}}";
        content_type = "application/json";
    } else if (strstr(request, "GET /opencode/api/session?") != NULL) {
        if (strstr(request, "cursor=page%2Ftwo") != NULL) {
            payload = "{\"data\":[{\"id\":\"ses_busy\",\"title\":\"Busy\"}],"
                "\"cursor\":{\"next\":null}}";
        } else {
            payload = "{\"data\":[{\"id\":\"ses_demo\",\"title\":\"Demo\"}],"
                "\"cursor\":{\"next\":\"page/two\"}}";
        }
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode/api/session/ses_demo/fork") !=
        NULL) {
        payload = "{\"data\":{\"id\":\"ses_fork\",\"title\":\"Demo fork\"}}";
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode/api/session/ses_demo/model") !=
        NULL) {
        payload = "{}";
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode/api/session/ses_demo/prompt") !=
        NULL) {
        turns++;
        payload = "{}";
        content_type = "application/json";
    } else if (strstr(request, "GET /opencode/api/session/ses_demo/message") !=
        NULL) {
        if (strstr(request, "cursor=older%2Fpage") != NULL) {
            payload = "{\"data\":[{\"id\":\"msg_new\",\"type\":\"assistant\","
                "\"content\":[{\"type\":\"text\",\"text\":\"first reply\"}]},"
                "{\"id\":\"idle_old\",\"type\":\"idle\","
                "\"outcome\":\"succeeded\"}],\"cursor\":{\"next\":null}}";
        } else if (turns == 0U) {
            payload = "{\"data\":[{\"id\":\"idle_old\",\"type\":\"idle\","
                "\"outcome\":\"succeeded\"},{\"id\":\"msg_old\","
                "\"type\":\"assistant\",\"content\":[{\"type\":\"text\","
                "\"text\":\"old reply\"}]}]}";
        } else if (turns == 1U) {
            payload = "{\"data\":[{\"id\":\"idle_new\",\"type\":\"idle\","
                "\"outcome\":\"succeeded\"}],"
                "\"cursor\":{\"next\":\"older/page\"}}";
        } else {
            payload = "{\"data\":[{\"id\":\"idle_next\",\"type\":\"idle\","
                "\"outcome\":\"succeeded\"},{\"id\":\"msg_next\","
                "\"type\":\"assistant\",\"content\":[{\"type\":\"text\","
                "\"text\":\"second reply\"}]},{\"id\":\"idle_new\","
                "\"type\":\"idle\",\"outcome\":\"succeeded\"}]}";
        }
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode/api/session ") != NULL) {
        payload = "{\"data\":{\"id\":\"ses_created\",\"title\":\"Test\"}}";
        content_type = "application/json";
    } else if (strstr(request, "GET /opencode/api/model") != NULL) {
        payload = "{\"data\":[{\"providerID\":\"opencode\","
            "\"id\":\"model-a\"},{\"providerID\":\"other\",\"modelID\":\"model-b\"}]}";
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode-cloud/openai/v1/chat/completions") !=
        NULL && strstr(request, "x-opencode-session: ses_cloud") != NULL) {
        payload = completion;
        content_type = "application/json";
    } else if (strstr(request, "POST /opencode-cloud/openai/v1/chat/completions") !=
        NULL && strstr(request, "\"content\":\"cloud session\"") != NULL) {
        payload = "{\"error\":\"missing session header\"}";
        content_type = "application/json";
    } else if (strstr(request, "GET /opencode-cloud/v1/models") != NULL) {
        payload = "{\"data\":[{\"id\":\"model-a\",\"type\":\"chat\"},"
            "{\"id\":\"embedding-a\",\"type\":\"embedding\"}]}";
        content_type = "application/json";
    } else if (strstr(request, "GET /gemini/models") != NULL) {
        payload = "{\"models\":[{\"name\":\"models/model-a\","
            "\"supportedGenerationMethods\":[\"generateContent\"]}]}";
        content_type = "application/json";
    } else if (strstr(request, "GET /ollama/api/tags") != NULL) {
        payload = "{\"models\":[{\"model\":\"model-a\"}]}";
        content_type = "application/json";
    } else if (strstr(request, "GET /bedrock/foundation-models") != NULL) {
        payload = "{\"modelSummaries\":[{\"modelId\":\"model-a\","
            "\"outputModalities\":[\"TEXT\"]}]}";
        content_type = "application/json";
    } else if (strstr(request, "GET /bedrock/inference-profiles") != NULL) {
        payload = "{\"inferenceProfileSummaries\":["
            "{\"inferenceProfileId\":\"profile-a\",\"status\":\"ACTIVE\"}]}";
        content_type = "application/json";
    } else if (strncmp(request, "GET ", 4U) == 0) {
        payload = "{\"data\":[{\"id\":\"model-a\"},{\"id\":\"model-b\"}]}";
        content_type = "application/json";
    } else if (strstr(request, "/messages") != NULL) {
        payload = "{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],\"stop_reason\":\"end_turn\"}";
        content_type = "application/json";
    } else if (strstr(request, ":generateContent") != NULL) {
        payload = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello\"}]},\"finishReason\":\"STOP\"}]}";
        content_type = "application/json";
    } else if (strstr(request, "/responses") != NULL) {
        payload = "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello\"}]}]}";
        content_type = "application/json";
    } else if (strstr(request, "/api/chat") != NULL) {
        payload = "{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},\"done\":true,\"done_reason\":\"stop\"}";
        content_type = "application/json";
    } else if (strstr(request, "/converse") != NULL) {
        payload = "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"hello\"}]}},\"stopReason\":\"end_turn\"}";
        content_type = "application/json";
    } else if (strstr(request, "\"stream\":true") != NULL) {
        payload = events;
        content_type = "text/event-stream";
    } else {
        payload = completion;
        content_type = "application/json";
    }
    length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        content_type, strlen(payload), payload);
    if (length > 0 && (size_t)length < sizeof(response)) {
        (void)kc_netl_peer_send(peer, response, (size_t)length);
    }
    kc_netl_peer_close(peer);
}

/**
 * Hold the gateway used by the integration test.
 */
typedef struct {
    kc_kaiman_gateway_t *gateway;
} test_gateway_context_t;

/**
 * Run the gateway worker body.
 * @param userdata Borrowed gateway context.
 * @return void
 */
static void test_gateway_worker_body(void *userdata) {
    test_gateway_context_t *context = (test_gateway_context_t *)userdata;
    (void)kc_kaiman_gateway_run(context->gateway);
}

#ifdef _WIN32
/**
 * Run the gateway on a Windows worker thread.
 * @param userdata Borrowed gateway context.
 * @return Thread result.
 */
static unsigned __stdcall test_gateway_worker(void *userdata) {
    test_gateway_worker_body(userdata);
    return 0U;
}
#else
/**
 * Run the gateway on a POSIX worker thread.
 * @param userdata Borrowed gateway context.
 * @return Thread result.
 */
static void *test_gateway_worker(void *userdata) {
    test_gateway_worker_body(userdata);
    return NULL;
}
#endif

/**
 * Start one test worker thread.
 * @param thread Receives the thread handle.
 * @param run Worker entry point.
 * @param userdata Borrowed worker context.
 * @return 1 on success, 0 on failure.
 */
static int test_thread_start(test_thread_t *thread, test_thread_fn run,
    void *userdata) {
#ifdef _WIN32
    *thread = (HANDLE)_beginthreadex(NULL, 0U, run, userdata, 0U, NULL);
    return *thread != NULL;
#else
    return pthread_create(thread, NULL, run, userdata) == 0;
#endif
}

/**
 * Join one test worker thread.
 * @param thread Thread handle.
 * @return void
 */
static void test_thread_join(test_thread_t thread) {
#ifdef _WIN32
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
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
    printf("[1/" KC_TEST_TOTAL "] [PASS] open_defaults: default listen and empty manager\n");

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = "local-opencode";
    descriptor.adapter = "opencode";
    descriptor.base_url = "http://127.0.0.1:49374";
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
        strcmp(kc_kaiman_service_get(service, "url"),
            "http://127.0.0.1:49374") != 0 ||
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
    invalid.name = "invalid name";
    invalid.adapter = "opencode-local";
    invalid.base_url = NULL;
    if (kc_kaiman_add_service(kaiman, &invalid, &service) !=
        KC_KAIMAN_EINVAL ||
        strstr(kc_kaiman_error(kaiman), "invalid service name") == NULL) {
        fprintf(stderr, "invalid service name was accepted\n");
        result = 1;
    }
    if (kc_kaiman_add_service(kaiman, &descriptor, &service) !=
        KC_KAIMAN_EINVAL ||
        strstr(kc_kaiman_error(kaiman), "service already exists") == NULL) {
        fprintf(stderr, "duplicate service was accepted\n");
        result = 1;
    }
    printf("[2/" KC_TEST_TOTAL "] [PASS] add_and_get: managed service fields, identity and "
        "descriptor validation\n");

    if (kc_kaiman_default_url("unknown") != NULL ||
        strcmp(kc_kaiman_default_url("openai"),
            "https://api.openai.com/v1") != 0 ||
        kc_kaiman_service_set(service, "url", "") != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_service_get(service, "url"), "") != 0 ||
        strcmp(kc_kaiman_service_url(service), "http://127.0.0.1:4096") != 0 ||
        kc_kaiman_service_set(service, "adapter", "ollama") != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_service_url(service), "http://127.0.0.1:11434") != 0 ||
        kc_kaiman_service_set(service, "adapter", "opencode") != KC_KAIMAN_OK) {
        fprintf(stderr, "default URL resolution failed\n");
        result = 1;
    }
    status = kc_kaiman_service_set(service, "url", "http://127.0.0.1:49375");
    if (status != KC_KAIMAN_OK || strcmp(kc_kaiman_service_get(service,
        "url"), "http://127.0.0.1:49375") != 0 ||
        strcmp(kc_kaiman_service_url(service), "http://127.0.0.1:49375") != 0) {
        fprintf(stderr, "service set failed\n");
        result = 1;
    }
    if (kc_kaiman_service_set(service, "url", "") != KC_KAIMAN_OK ||
        kc_kaiman_service_set(service, "adapter", "opencode-local") !=
            KC_KAIMAN_OK || kc_kaiman_service_url(service) != NULL ||
        kc_kaiman_service_set(service, "adapter", "codex-local") !=
            KC_KAIMAN_OK || kc_kaiman_service_url(service) != NULL ||
        kc_kaiman_service_set(service, "adapter", "opencode") != KC_KAIMAN_OK ||
        kc_kaiman_service_set(service, "url", "http://127.0.0.1:49375") !=
            KC_KAIMAN_OK) {
        fprintf(stderr, "automatic local adapters failed\n");
        result = 1;
    }
    status = kc_kaiman_service_set(service, "adapter", "bad");
    if (status != KC_KAIMAN_EINVAL || kc_kaiman_error(kaiman) == NULL) {
        fprintf(stderr, "invalid adapter was accepted\n");
        result = 1;
    }
    if (kc_kaiman_service_set(service, "display-name",
        "Local OpenCode") != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_service_get(service, "display-name"),
            "Local OpenCode") != 0) {
        fprintf(stderr, "display name failed\n");
        result = 1;
    }
    printf("[3/" KC_TEST_TOTAL "] [PASS] set_and_validate: field mutation and validation\n");

    status = kc_kaiman_listen(kaiman, "127.0.0.1:50000");
    if (status != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_address(kaiman), "127.0.0.1:50000") != 0) {
        fprintf(stderr, "listen failed\n");
        result = 1;
    }
    printf("[4/" KC_TEST_TOTAL "] [PASS] listen: gateway address changed and persisted\n");

    status = kc_kaiman_open(&reloaded, &options);
    status = kc_kaiman_get_service(reloaded, "local-opencode", &found);
    if (status != KC_KAIMAN_OK ||
        strcmp(kc_kaiman_address(reloaded), "127.0.0.1:50000") != 0 ||
        strcmp(kc_kaiman_service_get(found, "url"),
            "http://127.0.0.1:49375") != 0 ||
        strcmp(kc_kaiman_service_get(found, "token-command"),
            "opencode service get password") != 0 ||
        strcmp(kc_kaiman_service_get(found, "display-name"),
            "Local OpenCode") != 0) {
        fprintf(stderr, "reload failed\n");
        result = 1;
    }
    printf("[5/" KC_TEST_TOTAL "] [PASS] reload: persisted listen and service fields\n");

    status = kc_kaiman_service_remove(found);
    status = kc_kaiman_get_service(reloaded, "local-opencode", &service);
    if (status != KC_KAIMAN_NOT_FOUND ||
        kc_kaiman_service_set(found, "url", "http://127.0.0.1:1") !=
            KC_KAIMAN_EINVAL) {
        fprintf(stderr, "remove lifecycle failed\n");
        result = 1;
    }
    printf("[6/" KC_TEST_TOTAL "] [PASS] remove_lifecycle: removal invalidates the handle\n");

    {
        const char *document_text =
            "{\"count\":2,\"name\":\"kaiman\",\"nested\":{\"ok\":true}}";
        kc_json_value_t *document = NULL;
        kc_json_value_t *nested;
        char *serialized = NULL;

        status = kc_json_parse(document_text, strlen(document_text),
            &document);
        if (status == KC_KAIMAN_OK) {
            nested = kc_json_member(document, "nested");
            status = kc_json_stringify(document, &serialized);
            if (status != KC_KAIMAN_OK ||
                strcmp(serialized, document_text) != 0 ||
                kc_json_number(kc_json_member(document, "count")) != 2.0 ||
                strcmp(kc_json_text(kc_json_member(document, "name")),
                    "kaiman") != 0 ||
                !kc_json_boolean(kc_json_member(nested, "ok"))) {
                status = KC_KAIMAN_EFORMAT;
            }
        }
        if (status != KC_KAIMAN_OK) {
            fprintf(stderr, "JSON parse and serialization failed\n");
            result = 1;
        }
        kc_kaiman_free(serialized);
        kc_json_free(document);
    }
    printf("[7/" KC_TEST_TOTAL "] [PASS] json_values: numbers, strings, nesting and "
        "serialization\n");

    {
        char *escaped = NULL;
        status = kc_json_escape("a\"b\\c\n", &escaped);
        if (status != KC_KAIMAN_OK || escaped == NULL ||
            strcmp(escaped, "a\\\"b\\\\c\\n") != 0) {
            fprintf(stderr, "JSON escaping failed\n");
            result = 1;
        }
        kc_kaiman_free(escaped);
    }
    printf("[8/" KC_TEST_TOTAL "] [PASS] json_escape: special characters and control bytes\n");

    if (kc_kaiman_get_service(kaiman, "missing-auth-test", &found) ==
        KC_KAIMAN_NOT_FOUND) {
        memset(&invalid, 0, sizeof(invalid));
        invalid.name = "missing-auth-test";
        invalid.auth_scheme = "none";
        if (kc_kaiman_add_service(kaiman, &invalid, &found) != KC_KAIMAN_OK ||
            kc_kaiman_service_authenticate(found, "bearer", NULL, NULL) !=
                KC_KAIMAN_EINVAL ||
            kc_kaiman_service_set(found, "env", "KAIMAN_TEST_KEY") !=
                KC_KAIMAN_OK ||
            kc_kaiman_service_set(found, "scheme", "bearer") != KC_KAIMAN_OK ||
            kc_kaiman_service_authenticate(found, "bearer", NULL, NULL) !=
                KC_KAIMAN_OK ||
            kc_kaiman_service_authenticate(found, "none", NULL, NULL) !=
                KC_KAIMAN_OK ||
            kc_kaiman_service_get(found, "env") != NULL ||
            kc_kaiman_service_remove(found) != KC_KAIMAN_OK) {
            fprintf(stderr, "authentication reference lifecycle failed\n");
            result = 1;
        }
    }

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
 * Exercise the real gateway against a deterministic upstream.
 * @param path Temporary configuration path.
 * @return 0 on success, 1 on failure.
 */
static int integration(const char *path) {
    static const char request_body[] =
        "{\"model\":\"mock/model-a\",\"messages\":[{\"role\":\"user\","
        "\"content\":\"hi\"}]}";
    static const char stream_body[] =
        "{\"model\":\"mock/model-a\",\"messages\":[{\"role\":\"user\","
        "\"content\":\"hi\"}],\"stream\":true}";
    static const char *const adapters[] = {"anthropic", "gemini",
        "responses", "ollama", "bedrock"};
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_gateway_t *gateway = NULL;
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_descriptor_t descriptor;
    kc_kaiman_options_t options;
    kc_netl_options_t mock_options;
    kc_netl_t *mock = NULL;
    test_gateway_context_t context;
    test_thread_t thread;
    size_t size = 0U;
    char *response = NULL;
    int thread_started = 0;
    size_t index;
    int result = 1;

    test_socket_start();
    reset_file(path);
    options.path = path;
    options.listen = "127.0.0.1:49381";
    if (kc_kaiman_open(&kaiman, &options) != KC_KAIMAN_OK) {
        fprintf(stderr, "integration open failed: %s\n",
            kc_kaiman_error(kaiman) != NULL ? kc_kaiman_error(kaiman) : "?");
        goto done;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = "mock";
    descriptor.adapter = "openai";
    descriptor.base_url = "http://127.0.0.1:49382/openai";
    descriptor.auth_scheme = "none";
    descriptor.timeout = 5;
    if (kc_kaiman_add_service(kaiman, &descriptor, &service) !=
        KC_KAIMAN_OK ||
        kc_kaiman_listen(kaiman, "127.0.0.1:49381") != KC_KAIMAN_OK) {
        fprintf(stderr, "integration setup failed: %s\n",
            kc_kaiman_error(kaiman) != NULL ? kc_kaiman_error(kaiman) : "?");
        goto done;
    }
    for (index = 0U; index < sizeof(adapters) / sizeof(adapters[0]); index++) {
        char base[128];

        (void)snprintf(base, sizeof(base), "http://127.0.0.1:49382/%s",
            adapters[index]);
        descriptor.name = adapters[index];
        descriptor.adapter = adapters[index];
        descriptor.base_url = base;
        if (kc_kaiman_add_service(kaiman, &descriptor, &service) != KC_KAIMAN_OK) {
            fprintf(stderr, "native adapter setup failed: %s\n", adapters[index]);
            goto done;
        }
    }
    descriptor.name = "opencode";
    descriptor.adapter = "opencode";
    descriptor.base_url = "http://127.0.0.1:49382/opencode";
    if (kc_kaiman_add_service(kaiman, &descriptor, &service) != KC_KAIMAN_OK) {
        fprintf(stderr, "OpenCode server setup failed\n");
        goto done;
    }
    descriptor.name = "opencode-cloud";
    descriptor.adapter = "opencode-cloud";
    descriptor.base_url = "http://127.0.0.1:49382/opencode-cloud";
    if (kc_kaiman_add_service(kaiman, &descriptor, &service) != KC_KAIMAN_OK) {
        fprintf(stderr, "OpenCode cloud setup failed\n");
        goto done;
    }
    kc_kaiman_close(kaiman);
    kaiman = NULL;

    memset(&mock_options, 0, sizeof(mock_options));
    mock_options.host = "127.0.0.1";
    mock_options.port = TEST_UPSTREAM_PORT;
    mock_options.protocol = KC_NETL_TCP;
    if (kc_netl_open(&mock, &mock_options, test_mock_receive,
        NULL, NULL, NULL) != KC_NETL_OK) {
        fprintf(stderr, "integration mock failed\n");
        goto done;
    }
    if (kc_kaiman_open(&kaiman, &options) != KC_KAIMAN_OK) {
        fprintf(stderr, "integration configuration failed\n");
        goto done;
    }
    {
        size_t models = 0U;
        size_t before_count = 0U;
        size_t after_count = 0U;
        kc_kaiman_service_t **items = NULL;

        descriptor.name = "unsaved-test";
        descriptor.adapter = "openai";
        descriptor.base_url = "http://127.0.0.1:49382/openai";
        descriptor.auth_scheme = "none";
        if (kc_kaiman_get_services(kaiman, &items, &before_count) !=
            KC_KAIMAN_OK ||
            kc_kaiman_service_test(kaiman, &descriptor, NULL, &models) !=
                KC_KAIMAN_OK || models != 2U ||
            kc_kaiman_get_services(kaiman, &items, &after_count) !=
                KC_KAIMAN_OK || before_count != after_count ||
            kc_kaiman_get_service(kaiman, descriptor.name, &service) !=
                KC_KAIMAN_NOT_FOUND) {
            fprintf(stderr, "unsaved service test failed\n");
            goto done;
        }
        descriptor.adapter = "invalid";
        if (kc_kaiman_service_test(kaiman, &descriptor, NULL, &models) !=
            KC_KAIMAN_EINVAL || models != 0U) {
            fprintf(stderr, "invalid service test failed\n");
            goto done;
        }
        descriptor.adapter = "openai";
        descriptor.auth_scheme = "bearer";
        if (kc_kaiman_service_test(kaiman, &descriptor, NULL, &models) !=
            KC_KAIMAN_EINVAL ||
            kc_kaiman_service_test(kaiman, &descriptor, "temporary", &models) !=
                KC_KAIMAN_OK || models != 2U) {
            fprintf(stderr, "temporary credential test failed\n");
            goto done;
        }
    }
    if (kc_kaiman_gateway_open(&gateway, kaiman) != KC_KAIMAN_OK) {
        fprintf(stderr, "integration gateway failed\n");
        goto done;
    }
    context.gateway = gateway;
    if (!test_thread_start(&thread, test_gateway_worker, &context)) {
        fprintf(stderr, "integration thread failed\n");
        goto done;
    }
    thread_started = 1;

    result = 0;
    if (!test_wait_gateway()) {
        fprintf(stderr, "gateway did not become ready\n");
        result = 1;
    }
    response = test_request("GET", "/health", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200 OK") == NULL ||
        strstr(response, "\"status\":\"ok\"") == NULL) {
        fprintf(stderr, "health endpoint failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/providers", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "{\"name\":\"OpenAI\",\"slug\":\"openai\"}") == NULL ||
        strstr(response, "\"slug\":\"opencode-local\"") == NULL ||
        strstr(response, "\"slug\":\"codex-local\"") == NULL ||
        strstr(response, "\"slug\":\"opencode\"") == NULL ||
        strstr(response, "\"slug\":\"opencode-cloud\"") == NULL ||
        strstr(response, "\"slug\":\"anthropic\"") == NULL ||
        strstr(response, "\"slug\":\"gemini\"") == NULL ||
        strstr(response, "\"slug\":\"responses\"") == NULL ||
        strstr(response, "\"slug\":\"ollama\"") == NULL ||
        strstr(response, "\"slug\":\"bedrock\"") == NULL ||
        strstr(response, "\"slug\":\"mock\"") != NULL) {
        fprintf(stderr, "providers endpoint failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/services", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "{\"data\":[") == NULL ||
        strstr(response, "\"slug\":\"mock\",\"provider\":\"openai\"") == NULL ||
        strstr(response, "\"slug\":\"ollama\",\"provider\":\"ollama\"") == NULL ||
        strstr(response, "\"slug\":\"gemini\",\"provider\":\"gemini\"") == NULL ||
        strstr(response, "\"slug\":\"bedrock\",\"provider\":\"bedrock\"") == NULL ||
        strstr(response, "\"slug\":\"openai\"") != NULL ||
        strstr(response, "\"slug\":\"opencode\"") == NULL ||
        strstr(response, "\"slug\":\"opencode-cloud\"") == NULL ||
        strstr(response, "\"slug\":\"codex-local\"") != NULL ||
        strstr(response, "127.0.0.1:49382") != NULL ||
        strstr(response, "\"keyring\"") != NULL) {
        fprintf(stderr, "services endpoint failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/sessions?service=opencode", NULL,
        &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"id\":\"opencode/ses_demo\"") == NULL ||
        strstr(response, "\"service\":\"opencode\"") == NULL ||
        strstr(response, "\"provider\":") != NULL ||
        strstr(response, "\"title\":\"Demo\",\"status\":\"idle\"") == NULL ||
        strstr(response, "\"id\":\"opencode/ses_busy\"") == NULL ||
        strstr(response, "\"title\":\"Busy\",\"status\":\"active\"") == NULL) {
        fprintf(stderr, "session discovery failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/sessions?service=mock", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "{\"data\":[]}") == NULL) {
        fprintf(stderr, "stateless provider session discovery failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/sessions",
        "{\"model\":\"opencode/opencode/model-a\",\"title\":\"Test\"}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"id\":\"opencode/ses_created\"") == NULL ||
        strstr(response, "\"title\":\"Test\"") == NULL) {
        fprintf(stderr, "session creation failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/sessions/fork",
        "{\"session\":\"opencode/ses_demo\",\"before\":\"msg_old\"}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"id\":\"opencode/ses_fork\"") == NULL) {
        fprintf(stderr, "session fork failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode/opencode/model-a\","
        "\"session\":\"opencode/ses_demo\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"one\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"content\":\"first reply\"") == NULL ||
        strstr(response, "old reply") != NULL) {
        fprintf(stderr, "explicit session completion failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode/opencode/model-a\","
        "\"session\":\"opencode/ses_demo\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"two\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"content\":\"second reply\"") == NULL ||
        strstr(response, "first reply") != NULL) {
        fprintf(stderr, "session continuation failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode/opencode/model-a\","
        "\"session\":\"opencode/ses_busy\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"hi\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 409") == NULL) {
        fprintf(stderr, "active session was not rejected\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode/opencode/model-a\","
        "\"session\":\"mock/ses_demo\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"hi\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 400") == NULL) {
        fprintf(stderr, "mismatched session service accepted\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"mock/model-a\",\"session\":\"mock/ses_demo\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 400") == NULL) {
        fprintf(stderr, "unsupported session service accepted\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models", NULL, &size);
    if (response == NULL || strstr(response, "\"id\":\"mock/model-a\"") == NULL ||
        strstr(response, "\"id\":\"mock/model-b\"") == NULL ||
        strstr(response, "\"id\":\"anthropic/model-a\"") == NULL ||
        strstr(response, "\"id\":\"gemini/model-a\"") == NULL ||
        strstr(response, "\"id\":\"responses/model-a\"") == NULL ||
        strstr(response, "\"id\":\"ollama/model-a\"") == NULL ||
        strstr(response, "\"id\":\"bedrock/model-a\"") == NULL ||
        strstr(response, "\"id\":\"bedrock/profile-a\"") == NULL ||
        strstr(response, "\"id\":\"opencode/opencode/model-a\"") == NULL ||
        strstr(response, "\"id\":\"opencode/other/model-b\"") == NULL ||
        strstr(response, "\"id\":\"opencode-cloud/model-a\"") == NULL ||
        strstr(response, "\"id\":\"opencode-cloud/embedding-a\"") != NULL) {
        fprintf(stderr, "models endpoint failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode-cloud/model-a\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"hi\"}]}", &size);
    if (response == NULL ||
        strstr(response, "\"model\":\"opencode-cloud/model-a\"") == NULL ||
        strstr(response, "\"content\":\"hello\"") == NULL) {
        fprintf(stderr, "OpenCode cloud completion failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"opencode-cloud/model-a\","
        "\"session\":\"opencode-cloud/ses_cloud\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"cloud session\"}]}", &size);
    if (response == NULL ||
        strstr(response, "\"content\":\"hello\"") == NULL) {
        fprintf(stderr, "OpenCode cloud session header failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?provider=mock", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 400") == NULL) {
        fprintf(stderr, "obsolete provider filter accepted\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?service=mock", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"id\":\"mock/model-a\"") == NULL ||
        strstr(response, "\"id\":\"mock/model-b\"") == NULL ||
        strstr(response, "\"id\":\"ollama/") != NULL ||
        strstr(response, "\"id\":\"anthropic/") != NULL) {
        fprintf(stderr, "single service filter failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?service=mock%2Collama",
        NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"id\":\"mock/model-a\"") == NULL ||
        strstr(response, "\"id\":\"ollama/model-a\"") == NULL ||
        strstr(response, "\"id\":\"gemini/") != NULL) {
        fprintf(stderr, "multiple service filter failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?service=mock&service=ollama",
        NULL, &size);
    if (response == NULL || strstr(response, "\"id\":\"mock/model-a\"") == NULL ||
        strstr(response, "\"id\":\"ollama/model-a\"") == NULL ||
        strstr(response, "\"id\":\"gemini/") != NULL) {
        fprintf(stderr, "repeated service filter failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?service=missing", NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\"data\":[]") == NULL) {
        fprintf(stderr, "unknown service filter failed\n");
        result = 1;
    }
    free(response);
    response = test_request("GET", "/v1/models?service=mock,,ollama",
        NULL, &size);
    if (response == NULL || strstr(response, "HTTP/1.1 400") == NULL) {
        fprintf(stderr, "invalid service filter accepted\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions", request_body,
        &size);
    if (response == NULL || strstr(response, "\"model\":\"mock/model-a\"") == NULL ||
        strstr(response, "\"content\":\"hello\"") == NULL) {
        fprintf(stderr, "chat completion failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions", stream_body,
        &size);
    if (response == NULL ||
        strstr(response, "text/event-stream") == NULL ||
        strstr(response, "\"model\":\"mock/model-a\"") == NULL ||
        strstr(response, "data: [DONE]") == NULL) {
        fprintf(stderr, "streaming chat completion failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"mock/model-b\",\"messages\":[{\"role\":\"user\","
        "\"content\":\"hi\"}]}", &size);
    if (response == NULL || strstr(response, "\"model\":\"mock/model-b\"") == NULL) {
        fprintf(stderr, "second provider model failed\n");
        result = 1;
    }
    free(response);
    response = test_request("POST", "/v1/chat/completions",
        "{\"model\":\"unknown/model-a\",\"messages\":[{\"role\":\"user\","
        "\"content\":\"hi\"}]}", &size);
    if (response == NULL || strstr(response, "HTTP/1.1 404") == NULL) {
        fprintf(stderr, "unknown service routing failed\n");
        result = 1;
    }
    free(response);
    for (index = 0U; index < sizeof(adapters) / sizeof(adapters[0]); index++) {
        char body[256];
        int streaming;

        for (streaming = 0; streaming <= 1; streaming++) {
            (void)snprintf(body, sizeof(body),
                "{\"model\":\"%s/model-a\",\"messages\":[{\"role\":\"system\","
                "\"content\":\"test\"},{\"role\":\"user\",\"content\":\"hi\"}],"
                "\"stream\":%s}", adapters[index], streaming ? "true" : "false");
            response = test_request("POST", "/v1/chat/completions", body, &size);
            if (response == NULL || strstr(response, "HTTP/1.1 200 OK") == NULL ||
                strstr(response, adapters[index]) == NULL ||
                strstr(response, "\"content\":\"hello\"") == NULL ||
                (streaming && (strstr(response, "text/event-stream") == NULL ||
                    strstr(response, "data: [DONE]") == NULL))) {
                fprintf(stderr, "native chat adapter failed: %s\n", adapters[index]);
                result = 1;
            }
            free(response);
        }
    }
    if (result == 0) {
        printf("[9/" KC_TEST_TOTAL "] [PASS] gateway_endpoints: health, models, chat and "
            "streaming\n");
    }

done:
    if (thread_started != 0) {
        kc_kaiman_gateway_stop(gateway);
        test_thread_join(thread);
    }
    if (gateway != NULL) {
        kc_kaiman_gateway_stop(gateway);
        kc_kaiman_gateway_close(gateway);
    }
    kc_netl_close(mock);
    kc_kaiman_close(kaiman);
    test_socket_stop();
    reset_file(path);
    return result;
}

/**
 * Read one test output file as text.
 * @param path File path.
 * @param size Receives the byte count, or NULL.
 * @return Owned NUL terminated text, or NULL on failure.
 */
static char *test_read_text(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    char *text;
    long length;
    size_t count;

    if (file == NULL) return NULL;
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    count = (size_t)length;
    text = (char *)malloc(count + 1U);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    if (count > 0U && fread(text, 1U, count, file) != count) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[count] = '\0';
    fclose(file);
    if (size != NULL) *size = count;
    return text;
}

#ifdef _WIN32
/**
 * Build one Windows command line from an argument vector.
 * @param buffer Output buffer.
 * @param size Output buffer size.
 * @param arguments NUL terminated argument vector.
 * @return 1 on success, 0 on failure.
 */
static int test_command_line(char *buffer, size_t size,
    char *const arguments[]) {
    size_t offset = 0U;
    int index;

    for (index = 0; arguments[index] != NULL; index++) {
        int length = snprintf(buffer + offset, size - offset, "%s\"%s\"",
            offset == 0U ? "" : " ", arguments[index]);
        if (length < 0 || (size_t)length >= size - offset) return 0;
        offset += (size_t)length;
    }
    return 1;
}
#endif

/**
 * Execute the CLI and capture its combined output.
 * @param arguments NUL terminated argument vector.
 * @param exit_code Receives the process exit status.
 * @param output Receives owned combined output.
 * @return 1 when the process was observed, or 0 on failure.
 */
static int test_cli_run(char *const arguments[], int *exit_code,
    char **output) {
#ifdef _WIN32
    char command[4096];
#endif
    int ok = 0;

    if (exit_code == NULL || output == NULL) return 0;
    *exit_code = 1;
    *output = NULL;
    reset_file(TEST_CLI_OUTPUT);
#ifdef _WIN32
    {
        SECURITY_ATTRIBUTES attributes;
        STARTUPINFOA startup;
        PROCESS_INFORMATION process;
        HANDLE file;
        DWORD code = 1U;

        memset(&attributes, 0, sizeof(attributes));
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        file = CreateFileA(TEST_CLI_OUTPUT, GENERIC_WRITE, FILE_SHARE_READ,
            &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return 0;
        if (!test_command_line(command, sizeof(command), arguments)) {
            CloseHandle(file);
            return 0;
        }
        memset(&startup, 0, sizeof(startup));
        memset(&process, 0, sizeof(process));
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = file;
        startup.hStdError = file;
        if (CreateProcessA(NULL, command, NULL, NULL, TRUE, 0U, NULL, NULL,
            &startup, &process)) {
            CloseHandle(file);
            if (WaitForSingleObject(process.hProcess, INFINITE) ==
                WAIT_OBJECT_0 &&
                GetExitCodeProcess(process.hProcess, &code)) {
                *exit_code = (int)code;
                ok = 1;
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else {
            CloseHandle(file);
        }
    }
#else
    {
        pid_t child = fork();
        int status = 0;

        if (child == 0) {
            int fd = open(TEST_CLI_OUTPUT, O_WRONLY | O_CREAT | O_TRUNC,
                0600);
            if (fd < 0) _exit(127);
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
            execv(TEST_CLI, arguments);
            _exit(127);
        }
        if (child > 0 && waitpid(child, &status, 0) == child &&
            WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
            ok = 1;
        }
    }
#endif
    if (ok) {
        *output = test_read_text(TEST_CLI_OUTPUT, NULL);
        if (*output == NULL) ok = 0;
    }
    return ok;
}

/**
 * Run one CLI expectation against the shared CLI test fixture.
 * @param arguments NUL terminated argument vector.
 * @param expected Expected process exit status.
 * @param present Required output text, or NULL.
 * @param absent Forbidden output text, or NULL.
 * @return 1 when the expectation holds, or 0 on failure.
 */
static int test_cli_check(char *const arguments[], int expected,
    const char *present, const char *absent) {
    char *output = NULL;
    int code = 1;
    int ok = 0;

    if (!test_cli_run(arguments, &code, &output)) return 0;
    if (code == expected &&
        (present == NULL || strstr(output, present) != NULL) &&
        (absent == NULL || strstr(output, absent) == NULL)) {
        ok = 1;
    }
    free(output);
    return ok;
}

/**
 * Exercise the flat CLI commands in the shared test harness.
 * @return 0 on success, 1 on failure.
 */
static int cli_commands(void) {
    const char *config_home = "kaiman-test-config";
    const char *state_home = "kaiman-test-state";
    char config_path[512];
    char pid_path[512];
    kc_kaiman_t *kaiman = NULL;
    char *output = NULL;
    int code = 1;
    int index;
    int result = 1;

    if (!test_directory(config_home) ||
        !test_directory("kaiman-test-config/kaiman") ||
        !test_directory(state_home) ||
        !test_directory("kaiman-test-state/kaiman")) {
        fprintf(stderr, "CLI test directories failed\n");
        return 1;
    }
    (void)snprintf(config_path, sizeof(config_path),
        "%s/kaiman/services.ini", config_home);
    (void)snprintf(pid_path, sizeof(pid_path),
        "%s/kaiman/kaiman.pid", state_home);
    test_env_set("XDG_CONFIG_HOME", config_home);
    test_env_set("XDG_STATE_HOME", state_home);
    reset_file(config_path);
    reset_file(pid_path);
    reset_file(TEST_CLI_OUTPUT);
    (void)kc_kaiman_stop();

    if (kc_kaiman_open(&kaiman, NULL) != KC_KAIMAN_OK ||
        kc_kaiman_listen(kaiman, "127.0.0.1:49383") != KC_KAIMAN_OK) {
        fprintf(stderr, "CLI setup failed: %s\n",
            kc_kaiman_error(kaiman) != NULL ? kc_kaiman_error(kaiman) : "?");
        goto done;
    }
    kc_kaiman_close(kaiman);
    kaiman = NULL;

    {
        char *arguments[] = { TEST_CLI, "-v", NULL };
        if (!test_cli_check(arguments, 0, kc_kaiman_version(), NULL)) {
            fprintf(stderr, "CLI short version failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "--version", NULL };
        if (!test_cli_check(arguments, 0, kc_kaiman_version(), NULL)) {
            fprintf(stderr, "CLI long version failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "-h", NULL };
        if (!test_cli_check(arguments, 0, "Usage:", NULL)) {
            fprintf(stderr, "CLI short help failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "--help", NULL };
        if (!test_cli_check(arguments, 0, "Usage:", NULL)) {
            fprintf(stderr, "CLI long help failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "--help", NULL };
        if (!test_cli_check(arguments, 0, "gui", NULL)) {
            fprintf(stderr, "CLI gui help failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "gui", "extra", NULL };
        if (!test_cli_check(arguments, 1, NULL, NULL)) {
            fprintf(stderr, "CLI gui argument check failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "add", "demo", "adapter=openai",
            "url=http://127.0.0.1:49374",
            "scheme=none", "timeout=5", NULL };
        if (!test_cli_check(arguments, 0, NULL, NULL)) {
            fprintf(stderr, "CLI add failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "list", NULL };
        if (!test_cli_check(arguments, 0, "demo", NULL)) {
            fprintf(stderr, "CLI list failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "edit", "demo",
            "url=http://127.0.0.1:49375", NULL };
        if (!test_cli_check(arguments, 0, NULL, NULL)) {
            fprintf(stderr, "CLI edit failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "list", NULL };
        if (!test_cli_check(arguments, 0, "http://127.0.0.1:49375", NULL)) {
            fprintf(stderr, "CLI edited list failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "start", NULL };
        if (!test_cli_check(arguments, 0, NULL, NULL)) {
            fprintf(stderr, "CLI start failed\n");
            goto done;
        }
    }
    for (index = 0; index < 50; index++) {
        char *arguments[] = { TEST_CLI, "status", NULL };
        if (!test_cli_run(arguments, &code, &output)) {
            fprintf(stderr, "CLI status launch failed\n");
            goto done;
        }
        if (code == 0 && strstr(output, "running") != NULL &&
            strstr(output, "address: 127.0.0.1:49383") != NULL) {
            free(output);
            output = NULL;
            break;
        }
        free(output);
        output = NULL;
        test_sleep(100);
    }
    if (index == 50) {
        fprintf(stderr, "CLI running status failed\n");
        goto done;
    }
    {
        char *arguments[] = { TEST_CLI, "stop", NULL };
        if (!test_cli_check(arguments, 0, NULL, NULL)) {
            fprintf(stderr, "CLI stop failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "status", NULL };
        if (!test_cli_check(arguments, 0, "stopped", NULL)) {
            fprintf(stderr, "CLI stopped status failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "remove", "demo", NULL };
        if (!test_cli_check(arguments, 0, NULL, NULL)) {
            fprintf(stderr, "CLI remove failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "list", NULL };
        if (!test_cli_check(arguments, 0, NULL, "demo")) {
            fprintf(stderr, "CLI removed list failed\n");
            goto done;
        }
    }
    {
        char *arguments[] = { TEST_CLI, "edit", "demo",
            "url=http://127.0.0.1:1", NULL };
        if (!test_cli_check(arguments, 1, NULL, NULL)) {
            fprintf(stderr, "CLI invalid edit succeeded\n");
            goto done;
        }
    }
    result = 0;

done:
    free(output);
    kc_kaiman_close(kaiman);
    (void)kc_kaiman_stop();
    reset_file(config_path);
    reset_file(pid_path);
    reset_file(TEST_CLI_OUTPUT);
    if (result == 0) {
        printf("[10/" KC_TEST_TOTAL "] [PASS] cli_commands: flat service and "
            "process commands\n");
    }
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
        fprintf(stderr, "10 tests run, 1 failed\n");
        return 1;
    }
    if (integration(argv[1]) != 0) {
        fprintf(stderr, "10 tests run, 1 failed\n");
        return 1;
    }
    if (cli_commands() != 0) {
        fprintf(stderr, "10 tests run, 1 failed\n");
        return 1;
    }
    printf("10 tests run, 0 failed\n");
    return 0;
}
