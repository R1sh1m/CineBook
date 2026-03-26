#ifndef PROMOS_H
#define PROMOS_H

#include "query.h"
#include "session.h"
#include "auth.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Promo struct — one promo code row from promos table
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int   promo_id;
    char  code[20];
    int   discount_type;           /* 0=PERCENT, 1=FLAT */
    float discount_value;
    float max_discount_cap;        /* -1 or 0 = no cap; otherwise INR limit */
    int   min_seats;
    int   role_mask;               /* bitmask: bit0=USER(1), bit1=STUDENT(2), bit2=ADMIN(4) */
    char  valid_from[20];          /* YYYY-MM-DD */
    char  valid_until[20];         /* YYYY-MM-DD */
    int   max_uses;                /* 0 = unlimited; >0 = max redemptions */
    int   current_uses;
    int   is_active;               /* 1 = active, 0 = deactivated */
} Promo;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * validate_promo — Case-insensitive promo code lookup with multi-condition check.
 *
 * Queries promos table WHERE code (uppercased) matches input (uppercased).
 * Validates:
 *   - is_active == 1
 *   - current date (YYYY-MM-DD) between valid_from and valid_until (inclusive)
 *   - current_uses < max_uses (if max_uses > 0)
 *   - seat_count >= min_seats
 *   - (role_mask >> role) & 1 == 1 (role is allowed)
 *
 * Returns heap-allocated Promo* on success, NULL on any validation failure.
 * Prints diagnostic message on each failure.
 * Caller must free() the returned pointer.
 */
Promo *validate_promo(const char *code, UserRole role, int seat_count,
                      SessionContext *ctx);

/*
 * apply_promo — Compute discount amount (INR) given base price and promo rules.
 *
 * If PERCENT (0):
 *   discount = base_amount × (discount_value / 100)
 *   if max_discount_cap > 0 and discount > cap, cap at max_discount_cap
 *
 * If FLAT (1):
 *   discount = discount_value, but capped at base_amount (cannot exceed total)
 *
 * Returns discount amount in INR (>= 0, <= base_amount).
 */
float apply_promo(const Promo *promo, float base_amount);

/*
 * increment_promo_uses — Increment current_uses counter by 1 after booking confirmed.
 *
 * Reads current value from promos table, adds 1, writes back via db_update.
 * Wrapped in wal_begin / wal_commit.
 */
void increment_promo_uses(int promo_id);

/*
 * create_promo — Admin: interactive prompt to create a new promo code.
 *
 * Prompts for all fields:
 *   - code (20-char max, case-insensitive storage: uppercased)
 *   - discount_type (0=PERCENT / 1=FLAT)
 *   - discount_value (amount or percent)
 *   - max_discount_cap (for PERCENT type; 0 = no cap)
 *   - min_seats (minimum seat count to qualify)
 *   - role_mask (bitmask: 1=USER, 2=STUDENT, 4=ADMIN; sum for multiple)
 *   - valid_from (YYYY-MM-DD)
 *   - valid_until (YYYY-MM-DD)
 *   - max_uses (0 = unlimited)
 *   - is_active (1=yes / 0=no)
 *
 * Auto-generates promo_id, sets current_uses=0.
 * Inserts row into promos table.
 */
void create_promo(void);

/*
 * deactivate_promo — Admin: set is_active=0 for a promo code.
 *
 * Updates promos SET is_active=0 WHERE promo_id=promo_id.
 */
void deactivate_promo(int promo_id);

#endif /* PROMOS_H */