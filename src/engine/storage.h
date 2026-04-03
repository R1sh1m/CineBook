#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────*/

#define PAGE_SIZE        4096u          /* total bytes per page               */
#define PAGE_HEADER_SIZE 8u             /* page_id(4) + record_count(2) + free_offset(2) */
#define PAGE_DATA_SIZE   (PAGE_SIZE - PAGE_HEADER_SIZE)  /* 4088 usable bytes */
#define POOL_SIZE        64             /* number of pages in the buffer pool  */
#define TABLE_NAME_MAX   64             /* max length of a table name string   */
#define DB_DIR           "data/db/"     /* directory for all .db files         */

/* Hard per-table safety limit so storage is explicitly finite. */
#define STORAGE_MAX_PAGES_PER_TABLE 1024u
#define STORAGE_MAX_ABSOLUTE_PAGES  2621440u  /* ~10GB at 4KB/page */
#define STORAGE_AUTO_EXPAND_THRESHOLD_PCT 82

/* ─────────────────────────────────────────────────────────────────────────────
 * Page struct
 * Raw 4096-byte page held in the buffer pool.
 * Header fields are stored in little-endian byte order matching the file.
 * Callers access header via the accessor macros below; the data[] array is
 * the raw page data region starting at byte offset 8.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    uint8_t raw[PAGE_SIZE];   /* complete on-disk image including 8-byte header */
} Page;

/* Inline accessors — memcpy-based to avoid unaligned/aliasing UB. */
static inline uint32_t PAGE_GET_ID(const Page *p)
{
    uint32_t v = 0;
    if (!p) return 0;
    memcpy(&v, p->raw + 0, sizeof(v));
    return v;
}

static inline uint16_t PAGE_GET_RECORD_COUNT(const Page *p)
{
    uint16_t v = 0;
    if (!p) return 0;
    memcpy(&v, p->raw + 4, sizeof(v));
    return v;
}

static inline uint16_t PAGE_GET_FREE_OFFSET(const Page *p)
{
    uint16_t v = 0;
    if (!p) return 0;
    memcpy(&v, p->raw + 6, sizeof(v));
    return v;
}

static inline void PAGE_SET_ID(Page *p, uint32_t v)
{
    if (!p) return;
    memcpy(p->raw + 0, &v, sizeof(v));
}

static inline void PAGE_SET_RECORD_COUNT(Page *p, uint16_t v)
{
    if (!p) return;
    memcpy(p->raw + 4, &v, sizeof(v));
}

static inline void PAGE_SET_FREE_OFFSET(Page *p, uint16_t v)
{
    if (!p) return;
    memcpy(p->raw + 6, &v, sizeof(v));
}

/* Pointer to the data region (byte 8 onward) */
#define PAGE_DATA(p)  ((uint8_t *)((p)->raw + PAGE_HEADER_SIZE))

/* ─────────────────────────────────────────────────────────────────────────────
 * PoolEntry struct
 * One slot in the 64-entry buffer pool.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    Page        page;                    /* the actual page data (4096 bytes)  */
    char        table[TABLE_NAME_MAX];   /* owning table name, "" = free slot  */
    uint32_t    page_id;                 /* page number within the table file  */
    int         is_dirty;                /* 1 = modified since last flush      */
    uint64_t    last_access;             /* monotonic counter, higher = newer  */
} PoolEntry;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Initialise the buffer pool; no files opened (lazy open). */
void   storage_init(void);

/* Flush all dirty pages to disk and close every open file handle. */
void   storage_shutdown(void);

/* Flush all dirty pages to disk without closing file handles. */
void   storage_flush_all(void);

/* Return pool-resident Page* for (table, page_id); loads from disk on miss.
 * Returns NULL if the file cannot be opened. */
Page  *page_read(const char *table, uint32_t page_id);

/* Mark a page dirty; wal_log() must already have been called by the caller
 * (slot_write handles this automatically). */
void   page_write(Page *page);

/* Copy record_size bytes from slot slot_id inside page into out buffer.
 * Returns 0 on success, -1 on bounds/argument failure. */
int    slot_read(const Page *page, uint16_t slot_id,
                 void *out, size_t record_size);

/* Write record_size bytes from data into slot slot_id; logs WAL first using
 * full-page before/after images, then marks page dirty.
 * Returns 0 on success, -1 on bounds/argument/WAL failure. */
int    slot_write(Page *page, uint16_t slot_id,
                  const void *data, size_t record_size);

/* Return a pool-resident page that has room for at least one more slot of
 * record_size bytes; allocates a new page on disk if needed.
 * Returns NULL on fatal I/O or pool-full error. */
Page  *page_alloc(const char *table, size_t record_size);

/* Return the number of 4096-byte pages in table's .db file (file_size/4096).
 * Returns 0 if the file does not exist or cannot be opened. */
int    storage_get_page_count(const char *table);

/* Capacity query for finite storage planning.
 * max_slots  = theoretical max records under STORAGE_MAX_PAGES_PER_TABLE
 * used_slots = currently consumed slots (sum of page record_count, clamped)
 * free_slots = max_slots - used_slots (never negative)
 * Returns 0 on success, -1 on argument/error failure. */
int    storage_get_capacity(const char *table, size_t record_size,
                            int *max_slots, int *used_slots, int *free_slots);

/* ─────────────────────────────────────────────────────────────────────────────
 * Storage Capacity Monitoring System
 * ───────────────────────────────────────────────────────────────────────────*/

/* Global storage capacity limit: 100,000 pages = ~400MB at 4KB per page */
#define MAX_STORAGE_PAGES 100000

/* Storage statistics structure */
typedef struct {
    int   total_pages_allocated;  /* Total pages allocated across all tables */
    int   pages_in_use;            /* Pages containing data (in pool or disk) */
    int   max_pages_allowed;       /* Maximum pages allowed (MAX_STORAGE_PAGES) */
    long  bytes_total;             /* Total bytes allocated (pages * PAGE_SIZE) */
    long  bytes_used;              /* Bytes used by actual data */
    float fragmentation_ratio;     /* Overall fragmentation (0.0 - 1.0) */
    int   table_count;             /* Number of active tables */
} StorageStats;

/* Per-table statistics structure */
typedef struct {
    char  table_name[TABLE_NAME_MAX];
    int   row_count;               /* Total rows in table */
    long  bytes_used;              /* Actual data bytes used */
    long  bytes_allocated;         /* Total bytes allocated (pages * PAGE_SIZE) */
    float fragmentation_ratio;     /* Fragmentation for this table (0.0 - 1.0) */
    int   page_count;              /* Number of pages allocated */
} TableStats;

/* Get current storage statistics across all tables.
 * Caller must free the returned pointer when done.
 * Returns NULL on failure. */
StorageStats *get_storage_stats(void);

/* Fill caller-provided summary struct with current storage statistics.
 * Returns 0 on success, -1 on failure. */
int storage_get_summary(StorageStats *out_stats);

/* Calculate storage capacity percentage (0-100).
 * Returns percentage of storage used, or -1 on error. */
int calculate_capacity_percentage(void);

/* Get storage statistics for a specific table.
 * Returns bytes used by the table's .db file, or -1 on error. */
long get_table_size_bytes(const char *table_name);

/* Get the number of rows (records) in a table.
 * Returns row count, or -1 on error. */
int get_table_row_count(const char *table_name);

/* Calculate fragmentation ratio for a table.
 * Fragmentation = (empty slots / total slots).
 * Returns 0.0 (no fragmentation) to 1.0 (high fragmentation), or -1.0 on error. */
float get_table_fragmentation(const char *table_name);

/* Get detailed statistics for a specific table.
 * Caller must provide a valid TableStats pointer to populate.
 * Returns 0 on success, -1 on failure. */
int get_table_stats(const char *table_name, TableStats *stats);

/* Dynamic storage capacity controls */
unsigned int storage_get_max_pages_per_table(void);
int storage_set_auto_expand(int enabled);
int storage_get_auto_expand(void);

#endif /* STORAGE_H */
