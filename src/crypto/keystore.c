/*
 * keystore.c — Encrypted API key storage implementation
 *
 * Implements AES-256-CBC encryption/decryption with machine-specific key
 * derivation for secure API key storage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <iphlpapi.h>
    #include <windows.h>
    #include <aclapi.h>
    #include <io.h>
    #include <sys/stat.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/ioctl.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <ifaddrs.h>
#endif

#include "keystore.h"

#define AES_KEY_SIZE 32   /* AES-256 */
#define AES_BLOCK_SIZE 16
#define IV_SIZE 16
#define MAX_PLAINTEXT_SIZE 512

/*
 * secure_zero — zero out sensitive memory with compiler barrier
 */
void secure_zero(void *ptr, size_t len)
{
    if (!ptr || len == 0) return;
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
}

/*
 * get_mac_address — retrieve primary network interface MAC address
 *
 * Returns: heap-allocated MAC address string (e.g., "00:1A:2B:3C:4D:5E")
 *          or NULL on failure. Caller must free.
 */
static char *get_mac_address(void)
{
#ifdef _WIN32
    IP_ADAPTER_INFO adapter_info[16];
    DWORD buf_len = sizeof(adapter_info);
    
    DWORD result = GetAdaptersInfo(adapter_info, &buf_len);
    if (result != ERROR_SUCCESS) {
        fprintf(stderr, "[keystore] GetAdaptersInfo failed: %lu\n", result);
        return NULL;
    }
    
    PIP_ADAPTER_INFO adapter = adapter_info;
    while (adapter) {
        if (adapter->Type == MIB_IF_TYPE_ETHERNET && adapter->AddressLength == 6) {
            char *mac = (char *)malloc(18);
            if (!mac) return NULL;
            
            snprintf(mac, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                     adapter->Address[0], adapter->Address[1],
                     adapter->Address[2], adapter->Address[3],
                     adapter->Address[4], adapter->Address[5]);
            return mac;
        }
        adapter = adapter->Next;
    }
    
    fprintf(stderr, "[keystore] No Ethernet adapter found\n");
    return NULL;
    
#else
    struct ifaddrs *ifaddr, *ifa;
    int family;
    char *mac = NULL;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("[keystore] getifaddrs");
        return NULL;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        family = ifa->ifa_addr->sa_family;
        
        if (family == AF_PACKET || family == AF_LINK) {
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) continue;
            
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
            
            if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *hwaddr = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                mac = (char *)malloc(18);
                if (mac) {
                    snprintf(mac, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                             hwaddr[0], hwaddr[1], hwaddr[2],
                             hwaddr[3], hwaddr[4], hwaddr[5]);
                }
                close(sock);
                break;
            }
            close(sock);
        }
    }
    
    freeifaddrs(ifaddr);
    
    if (!mac) {
        fprintf(stderr, "[keystore] No MAC address found\n");
    }
    
    return mac;
#endif
}

/*
 * get_hostname — retrieve system hostname
 */
static char *get_hostname(void)
{
    char *hostname = (char *)malloc(256);
    if (!hostname) return NULL;
    
#ifdef _WIN32
    DWORD size = 256;
    if (!GetComputerNameA(hostname, &size)) {
        fprintf(stderr, "[keystore] GetComputerName failed\n");
        free(hostname);
        return NULL;
    }
#else
    if (gethostname(hostname, 256) != 0) {
        perror("[keystore] gethostname");
        free(hostname);
        return NULL;
    }
#endif
    
    return hostname;
}

/*
 * get_machine_key — derive 32-byte key from MAC + hostname SHA-256 hash
 */
char *get_machine_key(void)
{
    char *mac = get_mac_address();
    char *hostname = get_hostname();
    
    if (!mac || !hostname) {
        free(mac);
        free(hostname);
        return NULL;
    }
    
    /* Concatenate MAC + hostname */
    size_t input_len = strlen(mac) + strlen(hostname);
    char *input = (char *)malloc(input_len + 1);
    if (!input) {
        free(mac);
        free(hostname);
        return NULL;
    }
    
    strcpy(input, mac);
    strcat(input, hostname);
    
    /* SHA-256 hash */
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)input, input_len, hash);
    
    /* Clean up */
    secure_zero(input, input_len);
    free(input);
    free(mac);
    free(hostname);
    
    /* Return as heap-allocated key */
    char *key = (char *)malloc(AES_KEY_SIZE);
    if (!key) return NULL;
    
    memcpy(key, hash, AES_KEY_SIZE);
    return key;
}

/*
 * set_restrictive_permissions — set file to owner-only access
 */
static int set_restrictive_permissions(const char *filepath)
{
#ifdef _WIN32
    /* Windows/MSYS2: avoid custom ACL edits that can lock the file.
     * Keep file writable/readable by current user context. */
    if (_chmod(filepath, _S_IREAD | _S_IWRITE) != 0) {
        perror("[keystore] _chmod");
        return -1;
    }
    return 0;
#else
    /* Unix: chmod 0600 */
    if (chmod(filepath, S_IRUSR | S_IWUSR) != 0) {
        perror("[keystore] chmod");
        return -1;
    }
    return 0;
#endif
}

/*
 * encrypt_api_key — encrypt plaintext to file with AES-256-CBC
 */
int encrypt_api_key(const char *plaintext, const char *output_file)
{
    if (!plaintext || !output_file) {
        fprintf(stderr, "[keystore] encrypt_api_key: NULL argument\n");
        return -1;
    }
    
    size_t plaintext_len = strlen(plaintext);
    if (plaintext_len == 0 || plaintext_len > MAX_PLAINTEXT_SIZE) {
        fprintf(stderr, "[keystore] Invalid plaintext length: %zu\n", plaintext_len);
        return -1;
    }
    
    /* Get machine-specific key */
    char *key = get_machine_key();
    if (!key) {
        fprintf(stderr, "[keystore] Failed to derive machine key\n");
        return -1;
    }
    
    /* Generate random IV */
    unsigned char iv[IV_SIZE];
    if (RAND_bytes(iv, IV_SIZE) != 1) {
        fprintf(stderr, "[keystore] RAND_bytes failed\n");
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        return -1;
    }
    
    /* Allocate output buffer (plaintext + padding) */
    int max_ciphertext_len = plaintext_len + AES_BLOCK_SIZE;
    unsigned char *ciphertext = (unsigned char *)malloc(max_ciphertext_len);
    if (!ciphertext) {
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        return -1;
    }
    
    /* Initialize cipher context */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "[keystore] EVP_CIPHER_CTX_new failed\n");
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        return -1;
    }
    
    int len, ciphertext_len;
    
    /* Initialize encryption */
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                          (unsigned char *)key, iv) != 1) {
        fprintf(stderr, "[keystore] EVP_EncryptInit_ex failed: %s\n",
                ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        return -1;
    }
    
    /* Encrypt */
    if (EVP_EncryptUpdate(ctx, ciphertext, &len,
                         (unsigned char *)plaintext, plaintext_len) != 1) {
        fprintf(stderr, "[keystore] EVP_EncryptUpdate failed\n");
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        return -1;
    }
    ciphertext_len = len;
    
    /* Finalize (PKCS7 padding) */
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
        fprintf(stderr, "[keystore] EVP_EncryptFinal_ex failed\n");
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        return -1;
    }
    ciphertext_len += len;
    
    EVP_CIPHER_CTX_free(ctx);
    secure_zero(key, AES_KEY_SIZE);
    free(key);
    
    /* Write to file: [IV][ciphertext] */
    FILE *f = fopen(output_file, "wb");
    if (!f) {
        perror("[keystore] fopen");
        secure_zero(ciphertext, ciphertext_len);
        free(ciphertext);
        return -1;
    }
    
    size_t written = fwrite(iv, 1, IV_SIZE, f);
    written += fwrite(ciphertext, 1, ciphertext_len, f);
    
    fclose(f);
    secure_zero(ciphertext, ciphertext_len);
    free(ciphertext);
    
    if (written != (size_t)(IV_SIZE + ciphertext_len)) {
        fprintf(stderr, "[keystore] Failed to write complete file\n");
        return -1;
    }
    
    /* Set restrictive permissions */
    if (set_restrictive_permissions(output_file) != 0) {
        fprintf(stderr, "[keystore] Warning: Failed to set restrictive permissions\n");
        /* Continue anyway - not fatal */
    }
    
    return 0;
}

/*
 * decrypt_api_key — decrypt API key from file
 */
char *decrypt_api_key(const char *input_file)
{
    if (!input_file) {
        fprintf(stderr, "[keystore] decrypt_api_key: NULL input_file\n");
        return NULL;
    }
    
    /* Open file */
    FILE *f = fopen(input_file, "rb");
    if (!f) {
        perror("[keystore] fopen");
        return NULL;
    }
    
    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size < IV_SIZE + AES_BLOCK_SIZE || file_size > 4096) {
        fprintf(stderr, "[keystore] Invalid file size: %ld\n", file_size);
        fclose(f);
        return NULL;
    }
    
    /* Read IV */
    unsigned char iv[IV_SIZE];
    if (fread(iv, 1, IV_SIZE, f) != IV_SIZE) {
        fprintf(stderr, "[keystore] Failed to read IV\n");
        fclose(f);
        return NULL;
    }
    
    /* Read ciphertext */
    int ciphertext_len = file_size - IV_SIZE;
    unsigned char *ciphertext = (unsigned char *)malloc(ciphertext_len);
    if (!ciphertext) {
        fclose(f);
        return NULL;
    }
    
    if (fread(ciphertext, 1, ciphertext_len, f) != (size_t)ciphertext_len) {
        fprintf(stderr, "[keystore] Failed to read ciphertext\n");
        fclose(f);
        free(ciphertext);
        return NULL;
    }
    fclose(f);
    
    /* Get machine-specific key */
    char *key = get_machine_key();
    if (!key) {
        fprintf(stderr, "[keystore] Failed to derive machine key\n");
        free(ciphertext);
        return NULL;
    }
    
    /* Allocate plaintext buffer */
    unsigned char *plaintext = (unsigned char *)malloc(ciphertext_len + AES_BLOCK_SIZE);
    if (!plaintext) {
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        return NULL;
    }
    
    /* Initialize cipher context */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "[keystore] EVP_CIPHER_CTX_new failed\n");
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        free(plaintext);
        return NULL;
    }
    
    int len, plaintext_len;
    
    /* Initialize decryption */
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                          (unsigned char *)key, iv) != 1) {
        fprintf(stderr, "[keystore] EVP_DecryptInit_ex failed: %s\n",
                ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        free(plaintext);
        return NULL;
    }
    
    /* Decrypt */
    if (EVP_DecryptUpdate(ctx, plaintext, &len,
                         ciphertext, ciphertext_len) != 1) {
        fprintf(stderr, "[keystore] EVP_DecryptUpdate failed\n");
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        secure_zero(plaintext, ciphertext_len + AES_BLOCK_SIZE);
        free(plaintext);
        return NULL;
    }
    plaintext_len = len;
    
    /* Finalize (remove PKCS7 padding) */
    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1) {
        /* Wrong machine key or stale/corrupted file: treat as key-not-available.
         * Callers handle fallback/recovery (wizard, legacy key migration, prompt). */
        EVP_CIPHER_CTX_free(ctx);
        secure_zero(key, AES_KEY_SIZE);
        free(key);
        free(ciphertext);
        secure_zero(plaintext, ciphertext_len + AES_BLOCK_SIZE);
        free(plaintext);
        return NULL;
    }
    plaintext_len += len;
    
    EVP_CIPHER_CTX_free(ctx);
    secure_zero(key, AES_KEY_SIZE);
    free(key);
    free(ciphertext);
    
    /* Null-terminate plaintext */
    plaintext[plaintext_len] = '\0';
    
    return (char *)plaintext;
}
