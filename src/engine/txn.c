/*
 * txn.c — Write-Ahead Log (WAL) for CineBook
 *
 * Guarantees atomicity for every page write in the system.
 * Before any page slot is modified, wal_log() appends a WALEntry containing
 * the full 4096-byte before_image and after_image to wal.log. On startup,
 * txn_init() scans for uncommitted entries and restores before_images to disk.
 *
 * C11  |  No external dependencies beyond <stdio.h> / <string.h> / <stdlib.h>
 */

#include "txn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers — forward declarations
 * ───────────────────────────────────────────────────────────────────────────*/
static uint32_t compute_checksum(const WALEntry *e);
static void     recover_wal(void);
static void     rewrite_wal_committed_only(WALEntry *entries, int count);
static int      restore_page(const char *table, uint32_t page_id,
                              const uint8_t *image);
static int      wal_has_seed_magic(FILE *f);
static int      wal_truncate_to_empty(void);
static void     reset_recovery_summary(void);

/* ─────────────────────────────────────────────────────────────────────────────
 * Global state
 * ───────────────────────────────────────────────────────────────────────────*/
static uint32_t g_current_txn_id = 0;  /* incremented on each wal_begin()    */
static FILE    *g_wal_file        = NULL;
static int      g_txn_active      = 0;  /* 1 if a transaction is in progress  */
static int      g_nested_started_count = 0; /* nested begins that started txns */
static WALRecoverySummary g_last_recovery_summary = {0};

/* ─────────────────────────────────────────────────────────────────────────────
 * compute_checksum
 * XOR all bytes in before_image and after_image together into a uint32_t.
 * ───────────────────────────────────────────────────────────────────────────*/
static uint32_t compute_checksum(const WALEntry *e)
{
    uint32_t csum = 0;
    const uint8_t *b = e->before_image;
    const uint8_t *a = e->after_image;

    for (size_t i = 0; i < WAL_PAGE_SIZE; i++) {
        csum ^= (uint32_t)b[i];
        csum ^= (uint32_t)a[i];
    }
    return csum;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * restore_page
 * Overwrite the 4096-byte page at offset (page_id * 4096) in
 * data/db/<table>.db with `image`. Returns 1 on success, 0 on I/O error.
 * ───────────────────────────────────────────────────────────────────────────*/
static int restore_page(const char *table, uint32_t page_id,
                        const uint8_t *image)
{
    char path[256];
    snprintf(path, sizeof(path), "data/db/%s.db", table);

    FILE *f = fopen(path, "r+b");
    if (!f) {
        /* File may not exist yet (table was created in the aborted txn).
         * Nothing to restore — the create itself will be absent from the
         * committed log, so we just skip. */
        return 1;
    }

    long offset = (long)page_id * (long)WAL_PAGE_SIZE;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fprintf(stderr, "[txn] restore_page: fseek failed for %s page %u\n",
                table, page_id);
        fclose(f);
        return 0;
    }

    size_t written = fwrite(image, 1, WAL_PAGE_SIZE, f);
    if (written != WAL_PAGE_SIZE) {
        fprintf(stderr, "[txn] restore_page: short write for %s page %u\n",
                table, page_id);
        fclose(f);
        return 0;
    }

    if (fflush(f) != 0) {
        fprintf(stderr, "[txn] restore_page: fflush failed for %s page %u\n",
                table, page_id);
        fclose(f);
        return 0;
    }

    if (fclose(f) != 0) {
        fprintf(stderr, "[txn] restore_page: fclose failed for %s page %u\n",
                table, page_id);
        return 0;
    }

    return 1;
}

static int wal_has_seed_magic(FILE *f)
{
    if (!f) {
        return 0;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        return 0;
    }

    char magic[4];
    size_t n = fread(magic, 1, sizeof(magic), f);
    if (n != sizeof(magic)) {
        return 0;
    }

    return (memcmp(magic, "CWAL", 4) == 0) ? 1 : 0;
}

static int wal_truncate_to_empty(void)
{
    if (g_wal_file) {
        fclose(g_wal_file);
        g_wal_file = NULL;
    }

    FILE *f = fopen(WAL_PATH, "wb");
    if (!f) {
        fprintf(stderr,
                "[txn] wal_truncate_to_empty: cannot truncate %s: %s\n",
                WAL_PATH, strerror(errno));
        return 0;
    }
    fclose(f);

    g_wal_file = fopen(WAL_PATH, "r+b");
    if (!g_wal_file) {
        fprintf(stderr,
                "[txn] wal_truncate_to_empty: cannot reopen %s in r+b mode\n",
                WAL_PATH);
        return 0;
    }

    return 1;
}

static void reset_recovery_summary(void)
{
    memset(&g_last_recovery_summary, 0, sizeof(g_last_recovery_summary));
    g_last_recovery_summary.next_txn_id = g_current_txn_id;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * rewrite_wal_committed_only
 * Truncates wal.log to contain only the committed entries in `entries`.
 * Called after crash recovery to prune rolled-back entries from the log.
 * ───────────────────────────────────────────────────────────────────────────*/
static void rewrite_wal_committed_only(WALEntry *entries, int count)
{
    /* Close current handle, reopen for write (truncate). */
    if (g_wal_file) {
        fclose(g_wal_file);
        g_wal_file = NULL;
    }

    FILE *f = fopen(WAL_PATH, "wb");
    if (!f) {
        fprintf(stderr, "[txn] rewrite_wal_committed_only: cannot open %s for write\n",
                WAL_PATH);
        /* Re-open in append mode so the rest of the system can continue. */
        g_wal_file = fopen(WAL_PATH, "a+b");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (entries[i].committed == 1) {
            size_t w = fwrite(&entries[i], sizeof(WALEntry), 1, f);
            if (w != 1) {
                fprintf(stderr, "[txn] rewrite_wal_committed_only: fwrite failed at entry %d\n", i);
                break;
            }
        }
    }
    if (fflush(f) != 0) {
        fprintf(stderr, "[txn] rewrite_wal_committed_only: fflush failed\n");
    }
    fclose(f);

    /* Re-open in read/write mode for normal operation.
     * IMPORTANT: append mode breaks in-place commit flag updates. */
    g_wal_file = fopen(WAL_PATH, "r+b");
    if (!g_wal_file) {
        fprintf(stderr, "[txn] rewrite_wal_committed_only: cannot reopen %s\n",
                WAL_PATH);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * recover_wal
 * Reads every WALEntry from the log. For each uncommitted entry, restores
 * the before_image to disk. Then rewrites the log keeping only committed
 * entries. Sets g_current_txn_id to max committed txn_id + 1.
 * ───────────────────────────────────────────────────────────────────────────*/
static void recover_wal(void)
{
    reset_recovery_summary();

    /* Guard: old seeder WAL format begins with 8-byte "CWAL" header.
     * This runtime WAL parser expects raw WALEntry records only. */
    if (wal_has_seed_magic(g_wal_file)) {
        g_last_recovery_summary.legacy_header_detected = 1;
        fprintf(stderr,
                "[txn] recover_wal: detected legacy seed WAL header 'CWAL'; "
                "treating WAL as empty\n");
        if (fseek(g_wal_file, 8, SEEK_SET) != 0) {
            /* Non-fatal; caller may still choose to truncate in txn_init(). */
        }
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }

    /* Seek to beginning of the already-open g_wal_file. */
    if (fseek(g_wal_file, 0, SEEK_END) != 0) {
        fprintf(stderr, "[txn] recover_wal: fseek(END) failed\n");
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }
    long file_size = ftell(g_wal_file);
    if (file_size <= 0) {
        /* Empty or unreadable file — clean start. */
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }

    /* Check if there are any complete entries. */
    long entry_count = file_size / (long)sizeof(WALEntry);
    if (entry_count == 0) {
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }

    /* Read all entries into a heap-allocated array. */
    WALEntry *entries = (WALEntry *)malloc((size_t)entry_count * sizeof(WALEntry));
    if (!entries) {
        fprintf(stderr, "[txn] recover_wal: malloc failed\n");
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }

    if (fseek(g_wal_file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[txn] recover_wal: fseek(0) failed\n");
        free(entries);
        g_last_recovery_summary.next_txn_id = g_current_txn_id;
        return;
    }

    size_t read_count = fread(entries, sizeof(WALEntry), (size_t)entry_count, g_wal_file);
    if ((long)read_count != entry_count) {
        fprintf(stderr, "[txn] recover_wal: short read (%zu of %ld entries)\n",
                read_count, entry_count);
        entry_count = (long)read_count;
    }

    g_last_recovery_summary.entries_scanned = (uint32_t)entry_count;

    int      rolled_back = 0;
    uint32_t max_committed_id = 0;

    for (long i = 0; i < entry_count; i++) {
        WALEntry *e = &entries[i];

        if (e->committed == 1) {
            g_last_recovery_summary.committed_entries++;
            /* Validate checksum on committed entries. */
            uint32_t expected = compute_checksum(e);
            if (expected != e->checksum) {
                g_last_recovery_summary.checksum_mismatches++;
                fprintf(stderr,
                        "[txn] recover_wal: checksum mismatch on committed entry "
                        "txn=%u table=%s page=%u (expected %08x got %08x) — skipping rollback\n",
                        e->txn_id, e->table, e->page_id, expected, e->checksum);
                /* Do not roll back; assume it committed correctly per spec. */
            }
            if (e->txn_id > max_committed_id) {
                max_committed_id = e->txn_id;
            }
        } else {
            g_last_recovery_summary.uncommitted_entries++;
            /* Uncommitted — restore before_image. */
            fprintf(stderr,
                    "[txn] recover_wal: rolling back uncommitted entry "
                    "txn=%u table=%s page=%u slot=%u\n",
                    e->txn_id, e->table, e->page_id, e->slot_id);
            if (!restore_page(e->table, e->page_id, e->before_image)) {
                g_last_recovery_summary.restore_failures++;
                fprintf(stderr,
                        "[txn] recover_wal: restore failed for txn=%u table=%s page=%u\n",
                        e->txn_id, e->table, e->page_id);
            }
            rolled_back++;
            g_last_recovery_summary.rolled_back_entries++;
        }
    }

    /* Set next txn_id beyond anything we saw. */
    g_current_txn_id = max_committed_id + 1;
    g_last_recovery_summary.next_txn_id = g_current_txn_id;

    if (rolled_back > 0) {
        fprintf(stderr, "[txn] recover_wal: rolled back %d entries; rewriting log\n",
                rolled_back);
        rewrite_wal_committed_only(entries, (int)entry_count);
        g_last_recovery_summary.wal_rewritten = 1;
    }

    free(entries);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * txn_init
 * Opens wal.log in "a+b" mode (creates if absent, preserves existing data,
 * supports both read and append). Runs crash recovery via recover_wal().
 * ───────────────────────────────────────────────────────────────────────────*/
void txn_init(void)
{
    /* Ensure file exists first. */
    FILE *bootstrap = fopen(WAL_PATH, "ab");
    if (!bootstrap) {
        fprintf(stderr, "[txn] txn_init: cannot create/open %s\n", WAL_PATH);
        return;
    }
    fclose(bootstrap);

    /* Open in read/write mode.
     * Never use append mode here, because wal_commit() performs in-place updates. */
    g_wal_file = fopen(WAL_PATH, "r+b");
    if (!g_wal_file) {
        fprintf(stderr, "[txn] txn_init: cannot open %s in r+b mode\n", WAL_PATH);
        return;
    }

    g_txn_active      = 0;
    g_nested_started_count = 0;
    g_current_txn_id  = 1;   /* default; recover_wal may raise this */
    reset_recovery_summary();

    /* Guard: seed.c may have left an 8-byte "CWAL" marker header. */
    if (wal_has_seed_magic(g_wal_file)) {
        fprintf(stderr,
                "[txn] txn_init: detected legacy seed WAL header 'CWAL'; "
                "resetting WAL file\n");
        if (!wal_truncate_to_empty()) {
            fprintf(stderr, "[txn] txn_init: WAL reset failed; continuing defensively\n");
        }
    }

    recover_wal();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * txn_shutdown
 * Commits any active transaction, flushes and closes wal.log.
 * ───────────────────────────────────────────────────────────────────────────*/
void txn_shutdown(void)
{
    if (g_txn_active) {
        wal_commit();
    }

    if (g_wal_file) {
        fflush(g_wal_file);
        fclose(g_wal_file);
        g_wal_file = NULL;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wal_begin
 * Starts a new logical transaction. If one is already active, auto-commits it
 * first so the system never has nested transactions.
 * ───────────────────────────────────────────────────────────────────────────*/
void wal_begin(void)
{
    if (g_txn_active) {
        /* Auto-commit the previous transaction before starting a new one. */
        wal_commit();
    }

    g_current_txn_id++;
    g_txn_active = 1;
    g_nested_started_count = 0;
}

int wal_is_active(void)
{
    return g_txn_active ? 1 : 0;
}

void wal_begin_nested(void)
{
    if (g_txn_active) {
        return;  /* participate in outer transaction */
    }

    wal_begin();
    g_nested_started_count++;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wal_log
 * Appends a WALEntry to wal.log with committed=0. Computes and stores the
 * checksum. If no transaction is currently active, auto-begins one.
 * storage.c calls this via extern declaration before every page slot write.
 * ───────────────────────────────────────────────────────────────────────────*/
int wal_log(const char *table, uint32_t page_id, uint16_t slot_id,
            const void *before_image, const void *after_image,
            size_t record_size)
{
    if (!g_wal_file) {
        fprintf(stderr, "[txn] wal_log: WAL not initialised — call txn_init() first\n");
        return -1;
    }
    if (!table || !before_image || !after_image) {
        fprintf(stderr, "[txn] wal_log: invalid arguments\n");
        return -1;
    }

    if (!g_txn_active) {
        /* Implicit transaction for callers that don't call wal_begin(). */
        wal_begin();
    }

    WALEntry e;
    memset(&e, 0, sizeof(WALEntry));

    e.txn_id      = g_current_txn_id;
    e.page_id     = page_id;
    e.slot_id     = slot_id;
    e.record_size = (uint16_t)(record_size > UINT16_MAX ? UINT16_MAX : record_size);
    e.committed   = 0;

    /* table name — null-terminated, truncated to fit */
    strncpy(e.table, table, sizeof(e.table) - 1);
    e.table[sizeof(e.table) - 1] = '\0';

    /* Full 4096-byte page images */
    memcpy(e.before_image, before_image, WAL_PAGE_SIZE);
    memcpy(e.after_image,  after_image,  WAL_PAGE_SIZE);

    e.checksum = compute_checksum(&e);

    /* Seek to end before writing (important after any reads during recovery) */
    if (fseek(g_wal_file, 0, SEEK_END) != 0) {
        fprintf(stderr, "[txn] wal_log: fseek(END) failed\n");
        return -1;
    }

    size_t written = fwrite(&e, sizeof(WALEntry), 1, g_wal_file);
    if (written != 1) {
        fprintf(stderr, "[txn] wal_log: fwrite failed for table=%s page=%u\n",
                table, page_id);
        return -1;
    }

    if (fflush(g_wal_file) != 0) {
        fprintf(stderr, "[txn] wal_log: fflush failed\n");
        return -1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wal_commit
 * Scans wal.log for every entry belonging to g_current_txn_id and sets
 * committed=1 in-place. Flushes after all updates.
 * ───────────────────────────────────────────────────────────────────────────*/
void wal_commit(void)
{
    if (!g_wal_file) {
        return;
    }

    /* Find the file size to calculate how many entries to scan. */
    if (fseek(g_wal_file, 0, SEEK_END) != 0) {
        fprintf(stderr, "[txn] wal_commit: fseek(END) failed\n");
        return;
    }
    long file_size = ftell(g_wal_file);
    if (file_size <= 0) {
        g_txn_active = 0;
        return;
    }

    long entry_count = file_size / (long)sizeof(WALEntry);

    /* Walk backwards through the file; entries for this txn are at the end. */
    for (long i = entry_count - 1; i >= 0; i--) {
        long offset = i * (long)sizeof(WALEntry);

        if (fseek(g_wal_file, offset, SEEK_SET) != 0) {
            fprintf(stderr, "[txn] wal_commit: fseek to entry %ld failed\n", i);
            continue;
        }

        WALEntry e;
        size_t r = fread(&e, sizeof(WALEntry), 1, g_wal_file);
        if (r != 1) {
            continue;
        }

        /* Once we see entries that belong to earlier transactions, stop. */
        if (e.txn_id < g_current_txn_id) {
            break;
        }

        if (e.txn_id == g_current_txn_id && e.committed == 0) {
            e.committed = 1;

            /* Seek back to the start of this entry and overwrite. */
            if (fseek(g_wal_file, offset, SEEK_SET) != 0) {
                fprintf(stderr, "[txn] wal_commit: fseek for overwrite failed\n");
                continue;
            }
            size_t w = fwrite(&e, sizeof(WALEntry), 1, g_wal_file);
            if (w != 1) {
                fprintf(stderr, "[txn] wal_commit: fwrite failed during in-place commit\n");
            }
        }
    }

    fflush(g_wal_file);
    g_txn_active = 0;
    g_nested_started_count = 0;
}

void wal_commit_nested(void)
{
    if (g_nested_started_count <= 0) {
        return;  /* outer txn owns the commit boundary */
    }

    wal_commit();
    /* wal_commit() resets g_nested_started_count to 0 */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wal_rollback
 * Restores before_images for all uncommitted entries of g_current_txn_id,
 * then prunes those entries from wal.log by rewriting the log without them.
 * ───────────────────────────────────────────────────────────────────────────*/
void wal_rollback(void)
{
    if (!g_wal_file) {
        g_txn_active = 0;
        return;
    }

    if (fseek(g_wal_file, 0, SEEK_END) != 0) {
        g_txn_active = 0;
        return;
    }
    long file_size = ftell(g_wal_file);
    if (file_size <= 0) {
        g_txn_active = 0;
        return;
    }

    long entry_count = file_size / (long)sizeof(WALEntry);
    if (entry_count == 0) {
        g_txn_active = 0;
        return;
    }

    WALEntry *entries = (WALEntry *)malloc((size_t)entry_count * sizeof(WALEntry));
    if (!entries) {
        fprintf(stderr, "[txn] wal_rollback: malloc failed\n");
        g_txn_active = 0;
        return;
    }

    if (fseek(g_wal_file, 0, SEEK_SET) != 0) {
        free(entries);
        g_txn_active = 0;
        return;
    }

    size_t read_count = fread(entries, sizeof(WALEntry), (size_t)entry_count, g_wal_file);
    entry_count = (long)read_count;

    /* Restore before_images for uncommitted entries of current txn. */
    for (long i = 0; i < entry_count; i++) {
        WALEntry *e = &entries[i];
        if (e->txn_id == g_current_txn_id && e->committed == 0) {
            fprintf(stderr,
                    "[txn] wal_rollback: restoring before_image for "
                    "table=%s page=%u slot=%u\n",
                    e->table, e->page_id, e->slot_id);
            if (!restore_page(e->table, e->page_id, e->before_image)) {
                fprintf(stderr,
                        "[txn] wal_rollback: restore failed for table=%s page=%u\n",
                        e->table, e->page_id);
            }
            /* Mark as rolled back so rewrite excludes it. */
            e->committed = -1;
        }
    }

    /* Rewrite log excluding rolled-back entries (committed == -1). */
    /* Temporarily treat -1 as "exclude"; rewrite_wal_committed_only checks == 1. */
    rewrite_wal_committed_only(entries, (int)entry_count);

    free(entries);
    g_txn_active = 0;
    g_nested_started_count = 0;
}

WALRecoverySummary wal_get_last_recovery_summary(void)
{
    return g_last_recovery_summary;
}
