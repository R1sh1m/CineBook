/*
 * integrity.c — Database Transaction Integrity Verification & Repair
 *
 * Validates referential integrity and transaction consistency across tables.
 * Repairs orphaned records, expired holds, and data corruption.
 *
 * Dependencies: query.h, schema.h, txn.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define access(path, mode) _access(path, mode)
#define F_OK 0
#else
#include <unistd.h>
#endif

#include "integrity.h"
#include "query.h"
#include "schema.h"
#include "txn.h"
#include "record.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────*/
#define LOG_PATH         "integrity_repairs.log"
#define BACKUP_DIR       "data/backups"
#define NULL_INT         ((int32_t)0x80000000)

/* Booking status enum (from bookings table) */
#define BOOKING_CONFIRMED  2

/* Seat status enum (from seat_status table) */
#define SEAT_AVAILABLE  0
#define SEAT_HELD       1
#define SEAT_BOOKED     2

/* ─────────────────────────────────────────────────────────────────────────────
 * Logging
 * ───────────────────────────────────────────────────────────────────────────*/
void log_integrity_event(const char *action, int count, const char *details)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;

    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(f, "[%s] %s | count=%d | %s\n", timestamp, action, count, 
            details ? details : "");
    fclose(f);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Backup
 * ───────────────────────────────────────────────────────────────────────────*/
int create_integrity_backup(void)
{
    /* Create backup directory if it doesn't exist */
    #ifdef _WIN32
    _mkdir(BACKUP_DIR);
    #else
    mkdir(BACKUP_DIR, 0755);
    #endif

    /* Timestamp for backup folder */
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", localtime(&now));

    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/backup_%s", BACKUP_DIR, timestamp);
    
    #ifdef _WIN32
    _mkdir(backup_path);
    #else
    mkdir(backup_path, 0755);
    #endif

    /* Copy all .db files from data/db/ to backup folder */
    const char *tables[] = {
        "users", "movies", "cast_members", "theatres", "screens", "seats",
        "shows", "seat_status", "bookings", "booking_seats", "payments",
        "refunds", "promos", "waitlist", "countries", "cities",
        "academic_domains", "refund_policy"
    };
    
    int copied = 0;
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "data/db/%s.db", tables[i]);
        snprintf(dst, sizeof(dst), "%s/%s.db", backup_path, tables[i]);

        /* Check if source exists */
        if (access(src, F_OK) != 0) continue;

        /* Copy file */
        FILE *fsrc = fopen(src, "rb");
        if (!fsrc) continue;

        FILE *fdst = fopen(dst, "wb");
        if (!fdst) {
            fclose(fsrc);
            continue;
        }

        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
            fwrite(buf, 1, n, fdst);
        }

        fclose(fsrc);
        fclose(fdst);
        copied++;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Backed up %d table files to %s", copied, backup_path);
    log_integrity_event("BACKUP_CREATED", copied, msg);

    return (copied > 0) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: Get current timestamp as formatted string
 * ───────────────────────────────────────────────────────────────────────────*/
static void get_current_timestamp(char *buf, size_t bufsize)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(buf, bufsize, "%04d-%02d-%02d %02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: Parse DATE field (20 bytes ASCII "YYYY-MM-DD HH:MM\0")
 * Returns UNIX timestamp or 0 if NULL/invalid
 * ───────────────────────────────────────────────────────────────────────────*/
static time_t parse_date_field(const char *date_str)
{
    if (!date_str || date_str[0] == '\0') return 0;

    struct tm t = {0};
    if (sscanf(date_str, "%d-%d-%d %d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min) != 5) {
        return 0;
    }

    t.tm_year -= 1900;
    t.tm_mon -= 1;
    return mktime(&t);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for orphaned booking_seats
 * booking_seats rows where booking_id does not exist in bookings table
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_orphaned_booking_seats(void)
{
    ResultSet *bs = db_select("booking_seats", NULL, 0, NULL, 0);
    if (!bs) return 0;

    int orphans = 0;
    for (int i = 0; i < bs->row_count; i++) {
        if (!bs->rows[i][1]) continue;  /* booking_id is column 1 */
        
        int booking_id = *(int32_t *)bs->rows[i][1];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            orphans++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(bs);
    return orphans;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for dangling seat holds
 * seat_status rows with held_by_user_id that doesn't exist in users table
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_dangling_seat_holds(void)
{
    ResultSet *ss = db_select("seat_status", NULL, 0, NULL, 0);
    if (!ss) return 0;

    int dangling = 0;
    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][4]) continue;  /* held_by_user_id is column 4 */
        
        int user_id = *(int32_t *)ss->rows[i][4];
        if (user_id == NULL_INT) continue;

        /* Check if user exists */
        WhereClause w = {"user_id", OP_EQ, &user_id, 0};
        ResultSet *usr = db_select("users", &w, 1, NULL, 0);
        
        if (!usr || usr->row_count == 0) {
            dangling++;
        }
        
        if (usr) result_set_free(usr);
    }

    result_set_free(ss);
    return dangling;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for orphaned payments
 * payments rows where booking_id does not exist in bookings table
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_orphaned_payments(void)
{
    ResultSet *pm = db_select("payments", NULL, 0, NULL, 0);
    if (!pm) return 0;

    int orphans = 0;
    for (int i = 0; i < pm->row_count; i++) {
        if (!pm->rows[i][1]) continue;  /* booking_id is column 1 */
        
        int booking_id = *(int32_t *)pm->rows[i][1];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            orphans++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(pm);
    return orphans;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for expired seat holds
 * seat_status rows where status=HELD and held_until < current_time
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_expired_holds(void)
{
    int status_held = SEAT_HELD;
    WhereClause w = {"status", OP_EQ, &status_held, 0};
    ResultSet *ss = db_select("seat_status", &w, 1, NULL, 0);
    if (!ss) return 0;

    time_t now = time(NULL);
    int expired = 0;

    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][5]) continue;  /* held_until is column 5 */
        
        const char *held_until_str = (const char *)ss->rows[i][5];
        time_t held_until = parse_date_field(held_until_str);
        
        if (held_until > 0 && held_until < now) {
            expired++;
        }
    }

    result_set_free(ss);
    return expired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for invalid booking references in seat_status
 * seat_status rows with booking_id that doesn't exist in bookings table
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_invalid_booking_refs(void)
{
    ResultSet *ss = db_select("seat_status", NULL, 0, NULL, 0);
    if (!ss) return 0;

    int invalid = 0;
    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][6]) continue;  /* booking_id is column 6 */
        
        int booking_id = *(int32_t *)ss->rows[i][6];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            invalid++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(ss);
    return invalid;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Verification: Check for duplicate seat allocations
 * Same show_id + seat_id with multiple booking_ids (status=BOOKED)
 * ───────────────────────────────────────────────────────────────────────────*/
static int check_duplicate_seats(void)
{
    int status_booked = SEAT_BOOKED;
    WhereClause w = {"status", OP_EQ, &status_booked, 0};
    ResultSet *ss = db_select("seat_status", &w, 1, NULL, 0);
    if (!ss) return 0;

    /* Build map of show_id|seat_id -> count */
    typedef struct {
        int show_id;
        int seat_id;
        int count;
    } SeatKey;

    SeatKey *keys = calloc(ss->row_count, sizeof(SeatKey));
    if (!keys) {
        result_set_free(ss);
        return 0;
    }

    int unique_count = 0;
    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][1] || !ss->rows[i][2]) continue;  /* show_id, seat_id */
        
        int show_id = *(int32_t *)ss->rows[i][1];
        int seat_id = *(int32_t *)ss->rows[i][2];
        
        /* Find or add to keys array */
        int found = 0;
        for (int j = 0; j < unique_count; j++) {
            if (keys[j].show_id == show_id && keys[j].seat_id == seat_id) {
                keys[j].count++;
                found = 1;
                break;
            }
        }
        
        if (!found) {
            keys[unique_count].show_id = show_id;
            keys[unique_count].seat_id = seat_id;
            keys[unique_count].count = 1;
            unique_count++;
        }
    }

    /* Count duplicates (count > 1) */
    int duplicates = 0;
    for (int i = 0; i < unique_count; i++) {
        if (keys[i].count > 1) {
            duplicates += (keys[i].count - 1);  /* Extra bookings are duplicates */
        }
    }

    free(keys);
    result_set_free(ss);
    return duplicates;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API: verify_transaction_state
 * ───────────────────────────────────────────────────────────────────────────*/
IntegrityReport *verify_transaction_state(void)
{
    IntegrityReport *report = calloc(1, sizeof(IntegrityReport));
    if (!report) return NULL;

    /* Run all verification checks */
    report->orphaned_booking_seats = check_orphaned_booking_seats();
    report->dangling_seat_holds    = check_dangling_seat_holds();
    report->orphaned_payments      = check_orphaned_payments();
    report->expired_holds          = check_expired_holds();
    report->invalid_booking_refs   = check_invalid_booking_refs();
    report->duplicate_seats        = check_duplicate_seats();

    report->total_issues = report->orphaned_booking_seats +
                          report->dangling_seat_holds +
                          report->orphaned_payments +
                          report->expired_holds +
                          report->invalid_booking_refs +
                          report->duplicate_seats;

    /* Log verification run */
    char msg[256];
    snprintf(msg, sizeof(msg), "Verification complete: %d total issues found", 
             report->total_issues);
    log_integrity_event("VERIFY", report->total_issues, msg);

    return report;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API: free_integrity_report
 * ───────────────────────────────────────────────────────────────────────────*/
void free_integrity_report(IntegrityReport *report)
{
    if (report) free(report);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API: print_integrity_report
 * ───────────────────────────────────────────────────────────────────────────*/
void print_integrity_report(const IntegrityReport *report)
{
    if (!report) {
        printf("\n  [!] No integrity report available.\n");
        return;
    }

    printf("\n");
    printf("  ═══════════════════════════════════════════════════════════\n");
    printf("  DATABASE INTEGRITY REPORT\n");
    printf("  ═══════════════════════════════════════════════════════════\n\n");

    /* Check results with visual indicators */
    printf("  %s Orphaned booking_seats: %d\n",
           report->orphaned_booking_seats > 0 ? "✗" : "✓",
           report->orphaned_booking_seats);

    printf("  %s Dangling seat holds: %d\n",
           report->dangling_seat_holds > 0 ? "✗" : "✓",
           report->dangling_seat_holds);

    printf("  %s Orphaned payments: %d\n",
           report->orphaned_payments > 0 ? "✗" : "✓",
           report->orphaned_payments);

    printf("  %s Expired seat holds: %d\n",
           report->expired_holds > 0 ? "✗" : "✓",
           report->expired_holds);

    printf("  %s Invalid booking references: %d\n",
           report->invalid_booking_refs > 0 ? "✗" : "✓",
           report->invalid_booking_refs);

    printf("  %s Duplicate seat allocations: %d\n",
           report->duplicate_seats > 0 ? "✗" : "✓",
           report->duplicate_seats);

    printf("\n  ───────────────────────────────────────────────────────────\n");
    if (report->total_issues == 0) {
        printf("  ✓ Database integrity: HEALTHY\n");
        printf("    All checks passed. No issues detected.\n");
    } else {
        printf("  ✗ Total issues found: %d\n", report->total_issues);
        printf("    Repair recommended.\n");
    }
    printf("  ═══════════════════════════════════════════════════════════\n\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Orphaned booking_seats
 * ───────────────────────────────────────────────────────────────────────────*/
int repair_orphaned_booking_seats(int dry_run)
{
    ResultSet *bs = db_select("booking_seats", NULL, 0, NULL, 0);
    if (!bs) return 0;

    int repaired = 0;
    for (int i = 0; i < bs->row_count; i++) {
        if (!bs->rows[i][1]) continue;
        
        int booking_id = *(int32_t *)bs->rows[i][1];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            /* Orphan found - delete it */
            if (!dry_run) {
                int bs_id = *(int32_t *)bs->rows[i][0];  /* bs_id is PK */
                WhereClause dw = {"bs_id", OP_EQ, &bs_id, 0};
                
                wal_begin_nested();
                db_delete("booking_seats", &dw, 1);
                wal_commit_nested();
            }
            repaired++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(bs);

    if (repaired > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s %d orphaned booking_seats records",
                 dry_run ? "Found" : "Deleted", repaired);
        log_integrity_event("REPAIR_BOOKING_SEATS", repaired, msg);
    }

    return repaired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Expired holds
 * ───────────────────────────────────────────────────────────────────────────*/
int release_expired_holds(int dry_run)
{
    int status_held = SEAT_HELD;
    WhereClause w = {"status", OP_EQ, &status_held, 0};
    ResultSet *ss = db_select("seat_status", &w, 1, NULL, 0);
    if (!ss) return 0;

    time_t now = time(NULL);
    int repaired = 0;

    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][5]) continue;
        
        const char *held_until_str = (const char *)ss->rows[i][5];
        time_t held_until = parse_date_field(held_until_str);
        
        if (held_until > 0 && held_until < now) {
            /* Expired hold - release it */
            if (!dry_run) {
                int status_id = *(int32_t *)ss->rows[i][0];
                int new_status = SEAT_AVAILABLE;
                
                WhereClause uw = {"status_id", OP_EQ, &status_id, 0};
                wal_begin_nested();
                int null_int = NULL_INT;
                db_update("seat_status", &uw, 1, "status", &new_status);
                db_update("seat_status", &uw, 1, "held_by_user_id", &null_int);
                wal_commit_nested();
            }
            repaired++;
        }
    }

    result_set_free(ss);

    if (repaired > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s %d expired seat holds",
                 dry_run ? "Found" : "Released", repaired);
        log_integrity_event("REPAIR_EXPIRED_HOLDS", repaired, msg);
    }

    return repaired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Dangling payments
 * ───────────────────────────────────────────────────────────────────────────*/
int fix_dangling_payments(int dry_run)
{
    ResultSet *pm = db_select("payments", NULL, 0, NULL, 0);
    if (!pm) return 0;

    int repaired = 0;
    for (int i = 0; i < pm->row_count; i++) {
        if (!pm->rows[i][1]) continue;
        
        int booking_id = *(int32_t *)pm->rows[i][1];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            /* Orphan found - delete it */
            if (!dry_run) {
                int payment_id = *(int32_t *)pm->rows[i][0];
                WhereClause dw = {"payment_id", OP_EQ, &payment_id, 0};
                
                wal_begin_nested();
                db_delete("payments", &dw, 1);
                wal_commit_nested();
            }
            repaired++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(pm);

    if (repaired > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s %d orphaned payment records",
                 dry_run ? "Found" : "Deleted", repaired);
        log_integrity_event("REPAIR_PAYMENTS", repaired, msg);
    }

    return repaired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Invalid booking references in seat_status
 * ───────────────────────────────────────────────────────────────────────────*/
int fix_invalid_booking_refs(int dry_run)
{
    ResultSet *ss = db_select("seat_status", NULL, 0, NULL, 0);
    if (!ss) return 0;

    int repaired = 0;
    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][6]) continue;
        
        int booking_id = *(int32_t *)ss->rows[i][6];
        if (booking_id == NULL_INT) continue;

        /* Check if booking exists */
        WhereClause w = {"booking_id", OP_EQ, &booking_id, 0};
        ResultSet *bk = db_select("bookings", &w, 1, NULL, 0);
        
        if (!bk || bk->row_count == 0) {
            /* Invalid ref - clear it and reset status */
            if (!dry_run) {
                int status_id = *(int32_t *)ss->rows[i][0];
                int new_status = SEAT_AVAILABLE;
                
                int null_int = NULL_INT;
                WhereClause uw = {"status_id", OP_EQ, &status_id, 0};
                wal_begin_nested();
                db_update("seat_status", &uw, 1, "booking_id", &null_int);
                db_update("seat_status", &uw, 1, "status", &new_status);
                wal_commit_nested();
            }
            repaired++;
        }
        
        if (bk) result_set_free(bk);
    }

    result_set_free(ss);

    if (repaired > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s %d invalid booking references",
                 dry_run ? "Found" : "Cleared", repaired);
        log_integrity_event("REPAIR_BOOKING_REFS", repaired, msg);
    }

    return repaired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Duplicate seat allocations
 * Keep the latest booking, clear others
 * ───────────────────────────────────────────────────────────────────────────*/
int fix_duplicate_seats(int dry_run)
{
    int status_booked = SEAT_BOOKED;
    WhereClause w = {"status", OP_EQ, &status_booked, 0};
    ResultSet *ss = db_select("seat_status", &w, 1, NULL, 0);
    if (!ss) return 0;

    /* Group by show_id|seat_id */
    typedef struct {
        int show_id;
        int seat_id;
        int status_ids[16];  /* Support up to 16 duplicates */
        int count;
    } SeatGroup;

    SeatGroup *groups = calloc(ss->row_count, sizeof(SeatGroup));
    if (!groups) {
        result_set_free(ss);
        return 0;
    }

    int group_count = 0;
    for (int i = 0; i < ss->row_count; i++) {
        if (!ss->rows[i][1] || !ss->rows[i][2]) continue;
        
        int show_id = *(int32_t *)ss->rows[i][1];
        int seat_id = *(int32_t *)ss->rows[i][2];
        int status_id = *(int32_t *)ss->rows[i][0];
        
        /* Find or create group */
        int found = 0;
        for (int j = 0; j < group_count; j++) {
            if (groups[j].show_id == show_id && groups[j].seat_id == seat_id) {
                if (groups[j].count < 16) {
                    groups[j].status_ids[groups[j].count++] = status_id;
                }
                found = 1;
                break;
            }
        }
        
        if (!found) {
            groups[group_count].show_id = show_id;
            groups[group_count].seat_id = seat_id;
            groups[group_count].status_ids[0] = status_id;
            groups[group_count].count = 1;
            group_count++;
        }
    }

    /* Clear duplicates (keep first, clear rest) */
    int repaired = 0;
    for (int i = 0; i < group_count; i++) {
        if (groups[i].count > 1) {
            /* Clear all but the first */
            for (int j = 1; j < groups[i].count; j++) {
                if (!dry_run) {
                    int status_id = groups[i].status_ids[j];
                    int new_status = SEAT_AVAILABLE;
                    
                    int null_int = NULL_INT;
                    WhereClause uw = {"status_id", OP_EQ, &status_id, 0};
                    wal_begin_nested();
                    db_update("seat_status", &uw, 1, "booking_id", &null_int);
                    db_update("seat_status", &uw, 1, "status", &new_status);
                    wal_commit_nested();
                }
                repaired++;
            }
        }
    }

    free(groups);
    result_set_free(ss);

    if (repaired > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s %d duplicate seat allocations",
                 dry_run ? "Found" : "Cleared", repaired);
        log_integrity_event("REPAIR_DUPLICATES", repaired, msg);
    }

    return repaired;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Repair: Rebuild seat_status index (not implemented - would require full scan)
 * ───────────────────────────────────────────────────────────────────────────*/
int rebuild_seat_status_index(int dry_run)
{
    (void)dry_run;
    /* This would require a full table rebuild - not implemented for safety */
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API: repair_orphaned_records
 * Run all repair operations
 * ───────────────────────────────────────────────────────────────────────────*/
int repair_orphaned_records(int dry_run)
{
    int total = 0;

    if (!dry_run) {
        printf("\n  Creating backup before repair...\n");
        if (!create_integrity_backup()) {
            printf("  [!] Backup failed. Aborting repair.\n");
            return -1;
        }
        printf("  ✓ Backup created successfully.\n\n");
    }

    printf("  %s repairs...\n\n", dry_run ? "Simulating" : "Running");

    total += repair_orphaned_booking_seats(dry_run);
    total += release_expired_holds(dry_run);
    total += fix_dangling_payments(dry_run);
    total += fix_invalid_booking_refs(dry_run);
    total += fix_duplicate_seats(dry_run);

    char msg[256];
    snprintf(msg, sizeof(msg), "%s: %d total issues %s",
             dry_run ? "Dry run" : "Repair complete",
             total,
             dry_run ? "detected" : "fixed");
    log_integrity_event("REPAIR_ALL", total, msg);

    return total;
}
