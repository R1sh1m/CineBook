#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Global index store
 * ───────────────────────────────────────────────────────────────────────────*/
static Index g_indexes[MAX_INDEXES];
static int   g_index_count = 0;

/* ─────────────────────────────────────────────────────────────────────────────
 * Catalog of all indexes: name → type
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct { const char *name; IndexType type; } IdxSpec;

static const IdxSpec k_index_specs[] = {
    { "users_id",          IDX_HASH   },
    { "movies_id",            IDX_HASH   },
    { "seat_status_showid",   IDX_SORTED },
    { "shows_datetime",       IDX_SORTED },
    { "shows_movieid",        IDX_SORTED },
    { "bookings_userid",      IDX_SORTED },
    { "theatres_cityid",      IDX_SORTED },
    { "cities_name",          IDX_SORTED },
};
#define NUM_SPECS (int)(sizeof(k_index_specs) / sizeof(k_index_specs[0]))

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers — declarations
 * ───────────────────────────────────────────────────────────────────────────*/
static Index           *find_index(const char *name);
static uint32_t         hash_probe(const HashEntry *tbl, int cap, int32_t key);
static void             hash_rehash(Index *idx);
static int              sorted_lower_bound(const SortedIndexEntry *arr,
                                           int count, int32_t key);
static void             load_hash_index(Index *idx);
static void             load_sorted_index(Index *idx);
static void             save_hash_index(const Index *idx);
static void             save_sorted_index(const Index *idx);

/* ─────────────────────────────────────────────────────────────────────────────
 * index_init — load all .idx files
 * ───────────────────────────────────────────────────────────────────────────*/
void index_init(void)
{
    g_index_count = 0;
    memset(g_indexes, 0, sizeof(g_indexes));

    for (int i = 0; i < NUM_SPECS; i++) {
        Index *idx = &g_indexes[g_index_count++];
        strncpy(idx->name, k_index_specs[i].name, sizeof(idx->name) - 1);
        idx->type = k_index_specs[i].type;

        if (idx->type == IDX_HASH) {
            load_hash_index(idx);
        } else {
            load_sorted_index(idx);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * index_shutdown — persist and free
 * ───────────────────────────────────────────────────────────────────────────*/
void index_shutdown(void)
{
    for (int i = 0; i < g_index_count; i++) {
        Index *idx = &g_indexes[i];
        if (idx->type == IDX_HASH) {
            save_hash_index(idx);
            free(idx->hash_entries);
            idx->hash_entries = NULL;
        } else {
            save_sorted_index(idx);
            free(idx->sorted_entries);
            idx->sorted_entries = NULL;
        }
    }
    g_index_count = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * index_lookup_hash — O(1) lookup via Knuth multiplicative hash + linear probe
 * ───────────────────────────────────────────────────────────────────────────*/
int index_lookup_hash(const char *index_name, int32_t key,
                      uint32_t *out_page_id, uint16_t *out_slot_id)
{
    Index *idx = find_index(index_name);
    if (!idx || idx->type != IDX_HASH || !idx->hash_entries) return 0;

    uint32_t pos = hash_probe(idx->hash_entries, idx->hash_capacity, key);
    /* linear probe until we find the key or an empty slot */
    for (int i = 0; i < idx->hash_capacity; i++) {
        uint32_t p = (pos + (uint32_t)i) % (uint32_t)idx->hash_capacity;
        HashEntry *e = &idx->hash_entries[p];
        /* empty slot (never written) — key is not present */
        if (e->key == 0 && e->page_id == 0 && e->slot_id == 0
                && e->is_deleted == 0) {
            return 0;
        }
        if (e->key == key && !e->is_deleted) {
            *out_page_id = e->page_id;
            *out_slot_id = e->slot_id;
            return 1;
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * index_range — binary search lower bound, scan forward
 * ───────────────────────────────────────────────────────────────────────────*/
SortedIndexEntry *index_range(const char *index_name,
                              int32_t key_lo, int32_t key_hi,
                              int *out_count)
{
    *out_count = 0;
    Index *idx = find_index(index_name);
    if (!idx || idx->type != IDX_SORTED || !idx->sorted_entries) return NULL;

    int lo = sorted_lower_bound(idx->sorted_entries, idx->sorted_count, key_lo);

    /* count matching live entries first */
    int match = 0;
    for (int i = lo; i < idx->sorted_count; i++) {
        SortedIndexEntry *e = &idx->sorted_entries[i];
        if (e->key > key_hi) break;
        if (!e->is_deleted) match++;
    }
    if (match == 0) return NULL;

    SortedIndexEntry *result =
        malloc((size_t)match * sizeof(SortedIndexEntry));
    if (!result) return NULL;

    int out = 0;
    for (int i = lo; i < idx->sorted_count && out < match; i++) {
        SortedIndexEntry *e = &idx->sorted_entries[i];
        if (e->key > key_hi) break;
        if (!e->is_deleted) result[out++] = *e;
    }
    *out_count = out;
    return result;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * index_insert — insert into hash or sorted index
 * ───────────────────────────────────────────────────────────────────────────*/
int index_insert(const char *index_name, int32_t key, const char *sort_key,
                 uint32_t page_id, uint16_t slot_id)
{
    Index *idx = find_index(index_name);
    if (!idx) return 0;

    if (idx->type == IDX_HASH) {
        /* rehash if load factor > 0.7 */
        if (idx->hash_count * 10 > idx->hash_capacity * 7)
            hash_rehash(idx);

        uint32_t pos = hash_probe(idx->hash_entries, idx->hash_capacity, key);
        for (int i = 0; i < idx->hash_capacity; i++) {
            uint32_t p = (pos + (uint32_t)i) % (uint32_t)idx->hash_capacity;
            HashEntry *e = &idx->hash_entries[p];
            /* empty slot or tombstone — place here */
            if (e->is_deleted || (e->key == 0 && e->page_id == 0)) {
                e->key        = key;
                e->page_id    = page_id;
                e->slot_id    = slot_id;
                e->is_deleted = 0;
                idx->hash_count++;
                return 1;
            }
        }
        return 0; /* table full — should not happen after rehash */

    } else {
        /* sorted insert: grow array if needed */
        if (idx->sorted_count >= idx->sorted_capacity) {
            int new_cap = idx->sorted_capacity ? idx->sorted_capacity * 2 : 64;
            SortedIndexEntry *tmp = realloc(idx->sorted_entries,
                                            (size_t)new_cap * sizeof(SortedIndexEntry));
            if (!tmp) return 0;
            idx->sorted_entries  = tmp;
            idx->sorted_capacity = new_cap;
        }

        int pos = sorted_lower_bound(idx->sorted_entries,
                                     idx->sorted_count, key);
        /* shift entries right */
        memmove(&idx->sorted_entries[pos + 1],
                &idx->sorted_entries[pos],
                (size_t)(idx->sorted_count - pos) * sizeof(SortedIndexEntry));

        SortedIndexEntry *e = &idx->sorted_entries[pos];
        e->key        = key;
        e->page_id    = page_id;
        e->slot_id    = slot_id;
        e->is_deleted = 0;
        if (sort_key)
            strncpy(e->sort_key, sort_key, sizeof(e->sort_key) - 1);
        else
            e->sort_key[0] = '\0';

        idx->sorted_count++;
        return 1;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * index_delete — tombstone first matching live entry
 * ───────────────────────────────────────────────────────────────────────────*/
int index_delete(const char *index_name, int32_t key)
{
    Index *idx = find_index(index_name);
    if (!idx) return 0;

    if (idx->type == IDX_HASH) {
        uint32_t pos = hash_probe(idx->hash_entries, idx->hash_capacity, key);
        for (int i = 0; i < idx->hash_capacity; i++) {
            uint32_t p = (pos + (uint32_t)i) % (uint32_t)idx->hash_capacity;
            HashEntry *e = &idx->hash_entries[p];
            if (e->key == 0 && e->page_id == 0 && !e->is_deleted) return 0;
            if (e->key == key && !e->is_deleted) {
                e->is_deleted = 1;
                return 1;
            }
        }
        return 0;

    } else {
        int lo = sorted_lower_bound(idx->sorted_entries,
                                    idx->sorted_count, key);
        for (int i = lo; i < idx->sorted_count; i++) {
            SortedIndexEntry *e = &idx->sorted_entries[i];
            if (e->key > key) break;
            if (e->key == key && !e->is_deleted) {
                e->is_deleted = 1;
                return 1;
            }
        }
        return 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Static helpers
 * ───────────────────────────────────────────────────────────────────────────*/

/* Linear search through the global store by name. */
static Index *find_index(const char *name)
{
    for (int i = 0; i < g_index_count; i++) {
        if (strcmp(g_indexes[i].name, name) == 0)
            return &g_indexes[i];
    }
    return NULL;
}

/* Knuth multiplicative hash → initial probe position. */
static uint32_t hash_probe(const HashEntry *tbl, int cap, int32_t key)
{
    (void)tbl;
    return ((uint32_t)key * 2654435761u) % (uint32_t)cap;
}

/* Double the hash table capacity and reinsert all live entries. */
static void hash_rehash(Index *idx)
{
    int new_cap = idx->hash_capacity * 2;
    HashEntry *new_tbl = calloc((size_t)new_cap, sizeof(HashEntry));
    if (!new_tbl) return;

    int live_count = 0;
    for (int i = 0; i < idx->hash_capacity; i++) {
        HashEntry *e = &idx->hash_entries[i];
        if (e->is_deleted || (e->key == 0 && e->page_id == 0)) continue;

        uint32_t pos = ((uint32_t)e->key * 2654435761u) % (uint32_t)new_cap;
        for (int j = 0; j < new_cap; j++) {
            uint32_t p = (pos + (uint32_t)j) % (uint32_t)new_cap;
            if (new_tbl[p].page_id == 0 && !new_tbl[p].is_deleted) {
                new_tbl[p] = *e;
                live_count++;
                break;
            }
        }
    }

    free(idx->hash_entries);
    idx->hash_entries  = new_tbl;
    idx->hash_capacity = new_cap;
    idx->hash_count = live_count;  /* tombstones dropped during rehash */
}

/* Binary search: first position where sorted_entries[pos].key >= key. */
static int sorted_lower_bound(const SortedIndexEntry *arr, int count, int32_t key)
{
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid].key < key) lo = mid + 1;
        else                    hi = mid;
    }
    return lo;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * File I/O helpers
 * ───────────────────────────────────────────────────────────────────────────*/

static void load_hash_index(Index *idx)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.idx", IDX_DIR, idx->name);

    idx->hash_capacity = HASH_INIT_CAP;
    idx->hash_entries  = calloc((size_t)HASH_INIT_CAP, sizeof(HashEntry));
    idx->hash_count    = 0;
    if (!idx->hash_entries) return;

    FILE *f = fopen(path, "rb");
    if (!f) return;   /* empty index on first run — not an error */

    HashEntry e;
    while (fread(&e, sizeof(HashEntry), 1, f) == 1) {
        if (e.is_deleted) continue;
        /* reinsert into open-address table */
        if (idx->hash_count * 10 > idx->hash_capacity * 7)
            hash_rehash(idx);
        uint32_t pos = hash_probe(idx->hash_entries, idx->hash_capacity, e.key);
        for (int i = 0; i < idx->hash_capacity; i++) {
            uint32_t p = (pos + (uint32_t)i) % (uint32_t)idx->hash_capacity;
            HashEntry *slot = &idx->hash_entries[p];
            if (slot->page_id == 0 && !slot->is_deleted) {
                *slot = e;
                idx->hash_count++;
                break;
            }
        }
    }
    fclose(f);
}

static void load_sorted_index(Index *idx)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.idx", IDX_DIR, idx->name);

    idx->sorted_capacity = 128;
    idx->sorted_count    = 0;
    idx->sorted_entries  = malloc((size_t)128 * sizeof(SortedIndexEntry));
    if (!idx->sorted_entries) return;

    FILE *f = fopen(path, "rb");
    if (!f) return;

    SortedIndexEntry e;
    while (fread(&e, sizeof(SortedIndexEntry), 1, f) == 1) {
        /* grow buffer if needed */
        if (idx->sorted_count >= idx->sorted_capacity) {
            int nc = idx->sorted_capacity * 2;
            SortedIndexEntry *tmp = realloc(idx->sorted_entries,
                                            (size_t)nc * sizeof(SortedIndexEntry));
            if (!tmp) break;
            idx->sorted_entries  = tmp;
            idx->sorted_capacity = nc;
        }
        idx->sorted_entries[idx->sorted_count++] = e;
    }
    fclose(f);
    /* file is written in sorted order by seed.c — no re-sort needed */
}

static void save_hash_index(const Index *idx)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.idx", IDX_DIR, idx->name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < idx->hash_capacity; i++) {
        const HashEntry *e = &idx->hash_entries[i];
        /* skip truly empty slots (never written) */
        if (e->key == 0 && e->page_id == 0 && e->slot_id == 0
                && e->is_deleted == 0) continue;
        fwrite(e, sizeof(HashEntry), 1, f);
    }
    fclose(f);
}

static void save_sorted_index(const Index *idx)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.idx", IDX_DIR, idx->name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(idx->sorted_entries, sizeof(SortedIndexEntry),
           (size_t)idx->sorted_count, f);
    fclose(f);
}