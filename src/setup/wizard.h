#ifndef SETUP_WIZARD_H
#define SETUP_WIZARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ensures first-run setup is complete.
 * force_prompt=0 -> only prompt when setup marker/API key is missing.
 * force_prompt=1 -> always prompt user to update key.
 * io_api_key receives active key on success when buffer is provided.
 * Returns 1 when setup is complete (with or without key), 0 when skipped/cancelled. */
int setup_wizard_run(int force_prompt, char *io_api_key, size_t io_api_key_len);

/* Low-level TMDB API key remote validation via /configuration endpoint.
 * Returns 1 if valid (HTTP 200), 0 otherwise. */
int setup_validate_tmdb_api_key(const char *api_key);

/* Marker helpers */
int setup_is_complete(void);
int setup_write_complete_marker(void);

#ifdef __cplusplus
}
#endif

#endif /* SETUP_WIZARD_H */