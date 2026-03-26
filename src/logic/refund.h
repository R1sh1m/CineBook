#ifndef REFUND_H
#define REFUND_H

/*
 * refund.h — Cancellation and refund logic for CineBook
 *
 * Covers:
 *   - Refund policy loading from refund_policy table
 *   - Per-booking refund computation (tiered, time-based)
 *   - Atomic WAL-backed cancellation execution
 *   - Bulk admin show cancellation (100% refund including convenience fee)
 *
 * C11.  No external libs.  Depends on: query.h, session.h, txn.h
 */

#include "session.h"   /* SessionContext */

/* ─────────────────────────────────────────────────────────────────────────────
 * RefundType — mirrors refund_type column in refunds table
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    REFUND_FULL    = 0,
    REFUND_PARTIAL = 1,
    REFUND_NONE    = 2
} RefundType;

/* ─────────────────────────────────────────────────────────────────────────────
 * RefundResult
 * Heap-allocated by compute_refund(); caller must free().
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int   refund_id;                 /* 0 until persisted to refunds table    */
    int   booking_id;
    int   payment_id;
    float gross_paid;                /* bookings.total_amount                  */
    float convenience_fee_deducted;  /* 0 for admin-cancelled shows           */
    int   refund_percentage;         /* 0 / 50 / 75 / 100                     */
    float refund_amount;             /* INR credited to wallet                 */
    int   refund_type;               /* RefundType enum value                  */
    char  reason[200];               /* human-readable label from policy tier  */
    char  processed_at[20];          /* "YYYY-MM-DD HH:MM\0"                  */
} RefundResult;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * load_refund_policy — load all rows from refund_policy table into a static
 * internal array.  Called once at startup after schema_load(); safe to call
 * again after admin modifies a tier.
 */
void load_refund_policy(void);

/*
 * compute_refund — calculate refund for a user-initiated cancellation.
 *
 * Read-only: loads booking row, loads show row, computes hours_remaining via
 * difftime(show_time, now)/3600, walks loaded refund_policy tiers.
 * Refund base = total_amount − convenience_fee  (fee always kept for user
 * cancellations).
 *
 * Returns heap-allocated RefundResult* on success, NULL on any error.
 * Caller must free().
 */
RefundResult *compute_refund(int booking_id, SessionContext *ctx);

/*
 * execute_cancellation — atomically cancel one booking and credit the wallet.
 *
 * WAL-wrapped steps:
 *   1. compute_refund() → RefundResult
 *   2. db_update bookings: status=2, cancelled_at=now
 *   3. For each booking_seats row: db_update seat_status → AVAILABLE (0),
 *      clear held_by_user_id / held_until / booking_id
 *   4. db_update users: wallet_balance += refund_amount
 *   5. db_insert refunds row; capture new refund_id
 *   6. wal_commit; ctx->wallet_balance refreshed
 *   7. Post-commit: notify earliest WAITING waitlist entry for the show
 *
 * Returns 1 on success, 0 on failure (wal_rollback called automatically).
 */
int execute_cancellation(int booking_id, SessionContext *ctx);

/*
 * execute_admin_show_cancellation — bulk-cancel all CONFIRMED bookings for
 * a show.  Grants 100% refund INCLUDING the convenience fee.
 * Sets shows.is_active=0 after all bookings are processed.
 *
 * Returns count of bookings cancelled, -1 on fatal error.
 */
int execute_admin_show_cancellation(int show_id, SessionContext *ctx);

#endif /* REFUND_H */