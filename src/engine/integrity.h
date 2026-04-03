#ifndef INTEGRITY_H
#define INTEGRITY_H

#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Integrity Report — summary of database inconsistencies
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int orphaned_booking_seats;  /* booking_seats without parent booking */
    int dangling_seat_holds;     /* seat_status with invalid held_by_user_id */
    int orphaned_payments;       /* payments without matching booking */
    int missing_seat_status;     /* bookings without seat_status entries */
    int expired_holds;           /* seat_status with held_until < now */
    int invalid_booking_refs;    /* seat_status with invalid booking_id */
    int duplicate_seats;         /* same seat booked multiple times */
    int total_issues;            /* sum of all issue counts */
} IntegrityReport;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Run full database integrity verification.
 * Returns heap-allocated report; caller must call free_integrity_report().
 * Returns NULL on critical error. */
IntegrityReport *verify_transaction_state(void);

/* Free an IntegrityReport allocated by verify_transaction_state(). */
void free_integrity_report(IntegrityReport *report);

/* Print detailed integrity report to stdout in admin panel format. */
void print_integrity_report(const IntegrityReport *report);

/* Repair all detected issues. Requires admin confirmation in interactive mode.
 * dry_run: 1 = show what would be fixed without fixing, 0 = perform repairs
 * Returns total number of issues repaired, or -1 on error. */
int repair_orphaned_records(int dry_run);

/* Individual repair functions */
int repair_orphaned_booking_seats(int dry_run);   /* Delete orphaned booking_seats */
int release_expired_holds(int dry_run);           /* Set seat_status=AVAILABLE */
int fix_dangling_payments(int dry_run);           /* Mark as REFUNDED or delete */
int rebuild_seat_status_index(int dry_run);       /* Rebuild from bookings */
int fix_invalid_booking_refs(int dry_run);        /* Clear invalid booking_id refs */
int fix_duplicate_seats(int dry_run);             /* Keep latest, clear others */

/* Log integrity event to integrity_repairs.log */
void log_integrity_event(const char *action, int count, const char *details);

/* Create backup of all .db files before repair.
 * Returns 1 on success, 0 on failure. */
int create_integrity_backup(void);

#endif /* INTEGRITY_H */
