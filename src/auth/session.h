#ifndef SESSION_H
#define SESSION_H

#include <time.h>
#include "auth.h"   /* UserRole */

/* ─────────────────────────────────────────────────────────────────────────────
 * SessionContext
 * Allocated once in main.c, passed by pointer to every function in the system.
 * Single source of truth for the currently logged-in user's state.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct SessionContext {
    /* ── identity ── */
    int       user_id;                      /* 0 = not logged in              */
    UserRole  role;                          /* GUEST / USER / STUDENT / ADMIN */
    char      name[100];
    char      email[151];                    /* may be empty string            */
    int       is_admin;                      /* 1 if role == ROLE_ADMIN        */

    /* ── location & currency ── */
    int       preferred_city_id;
    char      currency_sym[4];              /* "₹" = 3 UTF-8 bytes + NUL      */

    /* ── wallet ── */
    float     wallet_balance;

    /* ── seat hold ── */
    int      *held_seat_ids;                /* heap-allocated array; NULL=none */
    int       held_seat_count;
    time_t    hold_started_at;

    /* ── active booking flow ── */
    int       selected_show_id;
    int       active_payment_id;
    float     current_price_breakdown[12];

    /* ── notifications ── */
    int       unread_notifs;

    /* ── admin dashboard state ── */
    int       dashboard_days;              /* look-back window (default 30)   */
    int       dashboard_theatre_filter;    /* 0 = all theatres, N = specific  */
} SessionContext;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/
void        session_init(SessionContext *ctx);
void        session_set_user(SessionContext *ctx, int user_id);
void        session_clear(SessionContext *ctx);
const char *session_get_currency_sym(const SessionContext *ctx);

#endif /* SESSION_H */