#ifndef TXN_H
#define TXN_H

#include <stdint.h>
#include <stddef.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────*/
#define WAL_PATH      "data/wal.log"
#define WAL_PAGE_SIZE 4096u   /* full page image stored per entry */

/* ─────────────────────────────────────────────────────────────────────────────
 * WALEntry
 * One record in wal.log. before_image and after_image are full 4096-byte page
 * snapshots. record_size is stored for reference; crash recovery restores the
 * full page image (not just the slot) to guarantee correctness.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    uint32_t txn_id;
    char     table[64];
    uint32_t page_id;
    uint16_t slot_id;
    uint16_t record_size;
    uint8_t  before_image[WAL_PAGE_SIZE];
    uint8_t  after_image[WAL_PAGE_SIZE];
    uint32_t checksum;
    int32_t  committed;    /* 0 = in-flight / uncommitted, 1 = committed */
} WALEntry;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Open wal.log, run crash recovery on uncommitted entries, set g_current_txn_id. */
void txn_init(void);

/* Commit any active transaction, flush and close wal.log. */
void txn_shutdown(void);

/* Begin a new transaction; auto-commits any currently active transaction first. */
void wal_begin(void);

/* Return 1 if a transaction is currently active, else 0. */
int wal_is_active(void);

/* Nested begin for internal db_* usage:
 * if a txn is already active, no-op; otherwise start one. */
void wal_begin_nested(void);

/* Append a WALEntry with full page before/after images to wal.log.
 * Returns 0 on success, -1 on failure. */
int  wal_log(const char *table, uint32_t page_id, uint16_t slot_id,
             const void *before_image, const void *after_image,
             size_t record_size);

/* Mark every entry for the current txn_id as committed in wal.log. */
void wal_commit(void);

/* Nested commit for internal db_* usage:
 * commits only if wal_begin_nested() started the txn. */
void wal_commit_nested(void);

/* Restore before_images for all uncommitted entries of the current txn,
 * then prune those entries from wal.log. */
void wal_rollback(void);

#endif /* TXN_H */