#ifndef PRICING_H
#define PRICING_H

#include "session.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * pricing.h — Price breakdown engine for CineBook
 *
 * Computes the complete 12-slot price breakdown for a booking:
 * [0]  base_price (from shows.base_price)
 * [1]  screen_surcharge (2D=0, IMAX_2D=+80, IMAX_3D=+130, 4DX=+200)
 * [2]  seat_type_mult (STANDARD=1.0, PREMIUM=1.5, RECLINER=2.2)
 * [3]  subtotal_per_seat ((base + surcharge) × mult)
 * [4]  student_discount (-12% if ROLE_STUDENT)
 * [5]  group_discount (-8% if seat_count >= 6)
 * [6]  promo_discount (set to 0.0; promos.c fills this)
 * [7]  dynamic_surge (weekend +10%, peak +15%, occupancy>70% +20%)
 * [8]  taxable_amount ((subtotal + surge - discounts) × seat_count)
 * [9]  gst (18% of taxable_amount)
 * [10] convenience_fee (₹30 × seat_count)
 * [11] grand_total (taxable + gst + fee)
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * compute_price_breakdown — Calculate full 12-slot price breakdown.
 *
 * Queries shows, screens, seats, and seat_status to build a complete pricing
 * breakdown. Fills ctx->current_price_breakdown[12] in-place with float values.
 *
 * Parameters:
 *   show_id    — PK from shows table
 *   seat_ids   — array of seat_id values being booked
 *   seat_count — length of seat_ids array (must be > 0)
 *   ctx        — SessionContext; contains role for discounts, modified in-place
 *
 * Notes:
 *   - Uses first seat's type for seat_type_mult (no averaging)
 *   - Dynamic surge computed from show_datetime and current occupancy
 *   - Promo discount [6] always 0.0 here; promos.c updates post-selection
 *   - Does not validate holds, payment, or seat availability
 *   - All amounts in INR (₹)
 */
void compute_price_breakdown(int show_id, int *seat_ids, int seat_count,
                             SessionContext *ctx);

#endif /* PRICING_H */