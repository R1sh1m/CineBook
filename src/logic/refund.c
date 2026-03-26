/*
 * refund.c — Cancellation and refund logic for CineBook
 *
 * C11.  No external libs beyond what is already linked (libcurl / cJSON are
 * not used here).  Depends on: query.h (→ schema.h → record.h), session.h,
 * txn.h, refund.h, and time.h / string.h / stdlib.h / stdio.h.
 *
 * Column index constants follow the schema documented in decisions.md and
 * passed in the original prompt; they are kept as named #defines for
 * readability and to make future schema changes trivially auditable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "refund.h"
#include "query.h"
#include "txn.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — bookings table
 * ───────────────────────────────────────────────────────────────────────────*/
#define BK_COL_BOOKING_ID       0
#define BK_COL_USER_ID          1
#define BK_COL_SHOW_ID          2
#define BK_COL_SEAT_COUNT       3
#define BK_COL_SUBTOTAL         4
#define BK_COL_GST_AMOUNT       5
#define BK_COL_CONVENIENCE_FEE  6
#define BK_COL_DISCOUNT_AMOUNT  7
#define BK_COL_TOTAL_AMOUNT     8
#define BK_COL_PROMO_ID         9
#define BK_COL_PAYMENT_ID       10
#define BK_COL_STATUS           11
#define BK_COL_BOOKED_AT        12
#define BK_COL_CANCELLED_AT     13

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — shows table
 * ───────────────────────────────────────────────────────────────────────────*/
#define SH_COL_SHOW_ID          0
#define SH_COL_MOVIE_ID         1
#define SH_COL_SCREEN_ID        2
#define SH_COL_SHOW_DATETIME    3
#define SH_COL_BASE_PRICE       4
#define SH_COL_IS_ACTIVE        5

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — booking_seats table
 * ───────────────────────────────────────────────────────────────────────────*/
#define BS_COL_BS_ID            0
#define BS_COL_BOOKING_ID       1
#define BS_COL_SEAT_ID          2
#define BS_COL_SEAT_PRICE       3

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — seat_status table
 * ───────────────────────────────────────────────────────────────────────────*/
#define SS_COL_STATUS_ID        0
#define SS_COL_SHOW_ID          1
#define SS_COL_SEAT_ID          2
#define SS_COL_STATUS           3
#define SS_COL_HELD_BY_USER_ID  4
#define SS_COL_HELD_UNTIL       5
#define SS_COL_BOOKING_ID       6

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — refund_policy table
 * ───────────────────────────────────────────────────────────────────────────*/
#define RP_COL_POLICY_ID        0
#define RP_COL_HOURS_BEFORE     1
#define RP_COL_REFUND_PERCENT   2
#define RP_COL_LABEL            3

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — refunds table
 * ───────────────────────────────────────────────────────────────────────────*/
#define RF_COL_REFUND_ID                 0
#define RF_COL_BOOKING_ID                1
#define RF_COL_PAYMENT_ID                2
#define RF_COL_GROSS_PAID                3
#define RF_COL_CONVENIENCE_FEE_DEDUCTED  4
#define RF_COL_REFUND_PERCENTAGE         5
#define RF_COL_REFUND_AMOUNT             6
#define RF_COL_REFUND_TYPE               7
#define RF_COL_REASON                    8
#define RF_COL_PROCESSED_AT              9

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — users table
 * ───────────────────────────────────────────────────────────────────────────*/
#define US_COL_USER_ID          0
#define US_COL_WALLET_BALANCE   6   /* as per schema in decisions.md          */

/* ─────────────────────────────────────────────────────────────────────────────
 * Column indices — waitlist table
 * ───────────────────────────────────────────────────────────────────────────*/
#define WL_COL_WAITLIST_ID           0
#define WL_COL_USER_ID               1
#define WL_COL_SHOW_ID               2
#define WL_COL_SEAT_COUNT_REQUESTED  3
#define WL_COL_ADDED_AT              4
#define WL_COL_STATUS                5
#define WL_COL_NOTIFIED_AT           6

/* Booking status values */
#define BOOKING_STATUS_PENDING    0
#define BOOKING_STATUS_CONFIRMED  1
#define BOOKING_STATUS_CANCELLED  2

/* Seat-status status values */
#define SEAT_AVAILABLE  0
#define SEAT_HELD       1
#define SEAT_CONFIRMED  2
#define SEAT_CANCELLED  3

/* Waitlist status values */
#define WL_STATUS_WAITING    0
#define WL_STATUS_NOTIFIED   1
#define WL_STATUS_EXPIRED    2
#define WL_STATUS_FULFILLED  3

/* Maximum policy tiers we cache */
#define MAX_POLICY_TIERS 16

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal policy tier cache
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int  hours_before;     /* tier applies when hours_remaining >= this value */
    int  refund_percent;   /* 0 / 50 / 75 / 100                               */
    char label[50];        /* e.g. "Full Refund"                               */
} PolicyTier;

static PolicyTier g_tiers[MAX_POLICY_TIERS];
static int        g_tier_count = 0;

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers — forward declarations
 * ───────────────────────────────────────────────────────────────────────────*/
static int    compare_tiers_desc(const void *a, const void *b);
static time_t parse_show_datetime(const char *dt_str);
static void   current_datetime_str(char *out, size_t len);
static int    notify_waitlist_for_show(int show_id);
static int    insert_refund_row(const RefundResult *r);
static float  get_user_wallet(int user_id);

/* ─────────────────────────────────────────────────────────────────────────────
 * load_refund_policy
 * ───────────────────────────────────────────────────────────────────────────*/
void load_refund_policy(void)
{
    g_tier_count = 0;

    ResultSet *rs = db_select("refund_policy", NULL, 0, NULL, 0);
    if (!rs) {
        fprintf(stderr, "[refund] WARNING: could not load refund_policy table; "
                        "defaults will apply.\n");
        /* Install hardcoded fallback tiers so the system never crashes */
        g_tiers[0] = (PolicyTier){ .hours_before = 72, .refund_percent = 100,
                                   .label = "Full Refund" };
        g_tiers[1] = (PolicyTier){ .hours_before = 24, .refund_percent = 75,
                                   .label = "Partial Refund" };
        g_tiers[2] = (PolicyTier){ .hours_before = 6,  .refund_percent = 50,
                                   .label = "Late Cancellation" };
        g_tiers[3] = (PolicyTier){ .hours_before = 0,  .refund_percent = 0,
                                   .label = "No Refund" };
        g_tier_count = 4;
        return;
    }

    for (int i = 0; i < rs->row_count && g_tier_count < MAX_POLICY_TIERS; i++) {
        void **row = rs->rows[i];
        PolicyTier t;
        t.hours_before   = *(int32_t *)row[RP_COL_HOURS_BEFORE];
        t.refund_percent = *(int32_t *)row[RP_COL_REFUND_PERCENT];
        strncpy(t.label, (char *)row[RP_COL_LABEL], sizeof(t.label) - 1);
        t.label[sizeof(t.label) - 1] = '\0';
        g_tiers[g_tier_count++] = t;
    }
    result_set_free(rs);

    /* Sort descending by hours_before so the first matching tier wins when we
     * walk from index 0 downward. */
    qsort(g_tiers, (size_t)g_tier_count, sizeof(PolicyTier),
          compare_tiers_desc);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * compute_refund — read-only, returns heap-allocated RefundResult*
 * ───────────────────────────────────────────────────────────────────────────*/
RefundResult *compute_refund(int booking_id, SessionContext *ctx)
{
    (void)ctx;   /* ctx reserved for future use; not needed for read path */

    /* ── 1. Load the booking row ── */
    int bk_id_val = booking_id;
    WhereClause w_bk = { "booking_id", OP_EQ, &bk_id_val, 0 };
    ResultSet *bk_rs = db_select("bookings", &w_bk, 1, NULL, 0);
    if (!bk_rs || bk_rs->row_count == 0) {
        fprintf(stderr, "[refund] compute_refund: booking_id=%d not found.\n",
                booking_id);
        if (bk_rs) result_set_free(bk_rs);
        return NULL;
    }

    void **bk = bk_rs->rows[0];

    int   status    = *(int32_t *)bk[BK_COL_STATUS];
    if (status != BOOKING_STATUS_CONFIRMED) {
        fprintf(stderr, "[refund] compute_refund: booking_id=%d status=%d "
                        "(not CONFIRMED).\n", booking_id, status);
        result_set_free(bk_rs);
        return NULL;
    }

    int   user_id       = *(int32_t *)bk[BK_COL_USER_ID];
    int   show_id       = *(int32_t *)bk[BK_COL_SHOW_ID];
    int   payment_id    = *(int32_t *)bk[BK_COL_PAYMENT_ID];
    float total_amount  = *(float   *)bk[BK_COL_TOTAL_AMOUNT];
    float conv_fee      = *(float   *)bk[BK_COL_CONVENIENCE_FEE];
    (void)user_id;  /* available if needed for additional checks */

    result_set_free(bk_rs);

    /* ── 2. Load the show row to get show_datetime ── */
    int sh_id_val = show_id;
    WhereClause w_sh = { "show_id", OP_EQ, &sh_id_val, 0 };
    ResultSet *sh_rs = db_select("shows", &w_sh, 1, NULL, 0);
    if (!sh_rs || sh_rs->row_count == 0) {
        fprintf(stderr, "[refund] compute_refund: show_id=%d not found.\n",
                show_id);
        if (sh_rs) result_set_free(sh_rs);
        return NULL;
    }

    char show_dt_str[32];
    strncpy(show_dt_str, (char *)sh_rs->rows[0][SH_COL_SHOW_DATETIME],
            sizeof(show_dt_str) - 1);
    show_dt_str[sizeof(show_dt_str) - 1] = '\0';
    result_set_free(sh_rs);

    /* ── 3. Compute hours_remaining ── */
    time_t show_time = parse_show_datetime(show_dt_str);
    time_t now       = time(NULL);
    double hours_remaining = (show_time > now)
                             ? difftime(show_time, now) / 3600.0
                             : 0.0;

    /* ── 4. Walk tiers (sorted desc by hours_before) ── */
    int   matched_pct   = 0;
    char  matched_label[50];
    strncpy(matched_label, "No Refund", sizeof(matched_label));

    if (g_tier_count == 0) {
        /* Safety: reload if never called */
        load_refund_policy();
    }

    for (int i = 0; i < g_tier_count; i++) {
        if (hours_remaining >= (double)g_tiers[i].hours_before) {
            matched_pct = g_tiers[i].refund_percent;
            strncpy(matched_label, g_tiers[i].label,
                    sizeof(matched_label) - 1);
            matched_label[sizeof(matched_label) - 1] = '\0';
            break;
        }
    }

    /* ── 5. Calculate refund amount ── */
    float refund_base   = total_amount - conv_fee;
    if (refund_base < 0.0f) refund_base = 0.0f;
    float refund_amount = refund_base * ((float)matched_pct / 100.0f);

    RefundType rtype;
    if (matched_pct == 100)      rtype = REFUND_FULL;
    else if (matched_pct > 0)    rtype = REFUND_PARTIAL;
    else                         rtype = REFUND_NONE;

    /* ── 6. Build result ── */
    RefundResult *r = calloc(1, sizeof(RefundResult));
    if (!r) {
        fprintf(stderr, "[refund] compute_refund: calloc failed.\n");
        return NULL;
    }

    r->refund_id               = 0;   /* not persisted yet */
    r->booking_id              = booking_id;
    r->payment_id              = payment_id;
    r->gross_paid              = total_amount;
    r->convenience_fee_deducted = conv_fee;
    r->refund_percentage       = matched_pct;
    r->refund_amount           = refund_amount;
    r->refund_type             = (int)rtype;
    strncpy(r->reason, matched_label, sizeof(r->reason) - 1);
    r->reason[sizeof(r->reason) - 1] = '\0';
    current_datetime_str(r->processed_at, sizeof(r->processed_at));

    return r;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * execute_cancellation — WAL-wrapped; user-initiated
 * ───────────────────────────────────────────────────────────────────────────*/
int execute_cancellation(int booking_id, SessionContext *ctx)
{
    /* ── 1. Compute refund (read-only, outside WAL) ── */
    RefundResult *r = compute_refund(booking_id, ctx);
    if (!r) {
        fprintf(stderr, "[refund] execute_cancellation: "
                        "compute_refund failed for booking_id=%d.\n",
                booking_id);
        return 0;
    }

    /* We need the show_id for the seat_status update; re-fetch booking row. */
    int bk_id_val = booking_id;
    WhereClause w_bk = { "booking_id", OP_EQ, &bk_id_val, 0 };
    ResultSet *bk_rs = db_select("bookings", &w_bk, 1, NULL, 0);
    if (!bk_rs || bk_rs->row_count == 0) {
        fprintf(stderr, "[refund] execute_cancellation: "
                        "booking_id=%d vanished between compute and execute.\n",
                booking_id);
        if (bk_rs) result_set_free(bk_rs);
        free(r);
        return 0;
    }
    int show_id = *(int32_t *)bk_rs->rows[0][BK_COL_SHOW_ID];
    int user_id = *(int32_t *)bk_rs->rows[0][BK_COL_USER_ID];
    result_set_free(bk_rs);

    /* ── 2. Fetch seat list from booking_seats ── */
    WhereClause w_bs = { "booking_id", OP_EQ, &bk_id_val, 0 };
    ResultSet *bs_rs = db_select("booking_seats", &w_bs, 1, NULL, 0);
    /* bs_rs may be NULL if table empty — that would be a data integrity issue
     * but we proceed gracefully. */

    /* ── 3. Begin WAL transaction ── */
    wal_begin();

    int ok = 1;

    /* ── 4a. Update bookings: status=CANCELLED, cancelled_at=now ── */
    {
        int cancelled_status = BOOKING_STATUS_CANCELLED;
        WhereClause w = { "booking_id", OP_EQ, &bk_id_val, 0 };
        int rows = db_update("bookings", &w, 1,
                             "status", &cancelled_status);
        if (rows <= 0) {
            fprintf(stderr, "[refund] execute_cancellation: "
                            "failed to update bookings.status for id=%d.\n",
                    booking_id);
            ok = 0;
            goto cleanup;
        }

        char now_str[20];
        current_datetime_str(now_str, sizeof(now_str));
        rows = db_update("bookings", &w, 1, "cancelled_at", now_str);
        /* Non-fatal if this column update fails — log and continue. */
        if (rows <= 0) {
            fprintf(stderr, "[refund] execute_cancellation: "
                            "warning: could not set cancelled_at for "
                            "booking_id=%d.\n", booking_id);
        }
    }

    /* ── 4b. Free each seat in seat_status ── */
    if (bs_rs) {
        int avail_status      = SEAT_AVAILABLE;
        int null_int          = INT_NULL_SENTINEL;

        for (int i = 0; i < bs_rs->row_count; i++) {
            void **row    = bs_rs->rows[i];
            int    seat_id = *(int32_t *)row[BS_COL_SEAT_ID];

            WhereClause wc[2];
            wc[0].op = OP_EQ; strncpy(wc[0].col_name, "seat_id",
                              sizeof(wc[0].col_name));
            wc[0].value = &seat_id; wc[0].logic = 0;
            wc[1].op = OP_EQ; strncpy(wc[1].col_name, "show_id",
                              sizeof(wc[1].col_name));
            wc[1].value = &show_id; wc[1].logic = 0;

            db_update("seat_status", wc, 2, "status",          &avail_status);
            db_update("seat_status", wc, 2, "held_by_user_id", &null_int);
            db_update("seat_status", wc, 2, "booking_id",      &null_int);

            /* Clear held_until (DATE column — empty string signals NULL) */
            char empty[20] = "";
            db_update("seat_status", wc, 2, "held_until", empty);
        }
    }

    /* ── 4c. Credit wallet ── */
    if (r->refund_amount > 0.0f) {
        float current_balance = get_user_wallet(user_id);
        float new_balance     = current_balance + r->refund_amount;
        WhereClause w_u = { "user_id", OP_EQ, &user_id, 0 };
        int rows = db_update("users", &w_u, 1, "wallet_balance", &new_balance);
        if (rows <= 0) {
            fprintf(stderr, "[refund] execute_cancellation: "
                            "failed to credit wallet for user_id=%d.\n",
                    user_id);
            ok = 0;
            goto cleanup;
        }
    }

    /* ── 4d. Insert refunds row ── */
    {
        int new_refund_id = insert_refund_row(r);
        if (new_refund_id < 0) {
            fprintf(stderr, "[refund] execute_cancellation: "
                            "failed to insert refunds row.\n");
            ok = 0;
            goto cleanup;
        }
        r->refund_id = new_refund_id;
    }

cleanup:
    if (bs_rs) result_set_free(bs_rs);

    if (!ok) {
        wal_rollback();
        free(r);
        return 0;
    }

    /* ── 5. Commit ── */
    wal_commit();

    /* ── 6. Update session wallet cache ── */
    if (ctx && ctx->user_id == user_id) {
        ctx->wallet_balance += r->refund_amount;
    }

    /* ── 7. Post-commit: notify waitlist ── */
    notify_waitlist_for_show(show_id);

    free(r);
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * execute_admin_show_cancellation
 * ───────────────────────────────────────────────────────────────────────────*/
int execute_admin_show_cancellation(int show_id, SessionContext *ctx)
{
    /* Fetch all CONFIRMED bookings for this show */
    int sh_id_val   = show_id;
    int conf_status = BOOKING_STATUS_CONFIRMED;

    WhereClause wc[2];
    wc[0].op = OP_EQ;
    strncpy(wc[0].col_name, "show_id", sizeof(wc[0].col_name));
    wc[0].value = &sh_id_val; wc[0].logic = 0;
    wc[1].op = OP_EQ;
    strncpy(wc[1].col_name, "status", sizeof(wc[1].col_name));
    wc[1].value = &conf_status; wc[1].logic = 0;

    ResultSet *bk_rs = db_select("bookings", wc, 2, NULL, 0);
    if (!bk_rs) {
        fprintf(stderr, "[refund] execute_admin_show_cancellation: "
                        "db_select failed for show_id=%d.\n", show_id);
        return -1;
    }

    int count = 0;

    for (int i = 0; i < bk_rs->row_count; i++) {
        void **bk_row    = bk_rs->rows[i];
        int    booking_id = *(int32_t *)bk_row[BK_COL_BOOKING_ID];
        int    user_id    = *(int32_t *)bk_row[BK_COL_USER_ID];
        int    payment_id = *(int32_t *)bk_row[BK_COL_PAYMENT_ID];
        float  total      = *(float   *)bk_row[BK_COL_TOTAL_AMOUNT];

        /* Build a synthetic 100% RefundResult (convenience fee also refunded) */
        RefundResult r;
        memset(&r, 0, sizeof(r));
        r.refund_id                = 0;
        r.booking_id               = booking_id;
        r.payment_id               = payment_id;
        r.gross_paid               = total;
        r.convenience_fee_deducted = 0.0f;   /* admin cancel: fee returned too */
        r.refund_percentage        = 100;
        r.refund_amount            = total;  /* full amount including conv fee  */
        r.refund_type              = (int)REFUND_FULL;
        strncpy(r.reason, "Show cancelled by admin — full refund", 
                sizeof(r.reason) - 1);
        current_datetime_str(r.processed_at, sizeof(r.processed_at));

        /* Fetch seat list */
        int bk_id_val = booking_id;
        WhereClause w_bs = { "booking_id", OP_EQ, &bk_id_val, 0 };
        ResultSet *bs_rs = db_select("booking_seats", &w_bs, 1, NULL, 0);

        wal_begin();
        int ok = 1;

        /* Update booking status */
        {
            int cancelled = BOOKING_STATUS_CANCELLED;
            WhereClause w_bk = { "booking_id", OP_EQ, &bk_id_val, 0 };
            int rows = db_update("bookings", &w_bk, 1, "status", &cancelled);
            if (rows <= 0) { ok = 0; }

            char now_str[20];
            current_datetime_str(now_str, sizeof(now_str));
            db_update("bookings", &w_bk, 1, "cancelled_at", now_str);
        }

        /* Release seats */
        if (ok && bs_rs) {
            int avail    = SEAT_AVAILABLE;
            int null_int = INT_NULL_SENTINEL;
            char empty[20] = "";

            for (int j = 0; j < bs_rs->row_count; j++) {
                int seat_id = *(int32_t *)bs_rs->rows[j][BS_COL_SEAT_ID];

                WhereClause ws[2];
                ws[0].op = OP_EQ;
                strncpy(ws[0].col_name, "seat_id", sizeof(ws[0].col_name));
                ws[0].value = &seat_id; ws[0].logic = 0;
                ws[1].op = OP_EQ;
                strncpy(ws[1].col_name, "show_id", sizeof(ws[1].col_name));
                ws[1].value = &sh_id_val; ws[1].logic = 0;

                db_update("seat_status", ws, 2, "status",          &avail);
                db_update("seat_status", ws, 2, "held_by_user_id", &null_int);
                db_update("seat_status", ws, 2, "booking_id",      &null_int);
                db_update("seat_status", ws, 2, "held_until",      empty);
            }
        }

        /* Credit wallet */
        if (ok && r.refund_amount > 0.0f) {
            float cur  = get_user_wallet(user_id);
            float nbal = cur + r.refund_amount;
            WhereClause w_u = { "user_id", OP_EQ, &user_id, 0 };
            int rows = db_update("users", &w_u, 1, "wallet_balance", &nbal);
            if (rows <= 0) { ok = 0; }
        }

        /* Insert refund row */
        if (ok) {
            int rid = insert_refund_row(&r);
            if (rid < 0) { ok = 0; }
        }

        if (bs_rs) result_set_free(bs_rs);

        if (!ok) {
            wal_rollback();
            fprintf(stderr, "[refund] execute_admin_show_cancellation: "
                            "WAL rollback for booking_id=%d.\n", booking_id);
            continue;
        }

        wal_commit();

        /* Update session wallet if this booking belongs to current user */
        if (ctx && ctx->user_id == user_id) {
            ctx->wallet_balance += r.refund_amount;
        }

        count++;
    }

    result_set_free(bk_rs);

    /* Mark show as inactive */
    {
        int inactive = 0;
        WhereClause w_sh = { "show_id", OP_EQ, &sh_id_val, 0 };
        wal_begin();
        db_update("shows", &w_sh, 1, "is_active", &inactive);
        wal_commit();
    }

    /* Notify waitlist (all entries become EXPIRED since show is cancelled) */
    {
        int wl_show_val  = show_id;
        int wl_waiting   = WL_STATUS_WAITING;
        int wl_expired   = WL_STATUS_EXPIRED;
        WhereClause ww[2];
        ww[0].op = OP_EQ;
        strncpy(ww[0].col_name, "show_id", sizeof(ww[0].col_name));
        ww[0].value = &wl_show_val; ww[0].logic = 0;
        ww[1].op = OP_EQ;
        strncpy(ww[1].col_name, "status", sizeof(ww[1].col_name));
        ww[1].value = &wl_waiting; ww[1].logic = 0;

        wal_begin();
        db_update("waitlist", ww, 2, "status", &wl_expired);
        wal_commit();
    }

    return count;
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Static helpers
 * ═════════════════════════════════════════════════════════════════════════════*/

/* Sort PolicyTier descending by hours_before */
static int compare_tiers_desc(const void *a, const void *b)
{
    const PolicyTier *ta = (const PolicyTier *)a;
    const PolicyTier *tb = (const PolicyTier *)b;
    return tb->hours_before - ta->hours_before;
}

/* Parse "YYYY-MM-DD HH:MM" → time_t (local time via mktime) */
static time_t parse_show_datetime(const char *dt_str)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    int n = sscanf(dt_str, "%4d-%2d-%2d %2d:%2d",
                   &t.tm_year, &t.tm_mon, &t.tm_mday,
                   &t.tm_hour, &t.tm_min);
    if (n < 5) {
        /* Fallback: try date-only "YYYY-MM-DD" */
        sscanf(dt_str, "%4d-%2d-%2d", &t.tm_year, &t.tm_mon, &t.tm_mday);
        t.tm_hour = 0; t.tm_min = 0;
    }
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = -1;
    return mktime(&t);
}

/* Write current local time as "YYYY-MM-DD HH:MM" into out */
static void current_datetime_str(char *out, size_t len)
{
    time_t    now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min);
}

/* Return wallet_balance for user_id; 0.0f on any error */
static float get_user_wallet(int user_id)
{
    WhereClause w = { "user_id", OP_EQ, &user_id, 0 };
    ResultSet *rs = db_select("users", &w, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        if (rs) result_set_free(rs);
        return 0.0f;
    }
    float bal = *(float *)rs->rows[0][US_COL_WALLET_BALANCE];
    result_set_free(rs);
    return bal;
}

/*
 * insert_refund_row — build field_values array and call db_insert("refunds").
 * Returns new refund_id (>= 1) on success, -1 on failure.
 */
static int insert_refund_row(const RefundResult *r)
{
    Schema *s = get_schema("refunds");
    if (!s) {
        fprintf(stderr, "[refund] insert_refund_row: schema 'refunds' not loaded.\n");
        return -1;
    }

    /*
     * refunds schema column order (from decisions.md):
     * [0] refund_id          INT  PK (auto)
     * [1] booking_id         INT
     * [2] payment_id         INT
     * [3] gross_paid         FLOAT
     * [4] convenience_fee_deducted FLOAT
     * [5] refund_percentage  INT
     * [6] refund_amount      FLOAT
     * [7] refund_type        INT
     * [8] reason             CHAR(200)
     * [9] processed_at       DATE
     */

    /* We set refund_id to 0; db_insert auto-increments the PK. */
    int32_t  refund_id_val  = 0;
    int32_t  booking_id_val = (int32_t)r->booking_id;
    int32_t  payment_id_val = (int32_t)r->payment_id;
    float    gross_paid_val = r->gross_paid;
    float    conv_fee_val   = r->convenience_fee_deducted;
    int32_t  pct_val        = (int32_t)r->refund_percentage;
    float    amt_val        = r->refund_amount;
    int32_t  type_val       = (int32_t)r->refund_type;
    /* reason and processed_at are char arrays — pass as char* */

    void *fields[10];
    fields[RF_COL_REFUND_ID]                = &refund_id_val;
    fields[RF_COL_BOOKING_ID]               = &booking_id_val;
    fields[RF_COL_PAYMENT_ID]               = &payment_id_val;
    fields[RF_COL_GROSS_PAID]               = &gross_paid_val;
    fields[RF_COL_CONVENIENCE_FEE_DEDUCTED] = &conv_fee_val;
    fields[RF_COL_REFUND_PERCENTAGE]        = &pct_val;
    fields[RF_COL_REFUND_AMOUNT]            = &amt_val;
    fields[RF_COL_REFUND_TYPE]              = &type_val;
    fields[RF_COL_REASON]                   = (void *)r->reason;
    fields[RF_COL_PROCESSED_AT]             = (void *)r->processed_at;

    int new_id = db_insert("refunds", fields);
    return new_id;   /* db_insert returns -1 on failure, pk on success */
}

/*
 * notify_waitlist_for_show — flip the earliest WAITING entry to NOTIFIED and
 * increment unread_notifs on the waitlist user.
 *
 * Called after seat_status rows for a show are freed; only one user is
 * notified per cancellation event (they get a window to book the freed seat).
 * Returns 1 if a notification was issued, 0 otherwise.
 */
static int notify_waitlist_for_show(int show_id)
{
    int sh_val     = show_id;
    int wl_waiting = WL_STATUS_WAITING;

    WhereClause wc[2];
    wc[0].op = OP_EQ;
    strncpy(wc[0].col_name, "show_id", sizeof(wc[0].col_name));
    wc[0].value = &sh_val; wc[0].logic = 0;
    wc[1].op = OP_EQ;
    strncpy(wc[1].col_name, "status", sizeof(wc[1].col_name));
    wc[1].value = &wl_waiting; wc[1].logic = 0;

    ResultSet *rs = db_select("waitlist", wc, 2, NULL, 0);
    if (!rs || rs->row_count == 0) {
        if (rs) result_set_free(rs);
        return 0;
    }

    /* Find the earliest entry by added_at (lexicographic ISO date compare) */
    int earliest_idx = 0;
    for (int i = 1; i < rs->row_count; i++) {
        const char *t1 = (char *)rs->rows[earliest_idx][WL_COL_ADDED_AT];
        const char *t2 = (char *)rs->rows[i][WL_COL_ADDED_AT];
        if (strcmp(t2, t1) < 0) earliest_idx = i;
    }

    void  **wl_row     = rs->rows[earliest_idx];
    int32_t wl_id      = *(int32_t *)wl_row[WL_COL_WAITLIST_ID];
    int32_t notify_uid = *(int32_t *)wl_row[WL_COL_USER_ID];

    result_set_free(rs);

    /* Update waitlist row: status=NOTIFIED, notified_at=now */
    int notified = WL_STATUS_NOTIFIED;
    char now_str[20];
    current_datetime_str(now_str, sizeof(now_str));

    WhereClause w_wl = { "waitlist_id", OP_EQ, &wl_id, 0 };

    /* db_update uses wal_begin_nested internally — no outer transaction needed here */
    db_update("waitlist", &w_wl, 1, "status",       &notified);
    db_update("waitlist", &w_wl, 1, "notified_at",  now_str);

    /* Increment unread_notifs for the notified user */
    {
        WhereClause w_u = { "user_id", OP_EQ, &notify_uid, 0 };
        ResultSet *u_rs = db_select("users", &w_u, 1, NULL, 0);
        if (u_rs && u_rs->row_count > 0) {
            /* unread_notifs is not in the schema constants above —
             * fetch current value by column name "unread_notifs".
             * We rely on schema order matching decisions.md: it is not a
             * static column here, so we scan columns by name. */
            Schema *us = get_schema("users");
            int notif_col = -1;
            if (us) {
                for (int c = 0; c < us->col_count; c++) {
                    if (strcmp(us->columns[c].name, "unread_notifs") == 0) {
                        notif_col = c;
                        break;
                    }
                }
            }
            if (notif_col >= 0) {
                int32_t cur_notifs = *(int32_t *)u_rs->rows[0][notif_col];
                int32_t new_notifs = cur_notifs + 1;
                db_update("users", &w_u, 1, "unread_notifs", &new_notifs);
            }
            result_set_free(u_rs);
        } else {
            if (u_rs) result_set_free(u_rs);
        }
    }

    return 1;
}
