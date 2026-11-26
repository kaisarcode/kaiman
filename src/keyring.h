/**
 * keyring.h - Native credential storage for kaiman.
 * Summary: Internal system credential operations.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_KEYRING_H
#define KC_KAIMAN_KEYRING_H

/**
 * Create a random credential reference.
 * @param id Destination array of at least 33 bytes.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_id(char *id);

/**
 * Store a credential in the operating system's protected storage.
 * @param id Credential reference.
 * @param secret Credential text.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_store(const char *id, const char *secret);

/**
 * Read a credential from the operating system's protected storage.
 * @param id Credential reference.
 * @param out Receives owned credential text.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_read(const char *id, char **out);

/**
 * Delete a credential from the operating system's protected storage.
 * @param id Credential reference.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_keyring_remove(const char *id);

#endif
