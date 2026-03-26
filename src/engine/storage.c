/*
 * storage.c — Page-based binary file I/O with a 64-entry LRU buffer pool.
 *
 * Layout recap (from schema.cat):
 *   Page = 4096 bytes
 *   Header (8 B): page_id(uint32) | record_count(uint16) | free_offset(uint16)
 *   Data region: bytes [8, 4095]  (4088 bytes)
 *   Slot N starts at: PAGE_HEADER_SIZE + N * record_size
 *
 * All .db files live in data/db/  (DB_DIR).
 * WAL integration: slot_write() calls wal_log() before touching the page.
 *
 * C11  —  no external dependencies beyond libc.
 */

#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ── WAL forward declaration (implemented in txn.c) ─────────────────────── */
extern int wal_log(const char  *table,
                   uint32_t     page_id,
                   uint16_t     slot_id,
                   const void  *before_image,
                   const void  *after_image,
                   size_t       record_size);

/* ── Internal file-handle cache ──────────────────────────────────────────── */
/* We keep one FILE* open per table so we don't fopen/fclose on every access. */
#define MAX_OPEN_FILES 32

typedef struct {
    char  table[TABLE_NAME_MAX];   /* table name, "" = free slot              */
    FILE *fp;                      /* open file handle (mode "r+b")           */
} FileEntry;

static FileEntry  s_files[MAX_OPEN_FILES];

/* ── Buffer pool ─────────────────────────────────────────────────────────── */
static PoolEntry  s_pool[POOL_SIZE];
static uint64_t   s_clock = 0;   /* monotonic access counter                 */

#define CAPACITY_WARN_TABLES 64
static char s_capacity_warned[CAPACITY_WARN_TABLES][TABLE_NAME_MAX];

static int capacity_warn_once(const char *table, const char *msg)
{
    if (!table || table[0] == '\0' || !msg) return 0;

    for (int i = 0; i < CAPACITY_WARN_TABLES; i++) {
        if (s_capacity_warned[i][0] != '\0' &&
            strcmp(s_capacity_warned[i], table) == 0) {
            return 0;
        }
    }

    for (int i = 0; i < CAPACITY_WARN_TABLES; i++) {
        if (s_capacity_warned[i][0] == '\0') {
            strncpy(s_capacity_warned[i], table, TABLE_NAME_MAX - 1);
            s_capacity_warned[i][TABLE_NAME_MAX - 1] = '\0';
            break;
        }
    }

    fprintf(stderr, "%s\n", msg);
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────────*/

/* Build the file path for a table's .db file into buf (must be ≥ 128 bytes). */
static void build_path(const char *table, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%s%s.db", DB_DIR, table);
}

/* Return the FILE* for table, opening lazily on first use.
 * Returns NULL if the file cannot be opened. */
static FILE *get_file(const char *table)
{
    /* Search existing entries */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (s_files[i].table[0] != '\0' &&
            strcmp(s_files[i].table, table) == 0) {
            return s_files[i].fp;
        }
    }
    /* Find a free slot */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (s_files[i].table[0] == '\0') {
            char path[256];
            build_path(table, path, sizeof(path));
            FILE *fp = fopen(path, "r+b");
            if (!fp) {
                /* Create file if missing, then reopen in update mode. */
                fp = fopen(path, "w+b");
                if (!fp) {
                    fprintf(stderr,
                            "[storage] ERROR: cannot open '%s' for table '%s': ",
                            path, table);
                    perror("");
                    return NULL;
                }
            }
            strncpy(s_files[i].table, table, TABLE_NAME_MAX - 1);
            s_files[i].table[TABLE_NAME_MAX - 1] = '\0';
            s_files[i].fp = fp;
            return fp;
        }
    }
    fprintf(stderr, "[storage] ERROR: too many open files (MAX_OPEN_FILES=%d)\n",
            MAX_OPEN_FILES);
    return NULL;
}

/* Flush one dirty pool entry to disk. Returns 0 on success, -1 on failure. */
static int flush_entry(PoolEntry *e)
{
    if (!e->is_dirty) return 0;

    FILE *fp = get_file(e->table);
    if (!fp) return -1;

    long offset = (long)e->page_id * (long)PAGE_SIZE;
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fprintf(stderr, "[storage] ERROR: fseek failed for table '%s' page %u\n",
                e->table, e->page_id);
        return -1;
    }
    size_t written = fwrite(e->page.raw, 1, PAGE_SIZE, fp);
    if (written != PAGE_SIZE) {
        fprintf(stderr,
                "[storage] ERROR: fwrite wrote %zu/%u bytes for table '%s' page %u\n",
                written, PAGE_SIZE, e->table, e->page_id);
        return -1;
    }
    if (fflush(fp) != 0) {
        fprintf(stderr, "[storage] ERROR: fflush failed for table '%s'\n",
                e->table);
        return -1;
    }
    e->is_dirty = 0;
    return 0;
}

/*
 * lru_evict — find the best pool entry to evict.
 *
 * Strategy:
 *   1. Among all non-free (occupied) entries, prefer the clean page with the
 *      lowest last_access (true LRU among clean pages).
 *   2. If no clean page exists, take the dirty page with the lowest
 *      last_access, flush it, then return it.
 *   3. If there is a free (empty) slot, return that immediately.
 * Returns NULL only if every dirty flush fails (catastrophic I/O error).
 */
static PoolEntry *lru_evict(void)
{
    /* Pass 0: return the first free slot instantly */
    for (int i = 0; i < POOL_SIZE; i++) {
        if (s_pool[i].table[0] == '\0') {
            return &s_pool[i];
        }
    }

    /* Pass 1: find LRU clean occupied entry */
    PoolEntry *best_clean = NULL;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!s_pool[i].is_dirty) {
            if (!best_clean ||
                s_pool[i].last_access < best_clean->last_access) {
                best_clean = &s_pool[i];
            }
        }
    }
    if (best_clean) {
        /* Mark as free without flushing (clean = already on disk) */
        best_clean->table[0] = '\0';
        best_clean->is_dirty = 0;
        return best_clean;
    }

    /* Pass 2: all pages are dirty — flush the LRU dirty one */
    PoolEntry *best_dirty = NULL;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!best_dirty ||
            s_pool[i].last_access < best_dirty->last_access) {
            best_dirty = &s_pool[i];
        }
    }
    if (best_dirty) {
        if (flush_entry(best_dirty) != 0) {
            fprintf(stderr,
                    "[storage] FATAL: cannot evict dirty page (flush failed)\n");
            return NULL;
        }
        best_dirty->table[0] = '\0';
        best_dirty->is_dirty = 0;
        return best_dirty;
    }

    return NULL; /* should never reach here */
}

/* Find pool entry for (table, page_id). Returns NULL if not cached. */
static PoolEntry *pool_find(const char *table, uint32_t page_id)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        if (s_pool[i].table[0] != '\0' &&
            s_pool[i].page_id == page_id &&
            strcmp(s_pool[i].table, table) == 0) {
            return &s_pool[i];
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API implementation
 * ───────────────────────────────────────────────────────────────────────────*/

/* Initialise the buffer pool to all-empty; no files opened yet. */
void storage_init(void)
{
    memset(s_pool,  0, sizeof(s_pool));
    memset(s_files, 0, sizeof(s_files));
    memset(s_capacity_warned, 0, sizeof(s_capacity_warned));
    s_clock = 0;
}

/* Flush all dirty pages then close every open file handle. */
void storage_shutdown(void)
{
    storage_flush_all();

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (s_files[i].table[0] != '\0' && s_files[i].fp) {
            fclose(s_files[i].fp);
            s_files[i].fp = NULL;
            s_files[i].table[0] = '\0';
        }
    }
}

/* Flush all dirty pages to disk without closing file handles. */
void storage_flush_all(void)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        if (s_pool[i].table[0] != '\0' && s_pool[i].is_dirty) {
            if (flush_entry(&s_pool[i]) != 0) {
                fprintf(stderr,
                        "[storage] ERROR: failed flushing table '%s' page %u\n",
                        s_pool[i].table, s_pool[i].page_id);
            }
        }
    }
}

/* Return pool-resident Page* for (table, page_id); loads from disk on miss.
 * Returns NULL if the file cannot be opened or read fails. */
Page *page_read(const char *table, uint32_t page_id)
{
    if (!table || table[0] == '\0') {
        fprintf(stderr, "[storage] ERROR: page_read called with invalid table name\n");
        return NULL;
    }

    /* Cache hit */
    PoolEntry *e = pool_find(table, page_id);
    if (e) {
        e->last_access = ++s_clock;
        return &e->page;
    }

    /* Cache miss — get a free or evictable slot */
    PoolEntry *slot = lru_evict();
    if (!slot) {
        fprintf(stderr,
                "[storage] ERROR: buffer pool full and all evictions failed\n");
        return NULL;
    }

    /* Open the file and read the page */
    FILE *fp = get_file(table);
    if (!fp) return NULL;

    long offset = (long)page_id * (long)PAGE_SIZE;
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fprintf(stderr,
                "[storage] ERROR: fseek to page %u of '%s' failed\n",
                page_id, table);
        return NULL;
    }

    size_t nread = fread(slot->page.raw, 1, PAGE_SIZE, fp);
    if (nread != PAGE_SIZE) {
        if (nread == 0 && feof(fp)) {
            /* Brand-new page at end of file — initialize empty page image. */
            memset(slot->page.raw, 0, PAGE_SIZE);
            PAGE_SET_ID(&slot->page, page_id);
            PAGE_SET_RECORD_COUNT(&slot->page, 0);
            PAGE_SET_FREE_OFFSET(&slot->page, 0);
        } else {
            /* Partial page read means corruption/truncation; reject page. */
            fprintf(stderr,
                    "[storage] ERROR: invalid page read %zu/%u bytes for '%s' page %u\n",
                    nread, PAGE_SIZE, table, page_id);
            clearerr(fp);
            return NULL;
        }
    }

    /* Populate pool slot */
    strncpy(slot->table, table, TABLE_NAME_MAX - 1);
    slot->table[TABLE_NAME_MAX - 1] = '\0';
    slot->page_id     = page_id;
    slot->is_dirty    = 0;
    slot->last_access = ++s_clock;

    return &slot->page;
}

/* Mark a page dirty (page data already modified by caller). */
void page_write(Page *page)
{
    if (!page) {
        fprintf(stderr, "[storage] ERROR: page_write called with NULL page\n");
        return;
    }

    /* Locate the pool entry that owns this Page* */
    for (int i = 0; i < POOL_SIZE; i++) {
        if (&s_pool[i].page == page) {
            s_pool[i].is_dirty    = 1;
            s_pool[i].last_access = ++s_clock;
            return;
        }
    }

    fprintf(stderr, "[storage] ERROR: page_write called with non-pooled page pointer\n");
}

/* Copy record_size bytes from slot slot_id within page into out. */
int slot_read(const Page *page, uint16_t slot_id,
              void *out, size_t record_size)
{
    if (!page || !out || record_size == 0) {
        fprintf(stderr, "[storage] ERROR: slot_read invalid arguments\n");
        return -1;
    }

    size_t offset = (size_t)slot_id * record_size;
    if (record_size > PAGE_DATA_SIZE || offset > PAGE_DATA_SIZE ||
        offset + record_size > PAGE_DATA_SIZE) {
        fprintf(stderr,
                "[storage] ERROR: slot_read out-of-bounds slot=%u rec_size=%zu\n",
                slot_id, record_size);
        return -1;
    }

    memcpy(out, PAGE_DATA((Page *)(uintptr_t)page) + offset, record_size);
    return 0;
}

/* Write record_size bytes from data into slot slot_id; logs WAL entry first,
 * then marks the page dirty. */
int slot_write(Page *page, uint16_t slot_id,
               const void *data, size_t record_size)
{
    if (!page || !data || record_size == 0) {
        fprintf(stderr, "[storage] ERROR: slot_write invalid arguments\n");
        return -1;
    }

    size_t offset = (size_t)slot_id * record_size;
    if (record_size > PAGE_DATA_SIZE || offset > PAGE_DATA_SIZE ||
        offset + record_size > PAGE_DATA_SIZE) {
        fprintf(stderr,
                "[storage] ERROR: slot_write out-of-bounds slot=%u rec_size=%zu\n",
                slot_id, record_size);
        return -1;
    }

    /* Find owning pool entry (required for WAL table binding). */
    PoolEntry *owner = NULL;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (&s_pool[i].page == page) {
            owner = &s_pool[i];
            break;
        }
    }
    if (!owner) {
        fprintf(stderr, "[storage] ERROR: slot_write called on non-pooled page\n");
        return -1;
    }

    /* Build full-page before/after images for WAL correctness. */
    uint8_t before_page[PAGE_SIZE];
    uint8_t after_page[PAGE_SIZE];
    memcpy(before_page, page->raw, PAGE_SIZE);
    memcpy(after_page, page->raw, PAGE_SIZE);
    memcpy(after_page + PAGE_HEADER_SIZE + offset, data, record_size);

    if (owner->table[0] == '\0') {
        fprintf(stderr,
                "[storage] WARN: slot_write owner table is empty for page %u slot %u; "
                "skipping WAL entry\n",
                PAGE_GET_ID(page), slot_id);
    } else {
        if (wal_log(owner->table, PAGE_GET_ID(page), slot_id,
                    before_page, after_page, record_size) != 0) {
            fprintf(stderr, "[storage] ERROR: WAL log failed for table '%s' page %u slot %u\n",
                    owner->table, PAGE_GET_ID(page), slot_id);
            return -1;
        }
    }

    /* Apply write only after WAL log succeeds. */
    memcpy(page->raw + PAGE_HEADER_SIZE + offset, data, record_size);
    page_write(page);
    return 0;
}

/*
 * page_alloc — return a pool-resident page that has room for at least one more
 * slot of record_size bytes, for the given table.
 *
 * Algorithm:
 *   1. Scan the pool for a page belonging to this table that has free space.
 *   2. If none, scan the file for the last page — load it and check.
 *   3. If still no room, append a fresh zero page to the file.
 */
Page *page_alloc(const char *table, size_t record_size)
{
    if (!table || table[0] == '\0' ||
        record_size == 0 || record_size > PAGE_DATA_SIZE) {
        fprintf(stderr, "[storage] ERROR: page_alloc invalid args table/record_size\n");
        return NULL;
    }

    uint16_t slots_per_page = (uint16_t)(PAGE_DATA_SIZE / record_size);
    if (slots_per_page == 0) {
        fprintf(stderr, "[storage] ERROR: page_alloc slots_per_page=0 for '%s'\n", table);
        return NULL;
    }

    /* Step 1: check pool for an already-loaded page of this table with space */
    for (int i = 0; i < POOL_SIZE; i++) {
        if (s_pool[i].table[0] != '\0' &&
            strcmp(s_pool[i].table, table) == 0) {
            uint16_t rc = PAGE_GET_RECORD_COUNT(&s_pool[i].page);
            if (rc < slots_per_page) {
                s_pool[i].last_access = ++s_clock;
                return &s_pool[i].page;
            }
        }
    }

    /* Step 2: check the last page on disk */
    int total = storage_get_page_count(table);
    if (total < 0) {
        return NULL;
    }

    uint64_t est_rows = ((uint64_t)POOL_SIZE * (uint64_t)PAGE_SIZE) / (uint64_t)record_size;
    if (est_rows == 0) est_rows = 1;
    uint64_t page_guard = est_rows / (uint64_t)slots_per_page;
    if ((est_rows % (uint64_t)slots_per_page) != 0) page_guard++;
    if (page_guard == 0) page_guard = 1;

    if ((uint64_t)total >= page_guard) {
        char warn[256];
        snprintf(warn, sizeof(warn),
                 "[storage] WARN: capacity guard hit for table '%s' (pages=%d, guard=%llu, est_rows=%llu).",
                 table, total,
                 (unsigned long long)page_guard,
                 (unsigned long long)est_rows);
        capacity_warn_once(table, warn);
        return NULL;
    }

    if (total > 0) {
        uint32_t last_pid = (uint32_t)(total - 1);
        Page *p = page_read(table, last_pid);
        if (p) {
            uint16_t rc = PAGE_GET_RECORD_COUNT(p);
            if (rc < slots_per_page) {
                return p;
            }
        }
    }

    /* Storage is finite by design. */
    if ((uint32_t)total >= STORAGE_MAX_PAGES_PER_TABLE) {
        char warn[256];
        snprintf(warn, sizeof(warn),
                 "[storage] WARN: capacity exhausted for table '%s' (max pages=%u).",
                 table, STORAGE_MAX_PAGES_PER_TABLE);
        capacity_warn_once(table, warn);
        return NULL;
    }

    /* Step 3: append a new page */
    uint32_t new_pid = (uint32_t)(total);   /* next page index                */

    FILE *fp = get_file(table);
    if (!fp) return NULL;

    /* Seek to new page position and write a blank page */
    long new_offset = (long)new_pid * (long)PAGE_SIZE;
    if (fseek(fp, new_offset, SEEK_SET) != 0) {
        fprintf(stderr,
                "[storage] ERROR: fseek for new page in '%s' failed\n", table);
        return NULL;
    }

    uint8_t blank[PAGE_SIZE];
    memset(blank, 0, PAGE_SIZE);
    /* Write header safely (avoid unaligned type-punning stores). */
    uint32_t pid_le = new_pid;
    uint16_t rc0 = 0;
    uint16_t free0 = 0;
    memcpy(blank + 0, &pid_le, sizeof(pid_le));
    memcpy(blank + 4, &rc0, sizeof(rc0));
    memcpy(blank + 6, &free0, sizeof(free0));

    size_t written = fwrite(blank, 1, PAGE_SIZE, fp);
    if (written != PAGE_SIZE) {
        fprintf(stderr,
                "[storage] ERROR: fwrite new page failed for '%s'\n", table);
        return NULL;
    }
    if (fflush(fp) != 0) {
        fprintf(stderr, "[storage] ERROR: fflush failed after new page for '%s'\n", table);
        return NULL;
    }

    /* Load the newly created page into the pool */
    Page *p = page_read(table, new_pid);
    return p;
}

/* Return the number of 4096-byte pages in table's .db file. */
int storage_get_page_count(const char *table)
{
    FILE *fp = get_file(table);
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long size = ftell(fp);
    if (size < 0) return 0;

    long page_size_l = (long)PAGE_SIZE;
    if (page_size_l <= 0) return 0;

    if ((size % page_size_l) != 0) {
        fprintf(stderr,
                "[storage] ERROR: table '%s' file size %ld is not page-aligned (%ld)\n",
                table, size, page_size_l);
        return 0;
    }

    long pages = size / page_size_l;
    if (pages < 0) return 0;
    if ((uint32_t)pages > STORAGE_MAX_PAGES_PER_TABLE) {
        fprintf(stderr,
                "[storage] ERROR: table '%s' exceeds max configured pages (%u)\n",
                table, STORAGE_MAX_PAGES_PER_TABLE);
        pages = (long)STORAGE_MAX_PAGES_PER_TABLE;
    }

    if (pages > INT_MAX) {
        return INT_MAX;
    }

    return (int)pages;
}

int storage_get_capacity(const char *table, size_t record_size,
                         int *max_slots, int *used_slots, int *free_slots)
{
    if (!table || table[0] == '\0' || !max_slots || !used_slots || !free_slots) {
        fprintf(stderr, "[storage] ERROR: storage_get_capacity invalid arguments\n");
        return -1;
    }
    if (record_size == 0 || record_size > PAGE_DATA_SIZE) {
        fprintf(stderr, "[storage] ERROR: storage_get_capacity invalid record_size=%zu\n",
                record_size);
        return -1;
    }

    int spp = (int)(PAGE_DATA_SIZE / record_size);
    if (spp <= 0) {
        fprintf(stderr, "[storage] ERROR: storage_get_capacity slots_per_page=0\n");
        return -1;
    }

    long long max_total = (long long)STORAGE_MAX_PAGES_PER_TABLE * (long long)spp;
    if (max_total > INT_MAX) max_total = INT_MAX;
    *max_slots = (int)max_total;

    int total_pages = storage_get_page_count(table);
    if (total_pages < 0) total_pages = 0;
    if ((uint32_t)total_pages > STORAGE_MAX_PAGES_PER_TABLE) {
        total_pages = (int)STORAGE_MAX_PAGES_PER_TABLE;
    }

    long long used = 0;
    uint8_t raw_slot[PAGE_DATA_SIZE];
    for (int pg = 0; pg < total_pages; pg++) {
        Page *p = page_read(table, (uint32_t)pg);
        if (!p) {
            fprintf(stderr,
                    "[storage] ERROR: storage_get_capacity failed reading table '%s' page %d\n",
                    table, pg);
            continue;
        }

        for (int sl = 0; sl < spp; sl++) {
            if (slot_read(p, (uint16_t)sl, raw_slot, record_size) != 0) {
                continue;
            }

            int is_empty = 1;
            size_t probe = record_size < 4 ? record_size : 4;
            for (size_t b = 0; b < probe; b++) {
                if (raw_slot[b] != 0) {
                    is_empty = 0;
                    break;
                }
            }
            if (is_empty) continue;

            used++;
            if (used > INT_MAX) {
                used = INT_MAX;
                break;
            }
        }

        if (used >= INT_MAX) break;
    }

    *used_slots = (int)used;
    *free_slots = *max_slots - *used_slots;
    if (*free_slots < 0) *free_slots = 0;
    return 0;
}
