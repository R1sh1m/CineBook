/*
 * query.c — Public RDBMS API implementation for CineBook
 *
 * This is the single gateway between all business-logic modules and the
 * storage/index/WAL engine. Nothing above this file touches storage.c,
 * record.c, schema.c, index.c, or txn.c directly.
 *
 * C11. No external libraries.
 */

/*
 * record_count invariant:
 * After every db_insert, record_count on the target page equals the number
 * of valid records stored.
 */

#include "query.h"
#include "storage.h"
#include "record.h"
#include "schema.h"
#include "index.h"
#include "txn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal index name table
 * Maps (table, column) pairs to their index file name.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    const char *table;
    const char *col;
    const char *index_name;
    IndexType   idx_type;
} IndexHint;

static const IndexHint INDEX_HINTS[] = {
    { "users",  "user_id",  "users_id",  IDX_HASH },
    { "users",  "email",    "users_email",  IDX_HASH },
    { "movies",      "movie_id",  "movies_id",              IDX_HASH   },
    { "shows",       "movie_id",  "shows_movieid",          IDX_SORTED },
    { "shows",       "show_id",   "shows_datetime",         IDX_SORTED },
    { "bookings",    "user_id",   "bookings_userid",        IDX_SORTED },
    { "theatres",    "city_id",   "theatres_cityid",        IDX_SORTED },
    { "seat_status", "show_id",   "seat_status_showid",     IDX_SORTED },
    { "cities",      "name",      "cities_name",            IDX_SORTED },
    { NULL, NULL, NULL, IDX_HASH }
};

/* Return the IndexHint for (table, col), or NULL if none. */
static const IndexHint *find_index_for_column(const char *table,
                                               const char *col)
{
    for (int i = 0; INDEX_HINTS[i].table != NULL; i++) {
        if (strcmp(INDEX_HINTS[i].table, table) == 0 &&
            strcmp(INDEX_HINTS[i].col,   col)   == 0) {
            return &INDEX_HINTS[i];
        }
    }
    return NULL;
}

/* PK max cache:
 *   pk_cache[i][0] = table hash (0 => empty slot)
 *   pk_cache[i][1] = last known max PK for that table */
static int32_t pk_cache[MAX_TABLES][2];

static int32_t table_hash(const char *table)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*table) {
        h ^= (uint8_t)(*table++);
        h *= 16777619u;
    }
    if (h == 0u) h = 1u; /* reserve 0 as "empty" */
    return (int32_t)h;
}

static int pk_cache_lookup(int32_t h, int32_t *max_pk_out)
{
    for (int i = 0; i < MAX_TABLES; i++) {
        if (pk_cache[i][0] == h) {
            *max_pk_out = pk_cache[i][1];
            return 1;
        }
    }
    return 0;
}

static void pk_cache_store(int32_t h, int32_t max_pk)
{
    int free_slot = -1;

    for (int i = 0; i < MAX_TABLES; i++) {
        if (pk_cache[i][0] == h) {
            pk_cache[i][1] = max_pk;
            return;
        }
        if (free_slot < 0 && pk_cache[i][0] == 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        pk_cache[free_slot][0] = h;
        pk_cache[free_slot][1] = max_pk;
        return;
    }

    /* Cache full: deterministic replacement. */
    int slot = (int)((uint32_t)h % (uint32_t)MAX_TABLES);
    pk_cache[slot][0] = h;
    pk_cache[slot][1] = max_pk;
}

static void pk_cache_invalidate(const char *table)
{
    int32_t h = table_hash(table);
    for (int i = 0; i < MAX_TABLES; i++) {
        if (pk_cache[i][0] == h) {
            pk_cache[i][0] = 0;
            pk_cache[i][1] = 0;
            return;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * WHERE clause evaluation
 * ───────────────────────────────────────────────────────────────────────────*/

/* Compare two values of the given column type; returns <0, 0, >0 like strcmp. */
static int compare_field(const Column *col, const void *a, const void *b)
{
    switch (col->type) {
        case COL_INT: {
            int32_t va = *(const int32_t *)a;
            int32_t vb = *(const int32_t *)b;
            return (va > vb) - (va < vb);
        }
        case COL_FLOAT: {
            float va = *(const float *)a;
            float vb = *(const float *)b;
            return (va > vb) - (va < vb);
        }
        case COL_CHAR:
        case COL_DATE:
            return strcmp((const char *)a, (const char *)b);
    }
    return 0;
}

/* Evaluate all WHERE clauses (AND) against a deserialized row.
 * Returns 1 if all pass, 0 if any fail. */
static int eval_where(void **row_fields, const Schema *s,
                      WhereClause *where, int count)
{
    for (int w = 0; w < count; w++) {
        /* find column index */
        int ci = -1;
        for (int c = 0; c < s->col_count; c++) {
            if (strcmp(s->columns[c].name, where[w].col_name) == 0) {
                ci = c;
                break;
            }
        }
        if (ci < 0) return 0;   /* unknown column → no match */
        if (row_fields[ci] == NULL) return 0;  /* null field → no match */

        int cmp = compare_field(&s->columns[ci], row_fields[ci], where[w].value);

        int pass = 0;
        switch (where[w].op) {
            case OP_EQ:  pass = (cmp == 0); break;
            case OP_NEQ: pass = (cmp != 0); break;
            case OP_GT:  pass = (cmp >  0); break;
            case OP_LT:  pass = (cmp <  0); break;
            case OP_GTE: pass = (cmp >= 0); break;
            case OP_LTE: pass = (cmp <= 0); break;
        }
        if (!pass) return 0;
    }
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Column projection helper
 * Build a projected ResultSet row from a full deserialized row.
 * If cols == NULL / col_count == 0, copy all columns.
 * ───────────────────────────────────────────────────────────────────────────*/

/* Allocate a single copy of a field value from a deserialized row. */
static void *dup_field(const Column *col, void *src)
{
    size_t sz;
    switch (col->type) {
        case COL_INT:   sz = sizeof(int32_t);       break;
        case COL_FLOAT: sz = sizeof(float);          break;
        case COL_DATE:  sz = TYPE_DATE_SIZE;         break;
        case COL_CHAR:  sz = (size_t)col->char_len + 1; break;
        default:        sz = (size_t)col->size;      break;
    }
    void *copy = malloc(sz);
    if (copy) memcpy(copy, src, sz);
    return copy;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Full table scan
 * Iterates every page/slot. Applies WHERE filter. Appends matching rows to
 * a growing ResultSet.
 * ───────────────────────────────────────────────────────────────────────────*/
static ResultSet *table_scan(const char *table,
                             WhereClause *where, int where_count,
                             char **cols,        int col_count)
{
    Schema *s = get_schema(table);
    if (!s) return NULL;

    int total_pages = storage_get_page_count(table);

    /* Determine projected column indices */
    int proj[MAX_COLUMNS];
    int proj_count = 0;

    if (col_count == 0 || cols == NULL) {
        for (int i = 0; i < s->col_count; i++) proj[i] = i;
        proj_count = s->col_count;
    } else {
        for (int i = 0; i < col_count; i++) {
            for (int j = 0; j < s->col_count; j++) {
                if (strcmp(s->columns[j].name, cols[i]) == 0) {
                    proj[proj_count++] = j;
                    break;
                }
            }
        }
    }

    /* Allocate result */
    ResultSet *rs = calloc(1, sizeof(ResultSet));
    if (!rs) return NULL;
    rs->col_count = proj_count;
    rs->row_count = 0;
    rs->rows      = NULL;

    int capacity = 64;
    rs->rows = malloc((size_t)capacity * sizeof(void **));
    if (!rs->rows) { free(rs); return NULL; }

    int slots_per_page = (int)(PAGE_DATA_SIZE / (size_t)s->record_size);
    if (slots_per_page < 1) slots_per_page = 1;

    uint8_t raw_slot[PAGE_SIZE];   /* scratch buffer, large enough for any record */
    void   *fields[MAX_COLUMNS];
    memset(fields, 0, sizeof(fields));

    for (int pg = 0; pg < total_pages; pg++) {
        Page *page = page_read(table, (uint32_t)pg);
        if (!page) continue;

        /* Scan every possible slot, not just record_count. Historically
         * record_count was not maintained correctly; live rows are detected by
         * non-zero slot content (all-zero means deleted/empty). */
        int limit = slots_per_page;

        for (int sl = 0; sl < limit; sl++) {
            if (slot_read(page, (uint16_t)sl, raw_slot, (size_t)s->record_size) != 0) {
                continue;
            }

            /* skip deleted/empty slot (first 4 bytes all zero) */
            int all_zero = 1;
            for (int b = 0; b < 4 && b < s->record_size; b++) {
                if (raw_slot[b] != 0) { all_zero = 0; break; }
            }
            if (all_zero) continue;

            record_deserialize(s, raw_slot, fields);

            if (!eval_where(fields, s, where, where_count)) {
                /* free deserialized fields */
                for (int c = 0; c < s->col_count; c++) free(fields[c]);
                continue;
            }

            /* grow result array if needed */
            if (rs->row_count == capacity) {
                capacity *= 2;
                void ***tmp = realloc(rs->rows,
                                      (size_t)capacity * sizeof(void **));
                if (!tmp) {
                    for (int c = 0; c < s->col_count; c++) free(fields[c]);
                    /* return partial result — still useful */
                    goto done;
                }
                rs->rows = tmp;
            }

            /* project and store */
            void **row = malloc((size_t)proj_count * sizeof(void *));
            for (int pi = 0; pi < proj_count; pi++) {
                row[pi] = fields[proj[pi]];
                fields[proj[pi]] = NULL;   /* ownership transferred */
            }
            /* free any non-projected fields */
            for (int c = 0; c < s->col_count; c++) {
                if (fields[c]) { free(fields[c]); fields[c] = NULL; }
            }

            rs->rows[rs->row_count++] = row;
        }
    }
done:
    return rs;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * next_pk_for_table
 * Scans PK hash index for the table to find the current maximum PK, returns
 * max + 1. Falls back to full scan on index miss.
 * ───────────────────────────────────────────────────────────────────────────*/
static int next_pk_for_table(const char *table)
{
    Schema *s = get_schema(table);
    if (!s) return 1;

    /* Find the PK column */
    int pk_col = -1;
    for (int c = 0; c < s->col_count; c++) {
        if (s->columns[c].is_pk) { pk_col = c; break; }
    }
    if (pk_col < 0) return 1;

    /* Fast path: cache hit */
    int32_t h = table_hash(table);
    int32_t cached_max = 0;
    if (pk_cache_lookup(h, &cached_max)) {
        return (int)cached_max + 1;
    }

    char idx_name[128];
    snprintf(idx_name, sizeof(idx_name), "%s_id", table);

    /* Determine index type from INDEX_HINTS before calling index_range(). */
    int has_pk_idx_hint = 0;
    IndexType pk_idx_type = IDX_HASH;
    for (int i = 0; INDEX_HINTS[i].table != NULL; i++) {
        if (strcmp(INDEX_HINTS[i].table, table) == 0 &&
            strcmp(INDEX_HINTS[i].index_name, idx_name) == 0) {
            has_pk_idx_hint = 1;
            pk_idx_type = INDEX_HINTS[i].idx_type;
            break;
        }
    }

    if (has_pk_idx_hint && pk_idx_type == IDX_SORTED) {
        int count = 0;
        SortedIndexEntry *entries = index_range(idx_name,
                                                INT32_MIN, INT32_MAX, &count);
        if (entries) {
            int32_t max_key = 0;
            for (int i = 0; i < count; i++) {
                if (!entries[i].is_deleted && entries[i].key > max_key) {
                    max_key = entries[i].key;
                }
            }
            free(entries);
            pk_cache_store(h, max_key);
            return (int)max_key + 1;
        }
    }

    /* Full scan fallback: scan all slots_per_page slots like table_scan() */
    int32_t max_pk = 0;
    int total_pages = storage_get_page_count(table);
    int slots_per_page = (int)(PAGE_DATA_SIZE / (size_t)s->record_size);
    if (slots_per_page < 1) slots_per_page = 1;

    uint8_t raw[PAGE_SIZE];
    void   *fields[MAX_COLUMNS];
    memset(fields, 0, sizeof(fields));

    for (int pg = 0; pg < total_pages; pg++) {
        Page *page = page_read(table, (uint32_t)pg);
        if (!page) continue;

        int limit = slots_per_page;
        for (int sl = 0; sl < limit; sl++) {
            if (slot_read(page, (uint16_t)sl, raw, (size_t)s->record_size) != 0) {
                continue;
            }

            int all_zero = 1;
            for (int b = 0; b < 4 && b < s->record_size; b++) {
                if (raw[b] != 0) { all_zero = 0; break; }
            }
            if (all_zero) continue;

            record_deserialize(s, raw, fields);
            int32_t pk_val = *(int32_t *)fields[pk_col];
            if (pk_val > max_pk) max_pk = pk_val;

            for (int c = 0; c < s->col_count; c++) {
                free(fields[c]);
                fields[c] = NULL;
            }
        }
    }

    pk_cache_store(h, max_pk);
    return (int)max_pk + 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_select
 * ───────────────────────────────────────────────────────────────────────────*/
ResultSet *db_select(const char *table,
                     WhereClause *where, int where_count,
                     char **cols,        int col_count)
{
    Schema *s = get_schema(table);
    if (!s) return NULL;

    /* If we have at least one WHERE clause, try to use an index for the
     * first clause to narrow the scan. */
    if (where_count > 0) {
        const IndexHint *hint = find_index_for_column(table,
                                                       where[0].col_name);
        if (hint) {
            /* Only accelerate EQ queries through the index */
            if (where[0].op == OP_EQ) {

                /* Determine projected column indices */
                int proj[MAX_COLUMNS];
                int proj_count = 0;
                if (col_count == 0 || cols == NULL) {
                    for (int i = 0; i < s->col_count; i++) proj[i] = i;
                    proj_count = s->col_count;
                } else {
                    for (int i = 0; i < col_count; i++) {
                        for (int j = 0; j < s->col_count; j++) {
                            if (strcmp(s->columns[j].name, cols[i]) == 0) {
                                proj[proj_count++] = j;
                                break;
                            }
                        }
                    }
                }

                ResultSet *rs = calloc(1, sizeof(ResultSet));
                rs->col_count = proj_count;
                int capacity  = 16;
                rs->rows      = malloc((size_t)capacity * sizeof(void **));
                uint8_t raw[PAGE_SIZE];
                void   *fields[MAX_COLUMNS];
                memset(fields, 0, sizeof(fields));

                int32_t key = *(const int32_t *)where[0].value;

                if (hint->idx_type == IDX_HASH) {
                    uint32_t pg_id; uint16_t sl_id;
                    if (index_lookup_hash(hint->index_name, key,
                                          &pg_id, &sl_id)) {
                        Page *page = page_read(table, pg_id);
                        if (page && slot_read(page, sl_id, raw, (size_t)s->record_size) == 0) {
                            record_deserialize(s, raw, fields);
                            if (eval_where(fields, s, where, where_count)) {
                                void **row = malloc((size_t)proj_count * sizeof(void *));
                                for (int pi = 0; pi < proj_count; pi++) {
                                    row[pi] = fields[proj[pi]];
                                    fields[proj[pi]] = NULL;
                                }
                                for (int c = 0; c < s->col_count; c++) {
                                    if (fields[c]) free(fields[c]);
                                }
                                rs->rows[rs->row_count++] = row;
                            } else {
                                for (int c = 0; c < s->col_count; c++) free(fields[c]);
                            }
                        }
                    }
                } else { /* IDX_SORTED */
                    int cnt = 0;
                    SortedIndexEntry *entries = index_range(hint->index_name,
                                                            key, key, &cnt);
                    for (int i = 0; i < cnt; i++) {
                        if (entries[i].is_deleted) continue;
                        Page *page = page_read(table, entries[i].page_id);
                        if (!page) continue;
                        if (slot_read(page, entries[i].slot_id, raw,
                                      (size_t)s->record_size) != 0) {
                            continue;
                        }
                        record_deserialize(s, raw, fields);
                        if (eval_where(fields, s, where, where_count)) {
                            if (rs->row_count == capacity) {
                                int new_capacity = capacity * 2;
                                void ***tmp_rows = realloc(rs->rows,
                                    (size_t)new_capacity * sizeof(void **));
                                if (!tmp_rows) {
                                    for (int c = 0; c < s->col_count; c++) {
                                        free(fields[c]);
                                        fields[c] = NULL;
                                    }
                                    free(entries);
                                    result_set_free(rs);
                                    return NULL;
                                }
                                rs->rows = tmp_rows;
                                capacity = new_capacity;
                            }
                            void **row = malloc((size_t)proj_count * sizeof(void *));
                            for (int pi = 0; pi < proj_count; pi++) {
                                row[pi] = fields[proj[pi]];
                                fields[proj[pi]] = NULL;
                            }
                            for (int c = 0; c < s->col_count; c++) {
                                if (fields[c]) free(fields[c]);
                            }
                            rs->rows[rs->row_count++] = row;
                        } else {
                            for (int c = 0; c < s->col_count; c++) free(fields[c]);
                        }
                    }
                    free(entries);
                }
                return rs;
            }
        }
    }

    /* No usable index — fall back to full table scan */
    return table_scan(table, where, where_count, cols, col_count);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_insert
 * ───────────────────────────────────────────────────────────────────────────*/
int db_insert(const char *table, void **field_values)
{
    Schema *s = get_schema(table);
    if (!s || !field_values) return -1;
    if (s->record_size <= 0 || (size_t)s->record_size > PAGE_DATA_SIZE) {
        fprintf(stderr, "[query] db_insert: invalid record_size for table '%s'\n", table);
        return -1;
    }

    /* Find PK column index */
    int pk_col = -1;
    for (int c = 0; c < s->col_count; c++) {
        if (s->columns[c].is_pk) { pk_col = c; break; }
    }

    /* Auto-increment PK */
    int new_pk = next_pk_for_table(table);
    int32_t pk_val = (int32_t)new_pk;
    if (pk_col >= 0) {
        field_values[pk_col] = &pk_val;
    }

    /* Serialise */
    uint8_t raw[PAGE_SIZE];
    memset(raw, 0, sizeof(raw));
    record_serialize(s, field_values, raw);

    /* Find a page with space */
    Page *page = page_alloc(table, (size_t)s->record_size);
    if (!page) return -1;

    uint32_t pg_id = PAGE_GET_ID(page);
    uint16_t sl_id = PAGE_GET_RECORD_COUNT(page);

    size_t slots_per_page = PAGE_DATA_SIZE / (size_t)s->record_size;
    if ((size_t)sl_id >= slots_per_page) {
        fprintf(stderr, "[query] db_insert: table '%s' page %u is full\n", table, pg_id);
        return -1;
    }

    wal_begin_nested();
    if (slot_write(page, sl_id, raw, (size_t)s->record_size) != 0) {
        wal_rollback();
        return -1;
    }
    PAGE_SET_RECORD_COUNT(page, (uint16_t)(sl_id + 1));
    /* page is already dirty from slot_write — no extra page_write needed */
    wal_commit_nested();

    /* Keep PK cache hot after successful insert */
    pk_cache_store(table_hash(table), pk_val);

    /* Update all indexes that cover this table */
    for (int i = 0; INDEX_HINTS[i].table != NULL; i++) {
        if (strcmp(INDEX_HINTS[i].table, table) != 0) continue;
        /* find the column index */
        for (int c = 0; c < s->col_count; c++) {
            if (strcmp(s->columns[c].name, INDEX_HINTS[i].col) == 0) {
                int32_t idx_key = 0;
                char    sort_key[32] = {0};
                if (s->columns[c].type == COL_INT ||
                    s->columns[c].type == COL_FLOAT) {
                    idx_key = *(int32_t *)field_values[c];
                } else {
                    /* For CHAR/DATE sort indexes, key = pk_val */
                    idx_key = pk_val;
                    if (field_values[c]) {
                        strncpy(sort_key, (char *)field_values[c],
                                sizeof(sort_key) - 1);
                    }
                }
                index_insert(INDEX_HINTS[i].index_name, idx_key,
                             sort_key[0] ? sort_key : NULL,
                             pg_id, sl_id);
                break;
            }
        }
    }

    return new_pk;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_update
 * ───────────────────────────────────────────────────────────────────────────*/
int db_update(const char *table,
              WhereClause *where, int where_count,
              const char *col_name, void *new_val)
{
    Schema *s = get_schema(table);
    if (!s) return 0;

    /* Find target column */
    int target_col = -1;
    for (int c = 0; c < s->col_count; c++) {
        if (strcmp(s->columns[c].name, col_name) == 0) {
            target_col = c; break;
        }
    }
    if (target_col < 0) return 0;

    int updated = 0;
    int total_pages = storage_get_page_count(table);
    int slots_per_page = (int)(PAGE_DATA_SIZE / (size_t)s->record_size);
    if (slots_per_page < 1) slots_per_page = 1;

    uint8_t raw[PAGE_SIZE];
    void   *fields[MAX_COLUMNS];
    memset(fields, 0, sizeof(fields));

    wal_begin_nested();

    for (int pg = 0; pg < total_pages; pg++) {
        Page *page = page_read(table, (uint32_t)pg);
        if (!page) continue;

        int limit = slots_per_page;

        for (int sl = 0; sl < limit; sl++) {
            if (slot_read(page, (uint16_t)sl, raw, (size_t)s->record_size) != 0) {
                continue;
            }

            int all_zero = 1;
            for (int b = 0; b < 4; b++) {
                if (raw[b]) { all_zero = 0; break; }
            }
            if (all_zero) continue;

            record_deserialize(s, raw, fields);

            if (!eval_where(fields, s, where, where_count)) {
                for (int c = 0; c < s->col_count; c++) free(fields[c]);
                continue;
            }

            /* Patch the field in-memory */
            free(fields[target_col]);
            const Column *tc = &s->columns[target_col];
            fields[target_col] = dup_field(tc, new_val);

            /* Re-serialise and write back */
            uint8_t new_raw[PAGE_SIZE];
            memset(new_raw, 0, sizeof(new_raw));
            record_serialize(s, fields, new_raw);

            if (slot_write(page, (uint16_t)sl, new_raw, (size_t)s->record_size) != 0) {
                for (int c = 0; c < s->col_count; c++) free(fields[c]);
                wal_rollback();
                return updated;
            }
            /* db_update rewrites an existing slot in place; record_count
             * tracks inserts and must not change here. */

            /* Update index if the updated column is indexed */
            const IndexHint *hint = find_index_for_column(table, col_name);
            if (hint) {
                /* old key: re-read from original raw */
                void *old_fields[MAX_COLUMNS];
                record_deserialize(s, raw, old_fields);
                int32_t old_key = *(int32_t *)old_fields[target_col];
                int32_t new_key = *(int32_t *)new_val;
                index_delete(hint->index_name, old_key);
                char sort_key[32] = {0};
                if (tc->type == COL_CHAR || tc->type == COL_DATE) {
                    new_key = old_key;  /* pk stays the key for string cols */
                    strncpy(sort_key, (char *)new_val, sizeof(sort_key)-1);
                }
                index_insert(hint->index_name, new_key,
                             sort_key[0] ? sort_key : NULL,
                             PAGE_GET_ID(page), (uint16_t)sl);
                for (int c = 0; c < s->col_count; c++) free(old_fields[c]);
            }

            for (int c = 0; c < s->col_count; c++) free(fields[c]);
            updated++;
        }
    }
    wal_commit_nested();
    return updated;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_delete
 * ───────────────────────────────────────────────────────────────────────────*/
int db_delete(const char *table,
              WhereClause *where, int where_count)
{
    Schema *s = get_schema(table);
    if (!s) return 0;

    /* Any delete can reduce max PK visibility; invalidate cache. */
    pk_cache_invalidate(table);

    int deleted = 0;
    int total_pages = storage_get_page_count(table);
    int slots_per_page = (int)(PAGE_DATA_SIZE / (size_t)s->record_size);
    if (slots_per_page < 1) slots_per_page = 1;

    uint8_t raw[PAGE_SIZE];
    uint8_t zero_raw[PAGE_SIZE];
    memset(zero_raw, 0, sizeof(zero_raw));
    void  *fields[MAX_COLUMNS]; 
    
    wal_begin_nested();
    for (int pg = 0; pg < total_pages; pg++) {
        Page *page = page_read(table, (uint32_t)pg);
        if (!page) continue;

        int limit = slots_per_page;

        for (int sl = 0; sl < limit; sl++) {
            if (slot_read(page, (uint16_t)sl, raw, (size_t)s->record_size) != 0) {
                continue;
            }

            int all_zero = 1;
            for (int b = 0; b < 4; b++) {
                if (raw[b]) { all_zero = 0; break; }
            }
            if (all_zero) continue;

            record_deserialize(s, raw, fields);

            if (!eval_where(fields, s, where, where_count)) {
                for (int c = 0; c < s->col_count; c++) free(fields[c]);
                continue;
            }

            /* Remove from all indexes */
            for (int i = 0; INDEX_HINTS[i].table != NULL; i++) {
                if (strcmp(INDEX_HINTS[i].table, table) != 0) continue;
                for (int c = 0; c < s->col_count; c++) {
                    if (strcmp(s->columns[c].name, INDEX_HINTS[i].col) == 0) {
                        int32_t key = *(int32_t *)fields[c];
                        index_delete(INDEX_HINTS[i].index_name, key);
                        break;
                    }
                }
            }

            for (int c = 0; c < s->col_count; c++) free(fields[c]);

            if (slot_write(page, (uint16_t)sl, zero_raw, (size_t)s->record_size) != 0) {
                wal_rollback();
                return deleted;
            }
            /* db_delete tombstones slot bytes; do not decrement record_count.
             * table_scan detects deleted rows via all-zero slot content. */

            deleted++;
        }
    }
    wal_commit_nested();
    return deleted;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_join
 * ───────────────────────────────────────────────────────────────────────────*/
ResultSet *db_join(const char *t1,    const char *t2,
                   const char *col1,  const char *col2,
                   WhereClause *post_filter, int filter_count)
{
    Schema *s1 = get_schema(t1);
    Schema *s2 = get_schema(t2);
    if (!s1 || !s2) return NULL;

    int ci1 = -1, ci2 = -1;
    for (int c = 0; c < s1->col_count; c++) {
        if (strcmp(s1->columns[c].name, col1) == 0) { ci1 = c; break; }
    }
    for (int c = 0; c < s2->col_count; c++) {
        if (strcmp(s2->columns[c].name, col2) == 0) { ci2 = c; break; }
    }
    if (ci1 < 0 || ci2 < 0) return NULL;

    int combined_cols = s1->col_count + s2->col_count;

    ResultSet *rs = calloc(1, sizeof(ResultSet));
    rs->col_count = combined_cols;
    int capacity  = 64;
    rs->rows      = malloc((size_t)capacity * sizeof(void **));

    /* Build a fake combined schema for post-filter evaluation.
     * We put t1 columns first, then t2 columns.
     * post_filter col_names must match their respective table columns. */
    Schema combined_schema;
    memset(&combined_schema, 0, sizeof(combined_schema));
    snprintf(combined_schema.table_name, 64, "%s_%s", t1, t2);
    combined_schema.col_count = combined_cols;
    for (int c = 0; c < s1->col_count; c++)
        combined_schema.columns[c] = s1->columns[c];
    for (int c = 0; c < s2->col_count; c++)
        combined_schema.columns[s1->col_count + c] = s2->columns[c];

    /* Check if t2.col2 has an index */
    const IndexHint *hint2 = find_index_for_column(t2, col2);

    int total_pages1 = storage_get_page_count(t1);
    int spp1 = (int)(PAGE_DATA_SIZE / (size_t)s1->record_size);
    if (spp1 < 1) spp1 = 1;

    uint8_t raw1[PAGE_SIZE], raw2[PAGE_SIZE];
    void *f1[MAX_COLUMNS], *f2[MAX_COLUMNS];
    memset(f1, 0, sizeof(f1));   
    memset(f2, 0, sizeof(f2));

    for (int pg1 = 0; pg1 < total_pages1; pg1++) {
        Page *page1 = page_read(t1, (uint32_t)pg1);
        if (!page1) continue;

        int rc1    = PAGE_GET_RECORD_COUNT(page1);
        int limit1 = rc1 < spp1 ? rc1 : spp1;

        for (int sl1 = 0; sl1 < limit1; sl1++) {
            if (slot_read(page1, (uint16_t)sl1, raw1, (size_t)s1->record_size) != 0) {
                continue;
            }
            int az = 1;
            for (int b = 0; b < 4; b++) { if (raw1[b]) { az=0; break; } }
            if (az) continue;

            record_deserialize(s1, raw1, f1);
            int32_t join_key = *(int32_t *)f1[ci1];

            /* Inner loop: find matching t2 rows */
            if (hint2 && s2->columns[ci2].type == COL_INT) {
                /* use index on t2.col2 */
                uint32_t pg2_id; uint16_t sl2_id;
                if (hint2->idx_type == IDX_HASH) {
                    if (index_lookup_hash(hint2->index_name, join_key,
                                          &pg2_id, &sl2_id)) {
                        Page *page2 = page_read(t2, pg2_id);
                        if (page2 && slot_read(page2, sl2_id, raw2, (size_t)s2->record_size) == 0) {
                            record_deserialize(s2, raw2, f2);
                            /* assemble combined row */
                            void **combined = malloc((size_t)combined_cols * sizeof(void*));
                            for (int c = 0; c < s1->col_count; c++)
                                combined[c] = f1[c];
                            for (int c = 0; c < s2->col_count; c++)
                                combined[s1->col_count + c] = f2[c];
                            if (!eval_where(combined, &combined_schema,
                                            post_filter, filter_count)) {
                                for (int c = 0; c < combined_cols; c++) free(combined[c]);
                                free(combined);
                            } else {
                            if (rs->row_count == capacity) {
                                int new_capacity = capacity * 2;
                                void ***tmp_rows = realloc(rs->rows,
                                    (size_t)new_capacity * sizeof(void**));
                                if (!tmp_rows) {
                                    for (int c = 0; c < combined_cols; c++) free(combined[c]);
                                    free(combined);
                                    for (int c = 0; c < s1->col_count; c++) { free(f1[c]); f1[c] = NULL; }
                                    result_set_free(rs);
                                    return NULL;
                                }
                                rs->rows = tmp_rows;
                                capacity = new_capacity;
                            }
                                rs->rows[rs->row_count++] = combined;
                            }
                        } else {
                            for (int c = 0; c < s2->col_count; c++) free(f2[c]);
                        }
                    }
                } else { /* sorted index */
                    int cnt = 0;
                    SortedIndexEntry *entries = index_range(hint2->index_name,
                                                            join_key, join_key, &cnt);
                    for (int i = 0; i < cnt; i++) {
                        if (entries[i].is_deleted) continue;
                        Page *page2 = page_read(t2, entries[i].page_id);
                        if (!page2) continue;
                        if (slot_read(page2, entries[i].slot_id, raw2,
                                      (size_t)s2->record_size) != 0) {
                            continue;
                        }
                        record_deserialize(s2, raw2, f2);
                        void **combined = malloc((size_t)combined_cols * sizeof(void*));
                        for (int c = 0; c < s1->col_count; c++)
                            combined[c] = dup_field(&s1->columns[c], f1[c]);
                        for (int c = 0; c < s2->col_count; c++)
                            combined[s1->col_count + c] = f2[c];
                        if (!eval_where(combined, &combined_schema,
                                        post_filter, filter_count)) {
                            for (int c = 0; c < combined_cols; c++) free(combined[c]);
                            free(combined);
                        } else {
                            if (rs->row_count == capacity) {
                                int new_capacity = capacity * 2;
                                void ***tmp_rows = realloc(rs->rows,
                                    (size_t)new_capacity * sizeof(void**));
                                if (!tmp_rows) {
                                    for (int c = 0; c < combined_cols; c++) free(combined[c]);
                                    free(combined);
                                    free(entries);
                                    for (int c = 0; c < s1->col_count; c++) { free(f1[c]); f1[c] = NULL; }
                                    result_set_free(rs);
                                    return NULL;
                                }
                                rs->rows = tmp_rows;
                                capacity = new_capacity;
                            }
                            rs->rows[rs->row_count++] = combined;
                        }
                    }
                    free(entries);
                    /* f1 fields are dup-copied per inner hit; free originals now */
                    for (int c = 0; c < s1->col_count; c++) { free(f1[c]); f1[c]=NULL; }
                    goto next_outer1;
                }
            } else {
                /* Full scan of t2 */
                int total_pages2 = storage_get_page_count(t2);
                int spp2 = (int)(PAGE_DATA_SIZE / (size_t)s2->record_size);
                if (spp2 < 1) spp2 = 1;

                for (int pg2 = 0; pg2 < total_pages2; pg2++) {
                    Page *page2 = page_read(t2, (uint32_t)pg2);
                    if (!page2) continue;
                    int rc2    = PAGE_GET_RECORD_COUNT(page2);
                    int limit2 = rc2 < spp2 ? rc2 : spp2;
                    for (int sl2 = 0; sl2 < limit2; sl2++) {
                        if (slot_read(page2, (uint16_t)sl2, raw2, (size_t)s2->record_size) != 0) {
                            continue;
                        }
                        int az2 = 1;
                        for (int b=0;b<4;b++){if(raw2[b]){az2=0;break;}}
                        if (az2) continue;
                        record_deserialize(s2, raw2, f2);
                        int32_t k2 = *(int32_t *)f2[ci2];
                        if (k2 != join_key) {
                            for (int c=0;c<s2->col_count;c++) free(f2[c]);
                            continue;
                        }
                        void **combined = malloc((size_t)combined_cols * sizeof(void*));
                        for (int c = 0; c < s1->col_count; c++)
                            combined[c] = dup_field(&s1->columns[c], f1[c]);
                        for (int c = 0; c < s2->col_count; c++)
                            combined[s1->col_count + c] = f2[c];
                        if (!eval_where(combined, &combined_schema,
                                        post_filter, filter_count)) {
                            for (int c = 0; c < combined_cols; c++) free(combined[c]);
                            free(combined);
                        } else {
                            if (rs->row_count == capacity) {
                                int new_capacity = capacity * 2;
                                void ***tmp_rows = realloc(rs->rows,
                                    (size_t)new_capacity * sizeof(void**));
                                if (!tmp_rows) {
                                    for (int c = 0; c < combined_cols; c++) free(combined[c]);
                                    free(combined);
                                    for (int c = 0; c < s1->col_count; c++) { free(f1[c]); f1[c] = NULL; }
                                    result_set_free(rs);
                                    return NULL;
                                }
                                rs->rows = tmp_rows;
                                capacity = new_capacity;
                            }
                            rs->rows[rs->row_count++] = combined;
                        }
                    }
                }
            }
            for (int c = 0; c < s1->col_count; c++) free(f1[c]);
next_outer1:;
        }
    }
    return rs;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * db_count
 * ───────────────────────────────────────────────────────────────────────────*/
int db_count(const char *table,
             WhereClause *where, int where_count)
{
    /* If WHERE is empty and there is a PK index, we could count index entries.
     * For correctness we always do a filtered scan. */
    ResultSet *rs = db_select(table, where, where_count, NULL, 0);
    if (!rs) return 0;
    int count = rs->row_count;
    result_set_free(rs);
    return count;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * DDL helpers
 * ───────────────────────────────────────────────────────────────────────────*/

int db_create_table(const char *table,
                    Column *cols, int col_count,
                    int record_size)
{
    if (g_schema_count >= MAX_TABLES) return 0;

    Schema *s = &g_schemas[g_schema_count];
    memset(s, 0, sizeof(Schema));
    strncpy(s->table_name, table, 63);
    s->col_count   = col_count;
    s->record_size = record_size;
    for (int c = 0; c < col_count; c++) s->columns[c] = cols[c];
    g_schema_count++;

    /* Create empty .db file */
    char path[256];
    snprintf(path, sizeof(path), DB_DIR "%s.db", table);
    FILE *f = fopen(path, "wb");
    if (f) fclose(f);

    return 1;
}

int db_drop_table(const char *table)
{
    pk_cache_invalidate(table);

    int found = -1;
    for (int i = 0; i < g_schema_count; i++) {
        if (strcmp(g_schemas[i].table_name, table) == 0) {
            found = i; break;
        }
    }
    if (found < 0) return 0;

    /* Shift schemas down */
    for (int i = found; i < g_schema_count - 1; i++)
        g_schemas[i] = g_schemas[i + 1];
    g_schema_count--;

    /* Delete .db file */
    char path[256];
    snprintf(path, sizeof(path), DB_DIR "%s.db", table);
    remove(path);

    return 1;
}

int db_add_column(const char *table, Column *col)
{
    (void)table;
    (void)col;
    fprintf(stderr, "[query] db_add_column is disabled for safety (requires migration)\n");
    return 0;
}

int db_drop_column(const char *table, const char *col_name)
{
    (void)table;
    (void)col_name;
    fprintf(stderr, "[query] db_drop_column is disabled for safety (would corrupt existing records)\n");
    return 0;
}

#if 0
int db_drop_column_unsafe_disabled(const char *table, const char *col_name)
{
    Schema *s = get_schema(table);
    if (!s) return 0;

    int found = -1;
    for (int c = 0; c < s->col_count; c++) {
        if (strcmp(s->columns[c].name, col_name) == 0) {
            found = c; break;
        }
    }
    if (found < 0) return 0;

    int removed_size = s->columns[found].size;

    for (int c = found; c < s->col_count - 1; c++) {
        s->columns[c] = s->columns[c + 1];
        s->columns[c].offset -= removed_size;
    }
    s->col_count--;
    s->record_size -= removed_size;
    return 1;
}
#endif
