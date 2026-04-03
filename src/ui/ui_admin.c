/*
 * ui_admin.c — Admin Panel UI for CineBook
 *
 * Public surface:
 *   void admin_menu(SessionContext *ctx)
 *
 * Submenus:
 *   1. Theatre management
 *   2. Screen management  (+ generate_seat_layout static helper)
 *   3. Show management
 *   4. Movie management   (TMDB import + manual add + Super Import)
 *   5. Promo management
 *   6. Analytics dashboard
 *   7. System management  (cities, academic domains, refund policy)
 *   8. Logout
 *
 * Guard: ctx->is_admin must be 1 or function returns immediately.
 * C11.  No external libs beyond libcurl/cJSON (consumed inside reports.cpp).
 * Depends on: query.h, session.h, auth.h, promos.h, refund.h, reports.h, txn.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>

#include "query.h"    /* db_select/insert/update/delete, WhereClause, ResultSet */
#include "session.h"  /* SessionContext, session_clear                          */
#include "auth.h"     /* UserRole                                               */
#include "promos.h"   /* create_promo, deactivate_promo                        */
#include "refund.h"   /* execute_admin_show_cancellation                       */
#include "reports.h"  /* run_report, run_*_report, tmdb_search_and_import,
                          tmdb_bulk_import_now_playing                          */
#include "txn.h"      /* wal_begin, wal_commit, wal_rollback                   */
#include "storage.h"  /* storage_get_capacity + storage stats                   */
#include "ui_utils.h" /* smart_clear, draw_separator                            */
#include "integrity.h"/* verify_transaction_state, repair_orphaned_records      */
#include "compact.h"  /* compact_all_tables                                     */
#include "keystore.h" /* decrypt_api_key                                        */
#include "wizard.h"   /* setup_wizard_run                                       */

/* ─────────────────────────────────────────────────────────────────────────────
 * Limits
 * ───────────────────────────────────────────────────────────────────────────*/
#define MAX_LIST_ROWS   128
#define CONF_PATH       "cinebook.conf"
#define TMDB_KEY_MAX    128
#define BOX_INNER       56
#define MAX_COMPACT_RESULTS 64

/* ─────────────────────────────────────────────────────────────────────────────
 * Shared helpers
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

static void ui_smart_clear_and_title(const char *title)
{
    smart_clear(UI_CONTEXT_ADMIN_PANEL);
    if (title && title[0]) {
        printf("  %s\n", title);
        printf("  ──────────────────────────────────────────────────\n");
    }
    fflush(stdout);
}

static void print_tmdb_user_error(void)
{
    const TMDBErrorInfo *e = tmdb_get_last_error();
    if (!e || e->user_message[0] == '\0') {
        printf("  [TMDB] Request failed.\n");
        return;
    }
    printf("  [TMDB] %s\n", e->user_message);
    if (e->remediation_hint[0]) printf("  Hint: %s\n", e->remediation_hint);
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

static const char *rs_str(ResultSet *rs, int row, int col)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return "";
    if (!rs->rows[row][col]) return "";
    return (const char *)rs->rows[row][col];
}

/* Read a long integer from user input; returns LONG_MIN on bad input. */
static long read_long(const char *prompt)
{
    char buf[32];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return (long)(-0x7FFFFFFFL - 1);
    strip_nl(buf);
    char *ep = NULL;
    long v = strtol(buf, &ep, 10);
    if (ep == buf || *ep != '\0') return (long)(-0x7FFFFFFFL - 1);
    return v;
}

static float read_float(const char *prompt)
{
    char buf[32];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return -1.0f;
    strip_nl(buf);
    char *ep = NULL;
    float v = strtof(buf, &ep);
    if (ep == buf) return -1.0f;
    return v;
}

static void read_str(const char *prompt, char *out, int max)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, max, stdin)) { out[0] = '\0'; return; }
    strip_nl(out);
}

static int parse_datetime_local(const char *s, struct tm *out_tm)
{
    if (!s || !out_tm || strlen(s) < 16) return 0;
    int yy = 0, mo = 0, dd = 0, hh = 0, mm = 0;
    if (sscanf(s, "%d-%d-%d %d:%d", &yy, &mo, &dd, &hh, &mm) != 5) return 0;

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = yy - 1900;
    out_tm->tm_mon  = mo - 1;
    out_tm->tm_mday = dd;
    out_tm->tm_hour = hh;
    out_tm->tm_min  = mm;
    out_tm->tm_isdst = -1;
    return 1;
}

static int is_future_datetime(const char *s)
{
    struct tm t;
    if (!parse_datetime_local(s, &t)) return 0;
    time_t tt = mktime(&t);
    if (tt == (time_t)-1) return 0;
    return difftime(tt, time(NULL)) > 0.0;
}

typedef struct {
    int   theatre_count;
    int   shows_today;
    int   bookings_today;
    float revenue_today;
} AdminDashboardStats;

static void format_inr_amount(float amount, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;

    long long value = (long long)(amount + 0.5f);
    if (value < 0) value = 0;

    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", value);
    int len = (int)strlen(raw);

    char rev[48];
    int r = 0, grp = 0;
    for (int i = len - 1; i >= 0 && r < (int)sizeof(rev) - 1; i--) {
        if (grp == 3) {
            rev[r++] = ',';
            grp = 0;
        }
        rev[r++] = raw[i];
        grp++;
    }
    rev[r] = '\0';

    int o = 0;
    for (int i = r - 1; i >= 0 && o < (int)out_sz - 1; i--) out[o++] = rev[i];
    out[o] = '\0';
}

static void collect_admin_dashboard_stats(AdminDashboardStats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));

    int32_t one = 1;
    WhereClause tw[1];
    strncpy(tw[0].col_name, "is_active", 63); tw[0].col_name[63] = '\0';
    tw[0].op = OP_EQ; tw[0].value = &one; tw[0].logic = 0;
    stats->theatre_count = db_count("theatres", tw, 1);
    if (stats->theatre_count < 0) stats->theatre_count = 0;

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);

    int year = tm_now ? (tm_now->tm_year + 1900) : 1970;
    int mon  = tm_now ? (tm_now->tm_mon + 1) : 1;
    int mday = tm_now ? tm_now->tm_mday : 1;

    if (year < 0) year = 0;
    if (year > 9999) year = 9999;
    if (mon < 1) mon = 1;
    if (mon > 12) mon = 12;
    if (mday < 1) mday = 1;
    if (mday > 31) mday = 31;

    char day_start[20];
    char day_end[20];
    snprintf(day_start, sizeof(day_start), "%04d-%02d-%02d 00:00", year, mon, mday);
    snprintf(day_end, sizeof(day_end), "%04d-%02d-%02d 23:59", year, mon, mday);

    WhereClause sw[3];
    strncpy(sw[0].col_name, "show_datetime", 63); sw[0].col_name[63] = '\0';
    sw[0].op = OP_GTE; sw[0].value = day_start; sw[0].logic = 0;
    strncpy(sw[1].col_name, "show_datetime", 63); sw[1].col_name[63] = '\0';
    sw[1].op = OP_LTE; sw[1].value = day_end; sw[1].logic = 0;
    strncpy(sw[2].col_name, "is_active", 63); sw[2].col_name[63] = '\0';
    sw[2].op = OP_EQ; sw[2].value = &one; sw[2].logic = 0;
    stats->shows_today = db_count("shows", sw, 3);
    if (stats->shows_today < 0) stats->shows_today = 0;

    int32_t status_confirmed = 1;
    WhereClause bw[3];
    strncpy(bw[0].col_name, "booked_at", 63); bw[0].col_name[63] = '\0';
    bw[0].op = OP_GTE; bw[0].value = day_start; bw[0].logic = 0;
    strncpy(bw[1].col_name, "booked_at", 63); bw[1].col_name[63] = '\0';
    bw[1].op = OP_LTE; bw[1].value = day_end; bw[1].logic = 0;
    strncpy(bw[2].col_name, "status", 63); bw[2].col_name[63] = '\0';
    bw[2].op = OP_EQ; bw[2].value = &status_confirmed; bw[2].logic = 0;

    char *bcols[] = { "total_amount" };
    ResultSet *bk_rs = db_select("bookings", bw, 3, bcols, 1);
    if (!bk_rs || bk_rs->row_count <= 0) {
        if (bk_rs) result_set_free(bk_rs);
        return;
    }

    stats->bookings_today = bk_rs->row_count;
    float rev = 0.0f;
    for (int i = 0; i < bk_rs->row_count; i++) rev += rs_float(bk_rs, i, 0, 0.0f);
    stats->revenue_today = rev;
    result_set_free(bk_rs);
}

static void print_admin_console_header(const AdminDashboardStats *stats)
{
    char revenue_buf[32] = "0";
    char line[BOX_INNER + 1];
    char left[64];
    char right[32];
    char center[64];

    format_inr_amount(stats ? stats->revenue_today : 0.0f, revenue_buf, sizeof(revenue_buf));

    int theatre_count  = stats ? stats->theatre_count : 0;
    int shows_today    = stats ? stats->shows_today : 0;
    int bookings_today = stats ? stats->bookings_today : 0;

    printf("\n");
    printf("  ╔");
    for (int i = 0; i < BOX_INNER; i++) printf("═");
    printf("╗\n");

    snprintf(center, sizeof(center), "CINEBOOK ADMIN CONSOLE  Admin · CineBook");
    snprintf(line, sizeof(line), "%-*.*s", BOX_INNER, BOX_INNER, center);
    printf("  ║%s║\n", line);

    printf("  ╠");
    for (int i = 0; i < BOX_INNER; i++) printf("═");
    printf("╣\n");

    snprintf(left, sizeof(left), "Theatres: %d", theatre_count);
    snprintf(right, sizeof(right), "Active Shows: %d", shows_today);
    int gap = BOX_INNER - (int)strlen(left) - (int)strlen(right);
    if (gap < 1) gap = 1;
    snprintf(line, sizeof(line), "%s%*s%s", left, gap, "", right);
    {
    char tmp[BOX_INNER + 1];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    snprintf(line, sizeof(line), "%-*.*s", BOX_INNER, BOX_INNER, tmp);
    printf("  ║%s║\n", line);
    }
    snprintf(left, sizeof(left), "Revenue Rs.%s", revenue_buf);
    snprintf(right, sizeof(right), "Bookings: %d  Alerts: 0", bookings_today);
    gap = BOX_INNER - (int)strlen(left) - (int)strlen(right);
    if (gap < 1) gap = 1;
    snprintf(line, sizeof(line), "%s%*s%s", left, gap, "", right);
    {
    char tmp[BOX_INNER + 1];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    snprintf(line, sizeof(line), "%-*.*s", BOX_INNER, BOX_INNER, tmp);
    printf("  ║%s║\n", line);
    }
    printf("  ╚");
    for (int i = 0; i < BOX_INNER; i++) printf("═");
    printf("╝\n");
}

static int confirm_destructive_operation(const char *message_line)
{
    char confirm[32];
    printf("\n  \033[31m");
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║  ⚠  DESTRUCTIVE OPERATION — IRREVERSIBLE ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    printf("  \033[0m");
    if (message_line && message_line[0] != '\0') printf("%s\n", message_line);
    read_str("  Type 'CONFIRM' to proceed, or press Enter to cancel: ",
             confirm, sizeof(confirm));
    return strcmp(confirm, "CONFIRM") == 0;
}

static void print_bulk_screen_progress(int done, int total, int shows_created)
{
    if (total <= 0) total = 1;
    if (done < 0) done = 0;
    if (done > total) done = total;

    int bar_fill = (done * 20) / total;
    printf("\r  [");
    for (int i = 0; i < 20; i++) printf(i < bar_fill ? "█" : "░");
    printf("]  %d/%d screens  (%d shows created)", done, total, shows_created);
    fflush(stdout);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * row_index_to_label — 0-based row index → "A".."Z","AA".."AZ","BA"..
 * ───────────────────────────────────────────────────────────────────────────*/
static void row_index_to_label(int idx, char *out)
{
    if (idx < 26) {
        out[0] = (char)('A' + idx);
        out[1] = '\0';
    } else {
        int hi  = (idx / 26) - 1;   /* 0=A, 1=B, ...                         */
        int lo  = idx % 26;
        out[0]  = (char)('A' + hi);
        out[1]  = (char)('A' + lo);
        out[2]  = '\0';
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * read_tmdb_key — parse TMDB_API_KEY= from cinebook.conf
 * Returns 1 on success, 0 if key not found.
 * ───────────────────────────────────────────────────────────────────────────*/
static int read_tmdb_key(char *out_buf, int max_len)
{
    if (!out_buf || max_len <= 1) return 0;
    out_buf[0] = '\0';

    char *dec = decrypt_api_key(".api_key");
    if (dec && dec[0] != '\0') {
        strncpy(out_buf, dec, (size_t)(max_len - 1));
        out_buf[max_len - 1] = '\0';
        secure_zero(dec, strlen(dec));
        free(dec);
        return 1;
    }
    if (dec) {
        secure_zero(dec, strlen(dec));
        free(dec);
    }

    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TMDB_API_KEY=", 13) == 0) {
            strncpy(out_buf, line + 13, (size_t)(max_len - 1));
            out_buf[max_len - 1] = '\0';
            strip_nl(out_buf);
            fclose(f);
            return (out_buf[0] != '\0');
        }
    }
    fclose(f);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * generate_seat_layout
 *
 * Bulk-inserts into seats (all rows×cols) then bulk-inserts seat_status rows
 * for every existing active show on screen_id.
 *
 * Seat type assignment (1-based row number), counting from BACK rows:
 *   row_num > (layout_rows - recliner_rows_end)                         → RECLINER (2)
 *   row_num > (layout_rows - recliner_rows_end - premium_rows_end)      → PREMIUM  (1)
 *   otherwise                                                            → STANDARD (0)
 * ───────────────────────────────────────────────────────────────────────────*/
static void generate_seat_layout(int screen_id, int layout_rows, int layout_cols,
                                  int recliner_rows_end, int premium_rows_end)
{
    int total = layout_rows * layout_cols;
    if (total <= 0) return;

    int *seat_ids = (int *)malloc((size_t)total * sizeof(int));
    if (!seat_ids) { printf("  [!] Out of memory.\n"); return; }

    /* ── Insert seats ── */
    wal_begin();
    int inserted = 0;
    int32_t pk_zero    = 0;
    int32_t sc_id      = (int32_t)screen_id;
    int32_t is_act     = 1;

    for (int r = 0; r < layout_rows; r++) {
        char row_label[4];
        row_index_to_label(r, row_label);

        int row_num = r + 1;
        int32_t stype;
        int recliner_start = layout_rows - recliner_rows_end + 1;
        int premium_start  = layout_rows - recliner_rows_end - premium_rows_end + 1;

        if (recliner_rows_end > 0 && row_num >= recliner_start)
            stype = 2;                        /* RECLINER (back rows) */
        else if (premium_rows_end > 0 && row_num >= premium_start)
            stype = 1;                        /* PREMIUM (middle/back rows) */
        else
            stype = 0;                        /* STANDARD (front rows) */

        for (int c = 0; c < layout_cols; c++) {
            int32_t seat_num = (int32_t)(c + 1);
            void *f[6] = { &pk_zero, &sc_id, row_label,
                           &seat_num, &stype, &is_act };
            int new_id = db_insert("seats", f);
            if (new_id > 0) seat_ids[inserted++] = new_id;
        }
    }
    wal_commit();
    printf("  \xe2\x9c\x93 %d seat(s) created for Screen #%d.\n", inserted, screen_id);

    /* ── Seed seat_status for existing active shows ── */
    int32_t sc_q  = (int32_t)screen_id;
    int32_t one   = 1;
    WhereClause sw[2];
    sw[0].op = OP_EQ; sw[0].value = &sc_q; sw[0].logic = 0;
    strncpy(sw[0].col_name, "screen_id", 63); sw[0].col_name[63] = '\0';
    sw[1].op = OP_EQ; sw[1].value = &one;  sw[1].logic = 0;
    strncpy(sw[1].col_name, "is_active", 63); sw[1].col_name[63] = '\0';

    ResultSet *show_rs = db_select("shows", sw, 2, NULL, 0);
    if (show_rs && show_rs->row_count > 0) {
        wal_begin();
        int32_t ss_pk    = 0;
        int32_t status_v = 0;
        int32_t null_i   = INT_NULL_SENTINEL;
        char    null_d[20]; memset(null_d, 0, sizeof(null_d));
        int32_t null_b   = INT_NULL_SENTINEL;
        int seed_ok = 1;

        for (int si = 0; si < show_rs->row_count && seed_ok; si++) {
            int32_t show_v = (int32_t)rs_int(show_rs, si, 0, 0);
            for (int j = 0; j < inserted; j++) {
                int32_t seat_v = (int32_t)seat_ids[j];
                void *sf[7] = { &ss_pk, &show_v, &seat_v, &status_v,
                                &null_i, null_d, &null_b };
                if (db_insert("seat_status", sf) <= 0) {
                    seed_ok = 0;
                    break;
                }
            }
        }

        if (seed_ok) {
            wal_commit();
            printf("   Seat availability seeded for %d existing show(s).\n",
                   show_rs->row_count);
        } else {
            wal_rollback();
            printf("  [!] Seat availability seeding failed; no partial rows committed.\n");
        }
    }
    if (show_rs) result_set_free(show_rs);
    free(seat_ids);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * THEATRE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static void list_theatres(void)
{
    ResultSet *rs = db_select("theatres", NULL, 0, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no theatres on record)\n");
        if (rs) result_set_free(rs);
        return;
    }

    printf("\n  %-6s  %-34s  %-6s  %-7s  %s\n",
           "ID", "Name", "City", "Screens", "Status");
    printf("  %-6s  %-34s  %-6s  %-7s  %s\n",
           "------", "----------------------------------", "------", "-------",
           "--------");

    int32_t one = 1;
    for (int i = 0; i < rs->row_count; i++) {
        int theatre_id = rs_int(rs, i, 0, 0);
        int is_active  = rs_int(rs, i, 4, 0);

        int32_t tid_q = (int32_t)theatre_id;
        WhereClause sw[2];
        strncpy(sw[0].col_name, "theatre_id", 63); sw[0].col_name[63] = '\0';
        sw[0].op = OP_EQ; sw[0].value = &tid_q; sw[0].logic = 0;
        strncpy(sw[1].col_name, "is_active", 63); sw[1].col_name[63] = '\0';
        sw[1].op = OP_EQ; sw[1].value = &one; sw[1].logic = 0;
        int screen_count = db_count("screens", sw, 2);
        if (screen_count < 0) screen_count = 0;

        if (is_active) {
            printf("  %-6d  \033[0m%-34.34s\033[0m  %-6d  %-7d  \033[32mACTIVE\033[0m\n",
                   theatre_id, rs_str(rs, i, 1), rs_int(rs, i, 2, 0), screen_count);
        } else {
            char inactive_name[200];
            snprintf(inactive_name, sizeof(inactive_name), "%s [INACTIVE]", rs_str(rs, i, 1));
            printf("  %-6d  \033[90m%-34.34s\033[0m  %-6d  %-7d  \033[90mINACTIVE\033[0m\n",
                   theatre_id, inactive_name, rs_int(rs, i, 2, 0), screen_count);
        }
    }
    result_set_free(rs);
}

static void add_theatre(void)
{
    printf("\n  ── Add Theatre ─────────────────────────────────\n");

    /* Pick city */
    ResultSet *city_rs = db_select("cities", NULL, 0, NULL, 0);
    if (!city_rs || city_rs->row_count == 0) {
        printf("  [!] No cities in database. Add a city first.\n");
        if (city_rs) result_set_free(city_rs);
        return;
    }
    printf("\n  Available cities:\n");
    for (int i = 0; i < city_rs->row_count; i++)
        printf("    %d. %s (id=%d)\n", i + 1, rs_str(city_rs, i, 1),
               rs_int(city_rs, i, 0, 0));

    long city_pick = read_long("\n  Select city number: ");
    if (city_pick < 1 || city_pick > city_rs->row_count) {
        printf("  [!] Invalid selection.\n");
        result_set_free(city_rs);
        return;
    }
    int32_t city_id = (int32_t)rs_int(city_rs, (int)(city_pick - 1), 0, 0);
    result_set_free(city_rs);

    char name[151]; read_str("  Theatre name   : ", name, sizeof(name));
    char address[301]; read_str("  Address        : ", address, sizeof(address));
    if (name[0] == '\0') { printf("  [!] Name cannot be empty.\n"); return; }

    int32_t pk_zero = 0;
    int32_t is_act  = 1;
    void *f[5] = { &pk_zero, name, &city_id, address, &is_act };

    wal_begin();
    int new_id = db_insert("theatres", f);
    if (new_id > 0) {
        wal_commit();
        printf("  \xe2\x9c\x93 Theatre '%s' added (id=%d).\n", name, new_id);
    } else {
        wal_rollback();
        printf("  [!] Insert failed.\n");
    }
}

static void edit_theatre(void)
{
    printf("\n  ── Edit Theatre ────────────────────────────────\n");
    list_theatres();

    long tid = read_long("\n  Theatre ID to edit (0=cancel): ");
    if (tid <= 0) return;

    printf("  Edit field: 1=Name  2=Address  0=Cancel\n");
    long field = read_long("  Choice: ");
    if (field <= 0 || field > 2) return;

    int32_t tid_val = (int32_t)tid;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &tid_val; w[0].logic = 0;
    strncpy(w[0].col_name, "theatre_id", 63); w[0].col_name[63] = '\0';

    wal_begin();
    int rows = 0;
    if (field == 1) {
        char name[151]; read_str("  New name: ", name, sizeof(name));
        if (name[0] == '\0') { wal_rollback(); return; }
        rows = db_update("theatres", w, 1, "name", name);
    } else {
        char addr[301]; read_str("  New address: ", addr, sizeof(addr));
        rows = db_update("theatres", w, 1, "address", addr);
    }
    if (rows > 0) { wal_commit(); printf("  \xe2\x9c\x93 Theatre updated.\n"); }
    else          { wal_rollback(); printf("  [!] Theatre not found or update failed.\n"); }
}

static void deactivate_theatre(void)
{
    printf("\n  ── Deactivate Theatre ──────────────────────────\n");
    list_theatres();

    long tid = read_long("\n  Theatre ID to deactivate (0=cancel): ");
    if (tid <= 0) return;

    int32_t tid_val  = (int32_t)tid;
    int32_t zero_val = 0;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &tid_val; w[0].logic = 0;
    strncpy(w[0].col_name, "theatre_id", 63); w[0].col_name[63] = '\0';

    ResultSet *th_rs = db_select("theatres", w, 1, NULL, 0);
    if (!th_rs || th_rs->row_count == 0) {
        printf("  [!] Theatre not found.\n");
        if (th_rs) result_set_free(th_rs);
        return;
    }

    char msg[320];
    snprintf(msg, sizeof(msg),
             "  This will permanently deactivate Theatre #%ld (%s).",
             tid, rs_str(th_rs, 0, 1));
    result_set_free(th_rs);

    if (!confirm_destructive_operation(msg)) {
        printf("  Cancelled.\n");
        return;
    }

    wal_begin();
    int rows = db_update("theatres", w, 1, "is_active", &zero_val);
    if (rows > 0) { wal_commit(); printf("   Theatre #%ld deactivated.\n", tid); }
    else          { wal_rollback(); printf("  [!] Theatre not found.\n"); }
}

static void theatre_management(void)
{
    while (1) {
        printf("\n  ── Theatre Management ──────────────────────────\n");
        printf("  1. List theatres\n");
        printf("  2. Add theatre\n");
        printf("  3. Edit theatre\n");
        printf("  4. Deactivate theatre\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: printf("\n"); list_theatres(); press_enter(); break;
            case 2: add_theatre(); press_enter(); break;
            case 3: edit_theatre(); press_enter(); break;
            case 4: deactivate_theatre(); press_enter(); break;
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SCREEN MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static const char *screen_type_name(int t)
{
    switch (t) {
        case 0: return "2D";
        case 1: return "IMAX 2D";
        case 2: return "IMAX 3D";
        case 3: return "4DX";
        default: return "?";
    }
}

static void list_screens_for_theatre(int theatre_id)
{
    int32_t tid = (int32_t)theatre_id;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &tid; w[0].logic = 0;
    strncpy(w[0].col_name, "theatre_id", 63); w[0].col_name[63] = '\0';

    ResultSet *rs = db_select("screens", w, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no screens for this theatre)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  %-6s  %-20s  %-9s  %-6s  %s\n",
           "ID", "Name", "Type", "Seats", "Active");
    for (int i = 0; i < rs->row_count; i++) {
        printf("  %-6d  %-20.20s  %-9s  %-6d  %s\n",
               rs_int(rs, i, 0, 0), rs_str(rs, i, 2),
               screen_type_name(rs_int(rs, i, 3, 0)),
               rs_int(rs, i, 4, 0),
               rs_int(rs, i, 9, 0) ? "Yes" : "No");
    }
    result_set_free(rs);
}

static void add_screen(void)
{
    printf("\n  ── Add Screen ──────────────────────────────────\n");

    /* Pick theatre */
    ResultSet *th_rs = db_select("theatres", NULL, 0, NULL, 0);
    if (!th_rs || th_rs->row_count == 0) {
        printf("  [!] No theatres available.\n");
        if (th_rs) result_set_free(th_rs);
        return;
    }
    for (int i = 0; i < th_rs->row_count; i++)
        printf("    %d. %s (id=%d)\n", i + 1,
               rs_str(th_rs, i, 1), rs_int(th_rs, i, 0, 0));

    long tp = read_long("\n  Select theatre number: ");
    if (tp < 1 || tp > th_rs->row_count) {
        printf("  [!] Invalid selection.\n");
        result_set_free(th_rs);
        return;
    }
    int32_t theatre_id = (int32_t)rs_int(th_rs, (int)(tp - 1), 0, 0);
    result_set_free(th_rs);

    char sc_name[51];
    read_str("  Screen name (Enter for auto-name): ", sc_name, sizeof(sc_name));
    if (sc_name[0] == '\0') {
        int32_t tid_q = theatre_id;
        WhereClause sw[1];
        strncpy(sw[0].col_name, "theatre_id", 63); sw[0].col_name[63] = '\0';
        sw[0].op = OP_EQ; sw[0].value = &tid_q; sw[0].logic = 0;
        int existing = db_count("screens", sw, 1);
        snprintf(sc_name, sizeof(sc_name), "Screen %d", existing + 1);
        printf("   Auto name: %s\n", sc_name);
    }

    printf("  Screen type: 0=2D  1=IMAX_2D  2=IMAX_3D  3=4DX\n");
    long stype = read_long("  Screen type: ");
    if (stype < 0 || stype > 3) { printf("  [!] Invalid type.\n"); return; }

    long layout_rows = read_long("  Layout rows (e.g. 15): ");
    long layout_cols = read_long("  Seats per row (e.g. 20): ");
    if (layout_rows < 1 || layout_cols < 1) {
        printf("  [!] Invalid dimensions.\n"); return;
    }

    long recliner_end = read_long("  Recliner rows from back (0 if none): ");
    long premium_end  = read_long("  Premium rows from back (excluding recliners): ");

    if (recliner_end < 0 || premium_end < 0 ||
        recliner_end > layout_rows || premium_end > layout_rows ||
        (recliner_end + premium_end) > layout_rows) {
        printf("  [!] Invalid tier counts (recliner + premium cannot exceed total rows).\n");
        return;
    }

    int32_t pk_zero  = 0;
    int32_t sc_type  = (int32_t)stype;
    int32_t tot_s    = (int32_t)(layout_rows * layout_cols);
    int32_t lr       = (int32_t)layout_rows;
    int32_t lc       = (int32_t)layout_cols;
    int32_t rec_end  = (int32_t)recliner_end;
    int32_t prem_end = (int32_t)premium_end;
    int32_t is_act   = 1;

    void *f[10] = { &pk_zero, &theatre_id, sc_name, &sc_type, &tot_s,
                    &lr, &lc, &rec_end, &prem_end, &is_act };

    wal_begin();
    int new_scid = db_insert("screens", f);
    if (new_scid <= 0) {
        wal_rollback();
        printf("  [!] Failed to create screen.\n");
        return;
    }
    wal_commit();
    printf("  \xe2\x9c\x93 Screen '%s' created (id=%d). Generating seats...\n",
           sc_name, new_scid);

    generate_seat_layout(new_scid, (int)layout_rows, (int)layout_cols,
                         (int)recliner_end, (int)premium_end);
}

static void deactivate_seat(void)
{
    printf("\n  ── Deactivate Individual Seat ──────────────────\n");

    long screen_pick = read_long("  Screen ID containing the seat: ");
    if (screen_pick <= 0) return;

    int32_t sc_q = (int32_t)screen_pick;
    int32_t one  = 1;
    WhereClause w[2];
    w[0].op = OP_EQ; w[0].value = &sc_q; w[0].logic = 0;
    strncpy(w[0].col_name, "screen_id", 63); w[0].col_name[63] = '\0';
    w[1].op = OP_EQ; w[1].value = &one; w[1].logic = 0;
    strncpy(w[1].col_name, "is_active", 63); w[1].col_name[63] = '\0';

    ResultSet *seat_rs = db_select("seats", w, 2, NULL, 0);
    if (!seat_rs || seat_rs->row_count == 0) {
        printf("  (no active seats for this screen)\n");
        if (seat_rs) result_set_free(seat_rs);
        return;
    }
    static const char *stype_name[] = { "STANDARD", "PREMIUM", "RECLINER" };
    printf("\n  %-6s  %-5s  %-4s  %s\n", "ID", "Row", "Num", "Type");
    for (int i = 0; i < seat_rs->row_count; i++) {
        int st = rs_int(seat_rs, i, 4, 0);
        printf("  %-6d  %-5s  %-4d  %s\n",
               rs_int(seat_rs, i, 0, 0), rs_str(seat_rs, i, 2),
               rs_int(seat_rs, i, 3, 0),
               (st >= 0 && st <= 2) ? stype_name[st] : "?");
    }
    result_set_free(seat_rs);

    long seat_id = read_long("\n  Seat ID to deactivate (0=cancel): ");
    if (seat_id <= 0) return;

    int32_t sid   = (int32_t)seat_id;
    int32_t zero  = 0;
    WhereClause dw[1];
    dw[0].op = OP_EQ; dw[0].value = &sid; dw[0].logic = 0;
    strncpy(dw[0].col_name, "seat_id", 63); dw[0].col_name[63] = '\0';

    ResultSet *seat_info = db_select("seats", dw, 1, NULL, 0);
    if (!seat_info || seat_info->row_count == 0) {
        printf("  [!] Seat not found.\n");
        if (seat_info) result_set_free(seat_info);
        return;
    }

    char msg[320];
    snprintf(msg, sizeof(msg),
             "  This will permanently deactivate Seat #%ld (Row %s-%d) on Screen #%d.",
             seat_id, rs_str(seat_info, 0, 2), rs_int(seat_info, 0, 3, 0),
             rs_int(seat_info, 0, 1, 0));
    result_set_free(seat_info);

    if (!confirm_destructive_operation(msg)) {
        printf("  Cancelled.\n");
        return;
    }

    wal_begin();
    int rows = db_update("seats", dw, 1, "is_active", &zero);
    if (rows > 0) { wal_commit(); printf("   Seat #%ld deactivated.\n", seat_id); }
    else          { wal_rollback(); printf("  [!] Seat not found.\n"); }
}

static void screen_management(void)
{
    while (1) {
        printf("\n  ── Screen Management ───────────────────────────\n");
        printf("  1. List screens for a theatre\n");
        printf("  2. Add screen\n");
        printf("  3. Deactivate a seat\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: {
                long tid = read_long("  Theatre ID: ");
                if (tid > 0) list_screens_for_theatre((int)tid);
                press_enter(); break;
            }
            case 2: add_screen(); press_enter(); break;
            case 3: deactivate_seat(); press_enter(); break;
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SHOW MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static void list_shows_brief(void)
{
    ResultSet *rs = db_select("shows", NULL, 0, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no shows on record)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  %-6s  %-7s  %-8s  %-17s  %-8s  %s\n",
           "ID", "MovieID", "ScreenID", "Datetime", "Price", "Status");
    for (int i = 0; i < rs->row_count; i++) {
        int is_active = rs_int(rs, i, 5, 0);
        const char *status_col = "\033[90mPAST\033[0m";
        if (!is_active) {
            status_col = "\033[31mCANCELLED\033[0m";
        } else if (is_future_datetime(rs_str(rs, i, 3))) {
            status_col = "\033[32mUPCOMING\033[0m";
        }

        printf("  %-6d  %-7d  %-8d  %-17s  %-8.2f  %-18s\n",
               rs_int(rs, i, 0, 0), rs_int(rs, i, 1, 0),
               rs_int(rs, i, 2, 0), rs_str(rs, i, 3),
               rs_float(rs, i, 4, 0.0f), status_col);
    }
    result_set_free(rs);
}

static void schedule_show(void)
{
    printf("\n  ── Schedule Show ───────────────────────────────\n");

    /* List active movies */
    int32_t one = 1;
    WhereClause mw[1];
    mw[0].op = OP_EQ; mw[0].value = &one; mw[0].logic = 0;
    strncpy(mw[0].col_name, "is_active", 63); mw[0].col_name[63] = '\0';

    ResultSet *mv_rs = db_select("movies", mw, 1, NULL, 0);
    if (!mv_rs || mv_rs->row_count == 0) {
        printf("  [!] No active movies. Import or add a movie first.\n");
        if (mv_rs) result_set_free(mv_rs);
        return;
    }
    printf("\n  Active movies:\n");
    for (int i = 0; i < mv_rs->row_count; i++)
        printf("    %d. [id=%d] %s (%d min)\n",
               i + 1, rs_int(mv_rs, i, 0, 0),
               rs_str(mv_rs, i, 2), rs_int(mv_rs, i, 6, 0));

    long mp = read_long("\n  Select movie number: ");
    if (mp < 1 || mp > mv_rs->row_count) {
        printf("  [!] Invalid selection.\n");
        result_set_free(mv_rs);
        return;
    }
    int32_t movie_id = (int32_t)rs_int(mv_rs, (int)(mp - 1), 0, 0);
    char movie_title_buf[201];
    strncpy(movie_title_buf, rs_str(mv_rs, (int)(mp - 1), 2), sizeof(movie_title_buf) - 1);
    movie_title_buf[sizeof(movie_title_buf) - 1] = '\0';
    result_set_free(mv_rs);

    /* List active screens with city/theatre context */
    ResultSet *sc_rs = db_select("screens", mw, 1, NULL, 0);
    if (!sc_rs || sc_rs->row_count == 0) {
        printf("  [!] No active screens. Add a screen first.\n");
        if (sc_rs) result_set_free(sc_rs);
        return;
    }
    printf("\n  Active screens:\n");
    for (int i = 0; i < sc_rs->row_count; i++) {
        int screen_id_i  = rs_int(sc_rs, i, 0, 0);
        int theatre_id_i = rs_int(sc_rs, i, 1, 0);

        char theatre_name[151] = "Unknown Theatre";
        char city_name[101]    = "Unknown City";

        int32_t th_q = (int32_t)theatre_id_i;
        WhereClause thw[1];
        strncpy(thw[0].col_name, "theatre_id", 63); thw[0].col_name[63] = '\0';
        thw[0].op = OP_EQ; thw[0].value = &th_q; thw[0].logic = 0;
        ResultSet *th_rs = db_select("theatres", thw, 1, NULL, 0);
        int city_id_i = 0;
        if (th_rs && th_rs->row_count > 0) {
            strncpy(theatre_name, rs_str(th_rs, 0, 1), sizeof(theatre_name) - 1);
            theatre_name[sizeof(theatre_name) - 1] = '\0';
            city_id_i = rs_int(th_rs, 0, 2, 0);
        }
        result_set_free(th_rs);

        if (city_id_i > 0) {
            int32_t c_q = (int32_t)city_id_i;
            WhereClause cw[1];
            strncpy(cw[0].col_name, "city_id", 63); cw[0].col_name[63] = '\0';
            cw[0].op = OP_EQ; cw[0].value = &c_q; cw[0].logic = 0;
            ResultSet *c_rs = db_select("cities", cw, 1, NULL, 0);
            if (c_rs && c_rs->row_count > 0) {
                strncpy(city_name, rs_str(c_rs, 0, 1), sizeof(city_name) - 1);
                city_name[sizeof(city_name) - 1] = '\0';
            }
            result_set_free(c_rs);
        }

        printf("    %d. [id=%d] %s — %s (%d seats)  [%s / %s]\n",
               i + 1, screen_id_i,
               rs_str(sc_rs, i, 2),
               screen_type_name(rs_int(sc_rs, i, 3, 0)),
               rs_int(sc_rs, i, 4, 0),
               city_name, theatre_name);
    }

    long sp = read_long("\n  Select screen number: ");
    if (sp < 1 || sp > sc_rs->row_count) {
        printf("  [!] Invalid selection.\n");
        result_set_free(sc_rs);
        return;
    }
    int32_t screen_id = (int32_t)rs_int(sc_rs, (int)(sp - 1), 0, 0);
    char sc_name[51];
    strncpy(sc_name, rs_str(sc_rs, (int)(sp - 1), 2), sizeof(sc_name) - 1);
    sc_name[sizeof(sc_name) - 1] = '\0';
    result_set_free(sc_rs);

    char show_dt[32];   // fits "YYYY-MM-DD HH:MM" plus margin
    read_str("  Show datetime (YYYY-MM-DD HH:MM): ", show_dt, sizeof(show_dt));
    if (strlen(show_dt) < 16 || !is_future_datetime(show_dt)) {
        printf("  [!] Invalid or past datetime. Please schedule future shows only.\n"); return;
    }

    float base_price = read_float("  Base price (INR): ");
    if (base_price < 0.0f) { printf("  [!] Invalid price.\n"); return; }

    /* Check: does this screen have any show within ±duration_min minutes
     * of the requested datetime? */
    int duration_min = 0;
    WhereClause md_w[1];
    md_w[0].op = OP_EQ; md_w[0].value = &movie_id; md_w[0].logic = 0;
    strncpy(md_w[0].col_name, "movie_id", 63); md_w[0].col_name[63] = '\0';
    ResultSet *md_rs = db_select("movies", md_w, 1, NULL, 0);
    if (!md_rs || md_rs->row_count == 0) {
        printf("  [!] Unable to fetch movie runtime for conflict check.\n");
        if (md_rs) result_set_free(md_rs);
        return;
    }
    duration_min = rs_int(md_rs, 0, 6, 0);
    result_set_free(md_rs);

    struct tm req_tm;
    int yy = 0, mo = 0, dd = 0, hh = 0, mm = 0;
    if (sscanf(show_dt, "%d-%d-%d %d:%d", &yy, &mo, &dd, &hh, &mm) != 5) {
        printf("  [!] Invalid datetime format.\n");
        return;
    }
    memset(&req_tm, 0, sizeof(req_tm));
    req_tm.tm_year = yy - 1900;
    req_tm.tm_mon  = mo - 1;
    req_tm.tm_mday = dd;
    req_tm.tm_hour = hh;
    req_tm.tm_min  = mm;
    req_tm.tm_isdst = -1;
    time_t requested_time = mktime(&req_tm);
    if (requested_time == (time_t)-1) {
        printf("  [!] Invalid datetime.\n");
        return;
    }

    WhereClause sc_shows[2];
    sc_shows[0].op = OP_EQ; sc_shows[0].value = &screen_id; sc_shows[0].logic = 0;
    strncpy(sc_shows[0].col_name, "screen_id", 63); sc_shows[0].col_name[63] = '\0';
    sc_shows[1].op = OP_EQ; sc_shows[1].value = &one; sc_shows[1].logic = 0;
    strncpy(sc_shows[1].col_name, "is_active", 63); sc_shows[1].col_name[63] = '\0';
    ResultSet *existing = db_select("shows", sc_shows, 2, NULL, 0);

    int min_gap_required = duration_min + 30;
    int conflict_found = 0;
    char conflicting_movie_title[201] = "Unknown Movie";
    char conflicting_datetime[32] = "";

    if (existing && existing->row_count > 0) {
        for (int i = 0; i < existing->row_count; i++) {
            const char *existing_dt = rs_str(existing, i, 3);
            int eyy = 0, emo = 0, edd = 0, ehh = 0, emm = 0;
            struct tm t;
            if (sscanf(existing_dt, "%d-%d-%d %d:%d",
                       &eyy, &emo, &edd, &ehh, &emm) != 5) {
                continue;
            }
            memset(&t, 0, sizeof(t));
            t.tm_year = eyy - 1900;
            t.tm_mon  = emo - 1;
            t.tm_mday = edd;
            t.tm_hour = ehh;
            t.tm_min  = emm;
            t.tm_isdst = -1;

            time_t existing_show_time = mktime(&t);
            if (existing_show_time == (time_t)-1) continue;

            double gap_seconds = difftime(existing_show_time, requested_time);
            if (gap_seconds < 0) gap_seconds = -gap_seconds;
            double gap_minutes = gap_seconds / 60.0;

            if (gap_minutes < (double)min_gap_required) {
                conflict_found = 1;
                strncpy(conflicting_datetime, existing_dt, sizeof(conflicting_datetime) - 1);
                conflicting_datetime[sizeof(conflicting_datetime) - 1] = '\0';

                int32_t conflicting_movie_id = (int32_t)rs_int(existing, i, 1, 0);
                WhereClause cmw[1];
                cmw[0].op = OP_EQ; cmw[0].value = &conflicting_movie_id; cmw[0].logic = 0;
                strncpy(cmw[0].col_name, "movie_id", 63); cmw[0].col_name[63] = '\0';
                ResultSet *cm_rs = db_select("movies", cmw, 1, NULL, 0);
                if (cm_rs && cm_rs->row_count > 0) {
                    strncpy(conflicting_movie_title, rs_str(cm_rs, 0, 2),
                            sizeof(conflicting_movie_title) - 1);
                    conflicting_movie_title[sizeof(conflicting_movie_title) - 1] = '\0';
                }
                if (cm_rs) result_set_free(cm_rs);
                break;
            }
        }
    }

    if (existing) result_set_free(existing);

    if (conflict_found) {
        printf("\n  ✗ SCHEDULING CONFLICT\n");
        printf("  Screen '%s' already has '%s' at %s\n",
               sc_name, conflicting_movie_title, conflicting_datetime);
        printf("  Minimum gap required: %d min (runtime) + 30 min buffer\n",
               duration_min);
        printf("  Please choose a different time.\n\n");
        return;
    }

    printf("  ✓ No conflicts found. Scheduling show...\n");

    int32_t pk_zero = 0;
    int32_t is_act  = 1;
    void *sf[6] = { &pk_zero, &movie_id, &screen_id,
                    show_dt, &base_price, &is_act };

    /* Bulk-insert show row + seat_status rows atomically */
    int32_t sc_q = (int32_t)screen_id;
    WhereClause seat_w[2];
    seat_w[0].op = OP_EQ; seat_w[0].value = &sc_q; seat_w[0].logic = 0;
    strncpy(seat_w[0].col_name, "screen_id", 63); seat_w[0].col_name[63] = '\0';
    seat_w[1].op = OP_EQ; seat_w[1].value = &one; seat_w[1].logic = 0;
    strncpy(seat_w[1].col_name, "is_active", 63); seat_w[1].col_name[63] = '\0';

    ResultSet *seat_rs = db_select("seats", seat_w, 2, NULL, 0);
    wal_begin();

    int new_show_id = db_insert("shows", sf);
    int seed_ok = (new_show_id > 0);

    if (seed_ok && seat_rs && seat_rs->row_count > 0) {
        int32_t ss_pk  = 0;
        int32_t show_v = (int32_t)new_show_id;
        int32_t stat_v = 0;
        int32_t null_i = INT_NULL_SENTINEL;
        char    null_d[20]; memset(null_d, 0, sizeof(null_d));
        int32_t null_b = INT_NULL_SENTINEL;

        for (int i = 0; i < seat_rs->row_count; i++) {
            int32_t seat_v = (int32_t)rs_int(seat_rs, i, 0, 0);
            void *ff[7] = { &ss_pk, &show_v, &seat_v, &stat_v,
                            &null_i, null_d, &null_b };
            if (db_insert("seat_status", ff) <= 0) {
                seed_ok = 0;
                break;
            }
        }
    }

    if (!seed_ok) {
        wal_rollback();
        if (seat_rs) result_set_free(seat_rs);
        printf("  [!] Failed to schedule show (storage/capacity/write failure).\n");
        return;
    }

    wal_commit();

    printf("\n  ┌─────────────────────────────────────────────┐\n");
    printf("  │         SHOW SCHEDULED SUCCESSFULLY         │\n");
    printf("  ├─────────────────────────────────────────────┤\n");
    printf("  │  Show ID  : %-32d │\n", new_show_id);
    printf("  │  Movie    : %-32s │\n", movie_title_buf);
    printf("  │  Screen   : %-32s │\n", sc_name);
    printf("  │  Date/Time: %-32s │\n", show_dt);
    printf("  │  Price    : Rs.%-29.2f │\n", base_price);
    printf("  │  Status   : %-32s │\n", "LIVE (is_active=1)");
    printf("  └─────────────────────────────────────────────┘\n\n");

    if (seat_rs) {
        printf("   %d seat status rows initialised.\n", seat_rs->row_count);
        result_set_free(seat_rs);
    }
}

static void cancel_show_admin(SessionContext *ctx)
{
    printf("\n  ── Cancel Show (Admin) ─────────────────────────\n");
    list_shows_brief();

    long sid = read_long("\n  Show ID to cancel (0=cancel): ");
    if (sid <= 0) return;

    int32_t sid_q = (int32_t)sid;
    WhereClause sw[1];
    sw[0].op = OP_EQ; sw[0].value = &sid_q; sw[0].logic = 0;
    strncpy(sw[0].col_name, "show_id", 63); sw[0].col_name[63] = '\0';

    ResultSet *show_rs = db_select("shows", sw, 1, NULL, 0);
    if (!show_rs || show_rs->row_count == 0) {
        printf("  [!] Show not found.\n");
        if (show_rs) result_set_free(show_rs);
        return;
    }

    char msg[320];
    snprintf(msg, sizeof(msg),
             "  This will permanently cancel Show #%ld at %s and refund confirmed bookings.",
             sid, rs_str(show_rs, 0, 3));
    result_set_free(show_rs);

    if (!confirm_destructive_operation(msg)) {
        printf("  Cancelled.\n");
        return;
    }

    printf("\n  Processing bulk cancellation...\n");
    int count = execute_admin_show_cancellation((int)sid, ctx);
    if (count >= 0)
        printf("   Show #%ld cancelled. %d booking(s) refunded.\n", sid, count);
    else
        printf("  [!] Cancellation failed. Check logs.\n");
}

static void cancel_all_scheduled_shows_admin(SessionContext *ctx)
{
    printf("\n  ── Cancel ALL Currently Scheduled Shows ────────\n");

    int32_t one = 1;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &one; w[0].logic = 0;
    strncpy(w[0].col_name, "is_active", 63); w[0].col_name[63] = '\0';

    ResultSet *rs = db_select("shows", w, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  No active shows found.\n");
        if (rs) result_set_free(rs);
        return;
    }

    int upcoming_count = 0;
    for (int i = 0; i < rs->row_count; i++) {
        if (is_future_datetime(rs_str(rs, i, 3))) upcoming_count++;
    }

    if (upcoming_count == 0) {
        printf("  No currently scheduled (upcoming) shows found.\n");
        result_set_free(rs);
        return;
    }

    char msg[320];
    snprintf(msg, sizeof(msg),
             "  This will cancel %d upcoming show(s) and refund all confirmed bookings.",
             upcoming_count);

    if (!confirm_destructive_operation(msg)) {
        printf("  Cancelled.\n");
        result_set_free(rs);
        return;
    }

    int cancelled_shows = 0;
    int refunded_bookings = 0;
    int failed = 0;

    printf("\n  Processing mass cancellation...\n");
    for (int i = 0; i < rs->row_count; i++) {
        if (!is_future_datetime(rs_str(rs, i, 3))) continue;

        int show_id = rs_int(rs, i, 0, 0);
        int rc = execute_admin_show_cancellation(show_id, ctx);
        if (rc >= 0) {
            cancelled_shows++;
            refunded_bookings += rc;
        } else {
            failed++;
        }
    }

    result_set_free(rs);

    printf("   Cancelled shows : %d\n", cancelled_shows);
    printf("   Refunded bookings: %d\n", refunded_bookings);
    if (failed > 0) printf("   Failed cancellations: %d\n", failed);
}

static void set_show_base_price(void)
{
    printf("\n  ── Set Show Base Price ─────────────────────────\n");
    list_shows_brief();

    long sid = read_long("\n  Show ID (0=cancel): ");
    if (sid <= 0) return;

    float new_price = read_float("  New base price (INR): ");
    if (new_price < 0.0f) { printf("  [!] Invalid price.\n"); return; }

    int32_t sid_val = (int32_t)sid;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &sid_val; w[0].logic = 0;
    strncpy(w[0].col_name, "show_id", 63); w[0].col_name[63] = '\0';

    wal_begin();
    int rows = db_update("shows", w, 1, "base_price", &new_price);
    if (rows > 0) { wal_commit(); printf("   Base price updated to INR %.2f.\n", new_price); }
    else          { wal_rollback(); printf("  [!] Show not found.\n"); }
}

static void show_management(SessionContext *ctx)
{
    while (1) {
        printf("\n  ── Show Management ─────────────────────────────\n");
        printf("  1. List shows\n");
        printf("  2. Schedule show\n");
        printf("  3. Cancel show (bulk refund)\n");
        printf("  4. Cancel ALL currently scheduled shows\n");
        printf("  5. Set base price override\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: printf("\n"); list_shows_brief(); press_enter(); break;
            case 2: schedule_show(); press_enter(); break;
            case 3: cancel_show_admin(ctx); press_enter(); break;
            case 4: cancel_all_scheduled_shows_admin(ctx); press_enter(); break;
            case 5: set_show_base_price(); press_enter(); break;
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MOVIE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static void list_movies(void)
{
    ResultSet *rs = db_select("movies", NULL, 0, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no movies on record)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  %-6s  %-35s  %-6s  %-5s  %s\n",
           "ID", "Title", "Lang", "Min", "Active");
    for (int i = 0; i < rs->row_count; i++) {
        printf("  %-6d  %-35.35s  %-6.6s  %-5d  %s\n",
               rs_int(rs, i, 0, 0), rs_str(rs, i, 2),
               rs_str(rs, i, 5), rs_int(rs, i, 6, 0),
               rs_int(rs, i, 9, 0) ? "Yes" : "No");
    }
    result_set_free(rs);
}

static void tmdb_import(void)
{
    ui_smart_clear_and_title("── TMDB Movie Import ───────────────────────────");

    char api_key[TMDB_KEY_MAX];
    if (!read_tmdb_key(api_key, TMDB_KEY_MAX)) {
        printf("\033[33m  ⚠  TMDB API Key not configured.\033[0m\n");
        printf("  To import movies from TMDB:\n");
        printf("  1. Visit https://www.themoviedb.org/settings/api\n");
        printf("  2. Create a free account and generate an API key\n");
        printf("  3. Add to cinebook.conf: TMDB_API_KEY=your_key_here\n");
        printf("  4. Restart CineBook\n\n");
        printf("  Press Enter to continue...\n");
        char wait_buf[64];
        fgets(wait_buf, sizeof(wait_buf), stdin);
        return;
    }

    char query[201];
    read_str("  Search movie title: ", query, sizeof(query));
    if (query[0] == '\0') { printf("  [!] Query cannot be empty.\n"); return; }

    ui_smart_clear_and_title("── TMDB Search / Import ────────────────────────");
    printf("  Searching TMDB...\n");
    int new_movie_id = tmdb_search_and_import(query, api_key);
    if (new_movie_id > 0) {
        int32_t mid_q = (int32_t)new_movie_id;
        WhereClause mw[1];
        strncpy(mw[0].col_name, "movie_id", 63); mw[0].col_name[63] = '\0';
        mw[0].op = OP_EQ; mw[0].value = &mid_q; mw[0].logic = 0;

        ResultSet *m_rs = db_select("movies", mw, 1, NULL, 0);

        int32_t cast_mid = (int32_t)new_movie_id;
        WhereClause cw[1];
        strncpy(cw[0].col_name, "movie_id", 63); cw[0].col_name[63] = '\0';
        cw[0].op = OP_EQ; cw[0].value = &cast_mid; cw[0].logic = 0;
        int cast_count = db_count("cast_members", cw, 1);
        if (cast_count < 0) cast_count = 0;

        const char *title = "(unknown)";
        const char *lang  = "--";
        int duration = 0;

        if (m_rs && m_rs->row_count > 0) {
            title    = rs_str(m_rs, 0, 2);
            lang     = rs_str(m_rs, 0, 5);
            duration = rs_int(m_rs, 0, 6, 0);
        }

        printf("\n");
        printf("  ╔══════════════════════════════════════════════════════╗\n");
        printf("  ║  MOVIE IMPORTED SUCCESSFULLY                         ║\n");
        printf("  ╠══════════════════════════════════════════════════════╣\n");
        printf("  ║  Title    : %-40.40s ║\n", title);
        printf("  ║  Movie ID : %-40d ║\n", new_movie_id);
        printf("  ║  Language : %-40.40s ║\n", lang);
        printf("  ║  Duration : %-36d min ║\n", duration);
        printf("  ║  Cast     : %-31d members imported ║\n", cast_count);
        printf("  ╠══════════════════════════════════════════════════════╣\n");
        printf("  ║  Next step: Go to Show Management → Schedule Show    ║\n");
        printf("  ╚══════════════════════════════════════════════════════╝\n");

        if (m_rs) result_set_free(m_rs);
    } else {
        print_tmdb_user_error();
        printf("  [!] Import failed or cancelled.\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * bulk_schedule_shows — schedule 3 daily time-slots for all active movies
 * across all active screens for a configurable date range.
 *
 * Time slots  : 10:00 / 14:30 / 19:00
 * Base prices : 2D=200  IMAX_2D=300  IMAX_3D=350  4DX=450  (INR)
 * Duplicate check: show for (movie_id, screen_id, datetime) must not exist.
 * ───────────────────────────────────────────────────────────────────────────*/
static void bulk_schedule_shows(void)
{
    printf("\n  ── Bulk Schedule Shows ─────────────────────────\n");

    long days_ahead = read_long("  Days ahead to schedule (1-30, default 7): ");
    if (days_ahead < 1 || days_ahead > 30) days_ahead = 7;

    long day_offset = read_long("  Start day offset (0=today, 1=tomorrow, default 1): ");
    if (day_offset < 0 || day_offset > 30) day_offset = 1;

    int32_t one = 1;
    WhereClause mw[1];
    mw[0].op = OP_EQ; mw[0].value = &one; mw[0].logic = 0;
    strncpy(mw[0].col_name, "is_active", 63); mw[0].col_name[63] = '\0';

    ResultSet *mv_rs = db_select("movies", mw, 1, NULL, 0);
    if (!mv_rs || mv_rs->row_count == 0) {
        printf("  [!] No active movies to schedule.\n");
        if (mv_rs) result_set_free(mv_rs);
        return;
    }

    ResultSet *sc_rs = db_select("screens", mw, 1, NULL, 0);
    if (!sc_rs || sc_rs->row_count == 0) {
        printf("  [!] No active screens available.\n");
        result_set_free(mv_rs);
        if (sc_rs) result_set_free(sc_rs);
        return;
    }

    static const char *time_slots[] = { "10:00", "14:30", "19:00" };
    static const int n_slots = 3;
    static const float base_prices[] = { 200.0f, 300.0f, 350.0f, 450.0f };

    time_t now = time(NULL);
    time_t start_t = now + (time_t)day_offset * 86400L;
    time_t end_t = start_t + (time_t)(days_ahead - 1) * 86400L;

    char start_date[32] = "N/A";
    char end_date[32] = "N/A";
    struct tm tm_start_copy;
    struct tm tm_end_copy;
    struct tm *tm_start = localtime(&start_t);
    struct tm *tm_end = localtime(&end_t);
    if (tm_start) {
        tm_start_copy = *tm_start;
        strftime(start_date, sizeof(start_date), "%Y-%m-%d", &tm_start_copy);
    }
    if (tm_end) {
        tm_end_copy = *tm_end;
        strftime(end_date, sizeof(end_date), "%Y-%m-%d", &tm_end_copy);
    }

    int n_screens = sc_rs->row_count;
    int approx_max_shows = (int)(days_ahead * n_screens * n_slots);

    printf("\n");
    printf("  \033[1mBulk Scheduling Configuration:\033[0m\n");
    printf("  Start date  : %s (%s)\n", start_date,
           day_offset == 1 ? "tomorrow" : (day_offset == 0 ? "today" : "offset"));
    printf("  End date    : %s (%ld days)\n", end_date, days_ahead);
    printf("  Time slots  : 10:00 / 14:30 / 19:00\n");
    printf("  Screens     : %d active\n", n_screens);
    printf("  Max shows   : ~%d (3 per screen per day)\n\n", approx_max_shows);

    char confirm[16];
    read_str("  Proceed? (y/n): ", confirm, sizeof(confirm));
    if (!(confirm[0] == 'y' || confirm[0] == 'Y')) {
        printf("  Cancelled.\n");
        result_set_free(mv_rs);
        result_set_free(sc_rs);
        return;
    }

    int *screen_seat_counts = (int *)calloc((size_t)n_screens, sizeof(int));
    if (!screen_seat_counts) {
        printf("  [!] Out of memory while planning bulk schedule.\n");
        result_set_free(mv_rs);
        result_set_free(sc_rs);
        return;
    }

    for (int si = 0; si < n_screens; si++) {
        int32_t screen_id = (int32_t)rs_int(sc_rs, si, 0, 0);
        WhereClause seat_w[2];
        strncpy(seat_w[0].col_name, "screen_id", 63); seat_w[0].col_name[63] = '\0';
        seat_w[0].op = OP_EQ; seat_w[0].value = &screen_id; seat_w[0].logic = 0;
        strncpy(seat_w[1].col_name, "is_active", 63); seat_w[1].col_name[63] = '\0';
        seat_w[1].op = OP_EQ; seat_w[1].value = &one; seat_w[1].logic = 0;
        int seat_cnt = db_count("seats", seat_w, 2);
        if (seat_cnt < 0) seat_cnt = 0;
        screen_seat_counts[si] = seat_cnt;
    }

    Schema *shows_schema = get_schema("shows");
    Schema *seat_status_schema = get_schema("seat_status");
    if (!shows_schema || !seat_status_schema) {
        printf("  [!] Missing schema metadata for capacity planning.\n");
        free(screen_seat_counts);
        result_set_free(mv_rs);
        result_set_free(sc_rs);
        return;
    }

    int show_max = 0, show_used = 0, show_free = 0;
    int ss_max = 0, ss_used = 0, ss_free = 0;
    if (storage_get_capacity("shows", (size_t)shows_schema->record_size,
                             &show_max, &show_used, &show_free) != 0 ||
        storage_get_capacity("seat_status", (size_t)seat_status_schema->record_size,
                             &ss_max, &ss_used, &ss_free) != 0) {
        printf("  [!] Could not read storage capacity. Scheduling aborted safely.\n");
        free(screen_seat_counts);
        result_set_free(mv_rs);
        result_set_free(sc_rs);
        return;
    }

    int requested_unique = 0;
    int planned_insert = 0;
    int skipped_existing_or_invalid = 0;
    int skipped_no_seats = 0;
    int skipped_by_capacity_plan = 0;

    int rem_show_free = show_free;
    int rem_ss_free = ss_free;

    int n_movies = mv_rs->row_count;
    int movie_idx = 0;
    now = time(NULL);
    now += (time_t)day_offset * 86400L;

    for (int d = 0; d < (int)days_ahead; d++) {
        time_t day_t = now + (time_t)d * 86400L;
        struct tm tm_day_copy;
        struct tm *tm_day = localtime(&day_t);
        if (!tm_day) {
            skipped_existing_or_invalid += (n_screens * n_slots);
            continue;
        }
        tm_day_copy = *tm_day;
        char date_str[32];
        if (strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_day_copy) == 0) {
            skipped_existing_or_invalid += (n_screens * n_slots);
            continue;
        }

        for (int si = 0; si < n_screens; si++) {
            int32_t screen_id = (int32_t)rs_int(sc_rs, si, 0, 0);
            int32_t movie_id = (int32_t)rs_int(mv_rs, movie_idx % n_movies, 0, 0);
            movie_idx++;

            for (int ti = 0; ti < n_slots; ti++) {
                char show_dt[64];
                int dt_len = snprintf(show_dt, sizeof(show_dt), "%s %s", date_str, time_slots[ti]);
                if (dt_len < 0 || dt_len >= (int)sizeof(show_dt) || !is_future_datetime(show_dt)) {
                    skipped_existing_or_invalid++;
                    continue;
                }

                int32_t mid_q = movie_id;
                int32_t sid_q = screen_id;
                WhereClause dup_w[3];
                strncpy(dup_w[0].col_name, "movie_id", 63); dup_w[0].col_name[63] = '\0';
                dup_w[0].op = OP_EQ; dup_w[0].value = &mid_q; dup_w[0].logic = 0;
                strncpy(dup_w[1].col_name, "screen_id", 63); dup_w[1].col_name[63] = '\0';
                dup_w[1].op = OP_EQ; dup_w[1].value = &sid_q; dup_w[1].logic = 0;
                strncpy(dup_w[2].col_name, "show_datetime", 63); dup_w[2].col_name[63] = '\0';
                dup_w[2].op = OP_EQ; dup_w[2].value = show_dt; dup_w[2].logic = 0;

                if (db_count("shows", dup_w, 3) > 0) {
                    skipped_existing_or_invalid++;
                    continue;
                }

                int seats_for_screen = screen_seat_counts[si];
                if (seats_for_screen <= 0) {
                    skipped_no_seats++;
                    continue;
                }

                requested_unique++;

                if (rem_show_free <= 0 || rem_ss_free < seats_for_screen) {
                    skipped_by_capacity_plan++;
                    continue;
                }

                rem_show_free--;
                rem_ss_free -= seats_for_screen;
                planned_insert++;
            }
        }
    }

    printf("\n  Capacity (before scheduling): shows free=%d, seat_status free=%d\n", show_free, ss_free);
    printf("  Plan result: requested=%d, insertable=%d, skipped(capacity)=%d\n",
           requested_unique, planned_insert, skipped_by_capacity_plan);

    if (planned_insert <= 0) {
        printf("  Nothing to insert within current storage capacity.\n");
        free(screen_seat_counts);
        result_set_free(mv_rs);
        result_set_free(sc_rs);
        return;
    }

    int inserted = 0;
    int skipped_runtime_fail = 0;
    int remaining_budget = planned_insert;
    int total_screen_batches = (int)(days_ahead * n_screens);
    int processed_screen_batches = 0;

    movie_idx = 0;
    now = time(NULL);
    now += (time_t)day_offset * 86400L;

    for (int d = 0; d < (int)days_ahead; d++) {
        time_t day_t = now + (time_t)d * 86400L;
        struct tm tm_day_copy;
        struct tm *tm_day = localtime(&day_t);
        if (!tm_day) {
            processed_screen_batches += n_screens;
            continue;
        }
        tm_day_copy = *tm_day;
        char date_str[32];
        if (strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_day_copy) == 0) {
            processed_screen_batches += n_screens;
            continue;
        }

        for (int si = 0; si < n_screens; si++) {
            int32_t screen_id = (int32_t)rs_int(sc_rs, si, 0, 0);
            int screen_type = rs_int(sc_rs, si, 3, 0);
            float bp = (screen_type >= 0 && screen_type <= 3)
                       ? base_prices[screen_type] : 200.0f;

            int32_t movie_id = (int32_t)rs_int(mv_rs, movie_idx % n_movies, 0, 0);
            movie_idx++;

            for (int ti = 0; ti < n_slots; ti++) {
                char show_dt[64];
                int dt_len = snprintf(show_dt, sizeof(show_dt), "%s %s", date_str, time_slots[ti]);
                if (dt_len < 0 || dt_len >= (int)sizeof(show_dt) || !is_future_datetime(show_dt)) {
                    continue;
                }

                int32_t mid_q = movie_id;
                int32_t sid_q = screen_id;
                WhereClause dup_w[3];
                strncpy(dup_w[0].col_name, "movie_id", 63); dup_w[0].col_name[63] = '\0';
                dup_w[0].op = OP_EQ; dup_w[0].value = &mid_q; dup_w[0].logic = 0;
                strncpy(dup_w[1].col_name, "screen_id", 63); dup_w[1].col_name[63] = '\0';
                dup_w[1].op = OP_EQ; dup_w[1].value = &sid_q; dup_w[1].logic = 0;
                strncpy(dup_w[2].col_name, "show_datetime", 63); dup_w[2].col_name[63] = '\0';
                dup_w[2].op = OP_EQ; dup_w[2].value = show_dt; dup_w[2].logic = 0;

                if (db_count("shows", dup_w, 3) > 0) {
                    continue;
                }

                if (screen_seat_counts[si] <= 0) {
                    continue;
                }

                if (remaining_budget <= 0) {
                    continue;
                }

                int32_t pk_zero = 0;
                int32_t is_act = 1;
                void *sf[6] = { &pk_zero, &movie_id, &screen_id, show_dt, &bp, &is_act };

                WhereClause seat_w[2];
                strncpy(seat_w[0].col_name, "screen_id", 63); seat_w[0].col_name[63] = '\0';
                seat_w[0].op = OP_EQ; seat_w[0].value = &screen_id; seat_w[0].logic = 0;
                strncpy(seat_w[1].col_name, "is_active", 63); seat_w[1].col_name[63] = '\0';
                seat_w[1].op = OP_EQ; seat_w[1].value = &one; seat_w[1].logic = 0;

                ResultSet *seat_rs = db_select("seats", seat_w, 2, NULL, 0);

                wal_begin();
                int new_show_id = db_insert("shows", sf);
                int ok = (new_show_id > 0);

                if (ok && seat_rs && seat_rs->row_count > 0) {
                    int32_t ss_pk = 0;
                    int32_t show_v = (int32_t)new_show_id;
                    int32_t stat_v = 0;
                    int32_t null_i = INT_NULL_SENTINEL;
                    char null_d[20]; memset(null_d, 0, sizeof(null_d));
                    int32_t null_b = INT_NULL_SENTINEL;

                    for (int k = 0; k < seat_rs->row_count; k++) {
                        int32_t seat_v = (int32_t)rs_int(seat_rs, k, 0, 0);
                        void *ff[7] = { &ss_pk, &show_v, &seat_v, &stat_v, &null_i, null_d, &null_b };
                        if (db_insert("seat_status", ff) <= 0) {
                            ok = 0;
                            break;
                        }
                    }
                }

                if (ok) {
                    wal_commit();
                    inserted++;
                    remaining_budget--;
                } else {
                    wal_rollback();
                    skipped_runtime_fail++;
                }

                if (seat_rs) result_set_free(seat_rs);
            }

            processed_screen_batches++;
            print_bulk_screen_progress(processed_screen_batches, total_screen_batches, inserted);
        }
    }

    free(screen_seat_counts);
    result_set_free(mv_rs);
    result_set_free(sc_rs);

    int skipped_total = requested_unique - inserted;
    if (skipped_total < 0) skipped_total = 0;

    printf("\n");
    printf("   Bulk schedule complete.\n");
    printf("  Requested : %d show(s)\n", requested_unique);
    printf("  Inserted  : %d show(s)\n", inserted);
    printf("  Skipped   : %d show(s)\n", skipped_total);
    printf("  Details   : invalid/existing=%d, no-seats=%d, capacity(plan)=%d, runtime-fail=%d\n",
           skipped_existing_or_invalid, skipped_no_seats, skipped_by_capacity_plan, skipped_runtime_fail);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * super_import — Bulk TMDB now-playing import + optional auto-schedule
 * ───────────────────────────────────────────────────────────────────────────*/
static void super_import(void)
{
    ui_smart_clear_and_title("Super Import — TMDB Now Playing (India)");
    printf("  This will fetch up to 20 currently-showing titles\n");
    printf("  and import any that aren't already in your database.\n\n");

    char api_key[TMDB_KEY_MAX];
    if (!read_tmdb_key(api_key, TMDB_KEY_MAX)) {
        printf("  [!] TMDB_API_KEY not found in %s.\n", CONF_PATH);
        printf("  Add: TMDB_API_KEY=<your_32_char_key>\n");
        return;
    }

    ui_smart_clear_and_title("Super Import — Fetching from TMDB");
    printf("  Fetching now-playing titles from TMDB...\n\n");

    int new_ids[20];
    memset(new_ids, 0, sizeof(new_ids));

    int imported = tmdb_bulk_import_now_playing(api_key, new_ids, 20);

    if (imported < 0) {
        print_tmdb_user_error();
        printf("  [!] Super import failed.\n");
        return;
    }

    int fetched_limit = 20;
    int skipped = fetched_limit - imported;
    if (skipped < 0) skipped = 0;
    int total_movies = db_count("movies", NULL, 0);
    if (total_movies < 0) total_movies = 0;

    printf("\n");
    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  SUPER IMPORT COMPLETE                               │\n");
    printf("  ├──────────────────────────────────────────────────────┤\n");
    printf("  │  Imported : %-2d new titles                          │\n", imported);
    printf("  │  Skipped  : %-2d (already in database)               │\n", skipped);
    printf("  ├──────────────────────────────────────────────────────┤\n");
    printf("  │  Movies now in database: %-3d total                  │\n", total_movies);
    printf("  └──────────────────────────────────────────────────────┘\n");

    if (imported == 0) return;

    printf("\n  Scheduling options:\n");
    printf("    1. Import only (no scheduling)\n");
    printf("    2. Quick schedule (next 7 days, future slots only)\n");
    printf("    3. Custom scheduling\n");
    long opt = read_long("  Choice: ");
    if (opt == 1) return;
    if (opt == 2) {
        printf("  Running quick scheduling...\n");
        bulk_schedule_shows();
        return;
    }
    if (opt == 3) {
        bulk_schedule_shows();
        return;
    }
    printf("  [!] Invalid option. Import kept; scheduling skipped.\n");
}

static void manual_add_movie(void)
{
    printf("\n  ── Manual Add Movie ────────────────────────────\n");

    char title[201];    read_str("  Title (required): ", title, sizeof(title));
    if (title[0] == '\0') { printf("  [!] Title cannot be empty.\n"); return; }

    char synopsis[501]; read_str("  Synopsis        : ", synopsis, sizeof(synopsis));
    char genre[101];    read_str("  Genre(s)        : ", genre, sizeof(genre));
    char language[51];  read_str("  Language        : ", language, sizeof(language));
    long dur = read_long("  Duration (min)  : ");
    if (dur <= 0) { printf("  [!] Invalid duration.\n"); return; }
    char rel_date[21];  read_str("  Release date (YYYY-MM-DD): ", rel_date, sizeof(rel_date));
    char rating[11];    read_str("  Rating (U/UA/A) : ", rating, sizeof(rating));

    int32_t pk_zero  = 0;
    int32_t null_tmdb = INT_NULL_SENTINEL;
    int32_t dur_val  = (int32_t)dur;
    int32_t is_act   = 1;

    void *f[10] = { &pk_zero, &null_tmdb, title, synopsis, genre,
                    language, &dur_val, rel_date, rating, &is_act };

    wal_begin();
    int new_id = db_insert("movies", f);
    if (new_id > 0) {
        wal_commit();
        printf("  \xe2\x9c\x93 Movie '%s' added (id=%d).\n", title, new_id);
    } else {
        wal_rollback();
        printf("  [!] Insert failed.\n");
    }
}

static void movie_management(void)
{
    while (1) {
        printf("\n  ── Movie Management ────────────────────────────\n");

        int movie_total = db_count("movies", NULL, 0);
        if (movie_total == 0) {
            printf("\033[36m  No movies yet! Use Super Import to fetch current titles\033[0m\n");
            printf("  from TMDB, or add manually. Super Import is recommended.\n\n");
        }

        printf("  1. List movies\n");
        printf("  2. Import from TMDB (search by title)\n");
        printf("  3. Add movie manually\n");
        printf("  4. SUPER IMPORT  fetch all now-playing (India)\n");
        printf("  5. Update TMDB API key (secure wizard)\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: list_movies();      press_enter(); break;
            case 2: tmdb_import();      press_enter(); break;
            case 3: manual_add_movie(); press_enter(); break;
            case 4: super_import();     press_enter(); break;
            case 5: {
                char refreshed[TMDB_KEY_MAX];
                if (setup_wizard_run(1, refreshed, sizeof(refreshed))) {
                    printf("   TMDB API key updated successfully.\n");
                } else {
                    printf("  [!] API key update cancelled or failed.\n");
                }
                press_enter();
                break;
            }
            case 0: return;
            default: printf("  [!] Invalid.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PROMO MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static void list_promos(void)
{
    int32_t one = 1;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &one; w[0].logic = 0;
    strncpy(w[0].col_name, "is_active", 63); w[0].col_name[63] = '\0';

    ResultSet *rs = db_select("promos", w, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no active promos)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  %-6s  %-12s  %-5s  %-8s  %-10s  %-10s  %s/%s\n",
           "ID", "Code", "Type", "Value", "ValidFrom", "ValidUntil", "Uses", "Max");
    for (int i = 0; i < rs->row_count; i++) {
        int dtype = rs_int(rs, i, 2, 0);
        int max_u = rs_int(rs, i, 9, 0);
        char max_buf[16];
        if (max_u == 0) strncpy(max_buf, "unlim", sizeof(max_buf));
        else            snprintf(max_buf, sizeof(max_buf), "%d", max_u);
        printf("  %-6d  %-12.12s  %-5s  %-8.2f  %-10.10s  %-10.10s  %d/%s\n",
               rs_int(rs, i, 0, 0), rs_str(rs, i, 1),
               dtype == 0 ? "PCT" : "FLAT",
               rs_float(rs, i, 3, 0.0f),
               rs_str(rs, i, 7), rs_str(rs, i, 8),
               rs_int(rs, i, 10, 0), max_buf);
    }
    result_set_free(rs);
}

static void promo_management(void)
{
    while (1) {
        printf("\n  ── Promo Management ────────────────────────────\n");
        printf("  1. List active promos\n");
        printf("  2. Create promo\n");
        printf("  3. Deactivate promo\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: list_promos(); press_enter(); break;
            case 2: create_promo(); press_enter(); break;
            case 3: {
                list_promos();
                long pid = read_long("\n  Promo ID to deactivate (0=cancel): ");
                if (pid > 0) deactivate_promo((int)pid);
                press_enter(); break;
            }
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ANALYTICS DASHBOARD
 * ═══════════════════════════════════════════════════════════════════════════*/

static void analytics_menu(SessionContext *ctx)
{
    while (1) {
        printf("\n  ── Analytics Dashboard ─────────────────────────\n");
        printf("  Lookback window : %d day(s)\n", ctx->dashboard_days);
        printf("  Theatre filter  : %s\n",
               ctx->dashboard_theatre_filter == 0
                   ? "All theatres"
                   : "Specific theatre");
        printf("\n");
        printf("  1. Occupancy report\n");
        printf("  2. Revenue report\n");
        printf("  3. Booking trends report\n");
        printf("  4. All three reports\n");
        printf("  5. Set lookback days\n");
        printf("  6. Set theatre filter\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1:
                run_occupancy_report(ctx->dashboard_theatre_filter,
                                     ctx->dashboard_days);
                press_enter(); break;
            case 2:
                run_revenue_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                press_enter(); break;
            case 3:
                run_booking_report(ctx->dashboard_theatre_filter,
                                   ctx->dashboard_days);
                press_enter(); break;
            case 4:
                run_report(0, ctx->dashboard_theatre_filter, ctx->dashboard_days);
                run_report(1, ctx->dashboard_theatre_filter, ctx->dashboard_days);
                run_report(2, ctx->dashboard_theatre_filter, ctx->dashboard_days);
                press_enter(); break;
            case 5: {
                long days = read_long("  New lookback days (e.g. 30): ");
                if (days > 0 && days <= 365) {
                    ctx->dashboard_days = (int)days;
                    printf("  \xe2\x9c\x93 Lookback set to %d day(s).\n", ctx->dashboard_days);
                } else {
                    printf("  [!] Must be 1–365.\n");
                }
                break;
            }
            case 6: {
                printf("  0 = all theatres, or enter a theatre ID to filter.\n");
                long tf = read_long("  Theatre filter: ");
                if (tf >= 0) {
                    ctx->dashboard_theatre_filter = (int)tf;
                    printf("  \xe2\x9c\x93 Filter set.\n");
                }
                break;
            }
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SYSTEM MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

static void add_city(void)
{
    printf("\n  ── Add City ────────────────────────────────────\n");

    /* List countries for selection */
    ResultSet *cn_rs = db_select("countries", NULL, 0, NULL, 0);
    if (!cn_rs || cn_rs->row_count == 0) {
        printf("  [!] No countries in database.\n");
        if (cn_rs) result_set_free(cn_rs);
        return;
    }
    printf("  Countries:\n");
    for (int i = 0; i < cn_rs->row_count; i++)
        printf("    %d. %s (id=%d)\n", i + 1,
               rs_str(cn_rs, i, 1), rs_int(cn_rs, i, 0, 0));

    long cp = read_long("\n  Select country number: ");
    if (cp < 1 || cp > cn_rs->row_count) {
        printf("  [!] Invalid selection.\n");
        result_set_free(cn_rs);
        return;
    }
    int32_t country_id = (int32_t)rs_int(cn_rs, (int)(cp - 1), 0, 0);
    result_set_free(cn_rs);

    char city_name[101];
    read_str("  City name: ", city_name, sizeof(city_name));
    if (city_name[0] == '\0') { printf("  [!] Name cannot be empty.\n"); return; }

    int32_t pk_zero = 0;
    void *f[3] = { &pk_zero, city_name, &country_id };

    wal_begin();
    int new_id = db_insert("cities", f);
    if (new_id > 0) {
        wal_commit();
        printf("  \xe2\x9c\x93 City '%s' added (id=%d).\n", city_name, new_id);
    } else {
        wal_rollback();
        printf("  [!] Insert failed.\n");
    }
}

static void add_academic_domain(void)
{
    printf("\n  ── Add Academic Domain ─────────────────────────\n");

    char domain[101];
    read_str("  Domain (e.g. iitm.ac.in): ", domain, sizeof(domain));
    if (domain[0] == '\0') { printf("  [!] Domain cannot be empty.\n"); return; }

    int32_t pk_zero = 0;
    void *f[2] = { &pk_zero, domain };

    wal_begin();
    int new_id = db_insert("academic_domains", f);
    if (new_id > 0) {
        wal_commit();
        printf("  \xe2\x9c\x93 Domain '%s' added (id=%d).\n", domain, new_id);
    } else {
        wal_rollback();
        printf("  [!] Insert failed (domain may already exist).\n");
    }
}

static void list_refund_policy(void)
{
    ResultSet *rs = db_select("refund_policy", NULL, 0, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no refund policy rows)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  %-6s  %-14s  %-10s  %s\n",
           "ID", "Hours Before", "Refund %", "Label");
    printf("  %-6s  %-14s  %-10s  %s\n",
           "------", "--------------", "----------", "--------------------");
    for (int i = 0; i < rs->row_count; i++) {
        printf("  %-6d  %-14d  %-10d  %s\n",
               rs_int(rs, i, 0, 0), rs_int(rs, i, 1, 0),
               rs_int(rs, i, 2, 0), rs_str(rs, i, 3));
    }
    result_set_free(rs);
}

static void edit_refund_policy(void)
{
    printf("\n  ── Edit Refund Policy Tier ─────────────────────\n");
    list_refund_policy();

    long pid = read_long("\n  Policy ID to edit (0=cancel): ");
    if (pid <= 0) return;

    printf("  Edit field: 1=Hours Before  2=Refund %%  3=Label  0=Cancel\n");
    long field = read_long("  Choice: ");
    if (field <= 0 || field > 3) return;

    int32_t pid_val = (int32_t)pid;
    WhereClause w[1];
    w[0].op = OP_EQ; w[0].value = &pid_val; w[0].logic = 0;
    strncpy(w[0].col_name, "policy_id", 63); w[0].col_name[63] = '\0';

    wal_begin();
    int rows = 0;
    if (field == 1) {
        long hb = read_long("  New hours before: ");
        if (hb < 0) { wal_rollback(); return; }
        int32_t hb_v = (int32_t)hb;
        rows = db_update("refund_policy", w, 1, "hours_before", &hb_v);
    } else if (field == 2) {
        long pct = read_long("  New refund %% (0/50/75/100): ");
        if (pct < 0 || pct > 100) { wal_rollback(); return; }
        int32_t pct_v = (int32_t)pct;
        rows = db_update("refund_policy", w, 1, "refund_percent", &pct_v);
    } else {
        char label[51];
        read_str("  New label: ", label, sizeof(label));
        rows = db_update("refund_policy", w, 1, "label", label);
    }

    if (rows > 0) { wal_commit(); printf("  \xe2\x9c\x93 Policy tier updated.\n"); }
    else          { wal_rollback(); printf("  [!] Tier not found.\n"); }
}

static void list_academic_domains(void)
{
    ResultSet *rs = db_select("academic_domains", NULL, 0, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("  (no academic domains)\n");
        if (rs) result_set_free(rs);
        return;
    }
    printf("\n  Academic domains (%d):\n", rs->row_count);
    for (int i = 0; i < rs->row_count; i++)
        printf("    [%d] %s\n", rs_int(rs, i, 0, 0), rs_str(rs, i, 1));
    result_set_free(rs);
}

static void run_integrity_tools(void)
{
    IntegrityReport *r = verify_transaction_state();
    if (!r) {
        printf("  [!] Integrity check failed.\n");
        return;
    }

    print_integrity_report(r);

    if (r->total_issues > 0) {
        printf("  1. Dry-run repair plan\n");
        printf("  2. Repair now (with backup)\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        if (c == 1) {
            int n = repair_orphaned_records(1);
            printf("   Dry-run complete: %d issue(s) would be repaired.\n", n);
        } else if (c == 2) {
            int n = repair_orphaned_records(0);
            if (n >= 0) printf("   Repair complete: %d issue(s) repaired.\n", n);
            else printf("  [!] Repair failed.\n");
        }
    }

    free_integrity_report(r);
}

static void optimize_database_all_tables(void)
{
    CompactResult results[MAX_COMPACT_RESULTS];
    memset(results, 0, sizeof(results));

    int done = compact_all_tables(results, MAX_COMPACT_RESULTS);
    if (done < 0) {
        printf("  [!] Optimize failed.\n");
        return;
    }

    printf("\n  Optimization summary (%d table(s)):\n", done);
    for (int i = 0; i < done; i++) {
        print_compact_result(&results[i]);
    }
}

static void system_management(void)
{
    while (1) {
        printf("\n  ── System Management ───────────────────────────\n");
        printf("  1. Add city\n");
        printf("  2. Add academic domain\n");
        printf("  3. List academic domains\n");
        printf("  4. View refund policy tiers\n");
        printf("  5. Edit refund policy tier\n");
        printf("  6. Run integrity audit / repair\n");
        printf("  7. Optimize database\n");
        printf("  0. Back\n");
        long c = read_long("  Choice: ");
        switch (c) {
            case 1: add_city(); press_enter(); break;
            case 2: add_academic_domain(); press_enter(); break;
            case 3: list_academic_domains(); press_enter(); break;
            case 4: list_refund_policy(); press_enter(); break;
            case 5: edit_refund_policy(); press_enter(); break;
            case 6: run_integrity_tools(); press_enter(); break;
            case 7: optimize_database_all_tables(); press_enter(); break;
            case 0: return;
            default: printf("  [!] Invalid choice.\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * admin_menu — public entry point
 * ═══════════════════════════════════════════════════════════════════════════*/
void admin_menu(SessionContext *ctx)
{
    if (!ctx || !ctx->is_admin) {
        printf("\n  [!] Access denied. Admin privileges required.\n");
        return;
    }

    AdminDashboardStats stats;
    collect_admin_dashboard_stats(&stats);

    while (1) {
        print_admin_console_header(&stats);
        printf("  Logged in as : %s\n", ctx->name);
        printf("  ──────────────────────────────────────────────────\n");
        printf("  1. Theatre management\n");
        printf("  2. Screen management\n");
        printf("  3. Show management\n");
        printf("  4. Movie management\n");
        printf("  5. Promo management\n");
        printf("  6. Analytics dashboard\n");
        printf("  7. System management\n");
        printf("  8. Logout\n");
        printf("  ──────────────────────────────────────────────────\n");

        long c = read_long("  Choice: ");
        switch (c) {
            case 1: theatre_management(); break;
            case 2: screen_management(); break;
            case 3: show_management(ctx); break;
            case 4: movie_management(); break;
            case 5: promo_management(); break;
            case 6: analytics_menu(ctx); break;
            case 7: system_management(); break;
            case 8:
                printf("\n  Logging out...\n\n");
                session_clear(ctx);
                return;
            default:
                printf("  [!] Please enter 1–8.\n");
                break;
        }
    }
}
