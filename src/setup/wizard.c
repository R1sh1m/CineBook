#include "wizard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "keystore.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

#define SETUP_MARKER_FILE ".setup_complete"
#define ENCRYPTED_KEY_FILE ".api_key"
#define LEGACY_KEY_FILE "api_key.txt"

static int file_exists(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    return access(path, F_OK) == 0;
}

static void strip_newline(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int api_key_format_ok(const char *k)
{
    if (!k || k[0] == '\0') return 0;
    size_t n = strlen(k);
    if (n < 16 || n > 128) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)k[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int setup_is_complete(void)
{
    return file_exists(SETUP_MARKER_FILE) && file_exists(ENCRYPTED_KEY_FILE);
}

int setup_write_complete_marker(void)
{
    FILE *f = fopen(SETUP_MARKER_FILE, "wb");
    if (!f) return 0;
    fputs("setup_complete=1\n", f);
    fclose(f);
#ifndef _WIN32
    chmod(SETUP_MARKER_FILE, S_IRUSR | S_IWUSR);
#endif
    return 1;
}

static size_t sink_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

int setup_validate_tmdb_api_key(const char *api_key)
{
    if (!api_key_format_ok(api_key)) return 0;

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.themoviedb.org/3/configuration?api_key=%s",
             api_key);

    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        CURL *curl = curl_easy_init();
        if (!curl) return 0;

        long http_code = 0;
        char errbuf[CURL_ERROR_SIZE];
        errbuf[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CineBook/1.5.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && http_code == 200L) {
            return 1;
        }

        if (http_code == 401L) {
            return 0; /* invalid key is non-retryable */
        }

        if (attempt < max_attempts) {
            int backoff_ms = 250 * (1 << (attempt - 1)); /* 250, 500 */
            sleep_ms(backoff_ms);
        }
    }

    return 0;
}

static int load_legacy_key(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';

    FILE *f = fopen(LEGACY_KEY_FILE, "rb");
    if (!f) return 0;

    if (!fgets(out, (int)out_len, f)) {
        fclose(f);
        out[0] = '\0';
        return 0;
    }
    fclose(f);
    strip_newline(out);
    return api_key_format_ok(out);
}

int setup_wizard_run(int force_prompt, char *io_api_key, size_t io_api_key_len)
{
    char api_key[160];
    api_key[0] = '\0';

    if (!force_prompt && setup_is_complete()) {
        char *dec = decrypt_api_key(ENCRYPTED_KEY_FILE);
        if (dec) {
            if (io_api_key && io_api_key_len > 0) {
                strncpy(io_api_key, dec, io_api_key_len - 1);
                io_api_key[io_api_key_len - 1] = '\0';
            }
            secure_zero(dec, strlen(dec));
            free(dec);
            return 1;
        }
    }

    if (!force_prompt && file_exists(ENCRYPTED_KEY_FILE)) {
        char *dec = decrypt_api_key(ENCRYPTED_KEY_FILE);
        if (dec && api_key_format_ok(dec)) {
            if (io_api_key && io_api_key_len > 0) {
                strncpy(io_api_key, dec, io_api_key_len - 1);
                io_api_key[io_api_key_len - 1] = '\0';
            }
            secure_zero(dec, strlen(dec));
            free(dec);
            setup_write_complete_marker();
            return 1;
        }
        if (dec) {
            secure_zero(dec, strlen(dec));
            free(dec);
        }
    }

    if (!force_prompt && load_legacy_key(api_key, sizeof(api_key))) {
        if (setup_validate_tmdb_api_key(api_key)) {
            if (encrypt_api_key(api_key, ENCRYPTED_KEY_FILE) == 0) {
                setup_write_complete_marker();
                if (io_api_key && io_api_key_len > 0) {
                    strncpy(io_api_key, api_key, io_api_key_len - 1);
                    io_api_key[io_api_key_len - 1] = '\0';
                }
                secure_zero(api_key, sizeof(api_key));
                return 1;
            }
        }
    }

    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════╗\n");
    printf("  ║         CineBook First-Run Setup Wizard                 ║\n");
    printf("  ╚══════════════════════════════════════════════════════════╝\n");
    printf("  TMDB API key setup is required for movie import features.\n");
    printf("  Get your free key: https://www.themoviedb.org/settings/api\n\n");

    for (int attempt = 1; attempt <= 5; attempt++) {
        char line[192];
        printf("  Enter TMDB API key (or press Enter to skip): ");
        if (!fgets(line, sizeof(line), stdin)) break;
        strip_newline(line);

        if (line[0] == '\0') {
            printf("  Setup skipped. CineBook will run without TMDB import.\n");
            setup_write_complete_marker();
            if (io_api_key && io_api_key_len > 0) io_api_key[0] = '\0';
            return 1;
        }

        if (!api_key_format_ok(line)) {
            printf("  Invalid key format. Expected 16-128 alphanumeric characters.\n");
            continue;
        }

        printf("  Validating key with TMDB...\n");
        if (!setup_validate_tmdb_api_key(line)) {
            printf("  Validation failed. Check key and internet connection.\n");
            continue;
        }

        if (encrypt_api_key(line, ENCRYPTED_KEY_FILE) != 0) {
            printf("  Failed to save encrypted API key. Try again.\n");
            continue;
        }

        setup_write_complete_marker();
        if (io_api_key && io_api_key_len > 0) {
            strncpy(io_api_key, line, io_api_key_len - 1);
            io_api_key[io_api_key_len - 1] = '\0';
        }

        printf("  Setup complete. API key stored securely.\n");
        return 1;
    }

    if (io_api_key && io_api_key_len > 0) io_api_key[0] = '\0';
    return 0;
}