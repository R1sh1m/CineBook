/*
 * ui_booking.c — Seat selection, booking flow, payment, and booking history.
 *
 * Conversation 17 of 22.
 * Depends on: query.h, session.h, pricing.h, payment.h, promos.h, txn.h
 * Compiled with: gcc -Wall -Wextra -std=c11 -c ui_booking.c -o ui_booking.o
 *
 * Public surface:
 *   run_booking_flow(show_id, ctx)   — called from ui_browse.c
 *   view_upcoming_bookings(ctx)
 *   view_past_bookings(ctx)
 *
 * All static helpers (render_seat_map_interactive, hold_seats, release_holds,
 * display_price_breakdown, build_now_str) are internal to this file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "query.h"      /* db_select/insert/update, ResultSet, WhereClause  */
#include "session.h"    /* SessionContext                                    */
#include "pricing.h"    /* compute_price_breakdown                           */
#include "payment.h"    /* process_payment, PaymentMethod, PAY_STATUS_*     */
#include "promos.h"     /* validate_promo, apply_promo, increment_promo_uses */
#include "txn.h"        /* wal_begin, wal_commit, wal_rollback               */

/* ═══════════════════════════════════════════════════════════════════════════
 * Compile-time limits
 * ═══════════════════════════════════════════════════════════════════════════*/
#define MAX_SEATS_PER_SCREEN  512
#define MAX_SEL_SEATS         20
#define ROW_LABEL_MAX         4
#define LINE_WIDTH            76
#define BOX_INNER             56

/* ═══════════════════════════════════════════════════════════════════════════
 * ANSI colour helpers
 * ═══════════════════════════════════════════════════════════════════════════*/
#define ANSI_RESET  "\033[0m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_BLUE   "\033[34m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BOLD   "\033[1m"

/* ═══════════════════════════════════════════════════════════════════════════
 * SeatCell — in-memory representation of one seat during a booking session
 * ═══════════════════════════════════════════════════════════════════════════*/
typedef struct {
    int  seat_id;
    char row_label[ROW_LABEL_MAX];
    int  seat_number;
    int  seat_type;        /* 0=STANDARD 1=PREMIUM 2=RECLINER */
    int  is_active;        /* from seats.is_active */
    int  status;           /* from seat_status: 0=AVAIL 1=HELD 2=CONF 3=CANC */
    int  held_by_user_id;
    int  sel_order;        /* 0 = not selected, N = Nth user pick */
} SeatCell;

/* ═══════════════════════════════════════════════════════════════════════════
 * Generic helpers
 * ═══════════════════════════════════════════════════════════════════════════*/
static void hr(void)
{
    for (int i = 0; i < LINE_WIDTH; i++) putchar('-');
    putchar('\n');
}

static void banner(const char *t)
{
    putchar('\n');
    for (int i = 0; i < LINE_WIDTH; i++) putchar('=');
    printf("\n  %s\n", t);
    for (int i = 0; i < LINE_WIDTH; i++) putchar('=');
    putchar('\n');
}

static void read_line(char *buf, int size)
{
    if (fgets(buf, size, stdin)) {
        int len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
}

static const char *safe_str(void ***rows, int r, int c)
{
    if (!rows || !rows[r] || !rows[r][c]) return "";
    return (const char *)rows[r][c];
}
static int safe_int(void ***rows, int r, int c)
{
    if (!rows || !rows[r] || !rows[r][c]) return 0;
    return *(const int *)rows[r][c];
}
static float safe_float(void ***rows, int r, int c)
{
    if (!rows || !rows[r] || !rows[r][c]) return 0.0f;
    return *(const float *)rows[r][c];
}

static void time_to_str(time_t t, char *buf, size_t sz)
{
    struct tm *ti = localtime(&t);
    strftime(buf, sz, "%Y-%m-%d %H:%M", ti);
}

static time_t parse_datetime_str(const char *s)
{
    if (!s || strlen(s) < 16) return (time_t)-1;
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (sscanf(s, "%d-%d-%d %d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min) != 5)
        return (time_t)-1;
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = -1;
    return mktime(&t);
}

static void build_now_str(char *buf, size_t sz)
{
    time_t now = time(NULL);
    time_to_str(now, buf, sz);
}

static const char *screen_type_label(int t)
{
    switch (t) {
        case 0: return "2D";
        case 1: return "IMAX 2D";
        case 2: return "IMAX 3D";
        case 3: return "4DX";
        default: return "?";
    }
}

static const char *seat_type_label(int t)
{
    switch (t) {
        case 0: return "STANDARD";
        case 1: return "PREMIUM";
        case 2: return "RECLINER";
        default: return "STANDARD";
    }
}

static void format_currency_comma(float v, char *out, size_t sz)
{
    if (!out || sz == 0) return;

    long long paise = (long long)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int is_neg = (paise < 0);
    unsigned long long abs_paise =
        (unsigned long long)(is_neg ? -paise : paise);
    unsigned long long whole = abs_paise / 100ULL;
    unsigned int frac = (unsigned int)(abs_paise % 100ULL);

    char digits[32];
    snprintf(digits, sizeof(digits), "%llu", whole);

    char with_commas[48];
    int dlen = (int)strlen(digits);
    int commas = (dlen - 1) / 3;
    int wlen = dlen + commas;
    if (wlen >= (int)sizeof(with_commas)) wlen = (int)sizeof(with_commas) - 1;
    with_commas[wlen] = '\0';

    int di = dlen - 1;
    int wi = wlen - 1;
    int group = 0;
    while (di >= 0 && wi >= 0) {
        with_commas[wi--] = digits[di--];
        group++;
        if (group == 3 && di >= 0 && wi >= 0) {
            with_commas[wi--] = ',';
            group = 0;
        }
    }

    snprintf(out, sz, "%s%s.%02u", is_neg ? "-" : "", with_commas, frac);
}

static void format_int_comma(long long v, char *out, size_t sz)
{
    if (!out || sz == 0) return;

    int is_neg = (v < 0);
    unsigned long long abs_v = (unsigned long long)(is_neg ? -v : v);

    char digits[32];
    snprintf(digits, sizeof(digits), "%llu", abs_v);

    char with_commas[48];
    int dlen = (int)strlen(digits);
    int commas = (dlen - 1) / 3;
    int wlen = dlen + commas;
    if (wlen >= (int)sizeof(with_commas)) wlen = (int)sizeof(with_commas) - 1;
    with_commas[wlen] = '\0';

    int di = dlen - 1;
    int wi = wlen - 1;
    int group = 0;
    while (di >= 0 && wi >= 0) {
        with_commas[wi--] = digits[di--];
        group++;
        if (group == 3 && di >= 0 && wi >= 0) {
            with_commas[wi--] = ',';
            group = 0;
        }
    }

    snprintf(out, sz, "%s%s", is_neg ? "-" : "", with_commas);
}

static void print_receipt_text_line(const char *text)
{
    char line[57];
    snprintf(line, sizeof(line), "%-56.56s", text ? text : "");
    printf("  ║%s║\n", line);
}

static void print_receipt_amount_line(const char *label, float amount, int negative)
{
    char amt[32];
    char right[48];
    char left[32];
    char line[96];

    format_currency_comma(amount, amt, sizeof(amt));
    snprintf(left, sizeof(left), "  %-12s:", label);
    if (negative)
        snprintf(right, sizeof(right), "-Rs. %10s", amt);
    else
        snprintf(right, sizeof(right), " Rs. %10s", amt);

    int spaces = 56 - (int)strlen(left) - (int)strlen(right);
    if (spaces < 1) spaces = 1;

    snprintf(line, sizeof(line), "%s%*s%s", left, spaces, "", right);
    print_receipt_text_line(line);
}

static void build_seat_list_from_ids(const int *seat_ids, int seat_count,
                                     char *out, size_t out_sz,
                                     int *first_seat_type)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (first_seat_type) *first_seat_type = -1;

    int shown = (seat_count > 6) ? 5 : seat_count;
    for (int i = 0; i < seat_count; i++) {
        WhereClause sl_wc[1];
        strncpy(sl_wc[0].col_name, "seat_id", 63); sl_wc[0].col_name[63] = '\0';
        sl_wc[0].op = OP_EQ; sl_wc[0].value = (void *)&seat_ids[i]; sl_wc[0].logic = 0;

        ResultSet *sl_rs = db_select("seats", sl_wc, 1, NULL, 0);
        if (sl_rs && sl_rs->row_count > 0) {
            if (first_seat_type && *first_seat_type < 0)
                *first_seat_type = safe_int(sl_rs->rows, 0, 4);

            if (i < shown) {
                char seat_tok[24];
                snprintf(seat_tok, sizeof(seat_tok), "%s%d",
                         safe_str(sl_rs->rows, 0, 2),
                         safe_int(sl_rs->rows, 0, 3));
                if (out[0] != '\0')
                    strncat(out, ", ", out_sz - strlen(out) - 1);
                strncat(out, seat_tok, out_sz - strlen(out) - 1);
            }
        }
        result_set_free(sl_rs);
    }

    if (seat_count > 6) {
        char more[24];
        snprintf(more, sizeof(more), ", + %d more", seat_count - 5);
        strncat(out, more, out_sz - strlen(out) - 1);
    }
}

static void print_booking_receipt(int booking_id, const char *movie_title,
                                  const char *show_dt, const char *screen_name,
                                  const char *screen_type_str,
                                  int *seat_ids, int seat_count,
                                  float total_amt, SessionContext *ctx)
{
    char seat_list[192];
    int first_seat_type = 0;
    build_seat_list_from_ids(seat_ids, seat_count, seat_list, sizeof(seat_list),
                             &first_seat_type);

    float subtotal = ctx->current_price_breakdown[3] * (float)seat_count;
    float gst_amt  = ctx->current_price_breakdown[9];
    float conv_fee = ctx->current_price_breakdown[10];
    float discount = -(ctx->current_price_breakdown[4]
                     + ctx->current_price_breakdown[5]
                     + ctx->current_price_breakdown[6]);
    if (discount < 0.0f) discount = 0.0f;

    printf("\n" ANSI_GREEN);
    printf("  ╔════════════════════════════════════════════════════════╗\n");
    print_receipt_text_line("            🎬  BOOKING CONFIRMED  ✓");
    printf("  ╠════════════════════════════════════════════════════════╣\n");

    {
        char line[80];
        snprintf(line, sizeof(line), "  Booking ID  : #%d", booking_id);
        print_receipt_text_line(line);
    }

    printf("  ╠════════════════════════════════════════════════════════╣\n");
    {
        char line[80];
        snprintf(line, sizeof(line), "  FILM        : %.44s", movie_title ? movie_title : "");
        print_receipt_text_line(line);
        snprintf(line, sizeof(line), "  DATE & TIME : %s", show_dt ? show_dt : "");
        print_receipt_text_line(line);
        snprintf(line, sizeof(line), "  VENUE       : %.25s — [%.18s]",
                 screen_name ? screen_name : "", screen_type_str ? screen_type_str : "");
        print_receipt_text_line(line);
    }

    printf("  ╠════════════════════════════════════════════════════════╣\n");
    {
        char line[96];
        snprintf(line, sizeof(line), "  SEATS       : %.40s", seat_list);
        print_receipt_text_line(line);
        snprintf(line, sizeof(line), "  SEAT TYPE   : %s", seat_type_label(first_seat_type));
        print_receipt_text_line(line);
    }

    printf("  ╠════════════════════════════════════════════════════════╣\n");
    print_receipt_amount_line("SUBTOTAL", subtotal, 0);
    print_receipt_amount_line("GST (18%)", gst_amt, 0);
    print_receipt_amount_line("CONV. FEE", conv_fee, 0);
    if (ctx->current_price_breakdown[6] != 0.0f)
        print_receipt_amount_line("DISCOUNT", discount, 1);
    print_receipt_text_line("  ─────────────────────────────────────────────────");
    print_receipt_amount_line("TOTAL PAID", total_amt, 0);

    printf("  ╠════════════════════════════════════════════════════════╣\n");
    print_receipt_amount_line("WALLET BAL", ctx->wallet_balance, 0);
    printf("  ╠════════════════════════════════════════════════════════╣\n");
    print_receipt_text_line("  Enjoy your show! Cancellation available in My Bookings");
    printf("  ╚════════════════════════════════════════════════════════╝\n");
    printf(ANSI_RESET);

    printf("  " ANSI_CYAN
           "Booking reference saved. Present Booking ID #%d at the venue."
           ANSI_RESET "\n\n", booking_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SeatCell sort comparator: (row_label shorter-first, then alpha), then col
 * ═══════════════════════════════════════════════════════════════════════════*/
static int seat_cmp(const void *a, const void *b)
{
    const SeatCell *sa = (const SeatCell *)a;
    const SeatCell *sb = (const SeatCell *)b;
    int la = (int)strlen(sa->row_label);
    int lb = (int)strlen(sb->row_label);
    if (la != lb) return la - lb;
    int rc = strcmp(sa->row_label, sb->row_label);
    if (rc != 0) return rc;
    return sa->seat_number - sb->seat_number;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * release_holds — reset held seats back to AVAILABLE
 * ═══════════════════════════════════════════════════════════════════════════*/
static void release_holds(const int *seat_ids, int count, int show_id)
{
    int avail    = 0;
    int zero_uid = 0;
    char empty[20] = "";

    wal_begin();
    for (int i = 0; i < count; i++) {
        int sid = seat_ids[i];
        WhereClause wc[2];
        strncpy(wc[0].col_name, "show_id", 63); wc[0].col_name[63] = '\0';
        wc[0].op = OP_EQ; wc[0].value = &show_id; wc[0].logic = 0;
        strncpy(wc[1].col_name, "seat_id", 63); wc[1].col_name[63] = '\0';
        wc[1].op = OP_EQ; wc[1].value = &sid; wc[1].logic = 0;

        db_update("seat_status", wc, 2, "status",          &avail);
        db_update("seat_status", wc, 2, "held_by_user_id", &zero_uid);
        db_update("seat_status", wc, 2, "held_until",       empty);
    }
    wal_commit();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * hold_seats — mark seats as HELD with dynamic expiry.
 * Strict anti-double-booking guard:
 *   only transition AVAILABLE(0) -> HELD(1), except seats already held
 *   by the same user can be refreshed.
 * Returns 1 on success, 0 if any seat is no longer holdable.
 * ═══════════════════════════════════════════════════════════════════════════*/
static int hold_seats(int show_id, const char *show_datetime,
                      const int *seat_ids, int count, SessionContext *ctx)
{
    time_t now     = time(NULL);
    time_t show_t  = parse_datetime_str(show_datetime);
    long secs_to   = (show_t != (time_t)-1)
                   ? (long)difftime(show_t, now)
                   : (long)(50 * 3600);
    long hold_sec;
    if      (secs_to > 48L * 3600) hold_sec = 12L * 3600;
    else if (secs_to > 12L * 3600) hold_sec =  3L * 3600;
    else                            hold_sec = 45L * 60;

    char held_until_str[20];
    time_to_str(now + (time_t)hold_sec, held_until_str, sizeof(held_until_str));

    int status_held = 1;
    int status_avail = 0;

    wal_begin();
    for (int i = 0; i < count; i++) {
        int sid = seat_ids[i];

        /* First: AVAILABLE -> HELD by current user */
        WhereClause wc_take[3];
        strncpy(wc_take[0].col_name, "show_id", 63); wc_take[0].col_name[63] = '\0';
        wc_take[0].op = OP_EQ; wc_take[0].value = &show_id; wc_take[0].logic = 0;
        strncpy(wc_take[1].col_name, "seat_id", 63); wc_take[1].col_name[63] = '\0';
        wc_take[1].op = OP_EQ; wc_take[1].value = &sid; wc_take[1].logic = 0;
        strncpy(wc_take[2].col_name, "status", 63); wc_take[2].col_name[63] = '\0';
        wc_take[2].op = OP_EQ; wc_take[2].value = &status_avail; wc_take[2].logic = 0;

        int rows = db_update("seat_status", wc_take, 3, "status", &status_held);
        if (rows > 0) {
            db_update("seat_status", wc_take, 2, "held_by_user_id", &ctx->user_id);
            db_update("seat_status", wc_take, 2, "held_until", held_until_str);
            continue;
        }

        /* Second: allow refresh if already held by same user */
        WhereClause wc_refresh[4];
        strncpy(wc_refresh[0].col_name, "show_id", 63); wc_refresh[0].col_name[63] = '\0';
        wc_refresh[0].op = OP_EQ; wc_refresh[0].value = &show_id; wc_refresh[0].logic = 0;
        strncpy(wc_refresh[1].col_name, "seat_id", 63); wc_refresh[1].col_name[63] = '\0';
        wc_refresh[1].op = OP_EQ; wc_refresh[1].value = &sid; wc_refresh[1].logic = 0;
        strncpy(wc_refresh[2].col_name, "status", 63); wc_refresh[2].col_name[63] = '\0';
        wc_refresh[2].op = OP_EQ; wc_refresh[2].value = &status_held; wc_refresh[2].logic = 0;
        strncpy(wc_refresh[3].col_name, "held_by_user_id", 63); wc_refresh[3].col_name[63] = '\0';
        wc_refresh[3].op = OP_EQ; wc_refresh[3].value = &ctx->user_id; wc_refresh[3].logic = 0;

        rows = db_update("seat_status", wc_refresh, 4, "held_until", held_until_str);
        if (rows <= 0) {
            wal_rollback();
            return 0;
        }
    }
    wal_commit();

    free(ctx->held_seat_ids);
    ctx->held_seat_ids = malloc((size_t)count * sizeof(int));
    if (ctx->held_seat_ids) {
        memcpy(ctx->held_seat_ids, seat_ids, (size_t)count * sizeof(int));
        ctx->held_seat_count = count;
    } else {
        ctx->held_seat_count = 0;
    }
    ctx->hold_started_at = now;

    printf("\n  %d seat(s) held until %s", count, held_until_str);
    if (hold_sec >= 3600)
        printf("  (hold: %ld h)\n", hold_sec / 3600);
    else
        printf("  (hold: %ld min)\n", hold_sec / 60);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_price_breakdown
 * ═══════════════════════════════════════════════════════════════════════════*/
static void display_price_breakdown(const SessionContext *ctx, int seat_count,
                                    const char *screen_type_str,
                                    const char *promo_code)
{
    const float *bd = ctx->current_price_breakdown;
    const char *screen_lbl = (screen_type_str && screen_type_str[0])
                           ? screen_type_str : "2D";
    const char *seat_lbl = "STANDARD";
    if (bd[2] > 2.0f) seat_lbl = "RECLINER";
    else if (bd[2] > 1.2f) seat_lbl = "PREMIUM";

    float student_disc = (bd[4] > 0.001f) ? bd[4] : 0.0f;
    float group_disc   = (bd[5] > 0.001f) ? bd[5] : 0.0f;
    float promo_disc   = (bd[6] < -0.001f) ? -bd[6] : 0.0f;

    int is_weekend_show = 0;
    int is_peak_show = 0;
    {
        WhereClause sh_wc[1];
        strncpy(sh_wc[0].col_name, "show_id", 63); sh_wc[0].col_name[63] = '\0';
        sh_wc[0].op = OP_EQ;
        sh_wc[0].value = (void *)&ctx->selected_show_id;
        sh_wc[0].logic = 0;

        ResultSet *sh_rs = db_select("shows", sh_wc, 1, NULL, 0);
        if (sh_rs && sh_rs->row_count > 0) {
            time_t show_t = parse_datetime_str(safe_str(sh_rs->rows, 0, 3));
            if (show_t != (time_t)-1) {
                struct tm *tmv = localtime(&show_t);
                if (tmv) {
                    int wday = tmv->tm_wday;
                    int hour = tmv->tm_hour;
                    is_weekend_show = (wday == 0 || wday == 5 || wday == 6);
                    is_peak_show = (hour >= 18 && hour <= 23);
                }
            }
        }
        result_set_free(sh_rs);
    }

    float peak_surge = 0.0f;
    float weekend_surge = 0.0f;
    if (bd[7] > 0.001f) {
        if (is_peak_show && is_weekend_show) {
            peak_surge = bd[7] * (15.0f / 25.0f);
            weekend_surge = bd[7] * (10.0f / 25.0f);
        } else if (is_peak_show) {
            peak_surge = bd[7];
        } else if (is_weekend_show) {
            weekend_surge = bd[7];
        }
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║                    PRICE BREAKDOWN                       ║\n");
    printf("╠═══════════════════════════════╦══════════════╦═══════════╣\n");
    printf("║  Component                    ║  Per Seat    ║  Total    ║\n");
    printf("╠═══════════════════════════════╬══════════════╬═══════════╣\n");

    {
        char per[64], total[64], total_txt[64], label[96];

        snprintf(per, sizeof(per), "Rs. %.2f", bd[0]);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Base price", per, "");

        snprintf(label, sizeof(label), "Screen surcharge (%s)", screen_lbl);
        snprintf(per, sizeof(per), "Rs. %.2f", bd[1]);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", label, per, "");

        snprintf(label, sizeof(label), "Seat type mult. (%s)", seat_lbl);
        snprintf(per, sizeof(per), "×%.2f", bd[2]);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", label, per, "");

        snprintf(per, sizeof(per), "Rs. %.2f", bd[3]);
        format_int_comma((long long)(bd[3] * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "Rs.%.59s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Subtotal per seat", per, total_txt);
    }

    printf("╠═══════════════════════════════╬══════════════╬══════════╣\n");

    if (student_disc > 0.001f) {
        char per[64], total[64], total_txt[64];
        snprintf(per, sizeof(per), "-Rs. %.2f", student_disc);
        format_int_comma((long long)(student_disc * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "-Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Student discount (-12%)", per, total_txt);
    }

    if (group_disc > 0.001f) {
        char per[64], total[64], total_txt[64];
        snprintf(per, sizeof(per), "-Rs. %.2f", group_disc);
        format_int_comma((long long)(group_disc * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "-Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Group discount   (-8%)", per, total_txt);
    }

    if (promo_disc > 0.001f) {
        char per[64], total[64], total_txt[64], label[96];
        snprintf(label, sizeof(label), "Promo %.16s",
                 (promo_code && promo_code[0]) ? promo_code : "APPLIED");
        snprintf(per, sizeof(per), "-Rs. %.2f", promo_disc);
        format_int_comma((long long)(promo_disc * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "-Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", label, per, total_txt);
    }

    printf("╠═══════════════════════════════╬══════════════╬══════════╣\n");

    if (peak_surge > 0.001f) {
        char per[64], total[64], total_txt[64];
        snprintf(per, sizeof(per), "+Rs. %.2f", peak_surge);
        format_int_comma((long long)(peak_surge * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "+Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Peak hour surge (+15%)", per, total_txt);
    }

    if (weekend_surge > 0.001f) {
        char per[64], total[64], total_txt[64];
        snprintf(per, sizeof(per), "+Rs. %.2f", weekend_surge);
        format_int_comma((long long)(weekend_surge * (float)seat_count + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "+Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Weekend surge    (+10%)", per, total_txt);
    }

    printf("╠═══════════════════════════════╬══════════════╬══════════╣\n");
    {
        char total[64], total_txt[64];

        format_int_comma((long long)(bd[8] + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "Rs.%.59s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Taxable amount", "", total_txt);

        format_int_comma((long long)(bd[9] + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "+Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "GST 18%", "", total_txt);

        format_int_comma((long long)(bd[10] + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "+Rs.%.58s", total);
        printf("║  %-29.29s║ %12.12s ║ %8.8s ║\n", "Convenience fee", "", total_txt);
    }

    printf("╠═══════════════════════════════╬══════════════╬══════════╣\n");
    {
        char total[64], total_txt[64];
        format_int_comma((long long)(bd[11] + 0.5f), total, sizeof(total));
        snprintf(total_txt, sizeof(total_txt), "Rs.%.59s", total);
        printf("║  \033[1m%-29.29s║ %12.12s ║ %8.8s\033[0m ║\n", "GRAND TOTAL", "", total_txt);
    }
    printf("╚═══════════════════════════════╩══════════════╩══════════╝\n");

    printf("  %d seat(s)  ·  %s screening  ·  Wallet available: Rs.%.2f\n\n",
           seat_count, screen_lbl, ctx->wallet_balance);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * render_seat_map_interactive
 *
 * Builds the seat grid from DB, renders it with ANSI colours, and runs an
 * interactive toggle loop so the user can assemble a seat selection.
 *
 * Returns number of seats confirmed (>= 1), or 0 if the user cancelled.
 * Fills out_seat_ids[0..return_val-1].
 * ═══════════════════════════════════════════════════════════════════════════*/
static int render_seat_map_interactive(int show_id, int screen_id,
                                       SessionContext *ctx,
                                       int *out_seat_ids, int max_out)
{
    char input[256];

    int sel_ids[MAX_SEL_SEATS];
    int sel_count = 0;

    SeatCell *cells = NULL;
    int total = 0;
    int max_col = 0;
    char rows[128][ROW_LABEL_MAX];
    int row_count = 0;

    int need_reload = 1;

    char screen_name[51] = "Screen";
    int  scr_type = 0;

    /* Screen badge metadata */
    {
        WhereClause scr_wc[1];
        strncpy(scr_wc[0].col_name, "screen_id", 63);
        scr_wc[0].col_name[63] = '\0';
        scr_wc[0].op = OP_EQ;
        scr_wc[0].value = &screen_id;
        scr_wc[0].logic = 0;

        ResultSet *scr_rs = db_select("screens", scr_wc, 1, NULL, 0);
        if (scr_rs && scr_rs->row_count > 0) {
            strncpy(screen_name, safe_str(scr_rs->rows, 0, 2), 50);
            screen_name[50] = '\0';
            scr_type = safe_int(scr_rs->rows, 0, 3);
        }
        result_set_free(scr_rs);
    }

    for (;;) {
        if (need_reload) {
            free(cells);
            cells = NULL;
            total = 0;
            max_col = 0;
            row_count = 0;

            /* Fetch all seats for this screen */
            WhereClause seat_wc[1];
            strncpy(seat_wc[0].col_name, "screen_id", 63);
            seat_wc[0].col_name[63] = '\0';
            seat_wc[0].op = OP_EQ;
            seat_wc[0].value = &screen_id;
            seat_wc[0].logic = 0;

            ResultSet *seat_rs = db_select("seats", seat_wc, 1, NULL, 0);
            if (!seat_rs || seat_rs->row_count == 0) {
                printf("  No seats found for screen %d.\n", screen_id);
                result_set_free(seat_rs);
                return 0;
            }

            /* Fetch all seat_status rows for this show */
            WhereClause ss_wc[1];
            strncpy(ss_wc[0].col_name, "show_id", 63);
            ss_wc[0].col_name[63] = '\0';
            ss_wc[0].op = OP_EQ;
            ss_wc[0].value = &show_id;
            ss_wc[0].logic = 0;

            ResultSet *ss_rs = db_select("seat_status", ss_wc, 1, NULL, 0);

            total = seat_rs->row_count;
            if (total > MAX_SEATS_PER_SCREEN) total = MAX_SEATS_PER_SCREEN;

            cells = calloc((size_t)total, sizeof(SeatCell));
            if (!cells) {
                result_set_free(seat_rs);
                result_set_free(ss_rs);
                return 0;
            }

            for (int i = 0; i < total; i++) {
                cells[i].seat_id = safe_int(seat_rs->rows, i, 0);
                int is_active = safe_int(seat_rs->rows, i, 5);

                strncpy(cells[i].row_label,
                        safe_str(seat_rs->rows, i, 2), ROW_LABEL_MAX - 1);
                cells[i].row_label[ROW_LABEL_MAX - 1] = '\0';
                for (int k = 0; cells[i].row_label[k]; k++) {
                    cells[i].row_label[k] = (char)toupper(
                        (unsigned char)cells[i].row_label[k]);
                }

                cells[i].seat_number = safe_int(seat_rs->rows, i, 3);
                cells[i].seat_type   = safe_int(seat_rs->rows, i, 4);
                cells[i].is_active   = is_active;
                cells[i].status      = is_active ? 0 : 2;
                cells[i].held_by_user_id = 0;
                cells[i].sel_order = 0;

                if (ss_rs) {
                    for (int j = 0; j < ss_rs->row_count; j++) {
                        if (safe_int(ss_rs->rows, j, 2) == cells[i].seat_id) {
                            cells[i].status = safe_int(ss_rs->rows, j, 3);
                            cells[i].held_by_user_id = safe_int(ss_rs->rows, j, 4);
                            break;
                        }
                    }
                }
            }

            result_set_free(seat_rs);
            result_set_free(ss_rs);

            qsort(cells, (size_t)total, sizeof(SeatCell), seat_cmp);

            for (int i = 0; i < total; i++) {
                if (row_count == 0 ||
                    strcmp(cells[i].row_label, rows[row_count - 1]) != 0) {
                    if (row_count < 128) {
                        strncpy(rows[row_count], cells[i].row_label, ROW_LABEL_MAX - 1);
                        rows[row_count][ROW_LABEL_MAX - 1] = '\0';
                        row_count++;
                    }
                }
                if (cells[i].seat_number > max_col)
                    max_col = cells[i].seat_number;
            }

            /* Re-map existing selection after refresh/reload */
            int new_ids[MAX_SEL_SEATS];
            int new_count = 0;
            for (int i = 0; i < sel_count; i++) {
                for (int k = 0; k < total; k++) {
                    if (cells[k].seat_id == sel_ids[i]) {
                        int pickable = (cells[k].status == 0) ||
                                       (cells[k].status == 1 &&
                                        cells[k].held_by_user_id == ctx->user_id);
                        if (pickable && new_count < MAX_SEL_SEATS) {
                            cells[k].sel_order = new_count + 1;
                            new_ids[new_count++] = cells[k].seat_id;
                        }
                        break;
                    }
                }
            }
            memcpy(sel_ids, new_ids, (size_t)new_count * sizeof(int));
            sel_count = new_count;

            need_reload = 0;
        }

        /* Refresh mutable seat state for this show on every render */
        for (int i = 0; i < total; i++) {
            cells[i].status = cells[i].is_active ? 0 : 2;
            cells[i].held_by_user_id = 0;
        }

        {
            WhereClause ss_wc_live[1];
            strncpy(ss_wc_live[0].col_name, "show_id", 63);
            ss_wc_live[0].col_name[63] = '\0';
            ss_wc_live[0].op = OP_EQ;
            ss_wc_live[0].value = &show_id;
            ss_wc_live[0].logic = 0;

            ResultSet *ss_live = db_select("seat_status", ss_wc_live, 1, NULL, 0);
            if (ss_live) {
                for (int j = 0; j < ss_live->row_count; j++) {
                    int sid = safe_int(ss_live->rows, j, 2);
                    int status = safe_int(ss_live->rows, j, 3);
                    int held_uid = safe_int(ss_live->rows, j, 4);

                    for (int i = 0; i < total; i++) {
                        if (cells[i].seat_id == sid) {
                            cells[i].status = status;
                            cells[i].held_by_user_id = held_uid;
                            break;
                        }
                    }
                }
            }
            result_set_free(ss_live);
        }

        /* Keep only still-pickable seats in current selection */
        {
            int new_ids[MAX_SEL_SEATS];
            int new_count = 0;

            for (int i = 0; i < total; i++) cells[i].sel_order = 0;

            for (int i = 0; i < sel_count; i++) {
                int sid = sel_ids[i];
                SeatCell *cell = NULL;
                for (int k = 0; k < total; k++) {
                    if (cells[k].seat_id == sid) {
                        cell = &cells[k];
                        break;
                    }
                }
                if (!cell) continue;

                int pickable = (cell->status == 0) ||
                               (cell->status == 1 &&
                                cell->held_by_user_id == ctx->user_id);
                if (pickable && new_count < MAX_SEL_SEATS)
                    new_ids[new_count++] = sid;
            }

            memcpy(sel_ids, new_ids, (size_t)new_count * sizeof(int));
            sel_count = new_count;
            for (int i = 0; i < sel_count; i++) {
                for (int k = 0; k < total; k++) {
                    if (cells[k].seat_id == sel_ids[i]) {
                        cells[k].sel_order = i + 1;
                        break;
                    }
                }
            }
        }

        /* Live estimate */
        const char *sym = ctx->currency_sym[0] ? ctx->currency_sym : "Rs";
        float base_price = ctx->current_price_breakdown[0];
        if (base_price < 0.001f) base_price = 0.0f;

        float est_total = 0.0f;
        for (int i = 0; i < sel_count; i++) {
            for (int k = 0; k < total; k++) {
                if (cells[k].seat_id == sel_ids[i]) {
                    float mult = (cells[k].seat_type == 2) ? 2.2f
                               : (cells[k].seat_type == 1) ? 1.5f : 1.0f;
                    est_total += base_price * mult;
                    break;
                }
            }
        }

        printf("\n");
        printf("  STANDARD  [ ]  ×1.0 (%s%.2f)    PREMIUM  " ANSI_BLUE "[ ]" ANSI_RESET
               "  ×1.5 (%s%.2f)\n",
               sym, base_price * 1.0f, sym, base_price * 1.5f);
        printf("  RECLINER  " ANSI_CYAN "[ ]" ANSI_RESET "  ×2.2 (%s%.2f)    SCREEN "
               "────────────────\n",
               sym, base_price * 2.2f);

        /* Screen / stage direction indicator */
        {
            int bar_w = max_col * 4;
            int label_len = 14;
            int lpad = (bar_w - label_len) / 2;
            int rpad = bar_w - label_len - lpad;
            printf("       ");
            for (int i = 0; i < lpad; i++) printf("-");
            printf(" [  SCREEN  ] ");
            for (int i = 0; i < rpad; i++) printf("-");
            printf("\n");
            printf("       ");
            for (int i = 0; i < bar_w; i++) printf("^");
            printf("    (face this way)\n\n");
        }

        printf("  " ANSI_BOLD "%s" ANSI_RESET "  [%s]  %d total seats\n\n",
               screen_name, screen_type_label(scr_type), total);

        /* Column header */
        printf("       ");
        for (int c = 1; c <= max_col; c++) printf("%3d ", c);
        putchar('\n');
        printf("       ");
        for (int c = 1; c <= max_col; c++) printf("----");
        putchar('\n');

        for (int r = 0; r < row_count; r++) {
            int row_stype = 0;
            for (int i = 0; i < total; i++) {
                if (strcmp(cells[i].row_label, rows[r]) == 0) {
                    row_stype = cells[i].seat_type;
                    break;
                }
            }
            const char *rcol = (row_stype == 2) ? ANSI_CYAN
                             : (row_stype == 1) ? ANSI_BLUE : "";

            printf("  %s%-4s" ANSI_RESET " ", rcol, rows[r]);

            for (int c = 1; c <= max_col; c++) {
                SeatCell *cell = NULL;
                for (int i = 0; i < total; i++) {
                    if (strcmp(cells[i].row_label, rows[r]) == 0 &&
                        cells[i].seat_number == c) {
                        cell = &cells[i];
                        break;
                    }
                }
                if (!cell) {
                    printf("    ");
                    continue;
                }

                if (cell->sel_order > 0) {
                    printf(ANSI_GREEN "[%2d]" ANSI_RESET, cell->sel_order);
                } else if (cell->status == 0) {
                    printf("%s[  ]" ANSI_RESET, rcol);
                } else if (cell->status == 1) {
                    if (cell->held_by_user_id == ctx->user_id)
                        printf(ANSI_GREEN "[HU]" ANSI_RESET);
                    else
                        printf(ANSI_YELLOW "[HH]" ANSI_RESET);
                } else {
                    printf(ANSI_RED "[XX]" ANSI_RESET);
                }
            }

            if (row_stype == 2)
                printf("  " ANSI_CYAN "RECLINER" ANSI_RESET);
            else if (row_stype == 1)
                printf("  " ANSI_BLUE "PREMIUM" ANSI_RESET);

            putchar('\n');
        }

        printf("\n  Selected: %d seat(s)  │  Est. total: %s%.2f\n",
               sel_count, sym, est_total);
        printf("  Commands: <seats>  'done'  'clear'  'refresh'  'back'\n");
        printf("  Input: ");
        read_line(input, (int)sizeof(input));

        if (strcmp(input, "back") == 0 || strcmp(input, "0") == 0) {
            free(cells);
            return 0;
        }
        if (strcmp(input, "clear") == 0) {
            for (int i = 0; i < total; i++) cells[i].sel_order = 0;
            sel_count = 0;
            continue;
        }
        if (strcmp(input, "refresh") == 0) {
            need_reload = 1;
            printf(ANSI_YELLOW "  ↻  Seat map refreshed." ANSI_RESET "\n");
            continue;
        }
        if (strcmp(input, "done") == 0) {
            if (sel_count == 0) {
                printf("  Please select at least one seat first.\n");
                continue;
            }

            int removed_any = 0;
            for (int i = 0; i < sel_count; ) {
                int sid = sel_ids[i];
                int still_available = 0;

                WhereClause ck_wc[2];
                strncpy(ck_wc[0].col_name, "show_id", 63);
                ck_wc[0].col_name[63] = '\0';
                ck_wc[0].op = OP_EQ;
                ck_wc[0].value = &show_id;
                ck_wc[0].logic = 0;

                strncpy(ck_wc[1].col_name, "seat_id", 63);
                ck_wc[1].col_name[63] = '\0';
                ck_wc[1].op = OP_EQ;
                ck_wc[1].value = &sid;
                ck_wc[1].logic = 0;

                char *cols[] = { "status" };
                ResultSet *ck_rs = db_select("seat_status", ck_wc, 2, cols, 1);
                if (ck_rs && ck_rs->row_count > 0) {
                    still_available = (safe_int(ck_rs->rows, 0, 0) == 0);
                }
                result_set_free(ck_rs);

                if (still_available) {
                    i++;
                    continue;
                }

                const char *row_lbl = "?";
                int seat_no = 0;
                for (int k = 0; k < total; k++) {
                    if (cells[k].seat_id == sid) {
                        row_lbl = cells[k].row_label;
                        seat_no = cells[k].seat_number;
                        cells[k].sel_order = 0;
                        break;
                    }
                }

                printf(ANSI_RED "  ✗ Seat [%s%d] was just taken by another user." ANSI_RESET "\n",
                       row_lbl, seat_no);

                for (int m = i; m < sel_count - 1; m++)
                    sel_ids[m] = sel_ids[m + 1];
                sel_count--;
                removed_any = 1;
            }

            for (int i = 0; i < total; i++) cells[i].sel_order = 0;
            for (int i = 0; i < sel_count; i++) {
                for (int k = 0; k < total; k++) {
                    if (cells[k].seat_id == sel_ids[i]) {
                        cells[k].sel_order = i + 1;
                        break;
                    }
                }
            }

            if (removed_any || sel_count == 0) {
                printf("  Please choose available seats and type 'done' again.\n");
                continue;
            }

            int out = (sel_count < max_out) ? sel_count : max_out;
            memcpy(out_seat_ids, sel_ids, (size_t)out * sizeof(int));
            free(cells);
            return out;
        }

        const char *ptr = input;
        while (*ptr) {
            while (*ptr == ' ') ptr++;
            if (*ptr == '\0') break;

            char tok[32];
            int tlen = 0;
            while (*ptr && *ptr != ' ' && tlen < 31) tok[tlen++] = *ptr++;
            tok[tlen] = '\0';

            char parsed_row[ROW_LABEL_MAX] = "";
            int ri = 0, ci = 0;
            while (tok[ci] && isalpha((unsigned char)tok[ci]) && ri < 3)
                parsed_row[ri++] = (char)toupper((unsigned char)tok[ci++]);
            parsed_row[ri] = '\0';

            if (ri == 0 || !isdigit((unsigned char)tok[ci])) {
                printf("  Bad label '%s' — use format like A3 or AA12.\n", tok);
                continue;
            }
            int parsed_col = atoi(tok + ci);

            SeatCell *tgt = NULL;
            for (int i = 0; i < total; i++) {
                if (strcmp(cells[i].row_label, parsed_row) == 0 &&
                    cells[i].seat_number == parsed_col) {
                    tgt = &cells[i];
                    break;
                }
            }
            if (!tgt) {
                printf(ANSI_RED "  ✗ No seat labelled '%s%d' on this screen." ANSI_RESET "\n",
                       parsed_row, parsed_col);
                continue;
            }

            if (tgt->sel_order > 0) {
                int removed = tgt->sel_order;
                tgt->sel_order = 0;

                int new_ids[MAX_SEL_SEATS];
                int nc = 0;
                for (int i = 0; i < sel_count; i++)
                    if (sel_ids[i] != tgt->seat_id)
                        new_ids[nc++] = sel_ids[i];

                memcpy(sel_ids, new_ids, (size_t)nc * sizeof(int));
                sel_count = nc;

                for (int i = 0; i < total; i++)
                    if (cells[i].sel_order > removed)
                        cells[i].sel_order--;

                continue;
            }

            int blocked = !((tgt->status == 0) ||
                            (tgt->status == 1 &&
                             tgt->held_by_user_id == ctx->user_id));
            if (blocked) {
                printf(ANSI_RED "  ✗ Seat %s%d is taken." ANSI_RESET "\n",
                       parsed_row, parsed_col);
                continue;
            }

            if (sel_count >= MAX_SEL_SEATS) {
                printf(ANSI_YELLOW "  ! Maximum %d seats per booking." ANSI_RESET "\n",
                       MAX_SEL_SEATS);
                continue;
            }

            tgt->sel_order = sel_count + 1;
            sel_ids[sel_count++] = tgt->seat_id;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * run_booking_flow — public entry point called from ui_browse.c
 * ═══════════════════════════════════════════════════════════════════════════*/
void run_booking_flow(int show_id, SessionContext *ctx)
{
    char input[256];
    ctx->selected_show_id = show_id;

    /* ── Fetch show ─────────────────────────────────────────────────────── */
    WhereClause sh_wc[1];
    strncpy(sh_wc[0].col_name, "show_id", 63); sh_wc[0].col_name[63] = '\0';
    sh_wc[0].op = OP_EQ; sh_wc[0].value = &show_id; sh_wc[0].logic = 0;

    ResultSet *show_rs = db_select("shows", sh_wc, 1, NULL, 0);
    if (!show_rs || show_rs->row_count == 0) {
        printf("\n  Show (id=%d) not found.\n", show_id);
        result_set_free(show_rs);
        return;
    }
    /* shows: 0=show_id 1=movie_id 2=screen_id 3=show_datetime 4=base_price 5=is_active */
    int  movie_id_v   = safe_int(show_rs->rows, 0, 1);
    int  screen_id_v  = safe_int(show_rs->rows, 0, 2);
    char show_datetime[20];
    strncpy(show_datetime, safe_str(show_rs->rows, 0, 3), 19);
    show_datetime[19] = '\0';
    result_set_free(show_rs);

    /* ── Fetch movie title ──────────────────────────────────────────────── */
    WhereClause mv_wc[1];
    strncpy(mv_wc[0].col_name, "movie_id", 63); mv_wc[0].col_name[63] = '\0';
    mv_wc[0].op = OP_EQ; mv_wc[0].value = &movie_id_v; mv_wc[0].logic = 0;

    ResultSet *mv_rs = db_select("movies", mv_wc, 1, NULL, 0);
    char movie_title[201] = "Unknown";
    if (mv_rs && mv_rs->row_count > 0)
        strncpy(movie_title, safe_str(mv_rs->rows, 0, 2), 200);
    movie_title[200] = '\0';
    result_set_free(mv_rs);

    /* ── Fetch screen ───────────────────────────────────────────────────── */
    WhereClause scr_wc[1];
    strncpy(scr_wc[0].col_name, "screen_id", 63); scr_wc[0].col_name[63] = '\0';
    scr_wc[0].op = OP_EQ; scr_wc[0].value = &screen_id_v; scr_wc[0].logic = 0;

    ResultSet *scr_rs = db_select("screens", scr_wc, 1, NULL, 0);
    char screen_name[51] = "";
    int  scr_type = 0;
    if (scr_rs && scr_rs->row_count > 0) {
        strncpy(screen_name, safe_str(scr_rs->rows, 0, 2), 50);
        scr_type = safe_int(scr_rs->rows, 0, 3);
    }
    result_set_free(scr_rs);

    /* ── Booking header ─────────────────────────────────────────────────── */
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "BOOK SEATS \xe2\x80\x94 %.60s", movie_title);
    banner(hdr);
    printf("  Show   : %s\n", show_datetime);
    printf("  Screen : %s  [%s]\n", screen_name, screen_type_label(scr_type));
    printf("  Wallet : %s%.2f\n\n",
           ctx->currency_sym, ctx->wallet_balance);

    /* ── Seat selection ─────────────────────────────────────────────────── */
    int seat_ids[MAX_SEL_SEATS];
    int seat_count = render_seat_map_interactive(
                        show_id, screen_id_v, ctx,
                        seat_ids, MAX_SEL_SEATS);
    if (seat_count <= 0) {
        printf("\n  Booking cancelled.\n\n");
        return;
    }

    /* ── Hold selected seats ────────────────────────────────────────────── */
    if (!hold_seats(show_id, show_datetime, seat_ids, seat_count, ctx)) {
        printf("\n  \033[31mSome selected seats were just taken by another user.\033[0m\n");
        printf("  Please reopen the show and select seats again.\n\n");
        return;
    }

    /* ── Compute and display price breakdown ────────────────────────────── */
    compute_price_breakdown(show_id, seat_ids, seat_count, ctx);
    display_price_breakdown(ctx, seat_count, screen_type_label(scr_type), NULL);

    /* ── Optional promo code ────────────────────────────────────────────── */
    printf("  Promo code (Enter to skip): ");
    read_line(input, (int)sizeof(input));

    int promo_id_used = 0;
    if (input[0] != '\0') {
        Promo *promo = validate_promo(input, ctx->role, seat_count, ctx);
        if (promo) {
            float disc = apply_promo(promo, ctx->current_price_breakdown[8]);
            ctx->current_price_breakdown[6]  = -disc; /* stored as negative */
            ctx->current_price_breakdown[8] -= disc;
            if (ctx->current_price_breakdown[8] < 0.0f)
                ctx->current_price_breakdown[8] = 0.0f;
            /* Recompute GST and grand total */
            ctx->current_price_breakdown[9]  =
                ctx->current_price_breakdown[8] * 0.18f;
            ctx->current_price_breakdown[11] =
                ctx->current_price_breakdown[8]
              + ctx->current_price_breakdown[9]
              + ctx->current_price_breakdown[10];

            promo_id_used = promo->promo_id;
            printf("  " ANSI_GREEN "Promo applied — discount: %s%.2f" ANSI_RESET "\n",
                   ctx->currency_sym, disc);
            free(promo);
            printf("\n  Updated totals:\n");
            display_price_breakdown(ctx, seat_count, screen_type_label(scr_type), input);
        }
    }

    /* ── Compute amounts from breakdown ─────────────────────────────────── */
    float subtotal    = ctx->current_price_breakdown[3] * (float)seat_count;
    float gst_amt     = ctx->current_price_breakdown[9];
    float conv_fee    = ctx->current_price_breakdown[10];
    float disc_amt    = -(ctx->current_price_breakdown[4]
                        + ctx->current_price_breakdown[5]
                        + ctx->current_price_breakdown[6]);
    if (disc_amt < 0.0f) disc_amt = 0.0f;
    float total_amt   = ctx->current_price_breakdown[11];

    /* ── Payment method ─────────────────────────────────────────────────── */
    printf("  \033[36mTotal is Rs.%.2f. Proceed to payment? (y/n): \033[0m",
           total_amt);
    read_line(input, (int)sizeof(input));
    if (input[0] == 'n' || input[0] == 'N') {
        release_holds(seat_ids, seat_count, show_id);
        printf("\n  Booking cancelled. Holds released.\n\n");
        return;
    }

    PaymentMethod pay_method = PAY_WALLET;
    while (1) {
        print_payment_menu(ctx, total_amt);
        printf("  Choice: ");
        read_line(input, (int)sizeof(input));

        if (input[0] == '0' || input[0] == '\0') {
            release_holds(seat_ids, seat_count, show_id);
            printf("\n  Booking cancelled. Holds released.\n\n");
            return;
        }

        if (input[0] == '1') {
            if (ctx->wallet_balance + 0.001f < total_amt) {
                float shortfall = total_amt - ctx->wallet_balance;
                printf("\033[31m  ✗ Wallet balance is insufficient (need %s%.2f more).\033[0m\n",
                       ctx->currency_sym, shortfall);
                continue;
            }
            pay_method = PAY_WALLET;
            break;
        }
        if (input[0] == '2') { pay_method = PAY_UPI; break; }
        if (input[0] == '3') { pay_method = PAY_CARD; break; }
        if (input[0] == '4') { pay_method = PAY_NETBANKING; break; }

        printf("  Invalid choice. Please select 0-4.\n");
    }

    /* ── Insert PENDING booking ─────────────────────────────────────────── */
    char booked_at[20];
    build_now_str(booked_at, sizeof(booked_at));

    int   dummy_id     = 0;
    int   status_pend  = 0;
    int   pmt_zero     = 0;
    char  canc_empty[20] = "";

    void *bk_fields[14];
    bk_fields[0]  = &dummy_id;
    bk_fields[1]  = &ctx->user_id;
    bk_fields[2]  = &show_id;
    bk_fields[3]  = &seat_count;
    bk_fields[4]  = &subtotal;
    bk_fields[5]  = &gst_amt;
    bk_fields[6]  = &conv_fee;
    bk_fields[7]  = &disc_amt;
    bk_fields[8]  = &total_amt;
    bk_fields[9]  = &promo_id_used;
    bk_fields[10] = &pmt_zero;
    bk_fields[11] = &status_pend;
    bk_fields[12] = booked_at;
    bk_fields[13] = canc_empty;

    wal_begin();
    int new_bk_id = db_insert("bookings", bk_fields);
    if (new_bk_id <= 0) {
        wal_rollback();
        printf("  Failed to create booking record. Please try again.\n");
        release_holds(seat_ids, seat_count, show_id);
        return;
    }
    wal_commit();

    ctx->active_payment_id = 0;

    /* ── Process payment ────────────────────────────────────────────────── */
    int pay_result = PAY_STATUS_FAILED;
    while (1) {
        pay_result = process_payment(new_bk_id, pay_method, ctx);
        if (pay_result == PAY_STATUS_SUCCESS) break;

        if (pay_result == PAY_STATUS_RETRY) {
            while (1) {
                print_payment_menu(ctx, total_amt);
                printf("  Choice: ");
                read_line(input, (int)sizeof(input));

                if (input[0] == '0' || input[0] == '\0') {
                    pay_result = PAY_STATUS_FAILED;
                    break;
                }
                if (input[0] == '1') {
                    if (ctx->wallet_balance + 0.001f < total_amt) {
                        float shortfall = total_amt - ctx->wallet_balance;
                        printf("\033[31m  ✗ Wallet balance is insufficient (need %s%.2f more).\033[0m\n",
                               ctx->currency_sym, shortfall);
                        continue;
                    }
                    pay_method = PAY_WALLET;
                    break;
                }
                if (input[0] == '2') { pay_method = PAY_UPI; break; }
                if (input[0] == '3') { pay_method = PAY_CARD; break; }
                if (input[0] == '4') { pay_method = PAY_NETBANKING; break; }

                printf("  Invalid choice. Please select 0-4.\n");
            }

            if (pay_result == PAY_STATUS_FAILED) break;
            continue;
        }

        break;
    }

    if (pay_result != PAY_STATUS_SUCCESS) {
        /* Mark booking cancelled, release holds */
        int status_canc = 2;
        char canc_at[20];
        build_now_str(canc_at, sizeof(canc_at));

        WhereClause bk_wc[1];
        strncpy(bk_wc[0].col_name, "booking_id", 63); bk_wc[0].col_name[63] = '\0';
        bk_wc[0].op = OP_EQ; bk_wc[0].value = &new_bk_id; bk_wc[0].logic = 0;

        wal_begin();
        db_update("bookings", bk_wc, 1, "status",       &status_canc);
        db_update("bookings", bk_wc, 1, "cancelled_at",  canc_at);
        wal_commit();

        release_holds(seat_ids, seat_count, show_id);
        printf("\n  " ANSI_RED "Payment failed." ANSI_RESET
               " Holds released. Please try again.\n\n");
        return;
    }

    /* ── Payment succeeded — confirm booking atomically ─────────────────── */
    int status_conf = 1;
    int pmt_id_val  = ctx->active_payment_id;
    int dummy_bs    = 0;
    int status_ss_conf = 2;
    int status_ss_held = 1;

    WhereClause bk_wc[1];
    strncpy(bk_wc[0].col_name, "booking_id", 63); bk_wc[0].col_name[63] = '\0';
    bk_wc[0].op = OP_EQ; bk_wc[0].value = &new_bk_id; bk_wc[0].logic = 0;

    wal_begin();

    db_update("bookings", bk_wc, 1, "status",     &status_conf);
    db_update("bookings", bk_wc, 1, "payment_id", &pmt_id_val);

    int confirm_ok = 1;
    for (int i = 0; i < seat_count; i++) {
        float seat_price = ctx->current_price_breakdown[3];

        void *bs_f[4];
        bs_f[0] = &dummy_bs;
        bs_f[1] = &new_bk_id;
        bs_f[2] = &seat_ids[i];
        bs_f[3] = &seat_price;
        if (db_insert("booking_seats", bs_f) <= 0) {
            confirm_ok = 0;
            break;
        }

        WhereClause ss_wc_guard[4];
        strncpy(ss_wc_guard[0].col_name, "show_id", 63); ss_wc_guard[0].col_name[63] = '\0';
        ss_wc_guard[0].op = OP_EQ; ss_wc_guard[0].value = &show_id; ss_wc_guard[0].logic = 0;
        strncpy(ss_wc_guard[1].col_name, "seat_id", 63); ss_wc_guard[1].col_name[63] = '\0';
        ss_wc_guard[1].op = OP_EQ; ss_wc_guard[1].value = &seat_ids[i]; ss_wc_guard[1].logic = 0;
        strncpy(ss_wc_guard[2].col_name, "status", 63); ss_wc_guard[2].col_name[63] = '\0';
        ss_wc_guard[2].op = OP_EQ; ss_wc_guard[2].value = &status_ss_held; ss_wc_guard[2].logic = 0;
        strncpy(ss_wc_guard[3].col_name, "held_by_user_id", 63); ss_wc_guard[3].col_name[63] = '\0';
        ss_wc_guard[3].op = OP_EQ; ss_wc_guard[3].value = &ctx->user_id; ss_wc_guard[3].logic = 0;

        if (db_update("seat_status", ss_wc_guard, 4, "status", &status_ss_conf) <= 0) {
            confirm_ok = 0;
            break;
        }

        WhereClause ss_wc_final[2];
        strncpy(ss_wc_final[0].col_name, "show_id", 63); ss_wc_final[0].col_name[63] = '\0';
        ss_wc_final[0].op = OP_EQ; ss_wc_final[0].value = &show_id; ss_wc_final[0].logic = 0;
        strncpy(ss_wc_final[1].col_name, "seat_id", 63); ss_wc_final[1].col_name[63] = '\0';
        ss_wc_final[1].op = OP_EQ; ss_wc_final[1].value = &seat_ids[i]; ss_wc_final[1].logic = 0;

        db_update("seat_status", ss_wc_final, 2, "booking_id", &new_bk_id);
    }

    if (!confirm_ok) {
        wal_rollback();
        printf("\n  \033[31mSeat confirmation failed due to concurrent update.\033[0m\n");
        printf("  Booking was not finalized.\n\n");
        return;
    }

    wal_commit();

    if (promo_id_used > 0)
        increment_promo_uses(promo_id_used);

    /* Clear hold state from session */
    free(ctx->held_seat_ids);
    ctx->held_seat_ids   = NULL;
    ctx->held_seat_count = 0;

    /* Refresh wallet balance after payment */
    WhereClause u_wc[1];
    strncpy(u_wc[0].col_name, "user_id", 63); u_wc[0].col_name[63] = '\0';
    u_wc[0].op = OP_EQ; u_wc[0].value = &ctx->user_id; u_wc[0].logic = 0;
    ResultSet *u_rs = db_select("users", u_wc, 1, NULL, 0);
    /* users: 0=user_id ... 6=wallet_balance */
    if (u_rs && u_rs->row_count > 0)
        ctx->wallet_balance = safe_float(u_rs->rows, 0, 6);
    result_set_free(u_rs);

    /* ── Confirmation receipt ────────────────────────────────────────────── */
    print_booking_receipt(new_bk_id, movie_title, show_datetime,
                          screen_name, screen_type_label(scr_type),
                          seat_ids, seat_count, total_amt, ctx);

    printf("  Press Enter to continue...");
    read_line(input, (int)sizeof(input));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * view_upcoming_bookings — CONFIRMED bookings with future show dates
 * ═══════════════════════════════════════════════════════════════════════════*/
void view_upcoming_bookings(SessionContext *ctx)
{
    char input[32];
    banner("MY UPCOMING BOOKINGS");

    char now_str[20];
    build_now_str(now_str, sizeof(now_str));

    int status_conf = 1;
    WhereClause bk_wc[2];
    strncpy(bk_wc[0].col_name, "user_id", 63); bk_wc[0].col_name[63] = '\0';
    bk_wc[0].op = OP_EQ; bk_wc[0].value = &ctx->user_id; bk_wc[0].logic = 0;
    strncpy(bk_wc[1].col_name, "status",  63); bk_wc[1].col_name[63] = '\0';
    bk_wc[1].op = OP_EQ; bk_wc[1].value = &status_conf;  bk_wc[1].logic = 0;

    ResultSet *bk_rs = db_select("bookings", bk_wc, 2, NULL, 0);

    printf("  %-8s  %-22s  %-17s  %-5s  %s\n",
           "Bk.ID", "Movie", "Date & Time", "Seats", "Total");
    hr();

    typedef struct {
        int booking_id;
        int show_id;
        int seat_count;
        float total_amt;
        char movie[64];
        char show_dt[20];
    } UpcomingRow;

    UpcomingRow rows[256];
    int row_count = 0;
    int printed = 0;

    if (bk_rs) {
        for (int i = 0; i < bk_rs->row_count; i++) {
            int   bk_id      = safe_int  (bk_rs->rows, i, 0);
            int   bk_show_id = safe_int  (bk_rs->rows, i, 2);
            int   seat_ct    = safe_int  (bk_rs->rows, i, 3);
            float total_amt  = safe_float(bk_rs->rows, i, 8);

            WhereClause sh_wc[1];
            strncpy(sh_wc[0].col_name, "show_id", 63); sh_wc[0].col_name[63] = '\0';
            sh_wc[0].op = OP_EQ; sh_wc[0].value = &bk_show_id; sh_wc[0].logic = 0;
            ResultSet *sh_rs = db_select("shows", sh_wc, 1, NULL, 0);
            if (!sh_rs || sh_rs->row_count == 0) { result_set_free(sh_rs); continue; }

            const char *show_dt = safe_str(sh_rs->rows, 0, 3);
            int         mv_id   = safe_int(sh_rs->rows, 0, 1);

            if (strcmp(show_dt, now_str) < 0) { result_set_free(sh_rs); continue; }

            char dt_buf[20];
            strncpy(dt_buf, show_dt, 19); dt_buf[19] = '\0';
            result_set_free(sh_rs);

            WhereClause mv_wc[1];
            strncpy(mv_wc[0].col_name, "movie_id", 63); mv_wc[0].col_name[63] = '\0';
            mv_wc[0].op = OP_EQ; mv_wc[0].value = &mv_id; mv_wc[0].logic = 0;
            ResultSet *mv_rs = db_select("movies", mv_wc, 1, NULL, 0);
            char mv_t[64] = "(Unknown)";
            if (mv_rs && mv_rs->row_count > 0)
                snprintf(mv_t, sizeof(mv_t), "%s", safe_str(mv_rs->rows, 0, 2));
            result_set_free(mv_rs);

            printf("  %-8d  %-22.22s  %-17s  %-5d  %s%.2f\n",
                   bk_id, mv_t, dt_buf, seat_ct,
                   ctx->currency_sym, total_amt);

            if (row_count < (int)(sizeof(rows) / sizeof(rows[0]))) {
                rows[row_count].booking_id = bk_id;
                rows[row_count].show_id = bk_show_id;
                rows[row_count].seat_count = seat_ct;
                rows[row_count].total_amt = total_amt;
                strncpy(rows[row_count].movie, mv_t, sizeof(rows[row_count].movie) - 1);
                rows[row_count].movie[sizeof(rows[row_count].movie) - 1] = '\0';
                strncpy(rows[row_count].show_dt, dt_buf, sizeof(rows[row_count].show_dt) - 1);
                rows[row_count].show_dt[sizeof(rows[row_count].show_dt) - 1] = '\0';
                row_count++;
            }
            printed++;
        }
        result_set_free(bk_rs);
    }

    if (printed == 0) {
        printf("  No upcoming bookings.\n");
        printf("\n  Press Enter to continue...");
        read_line(input, (int)sizeof(input));
        return;
    }

    hr();
    printf("  %d booking(s).\n", printed);
    printf("  Enter Booking ID to view ticket (0 to back): ");
    read_line(input, (int)sizeof(input));

    if (input[0] == '\0' || input[0] == '0')
        return;

    int selected_id = atoi(input);
    int sel = -1;
    for (int i = 0; i < row_count; i++) {
        if (rows[i].booking_id == selected_id) {
            sel = i;
            break;
        }
    }

    if (sel < 0) {
        printf("  Invalid Booking ID.\n");
        printf("\n  Press Enter to continue...");
        read_line(input, (int)sizeof(input));
        return;
    }

    int seat_ids[64];
    int seat_count = 0;
    WhereClause bs_wc[1];
    strncpy(bs_wc[0].col_name, "booking_id", 63); bs_wc[0].col_name[63] = '\0';
    bs_wc[0].op = OP_EQ; bs_wc[0].value = &rows[sel].booking_id; bs_wc[0].logic = 0;

    ResultSet *bs_rs = db_select("booking_seats", bs_wc, 1, NULL, 0);
    if (bs_rs) {
        for (int i = 0; i < bs_rs->row_count && seat_count < 64; i++)
            seat_ids[seat_count++] = safe_int(bs_rs->rows, i, 2);
        result_set_free(bs_rs);
    }

    char seat_list[192];
    build_seat_list_from_ids(seat_ids, seat_count, seat_list, sizeof(seat_list), NULL);

    time_t now = time(NULL);
    time_t show_t = parse_datetime_str(rows[sel].show_dt);
    int cancellable = (show_t != (time_t)-1 && show_t > (now + 6 * 3600));

    printf("\n  ┌────────────────────────────────────────────────────────────┐\n");
    printf("  │ UPCOMING TICKET                                            │\n");
    printf("  ├────────────────────────────────────────────────────────────┤\n");
    printf("  │ Booking ID : %-46d│\n", rows[sel].booking_id);
    printf("  │ Movie      : %-46.46s│\n", rows[sel].movie);
    printf("  │ Date & Time: %-46.46s│\n", rows[sel].show_dt);
    printf("  │ Seats      : %-46.46s│\n", seat_list[0] ? seat_list : "-");
    printf("  │ Status     : %-46s│\n",
           cancellable
             ? ANSI_GREEN "CANCELLABLE" ANSI_RESET
             : ANSI_YELLOW "CANCELLATION WINDOW CLOSING" ANSI_RESET);
    printf("  └────────────────────────────────────────────────────────────┘\n");

    printf("\n  Press Enter to continue...");
    read_line(input, (int)sizeof(input));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * view_past_bookings — CONFIRMED + CANCELLED bookings with past show dates
 *
 * The query engine supports AND-only WHERE clauses, so we issue two separate
 * queries (status=1 and status=2) and process both result sets.
 * ═══════════════════════════════════════════════════════════════════════════*/
void view_past_bookings(SessionContext *ctx)
{
    char input[32];
    banner("MY PAST BOOKINGS");

    char now_str[20];
    build_now_str(now_str, sizeof(now_str));

    printf("  %-8s  %-22s  %-17s  %-5s  %-13s  %s\n",
           "Bk.ID", "Movie", "Date & Time", "Seats", "Status", "Total");
    hr();

    int printed = 0;

    /* Process one status value: 1 = Confirmed, 2 = Cancelled */
    for (int pass = 0; pass < 2; pass++) {
        int status_val = (pass == 0) ? 1 : 2;
        const char *status_label = (pass == 0)
                                 ? ANSI_GREEN "Confirmed" ANSI_RESET
                                 : ANSI_RED   "Cancelled" ANSI_RESET;

        WhereClause bk_wc[2];
        strncpy(bk_wc[0].col_name, "user_id", 63); bk_wc[0].col_name[63] = '\0';
        bk_wc[0].op = OP_EQ; bk_wc[0].value = &ctx->user_id; bk_wc[0].logic = 0;
        strncpy(bk_wc[1].col_name, "status",  63); bk_wc[1].col_name[63] = '\0';
        bk_wc[1].op = OP_EQ; bk_wc[1].value = &status_val;   bk_wc[1].logic = 0;

        ResultSet *bk_rs = db_select("bookings", bk_wc, 2, NULL, 0);
        if (!bk_rs) continue;

        for (int i = 0; i < bk_rs->row_count; i++) {
            int   bk_id      = safe_int  (bk_rs->rows, i, 0);
            int   bk_show_id = safe_int  (bk_rs->rows, i, 2);
            int   seat_ct    = safe_int  (bk_rs->rows, i, 3);
            float total_amt  = safe_float(bk_rs->rows, i, 8);

            WhereClause sh_wc[1];
            strncpy(sh_wc[0].col_name, "show_id", 63); sh_wc[0].col_name[63] = '\0';
            sh_wc[0].op = OP_EQ; sh_wc[0].value = &bk_show_id; sh_wc[0].logic = 0;
            ResultSet *sh_rs = db_select("shows", sh_wc, 1, NULL, 0);
            if (!sh_rs || sh_rs->row_count == 0) { result_set_free(sh_rs); continue; }

            const char *show_dt = safe_str(sh_rs->rows, 0, 3);
            int         mv_id   = safe_int(sh_rs->rows, 0, 1);

            /* Only past shows */
            if (strcmp(show_dt, now_str) >= 0) { result_set_free(sh_rs); continue; }

            char dt_buf[20];
            strncpy(dt_buf, show_dt, 19); dt_buf[19] = '\0';
            result_set_free(sh_rs);

            WhereClause mv_wc[1];
            strncpy(mv_wc[0].col_name, "movie_id", 63); mv_wc[0].col_name[63] = '\0';
            mv_wc[0].op = OP_EQ; mv_wc[0].value = &mv_id; mv_wc[0].logic = 0;
            ResultSet *mv_rs = db_select("movies", mv_wc, 1, NULL, 0);
            char mv_t[23] = "(Unknown)";
            if (mv_rs && mv_rs->row_count > 0)
                snprintf(mv_t, sizeof(mv_t), "%s", safe_str(mv_rs->rows, 0, 2));
            result_set_free(mv_rs);

            printf("  %-8d  %-22s  %-17s  %-5d  %-13s  %s%.2f\n",
                   bk_id, mv_t, dt_buf, seat_ct,
                   status_label, ctx->currency_sym, total_amt);
            printed++;
        }
        result_set_free(bk_rs);
    }

    if (printed == 0)
        printf("  No past bookings found.\n");
    else { hr(); printf("  %d past booking(s).\n", printed); }

    printf("\n  Press Enter to continue...");
    read_line(input, (int)sizeof(input));
}