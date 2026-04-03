/*
 * main.c — CineBook entry point
 *
 * Bootstrap order:
 *   1. schema_load()    — parse schema.cat into memory      [CRITICAL]
 *   2. storage_init()   — initialise buffer pool             [CRITICAL]
 *   3. txn_init()       — WAL crash recovery                 [warn-only]
 *   4. index_init()     — rebuild in-memory indexes          [warn-only]
 *   5. load_refund_policy() — cache refund tiers             [warn-only]
 *
 * Then read cinebook.conf for display defaults.
 * Then loop: show_auth_menu → route by role → logout → repeat.
 * Shutdown: storage_flush_all → index_shutdown → txn_shutdown.
 *
 * C11. No external libs in this file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>  /* usleep */
#endif

/* ── Engine headers ── */
#include "schema.h"    /* schema_load, get_schema                            */
#include "storage.h"   /* storage_init, storage_flush_all                    */
#include "txn.h"       /* txn_init, txn_shutdown                             */
#include "index.h"     /* index_init, index_shutdown                         */
#include "query.h"     /* WhereClause, ResultSet, db_* — brings schema.h too */

/* ── Auth / Session ── */
#include "auth.h"      /* show_auth_menu, UserRole                           */
#include "session.h"   /* SessionContext, session_init, session_clear        */

/* ── Business logic ── */
#include "refund.h"    /* load_refund_policy                                 */
#include "payment.h"   /* wallet_topup                                       */
#include "reports.h"   /* tmdb error contract + curl lifecycle               */
#include "keystore.h"  /* encrypted API key storage                          */
#include "integrity.h" /* transaction integrity verification                 */
#include "wizard.h"    /* first-run setup wizard                            */
#include "ui_utils.h"  /* smart screen clear                                */
#include "banner.h"    /* startup banner                                     */

int tmdb_bulk_import_now_playing(const char *api_key, int *out_movie_ids, int max_out);

/* ─────────────────────────────────────────────────────────────────────────────
 * Forward declarations — UI modules have no .h files
 * ───────────────────────────────────────────────────────────────────────────*/
void browse_movies(SessionContext *ctx);
void view_upcoming_bookings(SessionContext *ctx);
void view_past_bookings(SessionContext *ctx);
void cancel_booking_menu(SessionContext *ctx);
void account_menu(SessionContext *ctx);
void admin_menu(SessionContext *ctx);
void dashboard_menu(SessionContext *ctx);
void run_booking_flow(int show_id, SessionContext *ctx);

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────*/
#define CONF_PATH    "cinebook.conf"
#define SCHEMA_PATH  "data/schema.cat"
#define CITY_MAX     101
#define SYM_MAX      5
#define BOX_INNER    56

/*
 * g_tmdb_api_key — global populated from cinebook.conf by read_conf().
 * Exposed as extern to ui_browse.c (streaming lookup) and ui_admin.c
 * (TMDB import).  Any module that needs it just declares:
 *     extern char g_tmdb_api_key[128];
 */
char g_tmdb_api_key[128] = {0};
int  g_tmdb_debug_mode = 0;
int  g_tmdb_allow_insecure_ssl = 0;
char g_tmdb_ca_bundle[256] = {0};
static int g_debug_mode = 0;

static void diag_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);

    if (g_debug_mode) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        fflush(stdout);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * read_conf — parse key=value lines from cinebook.conf.
 * Reads DEFAULT_CITY, BASE_CURRENCY_SYM, and TMDB_API_KEY.
 * 
 * Enhanced with encrypted API key support:
 *   1. First checks for .api_key file (encrypted format)
 *   2. If found, decrypts and loads API key
 *   3. Otherwise, falls back to cinebook.conf TMDB_API_KEY
 *   4. If found in cinebook.conf, auto-migrates to encrypted .api_key
 * ───────────────────────────────────────────────────────────────────────────*/
static int tmdb_api_key_is_valid_format(const char *k)
{
    if (!k || k[0] == '\0') return 0;
    size_t n = strlen(k);
    if (n < 16 || n > 128) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = k[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

static int file_exists_path(const char *path)
{
    struct stat st;
    if (!path || path[0] == '\0') return 0;
    return stat(path, &st) == 0;
}

static void read_conf(char *default_city, int city_max,
                      char *currency_sym,  int sym_max)
{
    /* Safe defaults */
    strncpy(default_city, "Chennai",  (size_t)(city_max - 1));
    default_city[city_max - 1] = '\0';
    strncpy(currency_sym, "\xe2\x82\xb9", (size_t)(sym_max - 1)); /* UTF-8 ₹ */
    currency_sym[sym_max - 1] = '\0';

    /* ─────────────────────────────────────────────────────────────────────
     * Step 1: Try loading encrypted API key from .api_key file
     * If decrypt fails, self-heal by removing stale/corrupted file so
     * setup wizard can recreate it from legacy key or fresh prompt.
     * ───────────────────────────────────────────────────────────────────── */
    if (file_exists_path(".api_key")) {
        char *decrypted_key = decrypt_api_key(".api_key");
        if (decrypted_key) {
            strncpy(g_tmdb_api_key, decrypted_key, sizeof(g_tmdb_api_key) - 1);
            g_tmdb_api_key[sizeof(g_tmdb_api_key) - 1] = '\0';
            secure_zero(decrypted_key, strlen(decrypted_key));
            free(decrypted_key);
            
            /* Still parse cinebook.conf for other settings */
            FILE *f = fopen(CONF_PATH, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                        line[--len] = '\0';

                    if (strncmp(line, "DEFAULT_CITY=", 13) == 0) {
                        strncpy(default_city, line + 13, (size_t)(city_max - 1));
                        default_city[city_max - 1] = '\0';
                    } else if (strncmp(line, "BASE_CURRENCY_SYM=", 18) == 0) {
                        strncpy(currency_sym, line + 18, (size_t)(sym_max - 1));
                        currency_sym[sym_max - 1] = '\0';
                    } else if (strncmp(line, "TMDB_DEBUG=", 11) == 0) {
                        g_tmdb_debug_mode = atoi(line + 11) ? 1 : 0;
                    } else if (strncmp(line, "DEBUG=", 6) == 0) {
                        g_debug_mode = atoi(line + 6) ? 1 : 0;
                    } else if (strncmp(line, "TMDB_ALLOW_INSECURE_SSL=", 24) == 0) {
                        g_tmdb_allow_insecure_ssl = atoi(line + 24) ? 1 : 0;
                    } else if (strncmp(line, "TMDB_CA_BUNDLE=", 15) == 0) {
                        strncpy(g_tmdb_ca_bundle, line + 15, sizeof(g_tmdb_ca_bundle) - 1);
                        g_tmdb_ca_bundle[sizeof(g_tmdb_ca_bundle) - 1] = '\0';
                    }
                }
                fclose(f);
            }
            
            diag_log("[config] Loaded TMDB API key from encrypted .api_key file\n");
            return;
        }

        if (remove(".api_key") == 0) {
            diag_log("[config] Removed unreadable/corrupted .api_key; setup wizard will recreate it\n");
        } else {
            fprintf(stderr, "  [warn] .api_key unreadable and could not be removed; setup may prompt again.\n");
        }
    }

    /* ─────────────────────────────────────────────────────────────────────
     * Step 2: Fall back to cinebook.conf (legacy plaintext format)
     * ───────────────────────────────────────────────────────────────────── */
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        fprintf(stderr, "  [warn] Cannot open %s — using defaults.\n", CONF_PATH);
        return;
    }

    char line[256];
    char plaintext_api_key[128] = {0};
    int found_plaintext_key = 0;
    
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline / carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "DEFAULT_CITY=", 13) == 0) {
            strncpy(default_city, line + 13, (size_t)(city_max - 1));
            default_city[city_max - 1] = '\0';
        } else if (strncmp(line, "BASE_CURRENCY_SYM=", 18) == 0) {
            strncpy(currency_sym, line + 18, (size_t)(sym_max - 1));
            currency_sym[sym_max - 1] = '\0';
        } else if (strncmp(line, "TMDB_API_KEY=", 13) == 0) {
            strncpy(plaintext_api_key, line + 13, sizeof(plaintext_api_key) - 1);
            plaintext_api_key[sizeof(plaintext_api_key) - 1] = '\0';
            found_plaintext_key = 1;
        } else if (strncmp(line, "TMDB_DEBUG=", 11) == 0) {
            g_tmdb_debug_mode = atoi(line + 11) ? 1 : 0;
        } else if (strncmp(line, "DEBUG=", 6) == 0) {
            g_debug_mode = atoi(line + 6) ? 1 : 0;
        } else if (strncmp(line, "TMDB_ALLOW_INSECURE_SSL=", 24) == 0) {
            g_tmdb_allow_insecure_ssl = atoi(line + 24) ? 1 : 0;
        } else if (strncmp(line, "TMDB_CA_BUNDLE=", 15) == 0) {
            strncpy(g_tmdb_ca_bundle, line + 15, sizeof(g_tmdb_ca_bundle) - 1);
            g_tmdb_ca_bundle[sizeof(g_tmdb_ca_bundle) - 1] = '\0';
        }
    }
    fclose(f);
    
    /* ─────────────────────────────────────────────────────────────────────
     * Step 3: Auto-migrate plaintext API key to encrypted format
     * ───────────────────────────────────────────────────────────────────── */
    if (found_plaintext_key && plaintext_api_key[0] != '\0') {
        /* Copy to global variable */
        strncpy(g_tmdb_api_key, plaintext_api_key, sizeof(g_tmdb_api_key) - 1);
        g_tmdb_api_key[sizeof(g_tmdb_api_key) - 1] = '\0';
        
        /* Attempt migration to encrypted storage */
        diag_log("[config] Found plaintext TMDB_API_KEY in %s\n", CONF_PATH);
        diag_log("[config] Migrating to encrypted .api_key file...\n");
        
        if (encrypt_api_key(plaintext_api_key, ".api_key") == 0) {
            diag_log("[config] ✓ API key migrated to encrypted storage\n");
            diag_log("[config]   You can now remove TMDB_API_KEY from %s\n", CONF_PATH);
        } else {
            fprintf(stderr, "  [warn] Failed to migrate API key to encrypted storage\n");
            fprintf(stderr, "  [warn] Continuing with plaintext key from %s\n", CONF_PATH);
        }
        
        /* Zero out plaintext key from memory */
        secure_zero(plaintext_api_key, sizeof(plaintext_api_key));
    }
}


static void ensure_runtime_dirs(void)
{
    /* These directories must exist before any file I/O */
    /* Use mkdir with exist-ok semantics */
#ifdef _WIN32
    _mkdir("data");
    _mkdir("data/db");
    _mkdir("data/idx");
    _mkdir("exports");
#else
    mkdir("data",     0755);
    mkdir("data/db",  0755);
    mkdir("data/idx", 0755);
    mkdir("exports",  0755);
#endif
}

static int rs_int(void **row, int col)
{
    if (!row || !row[col]) return 0;
    return *(int *)row[col];
}

static int rs_cell_int(ResultSet *rs, int row, int col)
{
    if (!rs || row < 0 || col < 0) return 0;
    if (row >= rs->row_count || col >= rs->col_count) return 0;
    if (!rs->rows[row] || !rs->rows[row][col]) return 0;
    return *(int *)rs->rows[row][col];
}

static const char *rs_str(void **row, int col)
{
    if (!row || !row[col]) return "";
    return (const char *)row[col];
}

static void read_line(char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    if (!fgets(buf, (int)sz, stdin)) {
        buf[0] = '\0';
        return;
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
}

static void format_inr_amount(float amount, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;

    long long paise = (long long)(amount * 100.0f + (amount >= 0.0f ? 0.5f : -0.5f));
    int neg = (paise < 0);
    unsigned long long abs_paise = (unsigned long long)(neg ? -paise : paise);
    unsigned long long whole = abs_paise / 100ULL;
    unsigned int frac = (unsigned int)(abs_paise % 100ULL);

    char digits[32];
    snprintf(digits, sizeof(digits), "%llu", whole);

    char grouped[48];
    int dlen = (int)strlen(digits);
    int commas = (dlen - 1) / 3;
    int glen = dlen + commas;
    if (glen >= (int)sizeof(grouped)) glen = (int)sizeof(grouped) - 1;
    grouped[glen] = '\0';

    int di = dlen - 1;
    int gi = glen - 1;
    int chunk = 0;
    while (di >= 0 && gi >= 0) {
        grouped[gi--] = digits[di--];
        chunk++;
        if (chunk == 3 && di >= 0 && gi >= 0) {
            grouped[gi--] = ',';
            chunk = 0;
        }
    }

    snprintf(out, out_sz, "%s%s.%02u", neg ? "-" : "", grouped, frac);
}

static void current_datetime_key(char *out, size_t out_sz)
{
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    if (!tmv) {
        snprintf(out, out_sz, "1970-01-01 00:00");
        return;
    }
    strftime(out, out_sz, "%Y-%m-%d %H:%M", tmv);
}

static void current_date_short(char *out, size_t out_sz)
{
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    if (!tmv) {
        snprintf(out, out_sz, "--- -- ---");
        return;
    }
    strftime(out, out_sz, "%a %d %b", tmv);
}

static int is_future_datetime(const char *dt)
{
    if (!dt || dt[0] == '\0') return 0;
    char now_key[20];
    current_datetime_key(now_key, sizeof(now_key));
    return strcmp(dt, now_key) > 0;
}

static void resolve_city_name(const SessionContext *ctx, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    snprintf(out, out_sz, "Chennai");

    if (!ctx || ctx->preferred_city_id <= 0) return;

    int city_id = ctx->preferred_city_id;
    WhereClause wc[1];
    memset(wc, 0, sizeof(wc));
    strncpy(wc[0].col_name, "city_id", sizeof(wc[0].col_name) - 1);
    wc[0].op = OP_EQ;
    wc[0].value = &city_id;
    wc[0].logic = 0;

    ResultSet *rs = db_select("cities", wc, 1, NULL, 0);
    if (rs && rs->row_count > 0 && rs->rows[0] && rs->rows[0][1]) {
        snprintf(out, out_sz, "%s", (char *)rs->rows[0][1]);
    }
    if (rs) result_set_free(rs);
}

static int count_confirmed_upcoming_bookings(const SessionContext *ctx)
{
    if (!ctx || ctx->user_id <= 0) return 0;

    int user_id = ctx->user_id;
    int status_conf = 1;
    char now_key[20];
    current_datetime_key(now_key, sizeof(now_key));

    WhereClause bw[2];
    memset(bw, 0, sizeof(bw));
    strncpy(bw[0].col_name, "user_id", sizeof(bw[0].col_name) - 1);
    bw[0].op = OP_EQ;
    bw[0].value = &user_id;
    bw[0].logic = 0;
    strncpy(bw[1].col_name, "status", sizeof(bw[1].col_name) - 1);
    bw[1].op = OP_EQ;
    bw[1].value = &status_conf;
    bw[1].logic = 0;

    ResultSet *bk = db_select("bookings", bw, 2, NULL, 0);
    if (!bk) return 0;

    int total = 0;
    for (int i = 0; i < bk->row_count; i++) {
        int show_id = rs_int(bk->rows[i], 2);

        WhereClause sw[1];
        memset(sw, 0, sizeof(sw));
        strncpy(sw[0].col_name, "show_id", sizeof(sw[0].col_name) - 1);
        sw[0].op = OP_EQ;
        sw[0].value = &show_id;
        sw[0].logic = 0;

        ResultSet *sh = db_select("shows", sw, 1, NULL, 0);
        if (sh && sh->row_count > 0) {
            const char *show_dt = rs_str(sh->rows[0], 3);
            if (show_dt[0] != '\0' && strcmp(show_dt, now_key) >= 0)
                total++;
        }
        if (sh) result_set_free(sh);
    }

    result_set_free(bk);
    return total;
}

static void view_notifications(SessionContext *ctx)
{
    if (!ctx || ctx->user_id <= 0) return;

    char input[32];

    int user_id = ctx->user_id;
    int wl_notified = 1; /* NOTIFIED */

    WhereClause wc[2];
    memset(wc, 0, sizeof(wc));
    strncpy(wc[0].col_name, "user_id", sizeof(wc[0].col_name) - 1);
    wc[0].op = OP_EQ;
    wc[0].value = &user_id;
    wc[0].logic = 0;
    strncpy(wc[1].col_name, "status", sizeof(wc[1].col_name) - 1);
    wc[1].op = OP_EQ;
    wc[1].value = &wl_notified;
    wc[1].logic = 0;

    ResultSet *wl = db_select("waitlist", wc, 2, NULL, 0);

    printf("\n  ── Notifications ──────────────────────────────────────\n");

    if (!wl || wl->row_count == 0) {
        printf("  No unread notifications.\n");
        if (wl) result_set_free(wl);
        printf("\n  Press Enter to continue...");
        read_line(input, sizeof(input));
        return;
    }

    for (int i = 0; i < wl->row_count; i++) {
        int waitlist_id = rs_int(wl->rows[i], 0);
        int show_id = rs_int(wl->rows[i], 2);
        const char *notified_at = rs_str(wl->rows[i], 6);

        char show_dt[20] = "";
        char movie_title[201] = "Movie";

        WhereClause sw[1];
        memset(sw, 0, sizeof(sw));
        strncpy(sw[0].col_name, "show_id", sizeof(sw[0].col_name) - 1);
        sw[0].op = OP_EQ;
        sw[0].value = &show_id;
        sw[0].logic = 0;

        ResultSet *sh = db_select("shows", sw, 1, NULL, 0);
        int movie_id = 0;
        if (sh && sh->row_count > 0) {
            snprintf(show_dt, sizeof(show_dt), "%s", rs_str(sh->rows[0], 3));
            movie_id = rs_int(sh->rows[0], 1);
        }
        if (sh) result_set_free(sh);

        if (movie_id > 0) {
            WhereClause mw[1];
            memset(mw, 0, sizeof(mw));
            strncpy(mw[0].col_name, "movie_id", sizeof(mw[0].col_name) - 1);
            mw[0].op = OP_EQ;
            mw[0].value = &movie_id;
            mw[0].logic = 0;

            ResultSet *mv = db_select("movies", mw, 1, NULL, 0);
            if (mv && mv->row_count > 0) {
                snprintf(movie_title, sizeof(movie_title), "%s", rs_str(mv->rows[0], 2));
            }
            if (mv) result_set_free(mv);
        }

        printf("\n  🔔  Seats available for %s on %.10s at %.5s\n",
               movie_title,
               show_dt[0] ? show_dt : "----------",
               show_dt[0] ? show_dt + 11 : "--:--");
        printf("      Notified at: %s\n", notified_at[0] ? notified_at : "-");
        printf("      [B] Book now    [D] Dismiss    [N] Next\n");
        printf("      Choice: ");
        read_line(input, sizeof(input));

        if (input[0] == 'B' || input[0] == 'b') {
            run_booking_flow(show_id, ctx);
            int wl_fulfilled = 3; /* existing enum in refund.c */
            WhereClause ww[1];
            memset(ww, 0, sizeof(ww));
            strncpy(ww[0].col_name, "waitlist_id", sizeof(ww[0].col_name) - 1);
            ww[0].op = OP_EQ;
            ww[0].value = &waitlist_id;
            ww[0].logic = 0;
            db_update("waitlist", ww, 1, "status", &wl_fulfilled);
            if (ctx->unread_notifs > 0) ctx->unread_notifs--;
        } else if (input[0] == 'D' || input[0] == 'd') {
            int wl_expired = 2; /* EXPIRED per refund.c */
            WhereClause ww[1];
            memset(ww, 0, sizeof(ww));
            strncpy(ww[0].col_name, "waitlist_id", sizeof(ww[0].col_name) - 1);
            ww[0].op = OP_EQ;
            ww[0].value = &waitlist_id;
            ww[0].logic = 0;
            db_update("waitlist", ww, 1, "status", &wl_expired);
            if (ctx->unread_notifs > 0) ctx->unread_notifs--;
        }
    }

    /* Mark any remaining NOTIFIED rows as read/expired for this user */
    {
        int wl_expired = 2;
        db_update("waitlist", wc, 2, "status", &wl_expired);
    }
    ctx->unread_notifs = 0;

    result_set_free(wl);
    printf("\n  Press Enter to continue...");
    read_line(input, sizeof(input));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * user_menu — interactive menu for regular users (USER / STUDENT)
 * ───────────────────────────────────────────────────────────────────────────*/
static int bulk_schedule_from_main(void)
{
    const int days_ahead = 7;
    const int day_offset = 1;
    const char *time_slots[] = {"10:00", "14:30", "19:00"};
    const float base_prices[] = {200.0f, 300.0f, 350.0f, 450.0f};

    int one = 1;
    WhereClause aw[1];
    memset(aw, 0, sizeof(aw));
    strncpy(aw[0].col_name, "is_active", sizeof(aw[0].col_name) - 1);
    aw[0].op = OP_EQ;
    aw[0].value = &one;
    aw[0].logic = 0;

    ResultSet *mv_rs = db_select("movies", aw, 1, NULL, 0);
    if (!mv_rs || mv_rs->row_count == 0) {
        if (mv_rs) result_set_free(mv_rs);
        diag_log("[setup] No active movies available after import; skipping auto-scheduling.\n");
        return 0;
    }

    ResultSet *sc_rs = db_select("screens", aw, 1, NULL, 0);
    if (!sc_rs || sc_rs->row_count == 0) {
        if (sc_rs) result_set_free(sc_rs);
        result_set_free(mv_rs);
        diag_log("[setup] No active screens found; skipping auto-scheduling.\n");
        return 0;
    }

    const int n_movies = mv_rs->row_count;
    const int n_screens = sc_rs->row_count;
    int movie_idx = 0;
    int inserted = 0;
    int days_scheduled = 0;
    int stop_scheduling = 0;
    int storage_limit_hit = 0;

    diag_log("[setup] Auto-scheduling next %d days across %d active screens...\n",
             days_ahead, n_screens);

    time_t base = time(NULL) + (time_t)day_offset * 86400L;

    for (int d = 0; d < days_ahead; d++) {
        int day_has_insert = 0;
        time_t day_t = base + (time_t)d * 86400L;
        struct tm *tm_day = localtime(&day_t);
        if (!tm_day) continue;

        char date_str[11];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm_day);
        diag_log("[setup] Scheduling day %d/%d (%s)\n", d + 1, days_ahead, date_str);

        for (int si = 0; si < n_screens && !stop_scheduling; si++) {
            int screen_id = rs_cell_int(sc_rs, si, 0);
            int screen_type = rs_cell_int(sc_rs, si, 3);
            int movie_id = rs_cell_int(mv_rs, movie_idx % n_movies, 0);
            movie_idx++;

            float base_price = 200.0f;
            if (screen_type >= 0 && screen_type <= 3) base_price = base_prices[screen_type];

            for (int ti = 0; ti < 3; ti++) {
                char show_dt[20];
                snprintf(show_dt, sizeof(show_dt), "%s %s", date_str, time_slots[ti]);

                if (!is_future_datetime(show_dt)) continue;

                WhereClause dup_w[3];
                memset(dup_w, 0, sizeof(dup_w));
                strncpy(dup_w[0].col_name, "movie_id", sizeof(dup_w[0].col_name) - 1);
                dup_w[0].op = OP_EQ; dup_w[0].value = &movie_id;

                strncpy(dup_w[1].col_name, "screen_id", sizeof(dup_w[1].col_name) - 1);
                dup_w[1].op = OP_EQ; dup_w[1].value = &screen_id;

                strncpy(dup_w[2].col_name, "show_datetime", sizeof(dup_w[2].col_name) - 1);
                dup_w[2].op = OP_EQ; dup_w[2].value = show_dt;

                if (db_count("shows", dup_w, 3) > 0) continue;

                WhereClause seat_w[2];
                memset(seat_w, 0, sizeof(seat_w));
                strncpy(seat_w[0].col_name, "screen_id", sizeof(seat_w[0].col_name) - 1);
                seat_w[0].op = OP_EQ; seat_w[0].value = &screen_id;

                strncpy(seat_w[1].col_name, "is_active", sizeof(seat_w[1].col_name) - 1);
                seat_w[1].op = OP_EQ; seat_w[1].value = &one;

                ResultSet *seat_rs = db_select("seats", seat_w, 2, NULL, 0);
                if (!seat_rs || seat_rs->row_count == 0) {
                    if (seat_rs) result_set_free(seat_rs);
                    continue;
                }

                int pk_zero = 0;
                int is_active = 1;
                void *sf[6] = {&pk_zero, &movie_id, &screen_id, show_dt, &base_price, &is_active};

                wal_begin();
                int new_show_id = db_insert("shows", sf);
                int ok = (new_show_id > 0);
                int seat_status_insert_failed = 0;

                if (ok) {
                    int ss_pk = 0;
                    int show_v = new_show_id;
                    int status_v = 0;
                    int null_i = INT_NULL_SENTINEL;
                    char null_d[20];
                    int null_b = INT_NULL_SENTINEL;
                    memset(null_d, 0, sizeof(null_d));

                    for (int k = 0; k < seat_rs->row_count; k++) {
                        int seat_id = rs_cell_int(seat_rs, k, 0);
                        void *ff[7] = {&ss_pk, &show_v, &seat_id, &status_v, &null_i, null_d, &null_b};
                        if (db_insert("seat_status", ff) <= 0) {
                            ok = 0;
                            seat_status_insert_failed = 1;
                            break;
                        }
                    }
                }

                if (ok) {
                    wal_commit();
                    inserted++;
                    day_has_insert = 1;
                } else {
                    wal_rollback();
                    if (seat_status_insert_failed) {
                        storage_limit_hit = 1;
                        stop_scheduling = 1;
                    }
                }

                result_set_free(seat_rs);

                if (stop_scheduling) break;
            }
        }

        if (day_has_insert) days_scheduled++;
        if (stop_scheduling) break;
    }

    if (storage_limit_hit) {
        diag_log("[setup] Storage limit reached — scheduled %d shows across %d days. Increase POOL_SIZE or reduce screens to schedule more.\n",
                 inserted, days_scheduled);
    }

    result_set_free(mv_rs);
    result_set_free(sc_rs);

    return inserted;
}

static int first_run_setup(const char *api_key)
{
    int movie_count = db_count("movies", NULL, 0);
    if (movie_count > 0) return 1;

    printf("  Setting up CineBook for first use, please wait...\n");
    diag_log("[setup] First run detected — importing now-playing movies from TMDB. This may take 30-60 seconds...\n");

    if (!api_key || api_key[0] == '\0') {
        diag_log("[setup] TMDB_API_KEY not set in cinebook.conf.\n");
        diag_log("[setup] Warning: first-run setup skipped; continuing to auth menu.\n");
        printf("  Ready.\n\n");
        return 0;
    }

    int out_ids[20];
    memset(out_ids, 0, sizeof(out_ids));

    int imported = tmdb_bulk_import_now_playing(api_key, out_ids, 20);
    if (imported <= 0) {
        diag_log("[setup] Warning: TMDB import failed; skipping remaining first-run setup.\n");
        printf("  Ready.\n\n");
        return 0;
    }

    int scheduled = bulk_schedule_from_main();
    if (scheduled <= 0) {
        diag_log("[setup] Warning: auto-scheduling produced no shows; skipping remaining first-run setup.\n");
        printf("  Ready.\n\n");
        return 0;
    }

    int active_screen_count = 0;
    {
        int one = 1;
        WhereClause sw[1];
        memset(sw, 0, sizeof(sw));
        strncpy(sw[0].col_name, "is_active", sizeof(sw[0].col_name) - 1);
        sw[0].op = OP_EQ;
        sw[0].value = &one;
        sw[0].logic = 0;
        active_screen_count = db_count("screens", sw, 1);
        if (active_screen_count < 0) active_screen_count = 0;
    }

    diag_log("[setup] Auto-setup complete: %d movies imported, %d shows scheduled across %d screens.\n",
             imported, scheduled, active_screen_count);

    printf("  Ready.\n\n");
    return 1;
}

static void user_menu(SessionContext *ctx)
{
    char buf[16];

    while (1) {
        smart_clear(UI_CONTEXT_MENU);

        char city[64];
        char date_str[32];
        char wallet_amount[32];
        const char *role_str = (ctx->role == ROLE_STUDENT)
            ? "Student \033[32m✓ 12%% off\033[0m"
            : "User";

        resolve_city_name(ctx, city, sizeof(city));
        current_date_short(date_str, sizeof(date_str));
        format_inr_amount(ctx->wallet_balance, wallet_amount, sizeof(wallet_amount));

        int upcoming_confirmed = count_confirmed_upcoming_bookings(ctx);
        int unread = (ctx->unread_notifs > 0) ? ctx->unread_notifs : 0;

        {
            char line[BOX_INNER + 1];
            char left[96];
            char right[64];
            int gap = 0;

            printf("\n");
            printf("  ╔");
            for (int i = 0; i < BOX_INNER; i++) printf("═");
            printf("╗\n");

            snprintf(left, sizeof(left), "CINEBOOK");
            snprintf(right, sizeof(right), "%.22s  %.12s", ctx->name, date_str);
            gap = BOX_INNER - (int)strlen(left) - (int)strlen(right);
            if (gap < 1) gap = 1;
            snprintf(line, sizeof(line), "%s%*s%s", left, gap, "", right);
            printf("  ║%-*.*s║\n", BOX_INNER, BOX_INNER, line);

            if (ctx->wallet_balance == 0.0f)
                snprintf(left, sizeof(left), "%.14s  Wallet: Rs.%s (LOW)", city, wallet_amount);
            else
                snprintf(left, sizeof(left), "%.14s  Wallet: Rs.%s", city, wallet_amount);

            if (unread > 0)
                snprintf(right, sizeof(right), "Notifications: %d", unread);
            else
                snprintf(right, sizeof(right), "Notifications: 0");

            gap = BOX_INNER - (int)strlen(left) - (int)strlen(right);
            if (gap < 1) gap = 1;
            snprintf(line, sizeof(line), "%s%*s%s", left, gap, "", right);
            printf("  ║%-*.*s║\n", BOX_INNER, BOX_INNER, line);

            printf("  ╚");
            for (int i = 0; i < BOX_INNER; i++) printf("═");
            printf("╝\n\n");
        }

        printf("  Role: %s\n\n", role_str);

        printf("  ┌──────────────────────────────────────────────────────────┐\n");
        printf("  │                                                          │\n");
        printf("  │   [ 1 ]  Browse Movies & Book                            │\n");
        printf("  │   [ 2 ]  Upcoming Bookings          (%d confirmed)       │\n",   upcoming_confirmed);
        printf("  │   [ 3 ]  Past Bookings                                   │\n");
        printf("  │   [ 4 ]  Cancel a Booking                                │\n");
        printf("  │   [ 5 ]  Account Settings                                │\n");
        printf("  │   [ 6 ]  Add Funds to Wallet (quick)      Rs.%s balance  │\n", wallet_amount);
        if (unread > 0) {
            /* \033[5m blink is terminal-dependent and may not work on all Windows terminals. */
        printf("  │   [ 7 ]  View Notifications        %d unread \033[5m← new\033[0m         │\n",
                   unread);
        } else {
        printf("  │   [ 7 ]  View Notifications                              │\n");
        }
        printf("  │   [ 8 ]  Logout                                          │\n");
        printf("  │                                                          │\n");
        printf("  └──────────────────────────────────────────────────────────┘\n");
        printf("  Choice: ");
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) break;

        size_t blen = strlen(buf);
        if (blen > 0 && buf[blen - 1] == '\n') buf[--blen] = '\0';

        char *ep = NULL;
        long choice = strtol(buf, &ep, 10);
        if (ep == buf || *ep != '\0') {
            printf("  [!] Please enter 1-8.\n");
            continue;
        }

        switch (choice) {
            case 1: browse_movies(ctx);          break;
            case 2: view_upcoming_bookings(ctx); break;
            case 3: view_past_bookings(ctx);     break;
            case 4: cancel_booking_menu(ctx);    break;
            case 5: account_menu(ctx);           break;
            case 6: wallet_topup(ctx);           break;
            case 7: view_notifications(ctx);     break;
            case 8:
                printf("\n  Logging out...\n\n");
                session_clear(ctx);
                return;
            default:
                printf("  [!] Please enter 1-8.\n");
                break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────────*/
int main(void)
{
/* forward declaration — implemented in reports.cpp */
void cinebook_curl_init(void);
void cinebook_curl_cleanup(void);

    ensure_runtime_dirs();

#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    /* Enable ANSI escape processing so \033[2J and colour codes work */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    /* ── Banner ── */
    show_banner();

    /* ── initialise libcurl once for the entire process ── */
    cinebook_curl_init();

    /* ─────────────────────────────────────────────────────────────────────
     * Bootstrap
     * ───────────────────────────────────────────────────────────────────*/

    /* 1. Schema — CRITICAL */
    diag_log("  [boot] Loading schema from %s ... ", SCHEMA_PATH);
    schema_load(SCHEMA_PATH);
    if (g_schema_count == 0) {
        fprintf(stderr, "\n  [FATAL] schema_load() produced 0 tables. "
                "Check that %s exists and is well-formed.\n", SCHEMA_PATH);
        return 1;
    }
    diag_log("OK  (%d table(s))\n", g_schema_count);

    /* 2. Storage — CRITICAL */
    diag_log("  [boot] Initialising buffer pool ... ");
    storage_init();
    diag_log("OK\n");

    /* 3. WAL / transaction recovery — warn-only */
    diag_log("  [boot] Opening WAL and running crash recovery ... ");
    txn_init();
    {
        WALRecoverySummary wal_summary = wal_get_last_recovery_summary();
        diag_log("OK (scanned=%u committed=%u uncommitted=%u rolled_back=%u restore_fail=%u next_txn=%u)\n",
                 wal_summary.entries_scanned,
                 wal_summary.committed_entries,
                 wal_summary.uncommitted_entries,
                 wal_summary.rolled_back_entries,
                 wal_summary.restore_failures,
                 wal_summary.next_txn_id);
        if (wal_summary.checksum_mismatches > 0 || wal_summary.legacy_header_detected || wal_summary.wal_rewritten) {
            diag_log("  [boot] WAL notes: checksum_mismatch=%u legacy_header=%u rewritten=%u\n",
                     wal_summary.checksum_mismatches,
                     wal_summary.legacy_header_detected,
                     wal_summary.wal_rewritten);
        }
    }

    /* 3b. Integrity verification after WAL recovery */
    diag_log("  [boot] Verifying transaction integrity ... ");
    IntegrityReport *integrity = verify_transaction_state();
    if (integrity) {
        if (integrity->total_issues > 0) {
            diag_log("WARN (%d issue(s) detected)\n", integrity->total_issues);
        } else {
            diag_log("OK\n");
        }
        free_integrity_report(integrity);
    } else {
        diag_log("WARN (integrity check unavailable)\n");
    }

    /* 4. Indexes — warn-only */
    diag_log("  [boot] Rebuilding in-memory indexes ... ");
    index_init();
    diag_log("OK\n");

    /* 5. Refund policy cache — warn-only */
    diag_log("  [boot] Caching refund policy tiers ... ");
    load_refund_policy();
    diag_log("OK\n");

    diag_log("\n  Engine ready.\n\n");

    /* ─────────────────────────────────────────────────────────────────────
     * Read cinebook.conf
     * ───────────────────────────────────────────────────────────────────*/
    char default_city[CITY_MAX];
    char currency_sym[SYM_MAX];
    read_conf(default_city, CITY_MAX, currency_sym, SYM_MAX);

    /* First-run interactive setup wizard (idempotent) */
    if (!setup_wizard_run(0, g_tmdb_api_key, sizeof(g_tmdb_api_key))) {
        fprintf(stderr, "  [warn] Setup wizard did not complete; TMDB features may be limited.\n");
    }

    if (g_tmdb_api_key[0] != '\0' && !tmdb_api_key_is_valid_format(g_tmdb_api_key)) {
        fprintf(stderr, "  [warn] TMDB_API_KEY format looks invalid; TMDB features disabled.\n");
        g_tmdb_api_key[0] = '\0';
    }

    if (g_tmdb_allow_insecure_ssl) {
        fprintf(stderr, "  [warn] TMDB insecure SSL mode is ON (dev-only). Disable for production.\n");
    }

    diag_log("  Config  ->  City: %-12s  Currency: %-4s  TMDB key: %s  Debug:%s\n\n",
             default_city,
             currency_sym,
             g_tmdb_api_key[0] != '\0' ? "loaded" : "not set (TMDB features disabled)",
             g_tmdb_debug_mode ? "on" : "off");

    if (first_run_setup(g_tmdb_api_key) <= 0) {
        diag_log("[setup] Continuing startup with partial/no first-run data.\n");
    }

    /* ─────────────────────────────────────────────────────────────────────
     * Session context
     * ───────────────────────────────────────────────────────────────────*/
    SessionContext ctx;
    session_init(&ctx);

    /* ─────────────────────────────────────────────────────────────────────
     * Main loop
     * ───────────────────────────────────────────────────────────────────*/
    while (1) {
        /* Auth loop — blocks until user authenticates or picks Exit */
        show_auth_menu(&ctx);

        /* show_auth_menu returns with user_id==0 on Exit */
        if (ctx.user_id == 0)
            break;

        smart_clear(UI_CONTEXT_MENU);

        if (ctx.is_admin)
            admin_menu(&ctx);
        else
            user_menu(&ctx);

        /* Safety clear in case a role menu returned without calling logout */
        if (ctx.user_id != 0)
            session_clear(&ctx);
    }

    /* ─────────────────────────────────────────────────────────────────────
     * Shutdown
     * ───────────────────────────────────────────────────────────────────*/
    diag_log("\n  Shutting down...\n");

    diag_log("  [shutdown] Flushing dirty pages ... ");
    storage_flush_all();
    diag_log("OK\n");

    diag_log("  [shutdown] Persisting indexes ... ");
    index_shutdown();
    diag_log("OK\n");

    diag_log("  [shutdown] Committing WAL ... ");
    txn_shutdown();
    diag_log("OK\n");

    diag_log("\n  Goodbye.\n\n");
    cinebook_curl_cleanup();
    return 0;
}