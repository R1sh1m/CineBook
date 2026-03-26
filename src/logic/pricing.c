#include "pricing.h"
#include "query.h"
#include "session.h"
#include "auth.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: Parse "YYYY-MM-DD HH:MM" into day-of-week and hour
 * ───────────────────────────────────────────────────────────────────────────*/
static void parse_show_datetime(const char *datetime_str,
                                int *out_day_of_week,
                                int *out_hour) {
    struct tm tm_info;
    time_t t;
    int year, month, day, hour, minute;

    if (!datetime_str || strlen(datetime_str) < 16) {
        *out_day_of_week = 0;
        *out_hour = 0;
        return;
    }

    /* Parse "YYYY-MM-DD HH:MM" */
    if (sscanf(datetime_str, "%d-%d-%d %d:%d",
               &year, &month, &day, &hour, &minute) != 5) {
        *out_day_of_week = 0;
        *out_hour = 0;
        return;
    }

    /* Convert to time_t via mktime */
    memset(&tm_info, 0, sizeof(tm_info));
    tm_info.tm_year = year - 1900;
    tm_info.tm_mon = month - 1;
    tm_info.tm_mday = day;
    tm_info.tm_hour = hour;
    tm_info.tm_min = minute;
    tm_info.tm_sec = 0;
    tm_info.tm_isdst = -1;

    t = mktime(&tm_info);
    if (t == (time_t)-1) {
        *out_day_of_week = 0;
        *out_hour = 0;
        return;
    }

    struct tm *result = localtime(&t);
    if (!result) {
        *out_day_of_week = 0;
        *out_hour = 0;
        return;
    }

    *out_day_of_week = result->tm_wday;  /* 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat */
    *out_hour = hour;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: Check if day-of-week is Friday (5), Saturday (6), or Sunday (0)
 * ───────────────────────────────────────────────────────────────────────────*/
static int is_weekend(int day_of_week) {
    return day_of_week == 0 || day_of_week == 5 || day_of_week == 6;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: Check if hour is in peak time (18:00–23:00 = hours 18, 19, ..., 23)
 * ───────────────────────────────────────────────────────────────────────────*/
static int is_peak_hour(int hour) {
    return hour >= 18 && hour <= 23;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Main function: compute_price_breakdown
 * ───────────────────────────────────────────────────────────────────────────*/
void compute_price_breakdown(int show_id, int *seat_ids, int seat_count,
                             SessionContext *ctx) {
    if (!ctx || !seat_ids || seat_count <= 0) {
        return;
    }

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 1: Query shows table to get base_price, screen_id, show_datetime
     * ────────────────────────────────────────────────────────────────────────*/
    WhereClause where_show;
    strcpy(where_show.col_name, "show_id");
    where_show.op = OP_EQ;
    where_show.value = &show_id;
    where_show.logic = 0;

    ResultSet *rs_show = db_select("shows", &where_show, 1, NULL, 0);
    if (!rs_show || rs_show->row_count == 0) {
        if (rs_show) result_set_free(rs_show);
        return;
    }

    float base_price = *((float *)rs_show->rows[0][4]);  /* col 4: base_price */
    int screen_id = *((int *)rs_show->rows[0][2]);       /* col 2: screen_id */
    char *show_datetime = (char *)rs_show->rows[0][3];   /* col 3: show_datetime */

    /* Make a copy of show_datetime since ResultSet will be freed */
    char datetime_copy[40];
    strcpy(datetime_copy, show_datetime);

    result_set_free(rs_show);

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 2: Query screens table to get screen_type and total_seats
     * ────────────────────────────────────────────────────────────────────────*/
    WhereClause where_screen;
    strcpy(where_screen.col_name, "screen_id");
    where_screen.op = OP_EQ;
    where_screen.value = &screen_id;
    where_screen.logic = 0;

    ResultSet *rs_screen = db_select("screens", &where_screen, 1, NULL, 0);
    if (!rs_screen || rs_screen->row_count == 0) {
        if (rs_screen) result_set_free(rs_screen);
        return;
    }

    int screen_type = *((int *)rs_screen->rows[0][3]);    /* col 3: screen_type */
    int total_seats = *((int *)rs_screen->rows[0][4]);    /* col 4: total_seats */

    result_set_free(rs_screen);

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 3: Query first seat for seat_type (used for all seats' pricing)
     * ────────────────────────────────────────────────────────────────────────*/
    int first_seat_id = seat_ids[0];
    WhereClause where_seat;
    strcpy(where_seat.col_name, "seat_id");
    where_seat.op = OP_EQ;
    where_seat.value = &first_seat_id;
    where_seat.logic = 0;

    ResultSet *rs_seat = db_select("seats", &where_seat, 1, NULL, 0);
    if (!rs_seat || rs_seat->row_count == 0) {
        if (rs_seat) result_set_free(rs_seat);
        return;
    }

    int seat_type = *((int *)rs_seat->rows[0][4]);  /* col 4: seat_type */

    result_set_free(rs_seat);

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 4: Compute slot [0]: base_price
     * ────────────────────────────────────────────────────────────────────────*/
    ctx->current_price_breakdown[0] = base_price;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 5: Compute slot [1]: screen_surcharge
     * ────────────────────────────────────────────────────────────────────────*/
    float screen_surcharge = 0.0f;
    switch (screen_type) {
        case 0:  /* 2D */
            screen_surcharge = 0.0f;
            break;
        case 1:  /* IMAX_2D */
            screen_surcharge = 80.0f;
            break;
        case 2:  /* IMAX_3D */
            screen_surcharge = 130.0f;
            break;
        case 3:  /* 4DX */
            screen_surcharge = 200.0f;
            break;
        default:
            screen_surcharge = 0.0f;
            break;
    }
    ctx->current_price_breakdown[1] = screen_surcharge;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 6: Compute slot [2]: seat_type_mult
     * ────────────────────────────────────────────────────────────────────────*/
    float seat_type_mult = 1.0f;
    switch (seat_type) {
        case 0:  /* STANDARD */
            seat_type_mult = 1.0f;
            break;
        case 1:  /* PREMIUM */
            seat_type_mult = 1.5f;
            break;
        case 2:  /* RECLINER */
            seat_type_mult = 2.2f;
            break;
        default:
            seat_type_mult = 1.0f;
            break;
    }
    ctx->current_price_breakdown[2] = seat_type_mult;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 7: Compute slot [3]: subtotal_per_seat
     * ────────────────────────────────────────────────────────────────────────*/
    float subtotal_per_seat = (base_price + screen_surcharge) * seat_type_mult;
    ctx->current_price_breakdown[3] = subtotal_per_seat;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 8: Compute slot [4]: student_discount
     * ────────────────────────────────────────────────────────────────────────*/
    float student_discount = 0.0f;
    if (ctx->role == ROLE_STUDENT) {
        student_discount = subtotal_per_seat * 0.12f;  /* 12% of subtotal */
    }
    ctx->current_price_breakdown[4] = student_discount;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 9: Compute slot [5]: group_discount
     * ────────────────────────────────────────────────────────────────────────*/
    float group_discount = 0.0f;
    if (seat_count >= 6) {
        group_discount = subtotal_per_seat * 0.08f;  /* 8% of subtotal */
    }
    ctx->current_price_breakdown[5] = group_discount;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 10: Compute slot [6]: promo_discount (always 0 here; promos.c fills)
     * ────────────────────────────────────────────────────────────────────────*/
    ctx->current_price_breakdown[6] = 0.0f;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 11: Compute slot [7]: dynamic_surge
     * Conditions: weekend +10%, peak_hour +15%, occupancy >70% +20%
     * ────────────────────────────────────────────────────────────────────────*/
    float surge_percent = 0.0f;

    /* Parse show_datetime to extract day-of-week and hour */
    int day_of_week, hour;
    parse_show_datetime(datetime_copy, &day_of_week, &hour);

    /* Weekend surcharge */
    if (is_weekend(day_of_week)) {
        surge_percent += 0.10f;  /* +10% */
    }

    /* Peak hour surcharge */
    if (is_peak_hour(hour)) {
        surge_percent += 0.15f;  /* +15% */
    }

    /* Occupancy surcharge: count confirmed seats (status=2) for this show */
    WhereClause where_occ[2];
    strcpy(where_occ[0].col_name, "show_id");
    where_occ[0].op = OP_EQ;
    where_occ[0].value = &show_id;
    where_occ[0].logic = 0;

    int status_confirmed = 2;
    strcpy(where_occ[1].col_name, "status");
    where_occ[1].op = OP_EQ;
    where_occ[1].value = &status_confirmed;
    where_occ[1].logic = 0;

    int confirmed_count = db_count("seat_status", where_occ, 2);
    float occupancy_ratio = (total_seats > 0) ? (float)confirmed_count / (float)total_seats : 0.0f;

    if (occupancy_ratio > 0.70f) {
        surge_percent += 0.20f;  /* +20% */
    }

    float discounted_sub = subtotal_per_seat - student_discount - group_discount;
    float dynamic_surge  = discounted_sub * surge_percent;
    ctx->current_price_breakdown[7] = dynamic_surge;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 12: Compute slot [8]: taxable_amount
     * Formula: (subtotal + surge - all_discounts) × seat_count
     * ────────────────────────────────────────────────────────────────────────*/
    float taxable_per_seat = subtotal_per_seat
                           + dynamic_surge
                           - student_discount
                           - group_discount
                           - ctx->current_price_breakdown[6];  /* promo = 0.0 for now */

    float taxable_amount = taxable_per_seat * (float)seat_count;
    ctx->current_price_breakdown[8] = taxable_amount;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 13: Compute slot [9]: gst (18% of taxable_amount)
     * ────────────────────────────────────────────────────────────────────────*/
    float gst = taxable_amount * 0.18f;
    ctx->current_price_breakdown[9] = gst;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 14: Compute slot [10]: convenience_fee (₹30 per seat, non-refundable)
     * ────────────────────────────────────────────────────────────────────────*/
    float convenience_fee = 30.0f * (float)seat_count;
    ctx->current_price_breakdown[10] = convenience_fee;

    /* ──────────────────────────────────────────────────────────────────────────
     * Step 15: Compute slot [11]: grand_total
     * ────────────────────────────────────────────────────────────────────────*/
    float grand_total = taxable_amount + gst + convenience_fee;
    ctx->current_price_breakdown[11] = grand_total;
}