#ifndef INDEX_H
#define INDEX_H

#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────*/
#define IDX_DIR       "data/idx/"
#define MAX_INDEXES   16
#define HASH_INIT_CAP 1024

/* ─────────────────────────────────────────────────────────────────────────────
 * IndexType
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    IDX_HASH   = 0,
    IDX_SORTED = 1
} IndexType;

/* ─────────────────────────────────────────────────────────────────────────────
 * HashEntry  — on-disk struct, raw binary, no padding surprises
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int32_t  key;
    uint32_t page_id;
    uint16_t slot_id;
    int16_t  is_deleted;   /* 1 = tombstone */
} HashEntry;

/* ─────────────────────────────────────────────────────────────────────────────
 * SortedIndexEntry  — on-disk struct
 * sort_key holds datetime string (shows) or city name (cities) for secondary
 * sort; empty string for indexes that don't need it.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int32_t  key;
    char     sort_key[32];
    uint32_t page_id;
    uint16_t slot_id;
    int16_t  is_deleted;   /* 1 = tombstone */
} SortedIndexEntry;

/* ─────────────────────────────────────────────────────────────────────────────
 * Index — one in-memory index
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    char              name[64];
    IndexType         type;
    /* hash fields */
    HashEntry        *hash_entries;
    int               hash_capacity;
    int               hash_count;       /* live + tombstoned, drives load factor */
    /* sorted fields */
    SortedIndexEntry *sorted_entries;
    int               sorted_count;
    int               sorted_capacity;
} Index;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Load all 8 .idx files from IDX_DIR; build in-memory structures. */
void index_init(void);

/* Persist all indexes to .idx files and free heap memory. */
void index_shutdown(void);

/* O(1) hash lookup by key. Returns 1 on found, 0 on miss. */
int index_lookup_hash(const char *index_name, int32_t key,
                      uint32_t *out_page_id, uint16_t *out_slot_id);

/* Binary-search range scan on a sorted index. Returns heap-allocated array
 * of matching live entries; caller must free(). *out_count set to result count.
 * Pass INT32_MIN/INT32_MAX to retrieve every live entry. */
SortedIndexEntry *index_range(const char *index_name,
                              int32_t key_lo, int32_t key_hi,
                              int *out_count);

/* Insert a new entry. sort_key may be NULL for hash indexes.
 * Returns 1 on success, 0 on failure (e.g. unknown index name). */
int index_insert(const char *index_name, int32_t key, const char *sort_key,
                 uint32_t page_id, uint16_t slot_id);

/* Tombstone the first live entry matching key.
 * Returns 1 if found and deleted, 0 if not found. */
int index_delete(const char *index_name, int32_t key);

#endif /* INDEX_H */