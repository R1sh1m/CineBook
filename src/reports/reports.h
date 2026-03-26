/*
 * reports.h — C-callable interface to the C++ OOP reporting and TMDB layer.
 *
 * RULES FOR INCLUSION:
 *   - C code  (.c files)  : #include "reports.h" directly — safe.
 *   - C++ code (.cpp)     : #include "reports.h" directly — safe.
 *   - NEVER include <curl/curl.h> or cJSON.h from this header.
 *     Those live ONLY inside reports.cpp.
 *
 * The extern "C" block ensures the five wrapper functions have C linkage
 * so gcc-compiled .o files can call into the g++-compiled reports.o.
 *
 * type codes for run_report():
 *   0 = OccupancyReport  — per-show fill rate, ANSI bar chart
 *   1 = RevenueReport    — revenue by movie and by theatre
 *   2 = BookingReport    — bookings by hour-of-day and day-of-week
 *
 * scope : reserved for future country/city scoping; pass 0 for all.
 * days  : look-back window in days (e.g. 30, 60, 90).
 */

#ifndef REPORTS_H
#define REPORTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────────────────────────────────────────────────────────────────────
 * TMDB typed error contract
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    TMDB_ERR_NETWORK_TIMEOUT = 0,
    TMDB_ERR_NETWORK_RESET,
    TMDB_ERR_DNS_FAILURE,
    TMDB_ERR_TLS_CERT_FAILURE,
    TMDB_ERR_PROXY_OR_FIREWALL_BLOCK,
    TMDB_ERR_HTTP_401_INVALID_KEY,
    TMDB_ERR_HTTP_429_RATE_LIMIT,
    TMDB_ERR_HTTP_4XX_CLIENT,
    TMDB_ERR_HTTP_5XX_SERVER,
    TMDB_ERR_JSON_PARSE_ERROR,
    TMDB_ERR_JSON_SCHEMA_MISMATCH,
    TMDB_ERR_EMPTY_RESPONSE,
    TMDB_ERR_DB_WRITE_FAILURE,
    TMDB_ERR_UNKNOWN
} TMDBErrorCategory;

typedef struct {
    TMDBErrorCategory category;
    char endpoint[64];
    int  curl_code;      /* 0 if N/A */
    int  http_status;    /* 0 if N/A */
    int  attempt;
    int  max_attempts;
    char low_level_message[256];
    char user_message[128];
    char remediation_hint[192];
} TMDBErrorInfo;

/* Lifecycle — call cinebook_curl_init() once at startup,
   cinebook_curl_cleanup() at shutdown */
void cinebook_curl_init(void);
void cinebook_curl_cleanup(void);

/* Last TMDB error snapshot for current process (updated on every TMDB failure) */
const TMDBErrorInfo *tmdb_get_last_error(void);
void tmdb_clear_last_error(void);
const char *tmdb_error_category_name(TMDBErrorCategory category);

/* ─────────────────────────────────────────────────────────────────────────────
 * Report runners
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * run_report — polymorphic dispatch via Report* base pointer.
 * Constructs the correct subclass, calls build() then display(), deletes it.
 *   type  : 0=Occupancy  1=Revenue  2=Booking
 *   scope : 0 = all (only supported value in v1)
 *   days  : look-back window in days
 */
void run_report(int type, int scope, int days);

/* Direct entry points — called by ui_dashboard.c for individual report views */
void run_occupancy_report(int scope, int days);
void run_revenue_report  (int scope, int days);
void run_booking_report  (int scope, int days);

/* ─────────────────────────────────────────────────────────────────────────────
 * TMDB movie import
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * tmdb_search_and_import
 *
 * 1. Searches TMDB /search/movie?query=<query> via TMDBClient::search().
 * 2. Presents up to 10 numbered results for admin selection.
 * 3. Fetches movie_details + credits for the chosen entry.
 * 4. Parses JSON with cJSON, db_insert into movies + cast_members
 *    (top 10 cast by order; is_lead=1 for first 3).
 * 5. Returns new movie_id (>= 1) on success.
 *    Returns -1 on: network error, parse failure, admin cancel,
 *    or db_insert failure.  TMDBException is caught internally — never escapes.
 *
 * Both `query` and `api_key` must be non-NULL, non-empty C strings.
 */
int tmdb_search_and_import(const char *query, const char *api_key);

/*
 * tmdb_bulk_import_now_playing
 *
 * Fetches TMDB /movie/now_playing?region=IN&language=en-IN&page=1.
 * Imports up to 20 movies, skipping any whose tmdb_id already exists in
 * the movies table. For each new movie: fetches /movie/<id> for runtime,
 * fetches /movie/<id>/credits for cast (top 15 by order).
 * Inserts rows into movies + cast_members with WAL protection.
 *
 * out_movie_ids : caller-allocated int array of size >= 20.
 *                 Filled with the new movie_id values for movies inserted
 *                 this call (not skipped ones). May be NULL if caller
 *                 does not need the IDs.
 * Returns: count of newly imported movies (>= 0), or -1 on fatal error.
 * TMDBException is caught internally — never escapes this function.
 */
int tmdb_bulk_import_now_playing(const char *api_key,
                                 int *out_movie_ids,
                                 int  max_out);

/*
 * tmdb_get_streaming_platforms
 *
 * Fetches /movie/{tmdb_id}/watch/providers, parses the IN (India) section,
 * and returns a heap-allocated comma-separated string of platform names.
 * Returns NULL if api_key is empty, network fails, or no IN data found.
 * Caller must free() the returned pointer.
 */
char *tmdb_get_streaming_platforms(int tmdb_id, const char *api_key);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* REPORTS_H */