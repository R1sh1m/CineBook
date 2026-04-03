/*
 * keystore.h — Encrypted API key storage for CineBook
 *
 * Provides AES-256-CBC encryption/decryption of sensitive API keys
 * with machine-specific key derivation.
 *
 * Security features:
 *   - AES-256-CBC encryption via OpenSSL
 *   - Machine-specific key derived from MAC address + hostname SHA-256 hash
 *   - Cryptographically secure random IV per encryption
 *   - File format: [16-byte IV][encrypted data]
 *   - Restrictive file permissions (0600 Unix, restricted ACL Windows)
 */

#ifndef KEYSTORE_H
#define KEYSTORE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * get_machine_key — derive a machine-specific encryption key.
 *
 * Generates a 32-byte key by hashing (SHA-256) the concatenation of:
 *   1. Primary network interface MAC address
 *   2. System hostname
 *
 * Returns: heap-allocated 32-byte key (caller must free), or NULL on failure.
 */
char *get_machine_key(void);

/*
 * encrypt_api_key — encrypt plaintext API key to file.
 *
 * Uses AES-256-CBC with:
 *   - Machine-derived key from get_machine_key()
 *   - Cryptographically secure random 16-byte IV
 *   - PKCS7 padding
 *
 * File format: [16-byte IV][encrypted ciphertext]
 * Sets file permissions to 0600 (Unix) or restricted ACL (Windows).
 *
 * plaintext    : API key to encrypt (null-terminated string)
 * output_file  : path to output file (e.g., ".api_key")
 *
 * Returns: 0 on success, -1 on failure.
 */
int encrypt_api_key(const char *plaintext, const char *output_file);

/*
 * decrypt_api_key — decrypt API key from file.
 *
 * Reads file created by encrypt_api_key(), extracts IV, decrypts using
 * machine-derived key.
 *
 * input_file : path to encrypted key file
 *
 * Returns: heap-allocated plaintext string (caller must free), or NULL on failure.
 *          Zeroes out sensitive memory before freeing on error.
 */
char *decrypt_api_key(const char *input_file);

/*
 * secure_zero — zero out sensitive memory.
 *
 * Uses compiler barrier to prevent optimization from removing the zero operation.
 *
 * ptr  : pointer to memory to zero
 * len  : number of bytes to zero
 */
void secure_zero(void *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTORE_H */
