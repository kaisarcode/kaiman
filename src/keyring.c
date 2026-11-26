/**
 * keyring.c - Native credential storage for kaiman.
 * Summary: Stores secrets in the platform credential service.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "keyring.h"

#include "libkaiman.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <pthread.h>
#endif

/**
 * Create a random credential reference.
 * @param id Destination array of at least 33 bytes.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_id(char *id) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16];
    size_t index;

    if (id == NULL) return KC_KAIMAN_EINVAL;
#ifdef _WIN32
    if (BCryptGenRandom(NULL, bytes, sizeof(bytes),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return KC_KAIMAN_EIO;
#elif defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof(bytes), bytes) !=
        errSecSuccess) return KC_KAIMAN_EIO;
#else
    FILE *file = fopen("/dev/urandom", "rb");
    size_t count;

    if (file == NULL) return KC_KAIMAN_EIO;
    count = fread(bytes, 1U, sizeof(bytes), file);
    fclose(file);
    if (count != sizeof(bytes)) return KC_KAIMAN_EIO;
#endif
    for (index = 0U; index < sizeof(bytes); index++) {
        id[index * 2U] = hex[bytes[index] >> 4U];
        id[index * 2U + 1U] = hex[bytes[index] & 15U];
    }
    id[32] = '\0';
    return KC_KAIMAN_OK;
}

#ifdef _WIN32

/**
 * Operate on a generic Windows credential.
 * @param id Credential reference.
 * @param secret Secret to store, or NULL.
 * @param out Receives a secret for read operations, or NULL.
 * @return KC_KAIMAN_OK or an error status.
 */
static int keyring_access(const char *id, const char *secret, char **out) {
    char target[48];
    CREDENTIALA entry;
    PCREDENTIALA found = NULL;
    size_t length;

    (void)snprintf(target, sizeof(target), "kaiman:%s", id);
    if (secret != NULL) {
        length = strlen(secret);
        if (length > CRED_MAX_CREDENTIAL_BLOB_SIZE) return KC_KAIMAN_EINVAL;
        memset(&entry, 0, sizeof(entry));
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = target;
        entry.CredentialBlob = (LPBYTE)secret;
        entry.CredentialBlobSize = (DWORD)length;
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteA(&entry, 0) ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
    }
    if (out == NULL) {
        if (CredDeleteA(target, CRED_TYPE_GENERIC, 0) ||
            GetLastError() == ERROR_NOT_FOUND) return KC_KAIMAN_OK;
        return KC_KAIMAN_EIO;
    }
    if (!CredReadA(target, CRED_TYPE_GENERIC, 0, &found)) {
        return GetLastError() == ERROR_NOT_FOUND ?
            KC_KAIMAN_NOT_FOUND : KC_KAIMAN_EIO;
    }
    length = found->CredentialBlobSize;
    *out = (char *)malloc(length + 1U);
    if (*out != NULL) {
        memcpy(*out, found->CredentialBlob, length);
        (*out)[length] = '\0';
    }
    CredFree(found);
    return *out != NULL ? KC_KAIMAN_OK : KC_KAIMAN_ENOMEM;
}

#elif defined(__APPLE__)

/**
 * Operate on a generic password in the Apple Keychain.
 * @param id Credential reference.
 * @param secret Secret to store, or NULL.
 * @param out Receives a secret for read operations, or NULL.
 * @return KC_KAIMAN_OK or an error status.
 */
static int keyring_access(const char *id, const char *secret, char **out) {
    CFStringRef account = CFStringCreateWithCString(NULL, id,
        kCFStringEncodingUTF8);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef data = NULL;
    CFTypeRef found = NULL;
    OSStatus code = errSecParam;
    int status = KC_KAIMAN_ENOMEM;

    if (account == NULL || query == NULL) goto done;
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("com.kaisarcode.kaiman"));
    CFDictionarySetValue(query, kSecAttrAccount, account);
    if (secret != NULL) {
        data = CFDataCreate(NULL, (const UInt8 *)secret, (CFIndex)strlen(secret));
        if (data == NULL) goto done;
        CFDictionarySetValue(query, kSecValueData, data);
        code = SecItemAdd(query, NULL);
    } else if (out == NULL) {
        code = SecItemDelete(query);
        if (code == errSecItemNotFound) code = errSecSuccess;
    } else {
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        code = SecItemCopyMatching(query, &found);
        if (code == errSecSuccess) {
            size_t length;

            if (found == NULL || CFGetTypeID(found) != CFDataGetTypeID()) {
                status = KC_KAIMAN_EFORMAT;
                goto done;
            }
            length = (size_t)CFDataGetLength((CFDataRef)found);
            *out = (char *)malloc(length + 1U);
            if (*out == NULL) goto done;
            memcpy(*out, CFDataGetBytePtr((CFDataRef)found), length);
            (*out)[length] = '\0';
        }
    }
    status = code == errSecSuccess ? KC_KAIMAN_OK :
        code == errSecItemNotFound ? KC_KAIMAN_NOT_FOUND : KC_KAIMAN_EIO;

done:
    if (found != NULL) CFRelease(found);
    if (data != NULL) CFRelease(data);
    if (query != NULL) CFRelease(query);
    if (account != NULL) CFRelease(account);
    return status;
}

#elif defined(__linux__) && !defined(__ANDROID__)

typedef struct {
    void *(*schema_new)(const char *, int, ...);
    void (*schema_unref)(void *);
    int (*store)(const void *, const char *, const char *, const char *,
        void *, void **, ...);
    char *(*lookup)(const void *, void *, void **, ...);
    int (*clear)(const void *, void *, void **, ...);
    void (*password_free)(char *);
    void (*error_free)(void *);
} kc_secret_api_t;

static kc_secret_api_t secret_api;
static pthread_once_t secret_once = PTHREAD_ONCE_INIT;

/**
 * Resolve the optional system libsecret interface once per process.
 * @return void
 */
static void secret_load(void) {
    void *library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);

    if (library == NULL) return;
    secret_api.schema_new = dlsym(library, "secret_schema_new");
    secret_api.schema_unref = dlsym(library, "secret_schema_unref");
    secret_api.store = dlsym(library, "secret_password_store_sync");
    secret_api.lookup = dlsym(library, "secret_password_lookup_sync");
    secret_api.clear = dlsym(library, "secret_password_clear_sync");
    secret_api.password_free = dlsym(library, "secret_password_free");
    secret_api.error_free = dlsym(library, "g_error_free");
}

/**
 * Operate on a Secret Service credential through the system libsecret.
 * @param id Credential reference.
 * @param secret Secret to store, or NULL.
 * @param out Receives a secret for read operations, or NULL.
 * @return KC_KAIMAN_OK or an error status.
 */
static int keyring_access(const char *id, const char *secret, char **out) {
    void *schema;
    void *error = NULL;
    char *found;
    int status;

    (void)pthread_once(&secret_once, secret_load);
    if (secret_api.schema_new == NULL || secret_api.schema_unref == NULL ||
        secret_api.store == NULL || secret_api.lookup == NULL ||
        secret_api.clear == NULL || secret_api.password_free == NULL ||
        secret_api.error_free == NULL) return KC_KAIMAN_EPROTO;
    schema = secret_api.schema_new("com.kaisarcode.kaiman", 0,
        "id", 0, NULL);
    if (schema == NULL) return KC_KAIMAN_ENOMEM;
    if (secret != NULL) {
        status = secret_api.store(schema, NULL, "Kaiman credential", secret,
            NULL, &error, "id", id, NULL) ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
    } else if (out == NULL) {
        (void)secret_api.clear(schema, NULL, &error, "id", id, NULL);
        status = error == NULL ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
    } else {
        found = secret_api.lookup(schema, NULL, &error, "id", id, NULL);
        status = error == NULL ? KC_KAIMAN_NOT_FOUND : KC_KAIMAN_EIO;
        if (found != NULL) {
            *out = kc_util_strdup(found);
            secret_api.password_free(found);
            status = *out != NULL ? KC_KAIMAN_OK : KC_KAIMAN_ENOMEM;
        }
    }
    if (error != NULL) secret_api.error_free(error);
    secret_api.schema_unref(schema);
    return status;
}

#else

/**
 * Report unavailable secure storage on unsupported platforms.
 * @param id Credential reference.
 * @param secret Secret to store, or NULL.
 * @param out Receives a secret for read operations, or NULL.
 * @return KC_KAIMAN_EPROTO.
 */
static int keyring_access(const char *id, const char *secret, char **out) {
    (void)id;
    (void)secret;
    (void)out;
    return KC_KAIMAN_EPROTO;
}

#endif

/**
 * Store a credential in the operating system's protected storage.
 * @param id Credential reference.
 * @param secret Credential text.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_store(const char *id, const char *secret) {
    if (id == NULL || strlen(id) != 32U || secret == NULL || *secret == '\0') {
        return KC_KAIMAN_EINVAL;
    }
    return keyring_access(id, secret, NULL);
}

/**
 * Read a credential from the operating system's protected storage.
 * @param id Credential reference.
 * @param out Receives owned credential text.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_read(const char *id, char **out) {
    if (id == NULL || strlen(id) != 32U || out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    return keyring_access(id, NULL, out);
}

/**
 * Delete a credential from the operating system's protected storage.
 * @param id Credential reference.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_remove(const char *id) {
    if (id == NULL || strlen(id) != 32U) return KC_KAIMAN_EINVAL;
    return keyring_access(id, NULL, NULL);
}
