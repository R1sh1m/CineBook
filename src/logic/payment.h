#ifndef PAYMENT_H
#define PAYMENT_H

#include "session.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * payment.h — Payment processing module for CineBook
 *
 * Handles all payment methods (WALLET, UPI, CARD, NETBANKING) and wallet top-ups.
 * Integrates with the query engine (db_insert, db_update) and WAL system.
 * All simulated payments succeed after spinner delay (no real transactions).
 * ───────────────────────────────────────────────────────────────────────────*/

typedef enum {
    PAY_WALLET    = 0,
    PAY_UPI       = 1,
    PAY_CARD      = 2,
    PAY_NETBANKING = 3
} PaymentMethod;

typedef enum {
    PAY_STATUS_PENDING   = 0,
    PAY_STATUS_SUCCESS   = 1,
    PAY_STATUS_FAILED    = 2,
    PAY_STATUS_REFUNDED  = 3,
    PAY_STATUS_RETRY     = 4
} PaymentStatus;

void print_payment_menu(const SessionContext *ctx, float total);
int process_payment(int booking_id, PaymentMethod method, SessionContext *ctx);
int wallet_topup(SessionContext *ctx);

#endif /* PAYMENT_H */