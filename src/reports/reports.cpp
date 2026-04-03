/*
 * reports.cpp — C++17 analytics and TMDB import module for CineBook.
 *
 * Compiled with:  g++ -Wall -Wextra -std=c++17 -c reports.cpp -o reports.o
 * Linked via:     g++ ... reports.o ... -lcurl
 *
 * This file calls the C query engine through an extern "C" block so the
 * C-linkage symbols in query.c are resolved correctly by the g++ linker.
 *
 * External dependencies (this .cpp only):
 *   libcurl  — HTTP GET for TMDBClient
 *   cJSON    — lightweight JSON parsing (single .c file, included in build)
 */

/* ── Standard library ───────────────────────────────────────────────────── */
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

/* ── Third-party ────────────────────────────────────────────────────────── */
#include <curl/curl.h>
#include "cJSON.h"

/* ── C engine headers (C linkage) ───────────────────────────────────────── */
extern "C"
void cinebook_curl_init(void)  { curl_global_init(CURL_GLOBAL_ALL); }
extern "C"
void cinebook_curl_cleanup(void) { curl_global_cleanup(); }
extern "C" {
#include "query.h"
#include "schema.h"
#include "record.h"
#include "txn.h"
}

/* ── Own interface ──────────────────────────────────────────────────────── */
#include "reports.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal date helper
 * ═══════════════════════════════════════════════════════════════════════════*/
namespace {

static void cutoff_date_str(char *buf, size_t buf_sz, int days_back)
{
    time_t now = time(nullptr);
    now -= static_cast<time_t>(days_back) * 86400;
    struct tm *t = localtime(&now);
    snprintf(buf, buf_sz, "%04d-%02d-%02d 00:00",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

static int parse_hour(const char *dt_str)
{
    if (!dt_str || strlen(dt_str) < 13) return 0;
    int h = 0;
    sscanf(dt_str + 11, "%d", &h);
    return h;
}

static int parse_dow(const char *dt_str)
{
    if (!dt_str || strlen(dt_str) < 10) return 0;
    struct tm t = {};
    sscanf(dt_str, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    mktime(&t);
    return (t.tm_wday == 0) ? 6 : t.tm_wday - 1;
}

static std::string rs_str(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return "";
    return std::string(static_cast<char *>(rows[row][col]));
}

static int rs_int(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return 0;
    return *static_cast<int *>(rows[row][col]);
}

static float rs_float(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return 0.0f;
    return *static_cast<float *>(rows[row][col]);
}

static void print_progress_bar(const char *label, int done, int total)
{
    if (total <= 0) total = 1;
    if (done < 0) done = 0;
    if (done > total) done = total;

    int pct  = (done * 100) / total;
    int fill = (done * 20) / total;

    printf("  %-12s [", label ? label : "Progress");
    for (int i = 0; i < 20; i++) printf(i < fill ? "█" : "░");
    printf("] %3d%%\n", pct);
}

} /* anonymous namespace */

/* ═══════════════════════════════════════════════════════════════════════════
 * Abstract base class — Report
 * ═══════════════════════════════════════════════════════════════════════════*/
class Report {
protected:
    int scope_country;
    int days_back;

public:
    Report(int scope, int days)
        : scope_country(scope), days_back(days) {}

    virtual ~Report() = default;

    virtual void build()   = 0;
    virtual void display() = 0;

    void format_currency(float val, char *out, size_t len) const
    {
        long long whole = static_cast<long long>(val);
        int       frac  = static_cast<int>((val - static_cast<float>(whole)) * 100.0f + 0.5f);
        if (frac >= 100) { whole++; frac = 0; }

        char formatted[64] = {};
        if (whole < 10000LL) {
            snprintf(formatted, sizeof(formatted), "%lld", whole);
        } else {
            char digits[32];
            snprintf(digits, sizeof(digits), "%lld", whole);
            int dlen = static_cast<int>(strlen(digits));
            char rev[32];
            int ri = 0, di = dlen - 1, grp = 0, first = 1;
            while (di >= 0) {
                if (first && grp == 3) { rev[ri++] = ','; grp = 0; first = 0; }
                else if (!first && grp == 2) { rev[ri++] = ','; grp = 0; }
                rev[ri++] = digits[di--];
                grp++;
            }
            rev[ri] = '\0';
            int fi = 0;
            for (int i = ri - 1; i >= 0; i--)
                formatted[fi++] = rev[i];
            formatted[fi] = '\0';
        }
        snprintf(out, len, "\xe2\x82\xb9%s.%02d", formatted, frac);
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * OccupancyReport
 * ═══════════════════════════════════════════════════════════════════════════*/
struct OccRow {
    std::string show_label;
    int         show_id;
    int         confirmed;
    int         total;
    float       pct;
};

class OccupancyReport : public Report {
    std::vector<OccRow> rows_;

public:
    OccupancyReport(int scope, int days) : Report(scope, days) {}

    void build() override
    {
        rows_.clear();

        char cutoff[20];
        cutoff_date_str(cutoff, sizeof(cutoff), days_back);

        int is_active_val = 1;
        WhereClause wc[2];

        strncpy(wc[0].col_name, "show_datetime", sizeof(wc[0].col_name));
        wc[0].op    = OP_GTE;
        wc[0].value = static_cast<void *>(cutoff);
        wc[0].logic = 0;

        strncpy(wc[1].col_name, "is_active", sizeof(wc[1].col_name));
        wc[1].op    = OP_EQ;
        wc[1].value = static_cast<void *>(&is_active_val);
        wc[1].logic = 0;

        ResultSet *shows_rs = db_select("shows", wc, 2, nullptr, 0);
        if (!shows_rs) return;

        for (int i = 0; i < shows_rs->row_count; i++) {
            int show_id   = rs_int(shows_rs->rows, i, 0);
            int movie_id  = rs_int(shows_rs->rows, i, 1);
            int screen_id = rs_int(shows_rs->rows, i, 2);
            std::string show_dt = rs_str(shows_rs->rows, i, 3);

            int status_confirmed = 2;
            WhereClause sw[2];
            strncpy(sw[0].col_name, "show_id", sizeof(sw[0].col_name));
            sw[0].op    = OP_EQ;
            sw[0].value = static_cast<void *>(&show_id);
            sw[0].logic = 0;
            strncpy(sw[1].col_name, "status", sizeof(sw[1].col_name));
            sw[1].op    = OP_EQ;
            sw[1].value = static_cast<void *>(&status_confirmed);
            sw[1].logic = 0;
            int confirmed = db_count("seat_status", sw, 2);

            WhereClause scr_w[1];
            strncpy(scr_w[0].col_name, "screen_id", sizeof(scr_w[0].col_name));
            scr_w[0].op    = OP_EQ;
            scr_w[0].value = static_cast<void *>(&screen_id);
            scr_w[0].logic = 0;
            ResultSet *scr_rs = db_select("screens", scr_w, 1, nullptr, 0);
            int total = 1;
            std::string screen_name;
            int theatre_id = 0;
            if (scr_rs && scr_rs->row_count > 0) {
                total       = rs_int(scr_rs->rows, 0, 4);
                screen_name = rs_str(scr_rs->rows, 0, 2);
                theatre_id  = rs_int(scr_rs->rows, 0, 1);
                if (total <= 0) total = 1;
            }
            result_set_free(scr_rs);

            WhereClause mv_w[1];
            strncpy(mv_w[0].col_name, "movie_id", sizeof(mv_w[0].col_name));
            mv_w[0].op    = OP_EQ;
            mv_w[0].value = static_cast<void *>(&movie_id);
            mv_w[0].logic = 0;
            ResultSet *mv_rs = db_select("movies", mv_w, 1, nullptr, 0);
            std::string movie_title;
            if (mv_rs && mv_rs->row_count > 0)
                movie_title = rs_str(mv_rs->rows, 0, 2);
            result_set_free(mv_rs);

            WhereClause th_w[1];
            strncpy(th_w[0].col_name, "theatre_id", sizeof(th_w[0].col_name));
            th_w[0].op    = OP_EQ;
            th_w[0].value = static_cast<void *>(&theatre_id);
            th_w[0].logic = 0;
            ResultSet *th_rs = db_select("theatres", th_w, 1, nullptr, 0);
            std::string theatre_name;
            if (th_rs && th_rs->row_count > 0)
                theatre_name = rs_str(th_rs->rows, 0, 1);
            result_set_free(th_rs);

            std::string time_part = (show_dt.size() >= 16)
                                    ? show_dt.substr(11, 5) : show_dt;
            std::string label = movie_title + " @ " + time_part
                              + " (" + screen_name + ", " + theatre_name + ")";

            float pct = (total > 0)
                        ? (static_cast<float>(confirmed) / static_cast<float>(total)) * 100.0f
                        : 0.0f;

            rows_.push_back({label, show_id, confirmed, total, pct});
        }
        result_set_free(shows_rs);

        std::sort(rows_.begin(), rows_.end(),
                  [](const OccRow &a, const OccRow &b){ return a.pct > b.pct; });
    }

    void display() override
    {
        printf("\n");
        printf("  \033[1;36m╔══════════════════════════════════════════════════════════════════╗\033[0m\n");
        printf("  \033[1;36m║              OCCUPANCY REPORT  (last %3d days)                  ║\033[0m\n", days_back);
        printf("  \033[1;36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n\n");

        if (rows_.empty()) {
            printf("  No show data found for this period.\n\n");
            return;
        }

        printf("  %-55s %6s  %5s/%5s  %s\n",
               "Show", "Occ%", "Conf", "Total", "Bar (each # = 5%)");
        printf("  %s\n", std::string(110, '-').c_str());

        for (const auto &r : rows_) {
            int blocks = static_cast<int>(r.pct / 5.0f);
            if (blocks > 20) blocks = 20;
            std::string bar_str(static_cast<size_t>(blocks), '#');

            const char *colour = (r.pct >= 70.0f) ? "\033[32m"
                               : (r.pct >= 40.0f) ? "\033[33m"
                                                  : "\033[31m";

            std::string lbl = r.show_label;
            if (lbl.size() > 54) lbl = lbl.substr(0, 51) + "...";

            printf("  %-55s %s%5.1f%%\033[0m  %5d/%5d  %s\n",
                   lbl.c_str(), colour, r.pct,
                   r.confirmed, r.total, bar_str.c_str());
        }

        float sum = 0.0f;
        for (const auto &r : rows_) sum += r.pct;
        float avg = rows_.empty() ? 0.0f : sum / static_cast<float>(rows_.size());

        printf("  %s\n", std::string(110, '-').c_str());
        printf("  Average occupancy across %d shows: \033[1m%.1f%%\033[0m\n\n",
               static_cast<int>(rows_.size()), avg);
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * RevenueReport
 * ═══════════════════════════════════════════════════════════════════════════*/
class RevenueReport : public Report {
    std::map<std::string, float> by_movie_;
    std::map<std::string, float> by_theatre_;
    float                        grand_total_{0.0f};

public:
    RevenueReport(int scope, int days) : Report(scope, days) {}

    void build() override
    {
        by_movie_.clear();
        by_theatre_.clear();
        grand_total_ = 0.0f;

        char cutoff[20];
        cutoff_date_str(cutoff, sizeof(cutoff), days_back);

        int status_confirmed = 1;
        WhereClause bwc[2];
        strncpy(bwc[0].col_name, "status", sizeof(bwc[0].col_name));
        bwc[0].op    = OP_EQ;
        bwc[0].value = static_cast<void *>(&status_confirmed);
        bwc[0].logic = 0;
        strncpy(bwc[1].col_name, "booked_at", sizeof(bwc[1].col_name));
        bwc[1].op    = OP_GTE;
        bwc[1].value = static_cast<void *>(cutoff);
        bwc[1].logic = 0;

        ResultSet *bk_rs = db_select("bookings", bwc, 2, nullptr, 0);
        if (!bk_rs) return;

        for (int i = 0; i < bk_rs->row_count; i++) {
            int   show_id   = rs_int  (bk_rs->rows, i, 2);
            float total_amt = rs_float(bk_rs->rows, i, 8);

            WhereClause sw[1];
            strncpy(sw[0].col_name, "show_id", sizeof(sw[0].col_name));
            sw[0].op    = OP_EQ;
            sw[0].value = static_cast<void *>(&show_id);
            sw[0].logic = 0;
            ResultSet *sh_rs = db_select("shows", sw, 1, nullptr, 0);
            int movie_id  = 0;
            int screen_id = 0;
            if (sh_rs && sh_rs->row_count > 0) {
                movie_id  = rs_int(sh_rs->rows, 0, 1);
                screen_id = rs_int(sh_rs->rows, 0, 2);
            }
            result_set_free(sh_rs);

            WhereClause mv_w[1];
            strncpy(mv_w[0].col_name, "movie_id", sizeof(mv_w[0].col_name));
            mv_w[0].op    = OP_EQ;
            mv_w[0].value = static_cast<void *>(&movie_id);
            mv_w[0].logic = 0;
            ResultSet *mv_rs = db_select("movies", mv_w, 1, nullptr, 0);
            std::string movie_title = "(Unknown)";
            if (mv_rs && mv_rs->row_count > 0)
                movie_title = rs_str(mv_rs->rows, 0, 2);
            result_set_free(mv_rs);

            WhereClause scr_w[1];
            strncpy(scr_w[0].col_name, "screen_id", sizeof(scr_w[0].col_name));
            scr_w[0].op    = OP_EQ;
            scr_w[0].value = static_cast<void *>(&screen_id);
            scr_w[0].logic = 0;
            ResultSet *scr_rs = db_select("screens", scr_w, 1, nullptr, 0);
            int theatre_id = 0;
            if (scr_rs && scr_rs->row_count > 0)
                theatre_id = rs_int(scr_rs->rows, 0, 1);
            result_set_free(scr_rs);

            WhereClause th_w[1];
            strncpy(th_w[0].col_name, "theatre_id", sizeof(th_w[0].col_name));
            th_w[0].op    = OP_EQ;
            th_w[0].value = static_cast<void *>(&theatre_id);
            th_w[0].logic = 0;
            ResultSet *th_rs = db_select("theatres", th_w, 1, nullptr, 0);
            std::string theatre_name = "(Unknown)";
            if (th_rs && th_rs->row_count > 0)
                theatre_name = rs_str(th_rs->rows, 0, 1);
            result_set_free(th_rs);

            by_movie_[movie_title]    += total_amt;
            by_theatre_[theatre_name] += total_amt;
            grand_total_              += total_amt;
        }
        result_set_free(bk_rs);
    }

    void display() override
    {
        printf("\n");
        printf("  \033[1;33m╔══════════════════════════════════════════════════════════════════╗\033[0m\n");
        printf("  \033[1;33m║              REVENUE REPORT  (last %3d days)                    ║\033[0m\n", days_back);
        printf("  \033[1;33m╚══════════════════════════════════════════════════════════════════╝\033[0m\n\n");

        if (by_movie_.empty()) {
            printf("  No confirmed bookings found for this period.\n\n");
            return;
        }

        char buf[32];
        format_currency(grand_total_, buf, sizeof(buf));
        printf("  Total revenue: \033[1;33m%s\033[0m  (%d days)\n\n", buf, days_back);

        printf("  \033[1mRevenue by Movie\033[0m\n");
        printf("  %s\n", std::string(80, '-').c_str());

        std::vector<std::pair<std::string, float>> mv_sorted(by_movie_.begin(), by_movie_.end());
        std::sort(mv_sorted.begin(), mv_sorted.end(),
                  [](const auto &a, const auto &b){ return a.second > b.second; });

        float mv_max = mv_sorted.empty() ? 1.0f : mv_sorted[0].second;
        for (const auto &kv : mv_sorted) {
            std::string lbl = kv.first;
            if (lbl.size() > 30) lbl = lbl.substr(0, 27) + "...";
            int bar_len = static_cast<int>((kv.second / mv_max) * 40.0f);
            if (bar_len < 0) bar_len = 0;
            std::string bar_str(static_cast<size_t>(bar_len), '#');
            format_currency(kv.second, buf, sizeof(buf));
            printf("  %-32s \033[33m%-42s\033[0m %s\n",
                   lbl.c_str(), bar_str.c_str(), buf);
        }

        printf("\n  \033[1mRevenue by Theatre\033[0m\n");
        printf("  %s\n", std::string(80, '-').c_str());

        std::vector<std::pair<std::string, float>> th_sorted(by_theatre_.begin(), by_theatre_.end());
        std::sort(th_sorted.begin(), th_sorted.end(),
                  [](const auto &a, const auto &b){ return a.second > b.second; });

        float th_max = th_sorted.empty() ? 1.0f : th_sorted[0].second;
        for (const auto &kv : th_sorted) {
            std::string lbl = kv.first;
            if (lbl.size() > 30) lbl = lbl.substr(0, 27) + "...";
            int bar_len = static_cast<int>((kv.second / th_max) * 40.0f);
            if (bar_len < 0) bar_len = 0;
            std::string bar_str(static_cast<size_t>(bar_len), '#');
            format_currency(kv.second, buf, sizeof(buf));
            printf("  %-32s \033[33m%-42s\033[0m %s\n",
                   lbl.c_str(), bar_str.c_str(), buf);
        }
        printf("\n");
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * BookingReport
 * ═══════════════════════════════════════════════════════════════════════════*/
class BookingReport : public Report {
    std::map<int, int> by_hour_;
    std::map<int, int> by_day_;
    int                total_bookings_{0};

public:
    BookingReport(int scope, int days) : Report(scope, days) {}

    void build() override
    {
        by_hour_.clear();
        by_day_.clear();
        total_bookings_ = 0;

        for (int h = 0; h < 24; h++) by_hour_[h] = 0;
        for (int d = 0; d <  7; d++) by_day_[d]  = 0;

        char cutoff[20];
        cutoff_date_str(cutoff, sizeof(cutoff), days_back);

        int status_confirmed = 1;
        WhereClause bwc[2];
        strncpy(bwc[0].col_name, "status", sizeof(bwc[0].col_name));
        bwc[0].op    = OP_EQ;
        bwc[0].value = static_cast<void *>(&status_confirmed);
        bwc[0].logic = 0;
        strncpy(bwc[1].col_name, "booked_at", sizeof(bwc[1].col_name));
        bwc[1].op    = OP_GTE;
        bwc[1].value = static_cast<void *>(cutoff);
        bwc[1].logic = 0;

        ResultSet *bk_rs = db_select("bookings", bwc, 2, nullptr, 0);
        if (!bk_rs) return;

        for (int i = 0; i < bk_rs->row_count; i++) {
            std::string booked_at = rs_str(bk_rs->rows, i, 12);
            if (booked_at.empty()) continue;
            int h = parse_hour(booked_at.c_str());
            int d = parse_dow (booked_at.c_str());
            if (h >= 0 && h < 24) by_hour_[h]++;
            if (d >= 0 && d <  7) by_day_[d]++;
            total_bookings_++;
        }
        result_set_free(bk_rs);
    }

    void display() override
    {
        printf("\n");
        printf("  \033[1;35m╔══════════════════════════════════════════════════════════════════╗\033[0m\n");
        printf("  \033[1;35m║              BOOKING PATTERN REPORT  (last %3d days)             ║\033[0m\n", days_back);
        printf("  \033[1;35m╚══════════════════════════════════════════════════════════════════╝\033[0m\n\n");

        printf("  Total confirmed bookings: \033[1m%d\033[0m\n\n", total_bookings_);

        printf("  \033[1mBookings by Hour of Day\033[0m\n");
        printf("  %s\n", std::string(70, '-').c_str());

        int hour_max = 1;
        for (const auto &kv : by_hour_)
            if (kv.second > hour_max) hour_max = kv.second;

        for (int h = 0; h < 24; h++) {
            int cnt     = by_hour_[h];
            int bar_len = static_cast<int>(
                (static_cast<float>(cnt) / static_cast<float>(hour_max)) * 40.0f);
            if (bar_len < 0) bar_len = 0;
            std::string bar_str(static_cast<size_t>(bar_len), '#');
            const char *col = (h >= 18) ? "\033[35m"
                            : (h >= 6 && h < 12) ? "\033[36m"
                            : "\033[0m";
            printf("  %02d:00  %s%-42s\033[0m  %4d\n",
                   h, col, bar_str.c_str(), cnt);
        }

        printf("\n  \033[1mBookings by Day of Week\033[0m\n");
        printf("  %s\n", std::string(70, '-').c_str());

        const char *day_names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
        int day_max = 1;
        for (const auto &kv : by_day_)
            if (kv.second > day_max) day_max = kv.second;

        for (int d = 0; d < 7; d++) {
            int cnt     = by_day_[d];
            int bar_len = static_cast<int>(
                (static_cast<float>(cnt) / static_cast<float>(day_max)) * 40.0f);
            if (bar_len < 0) bar_len = 0;
            std::string bar_str(static_cast<size_t>(bar_len), '#');
            const char *col = (d >= 5) ? "\033[33m" : "\033[0m";
            printf("  %s  %s%-42s\033[0m  %4d\n",
                   day_names[d], col, bar_str.c_str(), cnt);
        }
        printf("\n");
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * TMDB typed errors + retryable HTTP client
 * ═══════════════════════════════════════════════════════════════════════════*/
extern "C" {
extern int  g_tmdb_debug_mode;
extern int  g_tmdb_allow_insecure_ssl;
extern char g_tmdb_ca_bundle[256];
}

namespace {
static TMDBErrorInfo g_last_tmdb_error = {};

static const char *tmdb_error_category_name_impl(TMDBErrorCategory category)
{
    switch (category) {
        case TMDB_ERR_NETWORK_TIMEOUT: return "NETWORK_TIMEOUT";
        case TMDB_ERR_NETWORK_RESET: return "NETWORK_RESET";
        case TMDB_ERR_DNS_FAILURE: return "DNS_FAILURE";
        case TMDB_ERR_TLS_CERT_FAILURE: return "TLS_CERT_FAILURE";
        case TMDB_ERR_PROXY_OR_FIREWALL_BLOCK: return "PROXY_OR_FIREWALL_BLOCK";
        case TMDB_ERR_HTTP_401_INVALID_KEY: return "HTTP_401_INVALID_KEY";
        case TMDB_ERR_HTTP_429_RATE_LIMIT: return "HTTP_429_RATE_LIMIT";
        case TMDB_ERR_HTTP_4XX_CLIENT: return "HTTP_4XX_CLIENT";
        case TMDB_ERR_HTTP_5XX_SERVER: return "HTTP_5XX_SERVER";
        case TMDB_ERR_JSON_PARSE_ERROR: return "JSON_PARSE_ERROR";
        case TMDB_ERR_JSON_SCHEMA_MISMATCH: return "JSON_SCHEMA_MISMATCH";
        case TMDB_ERR_EMPTY_RESPONSE: return "EMPTY_RESPONSE";
        case TMDB_ERR_DB_WRITE_FAILURE: return "DB_WRITE_FAILURE";
        default: return "UNKNOWN";
    }
}

static bool tmdb_is_transient(TMDBErrorCategory category)
{
    return category == TMDB_ERR_NETWORK_TIMEOUT ||
           category == TMDB_ERR_NETWORK_RESET ||
           category == TMDB_ERR_DNS_FAILURE ||
           category == TMDB_ERR_HTTP_429_RATE_LIMIT ||
           category == TMDB_ERR_HTTP_5XX_SERVER;
}

static void tmdb_set_error(TMDBErrorCategory category,
                           const std::string &endpoint,
                           int curl_code,
                           int http_status,
                           int attempt,
                           int max_attempts,
                           const std::string &low_level,
                           const std::string &user_msg,
                           const std::string &hint)
{
    memset(&g_last_tmdb_error, 0, sizeof(g_last_tmdb_error));
    g_last_tmdb_error.category = category;
    g_last_tmdb_error.curl_code = curl_code;
    g_last_tmdb_error.http_status = http_status;
    g_last_tmdb_error.attempt = attempt;
    g_last_tmdb_error.max_attempts = max_attempts;
    snprintf(g_last_tmdb_error.endpoint, sizeof(g_last_tmdb_error.endpoint), "%s",
             endpoint.c_str());
    snprintf(g_last_tmdb_error.low_level_message, sizeof(g_last_tmdb_error.low_level_message),
             "%s", low_level.c_str());
    snprintf(g_last_tmdb_error.user_message, sizeof(g_last_tmdb_error.user_message),
             "%s", user_msg.c_str());
    snprintf(g_last_tmdb_error.remediation_hint, sizeof(g_last_tmdb_error.remediation_hint),
             "%s", hint.c_str());

    if (g_tmdb_debug_mode) {
        fprintf(stderr,
                "[TMDB][debug] category=%s endpoint=%s attempt=%d/%d curl=%d http=%d cause=%s\n",
                tmdb_error_category_name_impl(category),
                g_last_tmdb_error.endpoint,
                attempt, max_attempts,
                curl_code, http_status,
                g_last_tmdb_error.low_level_message);
    }
}

static bool looks_like_api_key(const std::string &k)
{
    if (k.empty() || k.size() < 16 || k.size() > 128) return false;
    for (char ch : k) {
        if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '-')) return false;
    }
    return true;
}

static bool file_exists(const char *p)
{
    if (!p || p[0] == '\0') return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool curl_uses_schannel()
{
#ifdef _WIN32
    const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    if (vi && vi->ssl_version) {
        std::string ssl = vi->ssl_version;
        std::transform(ssl.begin(), ssl.end(), ssl.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return ssl.find("schannel") != std::string::npos;
    }
#endif
    return false;
}

static const char *explicit_ca_bundle_if_valid()
{
    if (!g_tmdb_ca_bundle[0]) return nullptr;
    return file_exists(g_tmdb_ca_bundle) ? g_tmdb_ca_bundle : nullptr;
}

static const char *find_ca_bundle()
{
    static char chosen[260] = {0};
    static int initialized = 0;

    if (initialized) return chosen[0] ? chosen : nullptr;
    initialized = 1;

    const char *env_cert_file = getenv("SSL_CERT_FILE");
    if (env_cert_file && file_exists(env_cert_file)) {
        snprintf(chosen, sizeof(chosen), "%s", env_cert_file);
        return chosen;
    }

    const char *env_curl_bundle = getenv("CURL_CA_BUNDLE");
    if (env_curl_bundle && file_exists(env_curl_bundle)) {
        snprintf(chosen, sizeof(chosen), "%s", env_curl_bundle);
        return chosen;
    }

    const char *ca_paths[] = {
        "C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt",
        "C:/msys64/usr/ssl/certs/ca-bundle.crt",
        "C:/msys64/mingw64/ssl/certs/ca-bundle.crt",
        "C:/msys64/etc/ssl/certs/ca-bundle.crt",
        "/etc/ssl/certs/ca-bundle.crt",
        "/mingw64/etc/ssl/certs/ca-bundle.crt",
        nullptr
    };

    for (int i = 0; ca_paths[i]; i++) {
        if (file_exists(ca_paths[i])) {
            snprintf(chosen, sizeof(chosen), "%s", ca_paths[i]);
            return chosen;
        }
    }

    return nullptr;
}

static void apply_tls_options(CURL *curl, const char *endpoint)
{
    static int warned_missing_explicit_bundle = 0;
    static int warned_no_discovered_bundle = 0;

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    if (g_tmdb_allow_insecure_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    long ssl_opts = 0L;
#ifdef CURLSSLOPT_NATIVE_CA
    ssl_opts |= CURLSSLOPT_NATIVE_CA;
#endif
#ifdef _WIN32
#ifdef CURLSSLOPT_NO_REVOKE
    ssl_opts |= CURLSSLOPT_NO_REVOKE;
#endif
#endif
    if (ssl_opts != 0L) {
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, ssl_opts);
    }

    const char *explicit_bundle = explicit_ca_bundle_if_valid();
    if (explicit_bundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, explicit_bundle);
        return;
    }

    if (g_tmdb_ca_bundle[0] && !warned_missing_explicit_bundle) {
        warned_missing_explicit_bundle = 1;
        fprintf(stderr,
                "[TMDB] Warning: TMDB_CA_BUNDLE path not found: %s\n",
                g_tmdb_ca_bundle);
    }

    if (!curl_uses_schannel()) {
        const char *ca_bundle = find_ca_bundle();
        if (ca_bundle) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
            return;
        }

        if (!warned_no_discovered_bundle) {
            warned_no_discovered_bundle = 1;
            fprintf(stderr,
                    "[TMDB] Warning: CA bundle auto-discovery failed; using libcurl default trust settings.\n");
        }
    }

    if (g_tmdb_debug_mode && endpoint) {
        fprintf(stderr, "[TMDB][debug] TLS verify ON endpoint=%s\n", endpoint);
    }
}

static bool curl_ssl_reachable()
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.themoviedb.org/3/configuration");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CineBook/1.0");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    apply_tls_options(curl, "ssl_preflight");

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

class TMDBException : public std::runtime_error {
public:
    TMDBErrorInfo info;
    explicit TMDBException(const TMDBErrorInfo &e)
        : std::runtime_error(std::string("TMDB error: ") + e.user_message), info(e) {}
};

} /* anonymous namespace */

extern "C" const TMDBErrorInfo *tmdb_get_last_error(void) { return &g_last_tmdb_error; }
extern "C" void tmdb_clear_last_error(void) { memset(&g_last_tmdb_error, 0, sizeof(g_last_tmdb_error)); }
extern "C" const char *tmdb_error_category_name(TMDBErrorCategory category)
{
    return tmdb_error_category_name_impl(category);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TMDBClient
 * ═══════════════════════════════════════════════════════════════════════════*/
class TMDBClient {
    std::string api_key_;
    std::string base_url_{"https://api.themoviedb.org/3"};

    static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *buf = static_cast<std::string *>(userdata);
        buf->append(static_cast<char *>(ptr), size * nmemb);
        return size * nmemb;
    }

    static TMDBErrorCategory classify_curl_error(CURLcode rc, const std::string &detail)
    {
        if (rc == CURLE_OPERATION_TIMEDOUT) return TMDB_ERR_NETWORK_TIMEOUT;
        if (rc == CURLE_RECV_ERROR && detail.find("reset") != std::string::npos)
            return TMDB_ERR_NETWORK_RESET;
        if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY)
            return TMDB_ERR_DNS_FAILURE;
        if (rc == CURLE_SSL_CACERT ||
            rc == CURLE_PEER_FAILED_VERIFICATION ||
            rc == CURLE_SSL_CONNECT_ERROR
#ifdef CURLE_SSL_CACERT_BADFILE
            || rc == CURLE_SSL_CACERT_BADFILE
#endif
           )
            return TMDB_ERR_TLS_CERT_FAILURE;
        if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_SEND_ERROR)
            return TMDB_ERR_PROXY_OR_FIREWALL_BLOCK;
        return TMDB_ERR_UNKNOWN;
    }

    static void sleep_backoff(int attempt)
    {
        const int base_ms = 350;
        int exp_ms = base_ms * (1 << (attempt - 1));
        int jitter = rand() % 240;
        std::this_thread::sleep_for(std::chrono::milliseconds(exp_ms + jitter));
    }

    std::string get_json(const std::string &endpoint, const std::string &url) const
    {
        if (!looks_like_api_key(api_key_)) {
            tmdb_set_error(TMDB_ERR_UNKNOWN, endpoint, 0, 0, 1, 1,
                           "TMDB_API_KEY missing or malformed",
                           "TMDB configuration issue.",
                           "Set a valid TMDB_API_KEY in cinebook.conf, then restart Cinebook.");
            throw TMDBException(g_last_tmdb_error);
        }

        const int max_attempts = 3;

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            CURL *curl = curl_easy_init();
            if (!curl) {
                tmdb_set_error(TMDB_ERR_UNKNOWN, endpoint, 0, 0, attempt, max_attempts,
                               "curl_easy_init failed",
                               "TMDB network stack initialization failed.",
                               "Restart app. If issue persists, reinstall curl runtime.");
                throw TMDBException(g_last_tmdb_error);
            }

            std::string body;
            long http_code = 0;
            char errbuf[CURL_ERROR_SIZE] = {0};

            curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT,      "CineBook/1.0");
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    errbuf);
            apply_tls_options(curl, endpoint.c_str());

            if (g_tmdb_debug_mode) {
                fprintf(stderr, "[TMDB][debug] GET %s (endpoint=%s, attempt=%d/%d)\n",
                        url.c_str(), endpoint.c_str(), attempt, max_attempts);
            }

            CURLcode rc = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (g_tmdb_debug_mode) {
                fprintf(stderr, "[TMDB][debug] Response: curl_code=%d http_code=%ld body_size=%zu\n",
                        (int)rc, http_code, body.size());
            }

            curl_easy_cleanup(curl);

            if (rc != CURLE_OK) {
                std::string low = errbuf[0] ? errbuf : curl_easy_strerror(rc);
                TMDBErrorCategory cat = classify_curl_error(rc, low);
                std::string user_msg = "TMDB connection problem.";
                std::string hint = "Check internet connection and retry.";

                if (cat == TMDB_ERR_NETWORK_TIMEOUT) {
                    user_msg = "TMDB request timed out - check internet connection.";
                    hint = "Slow or unstable internet detected. Retry in a few seconds.";
                } else if (cat == TMDB_ERR_NETWORK_RESET) {
                    user_msg = "TMDB connection was reset.";
                    hint = "Transient network reset. Retry shortly.";
                } else if (cat == TMDB_ERR_DNS_FAILURE) {
                    user_msg = "Cannot resolve TMDB host.";
                    hint = "Check DNS settings, proxy, or firewall rules.";
                } else if (cat == TMDB_ERR_TLS_CERT_FAILURE) {
                    user_msg = "SSL certificate verification failed - check system time and ca-certificates.";
                    hint = "Install/update CA bundle, verify system clock, or set TMDB_CA_BUNDLE in config.";
                } else if (cat == TMDB_ERR_PROXY_OR_FIREWALL_BLOCK) {
                    user_msg = "TMDB connection blocked.";
                    hint = "Check proxy/firewall/VPN restrictions.";
                }

                tmdb_set_error(cat, endpoint, (int)rc, 0, attempt, max_attempts,
                               low, user_msg, hint);

                if (tmdb_is_transient(cat) && attempt < max_attempts) {
                    sleep_backoff(attempt);
                    continue;
                }
                throw TMDBException(g_last_tmdb_error);
            }

            if (http_code == 401) {
                tmdb_set_error(TMDB_ERR_HTTP_401_INVALID_KEY, endpoint, 0, 401, attempt, max_attempts,
                               "HTTP 401 Unauthorized",
                               "Invalid TMDB API key - visit themoviedb.org to generate a new key.",
                               "Sign in to themoviedb.org, go to Settings → API, and update cinebook.conf.");
                throw TMDBException(g_last_tmdb_error);
            }
            if (http_code == 429) {
                const int base_ms = 350;
                int retry_seconds = base_ms * (1 << attempt) / 1000;
                char retry_msg[128];
                snprintf(retry_msg, sizeof(retry_msg),
                         "TMDB rate limit reached. Retry in %d seconds.", retry_seconds);
                tmdb_set_error(TMDB_ERR_HTTP_429_RATE_LIMIT, endpoint, 0, 429, attempt, max_attempts,
                               "HTTP 429 Too Many Requests",
                               retry_msg,
                               "Wait and retry after a short cooldown.");
                if (attempt < max_attempts) { sleep_backoff(attempt); continue; }
                throw TMDBException(g_last_tmdb_error);
            }
            if (http_code >= 500) {
                tmdb_set_error(TMDB_ERR_HTTP_5XX_SERVER, endpoint, 0, (int)http_code, attempt, max_attempts,
                               "TMDB server 5xx response",
                               "TMDB server is temporarily unavailable.",
                               "Retry after a short delay.");
                if (attempt < max_attempts) { sleep_backoff(attempt); continue; }
                throw TMDBException(g_last_tmdb_error);
            }
            if (http_code >= 400) {
                tmdb_set_error(TMDB_ERR_HTTP_4XX_CLIENT, endpoint, 0, (int)http_code, attempt, max_attempts,
                               "TMDB client-side HTTP error",
                               "TMDB request failed.",
                               "Check request parameters and API key permissions.");
                throw TMDBException(g_last_tmdb_error);
            }

            if (body.empty()) {
                tmdb_set_error(TMDB_ERR_EMPTY_RESPONSE, endpoint, 0, (int)http_code, attempt, max_attempts,
                               "empty response body",
                               "TMDB returned an empty response.",
                               "Retry shortly. If repeated, TMDB may be unstable.");
                if (attempt < max_attempts) { sleep_backoff(attempt); continue; }
                throw TMDBException(g_last_tmdb_error);
            }

            tmdb_clear_last_error();
            return body;
        }

        tmdb_set_error(TMDB_ERR_UNKNOWN, endpoint, 0, 0, max_attempts, max_attempts,
                       "retry loop exhausted",
                       "TMDB request failed.",
                       "Please retry later.");
        throw TMDBException(g_last_tmdb_error);
    }

public:
    explicit TMDBClient(const std::string &key) : api_key_(key) {}

    std::string search(const std::string &query) const
    {
        std::string enc;
        for (char c : query) {
            if (c == ' ') enc += "%20";
            else if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
                enc += c;
            else { char hex[8]; snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c); enc += hex; }
        }
        std::string url = base_url_ + "/search/movie?api_key=" + api_key_
                        + "&query=" + enc + "&language=en-US&page=1";
        return get_json("search_movie", url);
    }

    std::string movie_details(int tmdb_id) const
    {
        std::string url = base_url_ + "/movie/" + std::to_string(tmdb_id)
                        + "?api_key=" + api_key_ + "&language=en-US";
        return get_json("movie_details", url);
    }

    std::string credits(int tmdb_id) const
    {
        std::string url = base_url_ + "/movie/" + std::to_string(tmdb_id)
                        + "/credits?api_key=" + api_key_ + "&language=en-US";
        return get_json("credits", url);
    }

    std::string watch_providers(int tmdb_id) const
    {
        std::string url = base_url_ + "/movie/" + std::to_string(tmdb_id)
                        + "/watch/providers?api_key=" + api_key_;
        return get_json("watch_providers", url);
    }

    std::string now_playing(int page = 1) const
    {
        std::string url = base_url_ + "/movie/now_playing"
                        + "?api_key=" + api_key_
                        + "&language=en-IN&region=IN"
                        + "&page=" + std::to_string(page);
        return get_json("now_playing", url);
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * tmdb_get_streaming_platforms  (C-callable)
 *
 * Fetches /movie/{tmdb_id}/watch/providers, parses the IN (India) section,
 * and returns a heap-allocated comma-separated string of platform names,
 * e.g. "Netflix, Amazon Prime Video, JioCinema".
 *
 * Returns NULL if: api_key is empty, network fails, no IN data found.
 * Caller must free() the returned pointer.
 * ═══════════════════════════════════════════════════════════════════════════*/
extern "C"
char *tmdb_get_streaming_platforms(int tmdb_id, const char *api_key)
{
    if (tmdb_id <= 0 || !api_key || api_key[0] == '\0')
        return nullptr;

    TMDBClient client(api_key);
    std::string json;

    try {
        json = client.watch_providers(tmdb_id);
    } catch (const TMDBException &ex) {
        fprintf(stderr, "  [TMDB] %s\n", ex.what());
        return nullptr;
    }

    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) {
        tmdb_set_error(TMDB_ERR_JSON_PARSE_ERROR, "watch_providers", 0, 0, 1, 1,
                       "cJSON_Parse failed for watch/providers",
                       "Malformed TMDB streaming payload.",
                       "Retry later. If repeated, TMDB schema may have changed.");
        return nullptr;
    }

    /* Navigate: root -> "results" -> "IN" */
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    cJSON *in_obj  = results ? cJSON_GetObjectItemCaseSensitive(results, "IN") : nullptr;

    if (!in_obj) {
        tmdb_set_error(TMDB_ERR_JSON_SCHEMA_MISMATCH, "watch_providers", 0, 0, 1, 1,
                       "results.IN missing in watch/providers payload",
                       "Streaming information unavailable for this movie.",
                       "TMDB may not have region-specific provider data.");
        cJSON_Delete(root);
        return nullptr;
    }

    /* Collect platform names from flatrate first, then rent, then buy.
     * Use a set (via vector + linear check) to avoid duplicates. */
    std::vector<std::string> platforms;

    auto collect = [&](const char *key) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(in_obj, key);
        if (!cJSON_IsArray(arr)) return;
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "provider_name");
            if (!cJSON_IsString(name) || !name->valuestring) continue;
            std::string pname = name->valuestring;
            /* Deduplicate */
            bool found = false;
            for (const auto &p : platforms)
                if (p == pname) { found = true; break; }
            if (!found) platforms.push_back(pname);
        }
    };

    /* Priority: streaming first, then rent, then purchase */
    collect("flatrate");
    collect("rent");
    collect("buy");

    cJSON_Delete(root);

    if (platforms.empty()) {
        tmdb_set_error(TMDB_ERR_JSON_SCHEMA_MISMATCH, "watch_providers", 0, 0, 1, 1,
                       "No provider_name entries in flatrate/rent/buy arrays",
                       "No streaming information available.",
                       "Provider data may be unavailable in your selected region.");
        return nullptr;
    }

    /* Build comma-separated result string */
    std::string result;
    for (size_t i = 0; i < platforms.size(); i++) {
        if (i > 0) result += ", ";
        result += platforms[i];
    }

    /* Heap-allocate for C caller */
    char *out = static_cast<char *>(malloc(result.size() + 1));
    if (!out) return nullptr;
    memcpy(out, result.c_str(), result.size() + 1);
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * tmdb_search_and_import  (C-callable)
 * ═══════════════════════════════════════════════════════════════════════════*/
extern "C"
int tmdb_search_and_import(const char *query, const char *api_key)
{
    if (!query || !api_key || query[0] == '\0' || api_key[0] == '\0') {
        fprintf(stderr, "  [TMDB] query or api_key is empty.\n");
        return -1;
    }

    TMDBClient client(api_key);
    std::string search_json;

    try {
        printf("  Searching TMDB for \"%s\" ...\n", query);
        search_json = client.search(query);
    } catch (const TMDBException &ex) {
        printf("  [TMDB] %s\n", ex.what());
        const TMDBErrorInfo *e = tmdb_get_last_error();
        if (e && e->user_message[0]) {
            printf("  [TMDB] %s\n", e->user_message);
            if (e->remediation_hint[0]) printf("  Hint: %s\n", e->remediation_hint);
        } else {
            printf("  [TMDB] TMDB request failed.\n");
        }
        return -1;
    }

    cJSON *root = cJSON_Parse(search_json.c_str());
    if (!root) {
        tmdb_set_error(TMDB_ERR_JSON_PARSE_ERROR, "search_movie", 0, 0, 1, 1,
                       "cJSON_Parse failed for search response",
                       "Malformed TMDB response received.",
                       "Retry later. If repeated, enable debug mode for diagnostics.");
        printf("  [TMDB] Malformed TMDB response received.\n");
        return -1;
    }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        tmdb_set_error(TMDB_ERR_JSON_SCHEMA_MISMATCH, "search_movie", 0, 0, 1, 1,
                       "results missing/invalid in TMDB search payload",
                       "No matching movies found on TMDB.",
                       "Try a different title keyword.");
        printf("  [TMDB] No results found for \"%s\".\n", query);
        cJSON_Delete(root);
        return -1;
    }

    int result_count = cJSON_GetArraySize(results);
    if (result_count > 10) result_count = 10;

    printf("\n  Search results:\n");
    printf("  %s\n", std::string(60, '-').c_str());

    struct SearchHit { int tmdb_id; std::string title; std::string release_date; };
    std::vector<SearchHit> hits;

    for (int i = 0; i < result_count; i++) {
        cJSON *item  = cJSON_GetArrayItem(results, i);
        cJSON *id    = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        cJSON *rel   = cJSON_GetObjectItemCaseSensitive(item, "release_date");

        int         tmdb_id   = cJSON_IsNumber(id)    ? (int)id->valuedouble  : 0;
        std::string title_str = cJSON_IsString(title) ? title->valuestring    : "Unknown";
        std::string rel_str   = cJSON_IsString(rel)   ? rel->valuestring      : "";

        hits.push_back({tmdb_id, title_str, rel_str});
        printf("  %2d. %-40s  %s\n", i + 1, title_str.c_str(), rel_str.c_str());
    }
    cJSON_Delete(root);

    printf("  %s\n", std::string(60, '-').c_str());
    printf("  Enter number to import (0 to cancel): ");

    char inbuf[32] = {};
    if (!fgets(inbuf, sizeof(inbuf), stdin)) {
        printf("  Import cancelled.\n");
        return -1;
    }
    char *ep = nullptr;
    long choice_l = strtol(inbuf, &ep, 10);
    if (ep == inbuf || (*ep != '\0' && *ep != '\n') || choice_l < 0 || choice_l > result_count) {
        printf("  Invalid choice.\n");
        return -1;
    }
    if (choice_l == 0) {
        printf("  Import cancelled.\n");
        return -1;
    }
    int choice = static_cast<int>(choice_l);

    print_progress_bar("Import", 1, 5);

    const SearchHit &hit = hits[static_cast<size_t>(choice - 1)];
    printf("  Fetching details for \"%s\" (TMDB id %d) ...\n",
           hit.title.c_str(), hit.tmdb_id);

    std::string details_json, credits_json;
    try {
        details_json = client.movie_details(hit.tmdb_id);
        print_progress_bar("Import", 2, 5);
    } catch (const TMDBException &ex) {
        printf("  [TMDB] %s\n", ex.what());
        const TMDBErrorInfo *e = tmdb_get_last_error();
        printf("  [TMDB] %s\n", (e && e->user_message[0]) ? e->user_message : "Failed to fetch movie details.");
        return -1;
    }

    try {
        credits_json = client.credits(hit.tmdb_id);
        print_progress_bar("Import", 3, 5);
    } catch (const TMDBException &ex) {
        printf("  [TMDB] Warning: Failed to fetch cast - importing movie without cast information.\n");
        if (g_tmdb_debug_mode) {
            const TMDBErrorInfo *e = tmdb_get_last_error();
            printf("  [TMDB][debug] Credits fetch error: %s\n",
                   (e && e->user_message[0]) ? e->user_message : ex.what());
        }
        credits_json = ""; /* Continue without cast */
        print_progress_bar("Import", 3, 5);
    }

    cJSON *det = cJSON_Parse(details_json.c_str());
    if (!det) {
        tmdb_set_error(TMDB_ERR_JSON_PARSE_ERROR, "movie_details", 0, 0, 1, 1,
                       "cJSON_Parse failed for movie details",
                       "Malformed TMDB payload for selected movie.",
                       "Retry import. If repeated, TMDB schema may have changed.");
        printf("  [TMDB] Malformed TMDB payload for selected movie.\n");
        return -1;
    }

    auto get_str = [&](cJSON *obj, const char *key) -> std::string {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
        return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    };
    auto get_int = [&](cJSON *obj, const char *key) -> int {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
    };

    std::string title        = get_str(det, "title");
    std::string synopsis     = get_str(det, "overview");
    std::string language     = get_str(det, "original_language");
    std::string release_date = get_str(det, "release_date");
    int         duration_min = get_int(det, "runtime");
    int         tmdb_id_val  = hit.tmdb_id;

    std::string genre_str;
    cJSON *genres = cJSON_GetObjectItemCaseSensitive(det, "genres");
    if (cJSON_IsArray(genres)) {
        int gc = cJSON_GetArraySize(genres);
        for (int gi = 0; gi < gc; gi++) {
            cJSON *g  = cJSON_GetArrayItem(genres, gi);
            cJSON *gn = cJSON_GetObjectItemCaseSensitive(g, "name");
            if (cJSON_IsString(gn) && gn->valuestring) {
                if (!genre_str.empty()) genre_str += ",";
                genre_str += gn->valuestring;
            }
        }
    }
    cJSON_Delete(det);

    auto trunc = [](std::string &s, size_t max_len) {
        if (s.size() > max_len) s.resize(max_len);
    };
    trunc(title,        199);
    trunc(synopsis,     499);
    trunc(genre_str,    99);
    trunc(language,     49);
    trunc(release_date, 19);

    std::string rating     = "UA";
    int         is_active  = 1;
    int         dummy_id   = 0;

    char title_buf[200]  = {}; strncpy(title_buf,   title.c_str(),        sizeof(title_buf)  - 1);
    char synop_buf[500]  = {}; strncpy(synop_buf,   synopsis.c_str(),     sizeof(synop_buf)  - 1);
    char genre_buf[100]  = {}; strncpy(genre_buf,   genre_str.c_str(),    sizeof(genre_buf)  - 1);
    char lang_buf[50]    = {}; strncpy(lang_buf,    language.c_str(),     sizeof(lang_buf)   - 1);
    char reldate_buf[20] = {}; strncpy(reldate_buf, release_date.c_str(), sizeof(reldate_buf)- 1);
    char rating_buf[10]  = {}; strncpy(rating_buf,  rating.c_str(),       sizeof(rating_buf) - 1);

    void *movie_fields[10];
    movie_fields[0] = &dummy_id;
    movie_fields[1] = &tmdb_id_val;
    movie_fields[2] = title_buf;
    movie_fields[3] = synop_buf;
    movie_fields[4] = genre_buf;
    movie_fields[5] = lang_buf;
    movie_fields[6] = &duration_min;
    movie_fields[7] = reldate_buf;
    movie_fields[8] = rating_buf;
    movie_fields[9] = &is_active;

    wal_begin();
    int new_movie_id = db_insert("movies", movie_fields);
    if (new_movie_id <= 0) {
        wal_rollback();
        tmdb_set_error(TMDB_ERR_DB_WRITE_FAILURE, "movie_insert", 0, 0, 1, 1,
                       "db_insert(movies) failed",
                       "Failed to save imported movie.",
                       "Check DB health and write permissions.");
        printf("  [TMDB] Failed to save imported movie.\n");
        return -1;
    }
    print_progress_bar("Import", 4, 5);

    cJSON *cred_root = nullptr;
    int cast_inserted = 0;

    if (!credits_json.empty()) {
        cred_root = cJSON_Parse(credits_json.c_str());
    }

    if (!cred_root && !credits_json.empty()) {
        tmdb_set_error(TMDB_ERR_JSON_PARSE_ERROR, "credits", 0, 0, 1, 1,
                       "cJSON_Parse failed for credits payload",
                       "Malformed TMDB credits payload.",
                       "Movie imported without cast information.");
        printf("  [TMDB] Warning: Malformed credits payload - importing movie without cast.\n");
    } else if (cred_root) {
        cJSON *cast_arr = cJSON_GetObjectItemCaseSensitive(cred_root, "cast");
        if (cJSON_IsArray(cast_arr)) {
            int cast_total = cJSON_GetArraySize(cast_arr);
            if (cast_total > 15) cast_total = 15;

            for (int ci = 0; ci < cast_total; ci++) {
                cJSON *cm         = cJSON_GetArrayItem(cast_arr, ci);
                std::string cname = get_str(cm, "name");
                std::string crole = get_str(cm, "character");
                int         order = get_int(cm, "order");
                int         is_lead = (order < 5) ? 1 : 0;

                trunc(cname, 149);
                trunc(crole, 149);

                char cname_buf[150] = {}; strncpy(cname_buf, cname.c_str(), sizeof(cname_buf) - 1);
                char crole_buf[150] = {}; strncpy(crole_buf, crole.c_str(), sizeof(crole_buf) - 1);

                int dummy_cast_id = 0;
                void *cast_fields[5];
                cast_fields[0] = &dummy_cast_id;
                cast_fields[1] = &new_movie_id;
                cast_fields[2] = cname_buf;
                cast_fields[3] = crole_buf;
                cast_fields[4] = &is_lead;

                int rc = db_insert("cast_members", cast_fields);
                if (rc > 0) {
                    cast_inserted++;
                } else {
                    printf("  [TMDB] Warning: Failed to insert cast member #%d - continuing.\n", ci + 1);
                    break;
                }
            }
        }
        cJSON_Delete(cred_root);
    }

    wal_commit();
    print_progress_bar("Import", 5, 5);

    printf("\n  \033[32m[OK] Imported \"%s\" (movie_id=%d, %d cast members)\033[0m\n",
           title.c_str(), new_movie_id, cast_inserted);

    return new_movie_id;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * tmdb_bulk_import_now_playing  (C-callable)
 *
 * Column indices used (must match schema.cat):
 *   movies:       0=movie_id(PK,auto) 1=tmdb_id 2=title[200] 3=synopsis[500]
 *                 4=genre[100] 5=language[50] 6=duration_min(INT)
 *                 7=release_date[20] 8=rating[10] 9=is_active(INT)
 *   cast_members: 0=cast_id(PK,auto) 1=movie_id(INT) 2=person_name[150]
 *                 3=role_name[150] 4=is_lead(INT)
 * ═══════════════════════════════════════════════════════════════════════════*/
extern "C"
int tmdb_bulk_import_now_playing(const char *api_key,
                                 int        *out_movie_ids,
                                 int         max_out)
{
    /* ── Validate inputs ── */
    if (!api_key || api_key[0] == '\0') {
        fprintf(stderr, "  [TMDB] api_key is empty.\n");
        return -1;
    }

    /* ── Hard-coded TMDB genre ID → name map ── */
    static const std::map<int, std::string> genre_map = {
        {28,    "Action"},
        {12,    "Adventure"},
        {16,    "Animation"},
        {35,    "Comedy"},
        {80,    "Crime"},
        {99,    "Documentary"},
        {18,    "Drama"},
        {10751, "Family"},
        {14,    "Fantasy"},
        {36,    "History"},
        {27,    "Horror"},
        {10402, "Music"},
        {9648,  "Mystery"},
        {10749, "Romance"},
        {878,   "Science Fiction"},
        {10770, "TV Movie"},
        {53,    "Thriller"},
        {10752, "War"},
        {37,    "Western"}
    };

    /* ── Fetch now_playing ── */
    TMDBClient  client(api_key);
    std::string np_json;

    const int max_attempts = 3;
    bool fetched = false;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            if (attempt == 1) {
                printf("  Fetching now-playing list from TMDB (India)...\n");
            } else {
                printf("  Retrying now-playing fetch (%d/%d)...\n", attempt, max_attempts);
            }

            np_json = client.now_playing(1);
            fetched = true;
            break;
        } catch (const TMDBException &ex) {
            printf("  [TMDB] %s\n", ex.what());
            if (attempt == max_attempts) {
                const TMDBErrorInfo *e = tmdb_get_last_error();
                if (e && e->user_message[0]) {
                    printf("  [TMDB] %s\n", e->user_message);
                    if (e->remediation_hint[0]) printf("  Hint: %s\n", e->remediation_hint);
                } else {
                    printf("  [TMDB] Failed to fetch now-playing list.\n");
                }
                return -1;
            }
        }
    }

    if (!fetched) {
        printf("  [TMDB] Failed to fetch now-playing after retries.\n");
        return -1;
    }

    /* ── Parse outer JSON ── */
    cJSON *root = cJSON_Parse(np_json.c_str());
    if (!root) {
        tmdb_set_error(TMDB_ERR_JSON_PARSE_ERROR, "now_playing", 0, 0, 1, 1,
                       "cJSON_Parse failed for now_playing payload",
                       "Malformed TMDB now-playing payload.",
                       "Retry after some time.");
        printf("  [TMDB] Malformed TMDB now-playing payload.\n");
        return -1;
    }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        tmdb_set_error(TMDB_ERR_JSON_SCHEMA_MISMATCH, "now_playing", 0, 0, 1, 1,
                       "results missing/empty in now_playing payload",
                       "No now-playing movies returned by TMDB.",
                       "This can be temporary for region/language filters.");
        printf("  [TMDB] No now-playing results returned.\n");
        cJSON_Delete(root);
        return -1;
    }

    int total = cJSON_GetArraySize(results);
    if (total > 20) total = 20;

    /* ── helpers captured by lambda ── */
    auto get_str = [](cJSON *obj, const char *key) -> std::string {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
        return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    };
    auto get_int_j = [](cJSON *obj, const char *key) -> int {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
    };
    auto trunc_str = [](std::string &s, size_t maxlen) {
        if (s.size() > maxlen) s.resize(maxlen);
    };

    int imported_count = 0;
    int skipped_count  = 0;
    int processed_count = 0;

    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(results, i);
        if (!item) continue;

        /* Extract basic fields from now_playing result */
        int         tmdb_id      = get_int_j(item, "id");
        std::string title        = get_str(item, "title");
        std::string synopsis     = get_str(item, "overview");
        std::string language     = get_str(item, "original_language");
        std::string release_date = get_str(item, "release_date");

        if (tmdb_id <= 0 || title.empty()) continue;

        /* Build genre string from genre_ids array */
        std::string genre_str;
        cJSON *genre_ids = cJSON_GetObjectItemCaseSensitive(item, "genre_ids");
        if (cJSON_IsArray(genre_ids)) {
            int gc = cJSON_GetArraySize(genre_ids);
            for (int gi = 0; gi < gc; gi++) {
                cJSON *gid_item = cJSON_GetArrayItem(genre_ids, gi);
                if (!cJSON_IsNumber(gid_item)) continue;
                int gid = (int)gid_item->valuedouble;
                auto it = genre_map.find(gid);
                if (it != genre_map.end()) {
                    if (!genre_str.empty()) genre_str += ",";
                    genre_str += it->second;
                }
            }
        }

        /* ── Deduplication check ── */
        int32_t tid_check = (int32_t)tmdb_id;
        WhereClause dup_w[1];
        strncpy(dup_w[0].col_name, "tmdb_id", 63);
        dup_w[0].col_name[63] = '\0';
        dup_w[0].op    = OP_EQ;
        dup_w[0].value = &tid_check;
        dup_w[0].logic = 0;

        if (db_count("movies", dup_w, 1) > 0) {
            processed_count++;
            {
                int done = processed_count;
                int pct  = (done * 100) / total;
                int fill = (done * 20) / total;
                printf("  [");
                for (int _b = 0; _b < 20; _b++) printf(_b < fill ? "█" : "░");
                printf("] %3d%%  (%2d/%2d)  \033[33m%s\033[0m  — skipped\n", pct, done, total, title.c_str());
            }
            skipped_count++;
            continue;
        }

        /* ── Fetch runtime from /movie/<id> ── */
        int duration_min = 120; /* default if fetch fails */
        try {
            std::string det_json = client.movie_details(tmdb_id);
            cJSON *det = cJSON_Parse(det_json.c_str());
            if (det) {
                int rt = get_int_j(det, "runtime");
                if (rt > 0) duration_min = rt;
                cJSON_Delete(det);
            }
        } catch (const TMDBException &ex) {
            /* Non-fatal — proceed with default duration */
            if (g_tmdb_debug_mode) {
                printf("  [TMDB][debug] Detail fetch failed for %s: %s (using default %dmin)\n",
                       title.c_str(), ex.what(), duration_min);
            }
        }

        /* ── Fetch credits ── */
        std::string credits_json;
        try {
            credits_json = client.credits(tmdb_id);
        } catch (const TMDBException &ex) {
            printf("  [TMDB] %s\n", ex.what());
            /* Non-fatal — proceed without cast */
            credits_json = "";
        }

        /* ── Truncate strings to schema limits ── */
        trunc_str(title,        199);
        trunc_str(synopsis,     499);
        trunc_str(genre_str,     99);
        trunc_str(language,      49);
        trunc_str(release_date,  19);

        /* ── Prepare buffers for db_insert ── */
        char title_buf[200]  = {}; strncpy(title_buf,   title.c_str(),        sizeof(title_buf)  - 1);
        char synop_buf[500]  = {}; strncpy(synop_buf,   synopsis.c_str(),     sizeof(synop_buf)  - 1);
        char genre_buf[100]  = {}; strncpy(genre_buf,   genre_str.c_str(),    sizeof(genre_buf)  - 1);
        char lang_buf[50]    = {}; strncpy(lang_buf,    language.c_str(),     sizeof(lang_buf)   - 1);
        char reldate_buf[20] = {}; strncpy(reldate_buf, release_date.c_str(), sizeof(reldate_buf)- 1);
        char rating_buf[10]  = {}; strncpy(rating_buf,  "UA",                 sizeof(rating_buf) - 1);

        int dummy_movie_id = 0;
        int tmdb_id_val    = tmdb_id;
        int32_t dur_val    = (int32_t)duration_min;
        int is_active_val  = 1;

        void *movie_fields[10];
        movie_fields[0] = &dummy_movie_id;
        movie_fields[1] = &tmdb_id_val;
        movie_fields[2] = title_buf;
        movie_fields[3] = synop_buf;
        movie_fields[4] = genre_buf;
        movie_fields[5] = lang_buf;
        movie_fields[6] = &dur_val;
        movie_fields[7] = reldate_buf;
        movie_fields[8] = rating_buf;
        movie_fields[9] = &is_active_val;

        /* ── WAL: begin transaction for this movie ── */
        wal_begin();
        int new_movie_id = db_insert("movies", movie_fields);
        if (new_movie_id <= 0) {
            wal_rollback();
            processed_count++;
            printf("  [%2d/%2d] %-45s — db_insert failed, skipped\n",
                   processed_count, total, title.c_str());
            continue;
        }

        /* ── Insert cast members (same transaction) ── */
        int cast_inserted = 0;
        if (!credits_json.empty()) {
            cJSON *cred_root = cJSON_Parse(credits_json.c_str());
            if (cred_root) {
                cJSON *cast_arr = cJSON_GetObjectItemCaseSensitive(cred_root, "cast");
                if (cJSON_IsArray(cast_arr)) {
                    int cast_total = cJSON_GetArraySize(cast_arr);
                    if (cast_total > 15) cast_total = 15;

                    for (int ci = 0; ci < cast_total; ci++) {
                        cJSON *cm         = cJSON_GetArrayItem(cast_arr, ci);
                        std::string cname = get_str(cm, "name");
                        std::string crole = get_str(cm, "character");
                        int         order = get_int_j(cm, "order");
                        int         is_lead = (order < 5) ? 1 : 0;

                        trunc_str(cname, 149);
                        trunc_str(crole, 149);

                        char cname_buf[150] = {};
                        char crole_buf[150] = {};
                        strncpy(cname_buf, cname.c_str(), sizeof(cname_buf) - 1);
                        strncpy(crole_buf, crole.c_str(), sizeof(crole_buf) - 1);

                        int dummy_cast_id = 0;
                        void *cast_fields[5];
                        cast_fields[0] = &dummy_cast_id;
                        cast_fields[1] = &new_movie_id;
                        cast_fields[2] = cname_buf;
                        cast_fields[3] = crole_buf;
                        cast_fields[4] = &is_lead;

                        int rc = db_insert("cast_members", cast_fields);
                        if (rc > 0) {
                            cast_inserted++;
                        } else {
                            break;
                        }
                    }
                }
                cJSON_Delete(cred_root);
            }
        }

        /* ── Commit this movie's transaction ── */
        wal_commit();

        processed_count++;

        /* Progress bar */
        {
            int done = processed_count;
            int pct  = (done * 100) / total;
            int fill = (done * 20) / total;
            printf("  [");
            for (int _b = 0; _b < 20; _b++) printf(_b < fill ? "█" : "░");
            printf("] %3d%%  (%2d/%2d)  \033[32m%s\033[0m  (id=%d, %d cast)\n",
                   pct, done, total, title.c_str(), new_movie_id, cast_inserted);
        }

        /* ── Record new movie_id for caller ── */
        if (out_movie_ids != nullptr && imported_count < max_out)
            out_movie_ids[imported_count] = new_movie_id;

        imported_count++;
    }

    cJSON_Delete(root);

    printf("\n  \xe2\x9c\x93 Bulk import complete.  %d new, %d skipped (already existed).\n",
           imported_count, skipped_count);

    return imported_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * C-callable report dispatch wrappers
 * ═══════════════════════════════════════════════════════════════════════════*/

extern "C"
void run_occupancy_report(int scope, int days)
{
    try {
        std::unique_ptr<Report> r = std::make_unique<OccupancyReport>(scope, days);
        r->build();
        r->display();
    } catch (const std::exception &ex) {
        fprintf(stderr, "run_occupancy_report failed: %s\n", ex.what());
    } catch (...) {
        fprintf(stderr, "run_occupancy_report failed: unknown error\n");
    }
}

extern "C"
void run_revenue_report(int scope, int days)
{
    try {
        std::unique_ptr<Report> r = std::make_unique<RevenueReport>(scope, days);
        r->build();
        r->display();
    } catch (const std::exception &ex) {
        fprintf(stderr, "run_revenue_report failed: %s\n", ex.what());
    } catch (...) {
        fprintf(stderr, "run_revenue_report failed: unknown error\n");
    }
}

extern "C"
void run_booking_report(int scope, int days)
{
    std::unique_ptr<Report> r = std::make_unique<BookingReport>(scope, days);
    r->build();
    r->display();
}

extern "C"
void run_report(int type, int scope, int days)
{
    std::unique_ptr<Report> r;

    switch (type) {
        case 0:  r = std::make_unique<OccupancyReport>(scope, days); break;
        case 1:  r = std::make_unique<RevenueReport>  (scope, days); break;
        case 2:  r = std::make_unique<BookingReport>  (scope, days); break;
        default:
            fprintf(stderr, "run_report: unknown type %d (0=Occ 1=Rev 2=Booking)\n", type);
            return;
    }

    r->build();
    r->display();
}