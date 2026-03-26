/*
 * ui_dashboard.c — Admin Analytics Dashboard UI for CineBook
 *
 * Public surface:
 *   void dashboard_menu(SessionContext *ctx)
 *
 * Options:
 *   1. Occupancy report
 *   2. Revenue report
 *   3. Booking trends report
 *   4. All three reports
 *   5. Set lookback days
 *   6. Set theatre filter
 *   7. Export summary CSV  → exports/dashboard_YYYYMMDD_HHMMSS.csv
 *   0. Back
 *
 * Guard: ctx->is_admin must be 1.
 * C11.  No external libs.
 * Depends on: query.h, session.h, reports.h (locked contracts).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "query.h"    /* db_select, db_join, WhereClause, ResultSet,
                         result_set_free                                     */
#include "session.h"  /* SessionContext                                       */
#include "reports.h"  /* run_occupancy_report, run_revenue_report,
                         run_booking_report, run_report                      */

/* ─────────────────────────────────────────────────────────────────────────────
 * File-scope dashboard state
 * ───────────────────────────────────────────────────────────────────────────*/
static char g_last_export_time[32] = "Never";

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────────*/

static void strip_nl(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n') s[n - 1] = '\0';
}

static void press_enter(void)
{
    printf("  Press Enter to continue...");
    fflush(stdout);
    char b[64];
    fgets(b, sizeof(b), stdin);
}

/* Read a long from user; returns LONG_MIN-ish sentinel on bad/empty input. */
static long read_long(const char *prompt)
{
    char buf[32];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return (long)(-0x7FFFFFFFL - 1);
    strip_nl(buf);
    if (buf[0] == '\0') return (long)(-0x7FFFFFFFL - 1);
    char *ep = NULL;
    long v = strtol(buf, &ep, 10);
    if (ep == buf || *ep != '\0') return (long)(-0x7FFFFFFFL - 1);
    return v;
}

static int rs_int(ResultSet *rs, int row, int col, int def)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return def;
    if (!rs->rows[row][col]) return def;
    return (int)(*(int32_t *)rs->rows[row][col]);
}

static float rs_float(ResultSet *rs, int row, int col, float def)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return def;
    if (!rs->rows[row][col]) return def;
    return *(float *)rs->rows[row][col];
}

static void print_report_complete_separator(void)
{
    printf("\n  \033[90m─── Report complete. Scroll up to view. ───\033[0m\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Quick stats helpers
 *
 * Note:
 *   Quick stats intentionally use only bookings + shows, per requirement.
 *   Theatre-level filter is not applied here because theatre_id is not present
 *   in those two tables directly.
 * ───────────────────────────────────────────────────────────────────────────*/

typedef struct DashboardQuickStats {
    int   confirmed_bookings;
    float revenue;
    float avg_occupancy;
} DashboardQuickStats;

static void build_cutoff_datetime(int days, char *out_buf, size_t out_sz)
{
    if (days <= 0) days = 30;
    time_t now_t = time(NULL);
    time_t cutoff_t = now_t - (time_t)days * 24 * 60 * 60;
    struct tm *lt = localtime(&cutoff_t);
    strftime(out_buf, out_sz, "%Y-%m-%d %H:%M:%S", lt);
}

static int contains_int(const int *arr, int count, int target)
{
    for (int i = 0; i < count; i++) {
        if (arr[i] == target) return 1;
    }
    return 0;
}

static DashboardQuickStats compute_quick_stats(const SessionContext *ctx)
{
    DashboardQuickStats qs;
    qs.confirmed_bookings = 0;
    qs.revenue = 0.0f;
    qs.avg_occupancy = 0.0f;

    char cutoff[32];
    build_cutoff_datetime(ctx->dashboard_days, cutoff, sizeof(cutoff));

    WhereClause sw[1];
    memset(sw, 0, sizeof(sw));
    strncpy(sw[0].col_name, "show_datetime", sizeof(sw[0].col_name) - 1);
    sw[0].op = OP_GTE;
    sw[0].value = cutoff;
    sw[0].logic = 0;

    ResultSet *shows_rs = db_select("shows", sw, 1, NULL, 0);
    if (!shows_rs || shows_rs->row_count <= 0) {
        if (shows_rs) result_set_free(shows_rs);
        return qs;
    }

    int show_count = shows_rs->row_count;
    int *show_ids = (int *)calloc((size_t)show_count, sizeof(int));
    if (!show_ids) {
        result_set_free(shows_rs);
        return qs;
    }

    for (int i = 0; i < show_count; i++) {
        show_ids[i] = rs_int(shows_rs, i, 0, 0); /* shows.show_id */
    }
    result_set_free(shows_rs);

    ResultSet *bk_rs = db_select("bookings", NULL, 0, NULL, 0);
    if (!bk_rs) {
        free(show_ids);
        return qs;
    }

    int occupancy_denominator = 0; /* confirmed + cancelled in window */

    for (int i = 0; i < bk_rs->row_count; i++) {
        int sid = rs_int(bk_rs, i, 2, 0);         /* bookings.show_id */
        int status = rs_int(bk_rs, i, 11, -1);    /* bookings.status */
        float amt = rs_float(bk_rs, i, 8, 0.0f);  /* bookings.total_amount */

        if (!contains_int(show_ids, show_count, sid)) continue;

        if (status == 1) {
            qs.confirmed_bookings++;
            qs.revenue += amt;
            occupancy_denominator++;
        } else if (status == 2) {
            occupancy_denominator++;
        }
    }

    if (occupancy_denominator > 0) {
        qs.avg_occupancy =
            (100.0f * (float)qs.confirmed_bookings) / (float)occupancy_denominator;
    }

    result_set_free(bk_rs);
    free(show_ids);
    return qs;
}

static void print_quick_stats(const SessionContext *ctx)
{
    DashboardQuickStats qs = compute_quick_stats(ctx);

    char revenue_buf[64];
    snprintf(revenue_buf, sizeof(revenue_buf), "%.2f", (double)qs.revenue);

    printf("  Quick stats: %d confirmed bookings · Rs.%s revenue · %.1f%% avg occupancy\n",
           qs.confirmed_bookings, revenue_buf, qs.avg_occupancy);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ensure_exports_dir — create exports/ directory if absent (best-effort)
 * ───────────────────────────────────────────────────────────────────────────*/
static void ensure_exports_dir(void)
{
#ifdef _WIN32
    system("if not exist exports mkdir exports");
#else
    mkdir("exports", 0755);
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * build_export_filename — fills out_buf with
 * "exports/dashboard_YYYYMMDD_HHMMSS.csv"
 * ───────────────────────────────────────────────────────────────────────────*/
static void build_export_filename(char *out_buf, size_t max_len)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out_buf, max_len, "exports/dashboard_%Y%m%d_%H%M%S.csv", lt);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * csv_export
 *
 * Section A — metadata header
 * Section B — booking count per show_id  (db_select bookings, iterate & count)
 * Section C — revenue per movie_id       (db_join bookings←shows, sum totals)
 * Returns 1 on success, 0 on failure.
 * ───────────────────────────────────────────────────────────────────────────*/
static int csv_export(SessionContext *ctx)
{
    ensure_exports_dir();

    char filename[128];
    build_export_filename(filename, sizeof(filename));

    FILE *f = fopen(filename, "w");
    if (!f) {
        printf("  [!] Could not open '%s' for writing.\n", filename);
        return 0;
    }

    /* ── Section A: metadata header ── */
    time_t now_t = time(NULL);
    char now_str[32];
    strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S", localtime(&now_t));

    fprintf(f, "CINEBOOK DASHBOARD EXPORT\n");
    fprintf(f, "Generated,%s\n", now_str);
    fprintf(f, "Exported by,%s\n", ctx->name);
    fprintf(f, "Lookback days,%d\n", ctx->dashboard_days);
    fprintf(f, "Theatre filter,%s\n",
            ctx->dashboard_theatre_filter == 0
                ? "All theatres"
                : "Filtered");
    fprintf(f, "\n");

    /* ── Section B: booking count per show ── */
    fprintf(f, "SECTION,Booking Counts Per Show\n");
    fprintf(f, "show_id,booking_count,confirmed_count,cancelled_count\n");

    /*
     * Fetch all bookings (status=1 confirmed + status=2 cancelled).
     * We iterate the full result and tally per show_id using a simple
     * linear scan — no hash map needed for a dashboard CSV.
     * Max shows assumed <= 4096; realloc if needed.
     */
    ResultSet *bk_rs = db_select("bookings", NULL, 0, NULL, 0);
    if (bk_rs && bk_rs->row_count > 0) {

        /* Collect unique show_ids and counts in parallel arrays */
        int   max_shows  = 512;
        int  *show_ids   = (int  *)calloc((size_t)max_shows, sizeof(int));
        int  *total_cnt  = (int  *)calloc((size_t)max_shows, sizeof(int));
        int  *conf_cnt   = (int  *)calloc((size_t)max_shows, sizeof(int));
        int  *canc_cnt   = (int  *)calloc((size_t)max_shows, sizeof(int));
        int   ushow_cnt  = 0;

        if (show_ids && total_cnt && conf_cnt && canc_cnt) {
            for (int i = 0; i < bk_rs->row_count; i++) {
                /* bookings schema: booking_id(0) user_id(1) show_id(2)
                   ... status(11) */
                int sid    = rs_int(bk_rs, i, 2, 0);
                int status = rs_int(bk_rs, i, 11, -1);

                /* Only care about CONFIRMED(1) and CANCELLED(2) */
                if (status != 1 && status != 2) continue;

                /* Find or insert show_id slot */
                int slot = -1;
                for (int j = 0; j < ushow_cnt; j++) {
                    if (show_ids[j] == sid) { slot = j; break; }
                }
                if (slot < 0) {
                    if (ushow_cnt >= max_shows) {
                        /* grow arrays safely; keep old pointers if realloc fails */
                        int new_max = max_shows * 2;

                        void *tmp_show_ids = realloc(show_ids,
                                        (size_t)new_max * sizeof(int));
                        if (!tmp_show_ids) break;
                        show_ids = (int *)tmp_show_ids;

                        void *tmp_total_cnt = realloc(total_cnt,
                                        (size_t)new_max * sizeof(int));
                        if (!tmp_total_cnt) break;
                        total_cnt = (int *)tmp_total_cnt;

                        void *tmp_conf_cnt = realloc(conf_cnt,
                                        (size_t)new_max * sizeof(int));
                        if (!tmp_conf_cnt) break;
                        conf_cnt = (int *)tmp_conf_cnt;

                        void *tmp_canc_cnt = realloc(canc_cnt,
                                        (size_t)new_max * sizeof(int));
                        if (!tmp_canc_cnt) break;
                        canc_cnt = (int *)tmp_canc_cnt;

                        /* zero the new portion */
                        for (int k = ushow_cnt; k < new_max; k++) {
                            show_ids[k] = 0;
                            total_cnt[k] = conf_cnt[k] = canc_cnt[k] = 0;
                        }
                        max_shows = new_max;
                    }
                    slot = ushow_cnt;
                    show_ids[ushow_cnt++] = sid;
                    total_cnt[slot] = conf_cnt[slot] = canc_cnt[slot] = 0;
                }

                total_cnt[slot]++;
                if (status == 1) conf_cnt[slot]++;
                else             canc_cnt[slot]++;
            }

            for (int j = 0; j < ushow_cnt; j++) {
                fprintf(f, "%d,%d,%d,%d\n",
                        show_ids[j], total_cnt[j],
                        conf_cnt[j], canc_cnt[j]);
            }
        }

        free(show_ids);
        free(total_cnt);
        free(conf_cnt);
        free(canc_cnt);
    }
    if (bk_rs) result_set_free(bk_rs);

    fprintf(f, "\n");

    /* ── Section C: revenue per movie ── */
    fprintf(f, "SECTION,Revenue Per Movie\n");
    fprintf(f, "movie_id,booking_count,gross_revenue_INR\n");

    /*
     * db_join bookings ← shows ON bookings.show_id = shows.show_id.
     * Joined ResultSet columns: bookings cols (0..13) + shows cols (0..5)
     *   → bookings.show_id      = col 2
     *   → bookings.total_amount = col 8
     *   → bookings.status       = col 11
     *   → shows.movie_id        = col 14 + 1 = col 15
     *
     * shows schema: show_id(0) movie_id(1) screen_id(2) show_datetime(3)
     *               base_price(4) is_active(5)
     * In the joined row the shows columns start at offset 14 (bookings has 14
     * columns: indices 0-13).  So shows.movie_id is at joined index 14+1=15.
     */
    ResultSet *join_rs = db_join("bookings", "shows",
                                 "show_id", "show_id",
                                 NULL, 0);

    if (join_rs && join_rs->row_count > 0) {

        int   max_movies = 256;
        int  *mv_ids     = (int   *)calloc((size_t)max_movies, sizeof(int));
        int  *mv_cnt     = (int   *)calloc((size_t)max_movies, sizeof(int));
        float *mv_rev    = (float *)calloc((size_t)max_movies, sizeof(float));
        int   umv_cnt    = 0;

        if (mv_ids && mv_cnt && mv_rev) {
            for (int i = 0; i < join_rs->row_count; i++) {
                int   status  = rs_int  (join_rs, i, 11,  -1);
                float total   = rs_float(join_rs, i,  8, 0.0f);
                /* shows.movie_id: bookings has 14 columns (0..13),
                   shows.movie_id is column index 1 in shows → joined col 15 */
                int   movie_id = rs_int (join_rs, i, 15,  0);

                /* Only CONFIRMED bookings count for revenue */
                if (status != 1) continue;

                int slot = -1;
                for (int j = 0; j < umv_cnt; j++) {
                    if (mv_ids[j] == movie_id) { slot = j; break; }
                }
                if (slot < 0) {
                    if (umv_cnt >= max_movies) {
                        int new_max = max_movies * 2;

                        void *tmp_mv_ids = realloc(mv_ids,
                                      (size_t)new_max * sizeof(int));
                        if (!tmp_mv_ids) break;
                        mv_ids = (int *)tmp_mv_ids;

                        void *tmp_mv_cnt = realloc(mv_cnt,
                                      (size_t)new_max * sizeof(int));
                        if (!tmp_mv_cnt) break;
                        mv_cnt = (int *)tmp_mv_cnt;

                        void *tmp_mv_rev = realloc(mv_rev,
                                      (size_t)new_max * sizeof(float));
                        if (!tmp_mv_rev) break;
                        mv_rev = (float *)tmp_mv_rev;

                        for (int k = umv_cnt; k < new_max; k++) {
                            mv_ids[k] = 0; mv_cnt[k] = 0; mv_rev[k] = 0.0f;
                        }
                        max_movies = new_max;
                    }
                    slot = umv_cnt;
                    mv_ids[umv_cnt++] = movie_id;
                    mv_cnt[slot]  = 0;
                    mv_rev[slot]  = 0.0f;
                }

                mv_cnt[slot]++;
                mv_rev[slot] += total;
            }

            for (int j = 0; j < umv_cnt; j++) {
                fprintf(f, "%d,%d,%.2f\n",
                        mv_ids[j], mv_cnt[j], (double)mv_rev[j]);
            }
        }

        free(mv_ids);
        free(mv_cnt);
        free(mv_rev);
    }
    if (join_rs) result_set_free(join_rs);

    fprintf(f, "\n");
    fclose(f);

    strncpy(g_last_export_time, now_str, sizeof(g_last_export_time) - 1);
    g_last_export_time[sizeof(g_last_export_time) - 1] = '\0';

    printf("\033[32m  ✓ Exported to: %s\033[0m\n", filename);
    printf("\033[36m  Tip: open the CSV in Excel to view formatted tables.\033[0m\n");
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * print_dashboard_header
 * ───────────────────────────────────────────────────────────────────────────*/
static void print_dashboard_header(const SessionContext *ctx)
{
    char theatre_filter[32];
    char theatre_current[32];

    if (ctx->dashboard_theatre_filter == 0) {
        snprintf(theatre_filter, sizeof(theatre_filter), "All theatres");
        snprintf(theatre_current, sizeof(theatre_current), "all");
    } else {
        snprintf(theatre_filter, sizeof(theatre_filter),
                 "Theatre #%d", ctx->dashboard_theatre_filter);
        snprintf(theatre_current, sizeof(theatre_current),
                 "theatre #%d", ctx->dashboard_theatre_filter);
    }

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  ANALYTICS DASHBOARD                   CineBook Admin     ║\n");
    printf("╠═══════════════════╦═══════════════════╦═══════════════════╣\n");
    printf("║  Lookback         ║  Theatre Filter   ║  Last Export      ║\n");
    printf("║  \033[1m%-2d days\033[0m           ║  %-17s ║  %-16s      ║\n", ctx->dashboard_days, theatre_filter, g_last_export_time);
    printf("╠═══════════════════╩═══════════════════╩═══════════════════╣\n");
    printf("║  [ 1 ]  Occupancy Report   — fill rate per show           ║\n");
    printf("║  [ 2 ]  Revenue Report     — by movie and theatre         ║\n");
    printf("║  [ 3 ]  Booking Trends     — by hour and day of week      ║\n");
    printf("║  [ 4 ]  All Three Reports                                 ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  Settings:                                                ║\n");
    printf("║  [ 5 ]  Set lookback days (current: %d)                   ║\n",   ctx->dashboard_days);
    printf("║  [ 6 ]  Set theatre filter (current: %s)                  ║\n", theatre_current);
    printf("║  [ 7 ]  Export to CSV                                     ║\n");
    printf("║  [ 0 ]  Back                                              ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * dashboard_menu — public entry point
 * ───────────────────────────────────────────────────────────────────────────*/
void dashboard_menu(SessionContext *ctx)
{
    if (!ctx || !ctx->is_admin) {
        printf("\n  [!] Access denied. Admin privileges required.\n");
        return;
    }

    /* Ensure defaults are sane */
    if (ctx->dashboard_days <= 0) ctx->dashboard_days = 30;

    while (1) {
        print_dashboard_header(ctx);
        print_quick_stats(ctx);

        long c = read_long("  Choice: ");

        switch (c) {
            case 1:
                run_occupancy_report(ctx->dashboard_theatre_filter,
                                     ctx->dashboard_days);
                print_report_complete_separator();
                press_enter();
                break;

            case 2:
                run_revenue_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                print_report_complete_separator();
                press_enter();
                break;

            case 3:
                run_booking_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                print_report_complete_separator();
                press_enter();
                break;

            case 4:
                printf("\n  \033[36mGenerating Occupancy Report...\033[0m\n");
                run_occupancy_report(ctx->dashboard_theatre_filter,
                                     ctx->dashboard_days);
                print_report_complete_separator();

                printf("\n  \033[36mGenerating Revenue Report...\033[0m\n");
                run_revenue_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                print_report_complete_separator();

                printf("\n  \033[36mGenerating Booking Trends Report...\033[0m\n");
                run_booking_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                print_report_complete_separator();

                printf("\n  \033[32m✓ All reports generated.\033[0m\n");
                press_enter();
                break;

            case 5: {
                printf("  Presets: [1] 7 days  [2] 30 days  [3] 90 days  [4] Custom\n");
                long preset = read_long("  Select preset: ");

                long days = -1;
                if (preset == 1) days = 7;
                else if (preset == 2) days = 30;
                else if (preset == 3) days = 90;
                else if (preset == 4) {
                    days = read_long("  New lookback days (1–365): ");
                }

                if (days >= 1 && days <= 365) {
                    ctx->dashboard_days = (int)days;
                    printf("  ✓ Lookback set to %d day(s).\n", ctx->dashboard_days);
                } else {
                    printf("  [!] Invalid selection/value.\n");
                }
                press_enter();
                break;
            }

            case 6: {
                printf("  Enter 0 for all theatres, or a theatre ID to filter.\n");
                long tf = read_long("  Theatre filter: ");
                if (tf >= 0) {
                    ctx->dashboard_theatre_filter = (int)tf;
                    if (tf == 0)
                        printf("  ✓ Filter cleared — showing all theatres.\n");
                    else
                        printf("  ✓ Filter set to Theatre #%d.\n",
                               ctx->dashboard_theatre_filter);
                } else {
                    printf("  [!] Invalid value.\n");
                }
                press_enter();
                break;
            }

            case 7:
                csv_export(ctx);
                press_enter();
                break;

            case 0:
                return;

            default:
                printf("  [!] Please enter 0–7.\n");
                break;
        }
    }
}