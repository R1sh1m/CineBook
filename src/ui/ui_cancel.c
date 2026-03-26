/*
 * ui_cancel.c — Cancellation UI for CineBook
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "query.h"    /* db_select, WhereClause, ResultSet, result_set_free */
#include "session.h"  /* SessionContext */
#include "refund.h"   /* compute_refund, execute_cancellation, RefundResult */

#define MAX_CANCELLABLE  64
#define DATETIME_LEN     20    /* "YYYY-MM-DD HH:MM\0" */

/* ─────────────────────────────────────────────────────────────────────────── */
static void now_string(char buf[DATETIME_LEN])
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, DATETIME_LEN, "%Y-%m-%d %H:%M", lt);
}

static void strip_newline(char *s)
{
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') s[len - 1] = '\0';
}

static const char *cell_str(ResultSet *rs, int row, int col)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return "";
    if (!rs->rows[row][col]) return "";
    return (const char *)rs->rows[row][col];
}

static int cell_int(ResultSet *rs, int row, int col)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return 0;
    if (!rs->rows[row][col]) return 0;
    return *(int32_t *)rs->rows[row][col];
}

static float cell_float(ResultSet *rs, int row, int col)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return 0.0f;
    if (!rs->rows[row][col]) return 0.0f;
    return *(float *)rs->rows[row][col];
}

static int parse_show_datetime(const char *dt, struct tm *out_tm)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (!dt || !out_tm) return 0;

    if (sscanf(dt, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5)
        return 0;

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = y - 1900;
    out_tm->tm_mon  = mo - 1;
    out_tm->tm_mday = d;
    out_tm->tm_hour = h;
    out_tm->tm_min  = mi;
    out_tm->tm_sec  = 0;
    out_tm->tm_isdst = -1;
    return 1;
}

static double hours_until_show(const char *show_datetime)
{
    struct tm show_tm;
    if (!parse_show_datetime(show_datetime, &show_tm))
        return -1.0;

    time_t show_t = mktime(&show_tm);
    time_t now_t = time(NULL);
    if (show_t == (time_t)-1 || now_t == (time_t)-1)
        return -1.0;

    return difftime(show_t, now_t) / 3600.0;
}

static int get_refund_tier_percent(double hours_left)
{
    if (hours_left > 72.0) return 100;
    if (hours_left >= 24.0) return 75;
    if (hours_left >= 6.0) return 50;
    return 0;
}

static const char *get_time_left_label(double hours_left)
{
    if (hours_left > 72.0) {
        return "\033[32m>72 hrs (Full refund)\033[0m";
    }
    if (hours_left >= 24.0) {
        return "\033[32m24-72 h (75% refund)\033[0m";
    }
    if (hours_left >= 6.0) {
        return "\033[33m6-24 hrs (50% refund)\033[0m";
    }
    return "\033[31m< 6 hrs  (No refund)\033[0m";
}

static float estimate_refund_amount(float gross, float conv_fee, int tier_pct)
{
    float est = (gross * ((float)tier_pct / 100.0f)) - conv_fee;
    if (est < 0.0f) est = 0.0f;
    return est;
}

typedef struct {
    int   booking_id;
    int   show_id;
    int   movie_id;
    int   seat_count;
    float total_amount;
    float convenience_fee;
    char  show_datetime[DATETIME_LEN];
    char  movie_title[201];
} CancellableBooking;

static void load_seat_summary(int booking_id, char *out, size_t out_sz, int *seat_count_out)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (seat_count_out) *seat_count_out = 0;

    WhereClause bs_wc[1];
    bs_wc[0].op = OP_EQ;
    bs_wc[0].value = &booking_id;
    bs_wc[0].logic = 0;
    strncpy(bs_wc[0].col_name, "booking_id", 63);
    bs_wc[0].col_name[63] = '\0';

    ResultSet *bs_rs = db_select("booking_seats", bs_wc, 1, NULL, 0);
    if (!bs_rs || bs_rs->row_count == 0) {
        strncpy(out, "-", out_sz - 1);
        out[out_sz - 1] = '\0';
        if (bs_rs) result_set_free(bs_rs);
        return;
    }

    for (int i = 0; i < bs_rs->row_count; i++) {
        int seat_id = cell_int(bs_rs, i, 2);

        WhereClause s_wc[1];
        s_wc[0].op = OP_EQ;
        s_wc[0].value = &seat_id;
        s_wc[0].logic = 0;
        strncpy(s_wc[0].col_name, "seat_id", 63);
        s_wc[0].col_name[63] = '\0';

        ResultSet *s_rs = db_select("seats", s_wc, 1, NULL, 0);
        if (!s_rs || s_rs->row_count == 0) {
            if (s_rs) result_set_free(s_rs);
            continue;
        }

        const char *row_label = cell_str(s_rs, 0, 2);
        int seat_num = cell_int(s_rs, 0, 3);

        char seat_tok[24];
        snprintf(seat_tok, sizeof(seat_tok), "%s%d", row_label, seat_num);

        if (out[0] != '\0') {
            size_t used = strlen(out);
            if (used + 2 < out_sz) strncat(out, ", ", out_sz - used - 1);
        }
        {
            size_t used = strlen(out);
            if (used + strlen(seat_tok) < out_sz) strncat(out, seat_tok, out_sz - used - 1);
        }

        if (seat_count_out) (*seat_count_out)++;
        result_set_free(s_rs);
    }

    if (out[0] == '\0') {
        strncpy(out, "-", out_sz - 1);
        out[out_sz - 1] = '\0';
    }

    result_set_free(bs_rs);
}

static const char *policy_tier_label(const RefundResult *rr)
{
    if (!rr) return "Unknown";
    if (rr->refund_percentage >= 100) return "Full Refund (>72 hours)";
    if (rr->refund_percentage >= 75)  return "Partial Refund (24-72 hours)";
    if (rr->refund_percentage >= 50)  return "Partial Refund (6-24 hours)";
    return "No Refund (<6 hours)";
}

/* ─────────────────────────────────────────────────────────────────────────── */
static void render_refund_breakdown(const CancellableBooking *bk,
                                    const RefundResult       *rr,
                                    const SessionContext     *ctx)
{
    char seat_list[96];
    int seat_count = 0;
    float refundable_base = rr->gross_paid - rr->convenience_fee_deducted;
    float preview_wallet = ctx->wallet_balance + rr->refund_amount;
    const char *amt_color = (rr->refund_amount > 0.0f) ? "\033[32m" : "\033[31m";

    load_seat_summary(bk->booking_id, seat_list, sizeof(seat_list), &seat_count);
    if (seat_count <= 0) seat_count = bk->seat_count;

    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════╗\n");
    printf("  ║  CANCELLATION SUMMARY                                    ║\n");
    printf("  ╠══════════════════════════════════════════════════════════╣\n");
    printf("  ║  Film     : %-44.44s                                     ║\n", bk->movie_title);
    printf("  ║  Show     : %-44.44s                                     ║\n", bk->show_datetime);

    {
        char seats_line[128];
        snprintf(seats_line, sizeof(seats_line), "%s   (%d seats)", seat_list, seat_count);
        printf("  ║  Seats    : %-44.44s                                 ║\n", seats_line);
    }

    printf("  ╠══════════════════════════════════════════════════════════╣\n");
    printf("  ║  Original payment  :                         Rs.%8.2f    ║\n", rr->gross_paid);
    printf("  ║  Convenience fee   :                        -Rs.%8.2f    ║\n", rr->convenience_fee_deducted);
    printf("  ║  Refundable base   :                         Rs.%8.2f    ║\n", refundable_base);
    printf("  ╠══════════════════════════════════════════════════════════╣\n");
    printf("  ║  Policy tier       :                             %-32.32s║\n", policy_tier_label(rr));
    printf("  ║  Refund %%          :                               %-32d║\n", rr->refund_percentage);
    printf("  ║  REFUND AMOUNT     :                   %sRs.%8.2f\033[0m ║\n", amt_color, rr->refund_amount);
    printf("  ╠══════════════════════════════════════════════════════════╣\n");
    printf("  ║  Refund to         :  CineBook Wallet (instant)          ║\n");
    printf("  ║  New wallet balance:                        Rs.%8.2f     ║\n", preview_wallet);
    printf("  ╚══════════════════════════════════════════════════════════╝\n");

    if (rr->refund_amount <= 0.0f) {
        printf("\033[31m  ✗ No refund applies. The Rs.60 convenience fee is also forfeited.\033[0m\n");
    }
    printf("\n");
}

/* ─────────────────────────────────────────────────────────────────────────── */
static void render_booking_list(const CancellableBooking *list,
                                int                       count,
                                const char               *curr_sym)
{
    (void)curr_sym;

    printf("\n");
    printf("  %-3s %-7s %-24s %-16s %-5s %-10s %-30s %-12s\n",
           "#", "Bk.ID", "Movie", "Show Date/Time", "Seat", "Total", "Time Left", "Est. Refund");
    printf("  %-3s %-7s %-24s %-16s %-5s %-10s %-30s %-12s\n",
           "---", "-------", "------------------------", "----------------",
           "-----", "----------", "------------------------------", "------------");

    for (int i = 0; i < count; i++) {
        const CancellableBooking *b = &list[i];
        double hrs_left = hours_until_show(b->show_datetime);
        int tier_pct = get_refund_tier_percent(hrs_left);
        float est_refund = estimate_refund_amount(b->total_amount, b->convenience_fee, tier_pct);

        char title_buf[25];
        strncpy(title_buf, b->movie_title, 24);
        title_buf[24] = '\0';

        printf("  %-3d %-7d %-24s %-16s %-5d Rs.%7.2f %-30s Rs.%9.2f\n",
               i + 1,
               b->booking_id,
               title_buf,
               b->show_datetime,
               b->seat_count,
               b->total_amount,
               get_time_left_label(hrs_left),
               est_refund);
    }
    printf("\n");
}

/* ─────────────────────────────────────────────────────────────────────────── */
void cancel_booking_menu(SessionContext *ctx)
{
    if (!ctx || ctx->user_id == 0) {
        printf("\n  [!] You must be logged in to cancel a booking.\n");
        return;
    }

    const char *curr = session_get_currency_sym(ctx);

    int status_confirmed = 1;
    WhereClause bk_where[2];

    int uid = ctx->user_id;
    bk_where[0].op    = OP_EQ;
    bk_where[0].value = &uid;
    bk_where[0].logic = 0;
    strncpy(bk_where[0].col_name, "user_id", 63);
    bk_where[0].col_name[63] = '\0';

    bk_where[1].op    = OP_EQ;
    bk_where[1].value = &status_confirmed;
    bk_where[1].logic = 0;
    strncpy(bk_where[1].col_name, "status", 63);
    bk_where[1].col_name[63] = '\0';

    ResultSet *bk_rs = db_select("bookings", bk_where, 2, NULL, 0);

    if (!bk_rs || bk_rs->row_count == 0) {
        printf("\n  You have no confirmed bookings available to cancel.\n\n");
        if (bk_rs) result_set_free(bk_rs);
        printf("  Press Enter to continue...");
        fflush(stdout);
        {
            char dummy[64];
            fgets(dummy, sizeof(dummy), stdin);
        }
        return;
    }

    char now_str[DATETIME_LEN];
    now_string(now_str);

    CancellableBooking list[MAX_CANCELLABLE];
    int list_count = 0;

    for (int i = 0; i < bk_rs->row_count && list_count < MAX_CANCELLABLE; i++) {
        int booking_id   = cell_int(bk_rs, i, 0);
        int show_id      = cell_int(bk_rs, i, 2);
        int seat_count   = cell_int(bk_rs, i, 3);
        float total_amt  = cell_float(bk_rs, i, 8);
        float conv_fee   = cell_float(bk_rs, i, 6);

        int sid = show_id;
        WhereClause sw[1];
        sw[0].op    = OP_EQ;
        sw[0].value = &sid;
        sw[0].logic = 0;
        strncpy(sw[0].col_name, "show_id", 63);
        sw[0].col_name[63] = '\0';

        ResultSet *show_rs = db_select("shows", sw, 1, NULL, 0);
        if (!show_rs || show_rs->row_count == 0) {
            if (show_rs) result_set_free(show_rs);
            continue;
        }

        const char *show_dt = cell_str(show_rs, 0, 3);
        int movie_id = cell_int(show_rs, 0, 1);
        int is_active = cell_int(show_rs, 0, 5);

        if (strcmp(show_dt, now_str) <= 0 || is_active == 0) {
            result_set_free(show_rs);
            continue;
        }

        char dt_copy[DATETIME_LEN];
        strncpy(dt_copy, show_dt, DATETIME_LEN - 1);
        dt_copy[DATETIME_LEN - 1] = '\0';
        result_set_free(show_rs);

        int mid = movie_id;
        WhereClause mw[1];
        mw[0].op    = OP_EQ;
        mw[0].value = &mid;
        mw[0].logic = 0;
        strncpy(mw[0].col_name, "movie_id", 63);
        mw[0].col_name[63] = '\0';

        ResultSet *mv_rs = db_select("movies", mw, 1, NULL, 0);
        char title_buf[201];
        if (mv_rs && mv_rs->row_count > 0) {
            const char *t = cell_str(mv_rs, 0, 2);
            strncpy(title_buf, t, 200);
            title_buf[200] = '\0';
        } else {
            strncpy(title_buf, "(Unknown Movie)", 200);
            title_buf[200] = '\0';
        }
        if (mv_rs) result_set_free(mv_rs);

        CancellableBooking *b = &list[list_count++];
        b->booking_id      = booking_id;
        b->show_id         = show_id;
        b->movie_id        = movie_id;
        b->seat_count      = seat_count;
        b->total_amount    = total_amt;
        b->convenience_fee = conv_fee;
        strncpy(b->show_datetime, dt_copy, DATETIME_LEN - 1);
        b->show_datetime[DATETIME_LEN - 1] = '\0';
        strncpy(b->movie_title, title_buf, 200);
        b->movie_title[200] = '\0';
    }

    result_set_free(bk_rs);

    if (list_count == 0) {
        printf("\n  You have no upcoming bookings eligible for cancellation.\n");
        printf("  (Past or already-cancelled shows cannot be refunded.)\n\n");
        printf("  Press Enter to continue...");
        fflush(stdout);
        {
            char dummy[64];
            fgets(dummy, sizeof(dummy), stdin);
        }
        return;
    }

    printf("\n  ══════════════════════════════════════════════════\n");
    printf("              CANCEL A BOOKING\n");
    printf("  ══════════════════════════════════════════════════\n");

    render_booking_list(list, list_count, curr);

    char input_buf[16];
    int choice = -1;

    while (1) {
        printf("  Enter booking number to cancel (1-%d), or 0 to go back: ", list_count);
        fflush(stdout);

        if (!fgets(input_buf, sizeof(input_buf), stdin)) break;
        strip_newline(input_buf);

        char *endptr = NULL;
        long val = strtol(input_buf, &endptr, 10);

        if (endptr == input_buf || *endptr != '\0') {
            printf("  [!] Invalid input. Please enter a number.\n");
            continue;
        }

        if (val == 0) {
            printf("\n  Cancellation aborted. Returning to menu.\n\n");
            return;
        }

        if (val < 1 || val > list_count) {
            printf("  [!] Please enter a number between 1 and %d.\n", list_count);
            continue;
        }

        choice = (int)(val - 1);
        break;
    }

    if (choice < 0) return;

    CancellableBooking *selected = &list[choice];

    printf("\n  Loading refund details for Booking #%d...\n", selected->booking_id);

    RefundResult *rr = compute_refund(selected->booking_id, ctx);
    if (!rr) {
        printf("\n  [!] Unable to compute refund details. ");
        printf("The booking may have already been cancelled.\n");
        printf("  Please try again or contact support.\n\n");
        printf("  Press Enter to continue...");
        fflush(stdout);
        {
            char dummy[64];
            fgets(dummy, sizeof(dummy), stdin);
        }
        return;
    }

    render_refund_breakdown(selected, rr, ctx);

    {
        char confirm_buf[32];
        if (rr->refund_amount > 0.0f) {
            printf("  Type 'CANCEL' to confirm and receive Rs.%.2f refund: ", rr->refund_amount);
        } else {
            printf("  \033[31mType 'CANCEL' to confirm. No refund will be issued: \033[0m");
        }
        fflush(stdout);

        if (!fgets(confirm_buf, sizeof(confirm_buf), stdin)) {
            free(rr);
            return;
        }
        strip_newline(confirm_buf);

        if (strcmp(confirm_buf, "CANCEL") != 0) {
            printf("\n  Cancellation aborted. Your booking is unchanged.\n\n");
            free(rr);
            return;
        }
    }

    {
        float rr_amount = rr->refund_amount;
        int booking_id = selected->booking_id;
        free(rr);
        rr = NULL;

        printf("\n  Processing cancellation...\n");
        if (execute_cancellation(booking_id, ctx)) {
            printf("\n\033[32m  ✓ Booking #%d cancelled successfully.\033[0m\n", booking_id);
            printf("  \033[36m  Rs.%.2f has been added to your wallet.\033[0m\n", rr_amount);
            printf("  New wallet balance: \033[1mRs.%.2f\033[0m\n\n", ctx->wallet_balance);
        } else {
            printf("\n  [!] Cancellation failed. No changes were made.\n");
            printf("  Please try again. If the problem persists, contact support.\n\n");
        }
    }

    printf("  Press Enter to continue...");
    fflush(stdout);
    {
        char dummy[64];
        fgets(dummy, sizeof(dummy), stdin);
    }
}