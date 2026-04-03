/*
 * session.c — SessionContext lifecycle for CineBook
 * C11. No external libraries beyond the standard C library.
 *
 * Column index reference (from decisions.md):
 *   users:     user_id(0) name(1) phone(2) email(3) password_hash(4)
 *              role(5) wallet_balance(6) preferred_city_id(7) country_id(8)
 *              created_at(9) is_active(10)
 *   countries: country_id(0) name(1) currency_sym(2) currency_code(3)
 *   waitlist:  waitlist_id(0) user_id(1) show_id(2) seat_count_requested(3)
 *              added_at(4) status(5) notified_at(6)
 *   seat_status: status_id(0) show_id(1) seat_id(2) status(3)
 *                held_by_user_id(4) held_until(5) booking_id(6)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session.h"    /* brings in auth.h (UserRole) */
#include "query.h"      /* db_select, db_update, result_set_free, WhereClause */
#include "storage.h"    /* storage_flush_all */

/* Default currency symbol when lookup fails */
#define DEFAULT_CURRENCY_SYM  "\xe2\x82\xb9"  /* UTF-8 for ₹ */

/* Waitlist status value: 1 = NOTIFIED */
#define WAITLIST_STATUS_NOTIFIED  1

/* seat_status.status value: 1 = HELD */
#define SEAT_STATUS_HELD      1
#define SEAT_STATUS_AVAILABLE 0

/* ─────────────────────────────────────────────────────────────────────────────
 * session_init
 * ───────────────────────────────────────────────────────────────────────────*/
void session_init(SessionContext *ctx)
{
    if (!ctx) return;

    memset(ctx, 0, sizeof(SessionContext));

    ctx->role                   = ROLE_GUEST;
    ctx->dashboard_days         = 30;
    ctx->dashboard_theatre_filter = 0;

    /* Default currency symbol */
    strncpy(ctx->currency_sym, DEFAULT_CURRENCY_SYM, sizeof(ctx->currency_sym) - 1);
    ctx->currency_sym[sizeof(ctx->currency_sym) - 1] = '\0';
}

/* ─────────────────────────────────────────────────────────────────────────────
 * session_set_user
 * ───────────────────────────────────────────────────────────────────────────*/
void session_set_user(SessionContext *ctx, int user_id)
{
    if (!ctx || user_id <= 0) return;

    /* ── 1. Load user row ── */
    int uid_val = user_id;
    WhereClause where_user[1];
    memset(where_user, 0, sizeof(where_user));
    strncpy(where_user[0].col_name, "user_id", sizeof(where_user[0].col_name) - 1);
    where_user[0].op    = OP_EQ;
    where_user[0].value = &uid_val;
    where_user[0].logic = 0;

    ResultSet *rs_user = db_select("users", where_user, 1, NULL, 0);
    if (!rs_user || rs_user->row_count < 1) {
        if (rs_user) result_set_free(rs_user);
        return;
    }

    void **row = rs_user->row_count > 0 ? rs_user->rows[0] : NULL;
    if (!row) {
        result_set_free(rs_user);
        return;
    }

    /* Column indices per decisions.md */
    /* [0]=user_id [1]=name [2]=phone [3]=email [4]=password_hash
       [5]=role    [6]=wallet_balance [7]=preferred_city_id [8]=country_id
       [9]=created_at [10]=is_active */

    ctx->user_id = *(int *)row[0];

    /* name */
    strncpy(ctx->name, (char *)row[1], sizeof(ctx->name) - 1);
    ctx->name[sizeof(ctx->name) - 1] = '\0';

    /* email (nullable — check for NULL / empty) */
    ctx->email[0] = '\0';
    if (row[3] && !field_is_null_char((char *)row[3])) {
        strncpy(ctx->email, (char *)row[3], sizeof(ctx->email) - 1);
        ctx->email[sizeof(ctx->email) - 1] = '\0';
    }

    /* role */
    ctx->role = (UserRole)(*(int *)row[5]);

    /* wallet_balance */
    ctx->wallet_balance = *(float *)row[6];

    /* preferred_city_id */
    ctx->preferred_city_id = *(int *)row[7];

    /* convenience flag */
    ctx->is_admin = (ctx->role == ROLE_ADMIN) ? 1 : 0;

    /* country_id — needed for currency lookup */
    int country_id = *(int *)row[8];

    result_set_free(rs_user);

    /* ── 2. Currency symbol from countries table ── */
    strncpy(ctx->currency_sym, DEFAULT_CURRENCY_SYM, sizeof(ctx->currency_sym) - 1);
    ctx->currency_sym[sizeof(ctx->currency_sym) - 1] = '\0';

    if (country_id > 0) {
        WhereClause where_country[1];
        memset(where_country, 0, sizeof(where_country));
        strncpy(where_country[0].col_name, "country_id",
                sizeof(where_country[0].col_name) - 1);
        where_country[0].op    = OP_EQ;
        where_country[0].value = &country_id;
        where_country[0].logic = 0;

        ResultSet *rs_country = db_select("countries", where_country, 1, NULL, 0);
        if (rs_country && rs_country->row_count >= 1 && rs_country->rows[0]) {
            /* countries: [0]=country_id [1]=name [2]=currency_sym [3]=currency_code */
            void **crow = rs_country->rows[0];
            if (crow[2] && !field_is_null_char((char *)crow[2])) {
                strncpy(ctx->currency_sym, (char *)crow[2],
                        sizeof(ctx->currency_sym) - 1);
                ctx->currency_sym[sizeof(ctx->currency_sym) - 1] = '\0';
            }
        }
        if (rs_country) result_set_free(rs_country);
    }

    /* ── 3. Unread notifications — waitlist rows WHERE user_id=? AND status=1 ── */
    int notif_status = WAITLIST_STATUS_NOTIFIED;
    WhereClause where_notif[2];
    memset(where_notif, 0, sizeof(where_notif));

    strncpy(where_notif[0].col_name, "user_id",
            sizeof(where_notif[0].col_name) - 1);
    where_notif[0].op    = OP_EQ;
    where_notif[0].value = &ctx->user_id;
    where_notif[0].logic = 0;

    strncpy(where_notif[1].col_name, "status",
            sizeof(where_notif[1].col_name) - 1);
    where_notif[1].op    = OP_EQ;
    where_notif[1].value = &notif_status;
    where_notif[1].logic = 0;

    int notif_count = db_count("waitlist", where_notif, 2);
    ctx->unread_notifs = (notif_count > 0) ? notif_count : 0;

    /* ── 4. Clear seat hold fields ── */
    ctx->held_seat_ids   = NULL;
    ctx->held_seat_count = 0;
    ctx->hold_started_at = 0;

    /* ── 5. Clear transient booking flow fields ── */
    ctx->selected_show_id   = 0;
    ctx->active_payment_id  = 0;
    memset(ctx->current_price_breakdown, 0,
           sizeof(ctx->current_price_breakdown));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * session_clear
 * ───────────────────────────────────────────────────────────────────────────*/
void session_clear(SessionContext *ctx)
{
    if (!ctx) return;

    /* ── 1. Release any held seats in the database ── */
    if (ctx->user_id > 0 && ctx->held_seat_count >= 0) {
        int held_status    = SEAT_STATUS_HELD;
        int avail_status   = SEAT_STATUS_AVAILABLE;

        WhereClause where_held[2];
        memset(where_held, 0, sizeof(where_held));

        strncpy(where_held[0].col_name, "held_by_user_id",
                sizeof(where_held[0].col_name) - 1);
        where_held[0].op    = OP_EQ;
        where_held[0].value = &ctx->user_id;
        where_held[0].logic = 0;

        strncpy(where_held[1].col_name, "status",
                sizeof(where_held[1].col_name) - 1);
        where_held[1].op    = OP_EQ;
        where_held[1].value = &held_status;
        where_held[1].logic = 0;

        /* Set status = AVAILABLE (0) for all seats held by this user */
        db_update("seat_status", where_held, 2, "status", &avail_status);
        
        /* CRITICAL FIX: Flush all dirty pages to disk immediately to prevent
         * race condition where next user reads stale seat_status from disk */
        storage_flush_all();
    }

    /* ── 2. Free heap-allocated seat id array ── */
    if (ctx->held_seat_ids) {
        free(ctx->held_seat_ids);
        ctx->held_seat_ids = NULL;
    }

    /* ── 3. Zero everything and restore safe defaults ── */
    session_init(ctx);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * session_get_currency_sym
 * ───────────────────────────────────────────────────────────────────────────*/
const char *session_get_currency_sym(const SessionContext *ctx)
{
    if (!ctx) return DEFAULT_CURRENCY_SYM;
    return ctx->currency_sym;
}