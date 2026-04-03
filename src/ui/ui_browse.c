/*
 * ui_browse.c — Movie browsing, show listing, and theatre directory for CineBook.
 *
 * Conversation 16 of 22.
 * Depends on: query.h (→ schema.h → record.h), session.h, location.h, reports.h
 * Compiled with: gcc -Wall -Wextra -std=c11 -c ui_browse.c -o ui_browse.o
 *
 * Public surface (called from main.c routing and ui_admin.c):
 *   browse_movies(ctx)
 *   view_movie_detail(movie_id, ctx)
 *   list_theatres_by_city(ctx)
 *
 * run_booking_flow() is forward-declared here; defined in ui_booking.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "query.h"      /* db_select, db_join, ResultSet, WhereClause, result_set_free */
#include "session.h"    /* SessionContext                                               */
#include "location.h"   /* pick_city                                                   */
#include "reports.h"    /* tmdb_get_streaming_platforms                                */
#include "ui_utils.h"   /* smart_clear, draw_separator                                  */

#define MAX_SHOWS_DISPLAY 128
#define BOX_INNER         56

/* g_tmdb_api_key — defined at file scope in main.c, populated from cinebook.conf. */
extern char g_tmdb_api_key[128];

/* ─────────────────────────────────────────────────────────────────────────────
 * Streaming fallback table
 * Used when a movie has no tmdb_id (manually entered) or when the live
 * TMDB watch/providers call fails.  Match is a case-insensitive substring
 * of the movie title.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    const char *title_substr;
    const char *platforms;
} StreamingFallback;

static const StreamingFallback s_streaming_fallback[] = {
    { "kalki",         "Netflix"                 },
    { "stree",         "Amazon Prime Video"      },
    { "pushpa",        "Amazon Prime Video"      },
    { "singham",       "Netflix"                 },
    { "vettaiyan",     "Netflix"                 },
    { "lucky baskhar", "Amazon Prime Video"      },
    { "all we imagine","MUBI, JioCinema"         },
    { "devara",        "Netflix"                 },
};
static const int s_streaming_fallback_count =
    (int)(sizeof(s_streaming_fallback) / sizeof(s_streaming_fallback[0]));

/*
 * fallback_platforms — returns a static platform string if the movie title
 * contains one of the known keywords (case-insensitive), otherwise NULL.
 */
static const char *fallback_platforms(const char *title)
{
    if (!title) return NULL;

    char lower[200] = {0};
    for (int i = 0; title[i] && i < 199; i++)
        lower[i] = (char)tolower((unsigned char)title[i]);

    for (int i = 0; i < s_streaming_fallback_count; i++) {
        if (strstr(lower, s_streaming_fallback[i].title_substr))
            return s_streaming_fallback[i].platforms;
    }
    return NULL;
}

/* ── Forward declarations ────────────────────────────────────────────────── */
void view_movie_detail(int movie_id, SessionContext *ctx);  /* defined below  */
void run_booking_flow (int show_id,  SessionContext *ctx);  /* in ui_booking.c */

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal helpers
 * ═══════════════════════════════════════════════════════════════════════════*/

#define LINE_WIDTH 76

static const char *screen_type_label(int t)
{
    switch (t) {
        case 0:  return "2D";
        case 1:  return "IMAX 2D";
        case 2:  return "IMAX 3D";
        case 3:  return "4DX";
        default: return "Unknown";
    }
}

static void hr(void)
{
    for (int i = 0; i < LINE_WIDTH; i++) putchar('-');
    putchar('\n');
}

static void banner(const char *title)
{
    putchar('\n');
    for (int i = 0; i < LINE_WIDTH; i++) putchar('=');
    printf("\n  %s\n", title);
    for (int i = 0; i < LINE_WIDTH; i++) putchar('=');
    putchar('\n');
}

static const char *safe_str(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return "";
    return (const char *)rows[row][col];
}

static int safe_int(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return 0;
    return *(const int *)rows[row][col];
}

static float safe_float(void ***rows, int row, int col)
{
    if (!rows || !rows[row] || !rows[row][col]) return 0.0f;
    return *(const float *)rows[row][col];
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

static int str_icontains(const char *haystack, const char *needle)
{
    if (!needle || needle[0] == '\0') return 1;
    if (!haystack) return 0;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

static void print_box_line_56(const char *text)
{
    char line[BOX_INNER + 1];
    snprintf(line, sizeof(line), "%-*.*s", BOX_INNER, BOX_INNER, text ? text : "");
    printf("  │%s│\n", line);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * browse_movies
 *
 * Lists all active movies with optional genre substring filter.
 * After listing, user picks a number → view_movie_detail().
 * ═══════════════════════════════════════════════════════════════════════════*/
void browse_movies(SessionContext *ctx)
{
    char genre_filter[128];
    char language_filter[128];
    char input[32];

    smart_clear(UI_CONTEXT_BROWSING);
    printf("\n  \033[1m╔══ MOVIES IN CINEMAS ══════════════════════════════════╗\033[0m\n");

    printf("  Filter by genre  : ");
    read_line(genre_filter, (int)sizeof(genre_filter));
    printf("  Filter by language: ");
    read_line(language_filter, (int)sizeof(language_filter));

    int is_active_val = 1;
    WhereClause wc[1];
    strncpy(wc[0].col_name, "is_active", sizeof(wc[0].col_name) - 1);
    wc[0].col_name[sizeof(wc[0].col_name) - 1] = '\0';
    wc[0].op    = OP_EQ;
    wc[0].value = &is_active_val;
    wc[0].logic = 0;

    ResultSet *rs = db_select("movies", wc, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("\n  No movies found in the database.\n\n");
        result_set_free(rs);
        return;
    }

    char city_display[128] = "All Cities";
    if (ctx->preferred_city_id > 0) {
        int city_id_val = ctx->preferred_city_id;
        WhereClause city_wc[1];
        strncpy(city_wc[0].col_name, "city_id", sizeof(city_wc[0].col_name) - 1);
        city_wc[0].col_name[sizeof(city_wc[0].col_name) - 1] = '\0';
        city_wc[0].op    = OP_EQ;
        city_wc[0].value = &city_id_val;
        city_wc[0].logic = 0;
        ResultSet *city_rs = db_select("cities", city_wc, 1, NULL, 0);
        if (city_rs && city_rs->row_count > 0)
            snprintf(city_display, sizeof(city_display), "%s", safe_str(city_rs->rows, 0, 1));
        result_set_free(city_rs);
    }

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char today[11] = {0};
    if (lt) strftime(today, sizeof(today), "%Y-%m-%d", lt);

    int *display_rows = malloc((size_t)rs->row_count * sizeof(int));
    if (!display_rows) {
        fprintf(stderr, "ui_browse: malloc failed\n");
        result_set_free(rs);
        return;
    }

    int display_count = 0;
    for (int i = 0; i < rs->row_count; i++) {
        const char *genre = safe_str(rs->rows, i, 4);
        const char *lang  = safe_str(rs->rows, i, 5);

        int genre_ok = (genre_filter[0] == '\0') || str_icontains(genre, genre_filter);
        int lang_ok  = (language_filter[0] == '\0') || str_icontains(lang, language_filter);

        if (genre_ok && lang_ok)
            display_rows[display_count++] = i;
    }

    if (display_count == 0) {
        printf("\n  No movies match the selected filters.\n\n");
        free(display_rows);
        result_set_free(rs);
        return;
    }

    const int page_size = 5;
    int current_page = 0;
    int total_pages = (display_count + page_size - 1) / page_size;

    for (;;) {
        int start = current_page * page_size;
        int end   = start + page_size;
        if (end > display_count) end = display_count;

        putchar('\n');
        for (int idx = start; idx < end; idx++) {
            int row_idx  = display_rows[idx];
            int movie_id = safe_int(rs->rows, row_idx, 0);

            const char *title    = safe_str(rs->rows, row_idx, 2);
            const char *synopsis = safe_str(rs->rows, row_idx, 3);
            const char *genre    = safe_str(rs->rows, row_idx, 4);
            int duration_min     = safe_int(rs->rows, row_idx, 6);
            const char *rating   = safe_str(rs->rows, row_idx, 8);

            int has_2d = 0, has_imax = 0, has_4dx = 0, has_other = 0;
            int shows_today_count = 0;

            WhereClause swc[2];
            strncpy(swc[0].col_name, "movie_id", sizeof(swc[0].col_name) - 1);
            swc[0].col_name[sizeof(swc[0].col_name) - 1] = '\0';
            swc[0].op    = OP_EQ;
            swc[0].value = &movie_id;
            swc[0].logic = 0;

            strncpy(swc[1].col_name, "is_active", sizeof(swc[1].col_name) - 1);
            swc[1].col_name[sizeof(swc[1].col_name) - 1] = '\0';
            swc[1].op    = OP_EQ;
            swc[1].value = &is_active_val;
            swc[1].logic = 0;

            ResultSet *show_rs = db_select("shows", swc, 2, NULL, 0);
            if (show_rs && show_rs->row_count > 0) {
                for (int s = 0; s < show_rs->row_count; s++) {
                    int screen_id = safe_int(show_rs->rows, s, 2);
                    const char *show_dt = safe_str(show_rs->rows, s, 3);

                    WhereClause scr_wc[1];
                    strncpy(scr_wc[0].col_name, "screen_id", sizeof(scr_wc[0].col_name) - 1);
                    scr_wc[0].col_name[sizeof(scr_wc[0].col_name) - 1] = '\0';
                    scr_wc[0].op    = OP_EQ;
                    scr_wc[0].value = &screen_id;
                    scr_wc[0].logic = 0;

                    ResultSet *scr_rs = db_select("screens", scr_wc, 1, NULL, 0);
                    if (!scr_rs || scr_rs->row_count == 0) {
                        result_set_free(scr_rs);
                        continue;
                    }

                    int theatre_id = safe_int(scr_rs->rows, 0, 1);
                    int screen_type = safe_int(scr_rs->rows, 0, 3);
                    int screen_active = safe_int(scr_rs->rows, 0, 9);
                    result_set_free(scr_rs);

                    if (!screen_active) continue;

                    WhereClause th_wc[2];
                    strncpy(th_wc[0].col_name, "theatre_id", sizeof(th_wc[0].col_name) - 1);
                    th_wc[0].col_name[sizeof(th_wc[0].col_name) - 1] = '\0';
                    th_wc[0].op    = OP_EQ;
                    th_wc[0].value = &theatre_id;
                    th_wc[0].logic = 0;

                    strncpy(th_wc[1].col_name, "is_active", sizeof(th_wc[1].col_name) - 1);
                    th_wc[1].col_name[sizeof(th_wc[1].col_name) - 1] = '\0';
                    th_wc[1].op    = OP_EQ;
                    th_wc[1].value = &is_active_val;
                    th_wc[1].logic = 0;

                    ResultSet *th_rs = db_select("theatres", th_wc, 2, NULL, 0);
                    if (!th_rs || th_rs->row_count == 0) {
                        result_set_free(th_rs);
                        continue;
                    }

                    int th_city_id = safe_int(th_rs->rows, 0, 2);
                    result_set_free(th_rs);

                    if (ctx->preferred_city_id > 0 && th_city_id != ctx->preferred_city_id)
                        continue;

                    if (screen_type == 0) has_2d = 1;
                    else if (screen_type == 1 || screen_type == 2) has_imax = 1;
                    else if (screen_type == 3) has_4dx = 1;
                    else has_other = 1;

                    if (today[0] && show_dt[0] && strncmp(show_dt, today, 10) == 0)
                        shows_today_count++;
                }
            }
            result_set_free(show_rs);

            char badges_inner[48] = {0};
            int first_badge = 1;
            if (has_2d) {
                snprintf(badges_inner + strlen(badges_inner),
                         sizeof(badges_inner) - strlen(badges_inner),
                         "%s2D", first_badge ? "" : " / ");
                first_badge = 0;
            }
            if (has_imax) {
                snprintf(badges_inner + strlen(badges_inner),
                         sizeof(badges_inner) - strlen(badges_inner),
                         "%sIMAX", first_badge ? "" : " / ");
                first_badge = 0;
            }
            if (has_4dx) {
                snprintf(badges_inner + strlen(badges_inner),
                         sizeof(badges_inner) - strlen(badges_inner),
                         "%s4DX", first_badge ? "" : " / ");
                first_badge = 0;
            }
            if (has_other) {
                snprintf(badges_inner + strlen(badges_inner),
                         sizeof(badges_inner) - strlen(badges_inner),
                         "%sOther", first_badge ? "" : " / ");
            }

            char badges[56] = {0};
            if (badges_inner[0]) snprintf(badges, sizeof(badges), "[%s]", badges_inner);
            else snprintf(badges, sizeof(badges), "[—]");

            char synopsis_short[80] = {0};
            if ((int)strlen(synopsis) > 60)
                snprintf(synopsis_short, sizeof(synopsis_short), "%.60s...", synopsis);
            else
                snprintf(synopsis_short, sizeof(synopsis_short), "%s", synopsis);

            char title_short[40] = {0};
            if ((int)strlen(title) > 28)
                snprintf(title_short, sizeof(title_short), "%.25s...", title);
            else
                snprintf(title_short, sizeof(title_short), "%s", title);

            char genre_short[40] = {0};
            if ((int)strlen(genre) > 30)
                snprintf(genre_short, sizeof(genre_short), "%.27s...", genre);
            else
                snprintf(genre_short, sizeof(genre_short), "%s", genre);

            {
                char line[160];
                int slot = idx - start + 1;

                printf("  ┌");
                for (int k = 0; k < BOX_INNER; k++) printf("─");
                printf("┐\n");

                snprintf(line, sizeof(line), "[%d] %-28.28s %-13.13s [%-4.4s]",
                         slot, title_short, badges, rating[0] ? rating : "NR");
                print_box_line_56(line);

                snprintf(line, sizeof(line), "Genre: %-27.27s   Duration: %d min",
                         genre_short, duration_min);
                print_box_line_56(line);

                snprintf(line, sizeof(line), "Synopsis: %-46.46s", synopsis_short);
                print_box_line_56(line);

                if (shows_today_count > 0)
                    snprintf(line, sizeof(line), "Shows today: %.32s (%d)  Available",
                             city_display, shows_today_count);
                else
                    snprintf(line, sizeof(line), "Shows today: %.32s (%d)  No shows today",
                             city_display, shows_today_count);
                print_box_line_56(line);

                printf("  └");
                for (int k = 0; k < BOX_INNER; k++) printf("─");
                printf("┘\n");
            }
        }

        printf("\n  Page %d of %d — [N]ext  [P]rev  [1-5] Select  [0] Back\n",
               current_page + 1, total_pages);
        printf("  Choice: ");
        read_line(input, (int)sizeof(input));

        if (input[0] == '\0' || input[0] == '0') break;

        if (toupper((unsigned char)input[0]) == 'N') {
            if (current_page < total_pages - 1) current_page++;
            continue;
        }
        if (toupper((unsigned char)input[0]) == 'P') {
            if (current_page > 0) current_page--;
            continue;
        }

        char *endp = NULL;
        long slot_choice = strtol(input, &endp, 10);
        int items_on_page = end - start;

        if (endp == input || slot_choice < 1 || slot_choice > items_on_page || slot_choice > 5) {
            printf("  Invalid input. Use N, P, 1-%d, or 0.\n", items_on_page);
            continue;
        }

        int row_idx  = display_rows[start + (int)slot_choice - 1];
        int movie_id = safe_int(rs->rows, row_idx, 0);

        free(display_rows);
        result_set_free(rs);
        view_movie_detail(movie_id, ctx);
        return;
    }

    free(display_rows);
    result_set_free(rs);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * view_movie_detail
 *
 * Full movie info, cast list, streaming availability, and upcoming shows
 * for the user's preferred city.  User picks a show → run_booking_flow().
 *
 * KEY ORDERING RULE:
 *   All fields needed from mv_rs (including tmdb_id for the streaming
 *   section) are captured into local variables BEFORE result_set_free(mv_rs)
 *   is called.  The streaming block later uses only these local copies.
 * ═══════════════════════════════════════════════════════════════════════════*/
void view_movie_detail(int movie_id, SessionContext *ctx)
{
    char input[32];

    /* ── Fetch movie row ─────────────────────────────────────────────────── */
    WhereClause mwc[1];
    strncpy(mwc[0].col_name, "movie_id", sizeof(mwc[0].col_name) - 1);
    mwc[0].col_name[sizeof(mwc[0].col_name) - 1] = '\0';
    mwc[0].op    = OP_EQ;
    mwc[0].value = &movie_id;
    mwc[0].logic = 0;

    ResultSet *mv_rs = db_select("movies", mwc, 1, NULL, 0);
    if (!mv_rs || mv_rs->row_count == 0) {
        printf("\n  Movie not found (id=%d).\n\n", movie_id);
        result_set_free(mv_rs);
        return;
    }

    /*
     * Capture ALL fields we need from mv_rs into locals right here.
     * This includes tmdb_id for the streaming section further below.
     * safe_str() returns a pointer into mv_rs memory — we copy the title
     * into a local buffer so it remains valid after result_set_free(mv_rs).
     *
     * movies schema:
     *   0=movie_id  1=tmdb_id  2=title  3=synopsis  4=genre
     *   5=language  6=duration_min  7=release_date  8=rating  9=is_active
     */
    int  tmdb_id     = safe_int(mv_rs->rows, 0, 1);   /* 0 if NULL */

    char title_local[200]  = {0};
    char synop_local[500]  = {0};
    char genre_local[100]  = {0};
    char lang_local[50]    = {0};
    char reldate_local[20] = {0};
    char rating_local[10]  = {0};
    int  duration_min      = safe_int(mv_rs->rows, 0, 6);

    strncpy(title_local,   safe_str(mv_rs->rows, 0, 2), sizeof(title_local)   - 1);
    strncpy(synop_local,   safe_str(mv_rs->rows, 0, 3), sizeof(synop_local)   - 1);
    strncpy(genre_local,   safe_str(mv_rs->rows, 0, 4), sizeof(genre_local)   - 1);
    strncpy(lang_local,    safe_str(mv_rs->rows, 0, 5), sizeof(lang_local)    - 1);
    strncpy(reldate_local, safe_str(mv_rs->rows, 0, 7), sizeof(reldate_local) - 1);
    strncpy(rating_local,  safe_str(mv_rs->rows, 0, 8), sizeof(rating_local)  - 1);

    /* mv_rs is no longer needed — free it now. */
    result_set_free(mv_rs);
    mv_rs = NULL;

    char city_display[128] = "your city";
    if (ctx->preferred_city_id > 0) {
        int city_id_val = ctx->preferred_city_id;
        WhereClause city_wc[1];
        strncpy(city_wc[0].col_name, "city_id", sizeof(city_wc[0].col_name) - 1);
        city_wc[0].col_name[sizeof(city_wc[0].col_name) - 1] = '\0';
        city_wc[0].op    = OP_EQ;
        city_wc[0].value = &city_id_val;
        city_wc[0].logic = 0;
        ResultSet *city_rs = db_select("cities", city_wc, 1, NULL, 0);
        if (city_rs && city_rs->row_count > 0)
            snprintf(city_display, sizeof(city_display), "%s", safe_str(city_rs->rows, 0, 1));
        result_set_free(city_rs);
    }

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char today[11] = {0};
    if (lt) strftime(today, sizeof(today), "%Y-%m-%d", lt);

    int shows_today_in_city = 0;
    {
        int is_active_val = 1;
        WhereClause swc_today[2];
        strncpy(swc_today[0].col_name, "movie_id", sizeof(swc_today[0].col_name) - 1);
        swc_today[0].col_name[sizeof(swc_today[0].col_name) - 1] = '\0';
        swc_today[0].op    = OP_EQ;
        swc_today[0].value = &movie_id;
        swc_today[0].logic = 0;

        strncpy(swc_today[1].col_name, "is_active", sizeof(swc_today[1].col_name) - 1);
        swc_today[1].col_name[sizeof(swc_today[1].col_name) - 1] = '\0';
        swc_today[1].op    = OP_EQ;
        swc_today[1].value = &is_active_val;
        swc_today[1].logic = 0;

        ResultSet *today_rs = db_select("shows", swc_today, 2, NULL, 0);
        if (today_rs && today_rs->row_count > 0) {
            for (int i = 0; i < today_rs->row_count; i++) {
                int screen_id = safe_int(today_rs->rows, i, 2);
                const char *show_dt = safe_str(today_rs->rows, i, 3);

                if (!(today[0] && show_dt[0] && strncmp(show_dt, today, 10) == 0))
                    continue;

                WhereClause scr_wc[1];
                strncpy(scr_wc[0].col_name, "screen_id", sizeof(scr_wc[0].col_name) - 1);
                scr_wc[0].col_name[sizeof(scr_wc[0].col_name) - 1] = '\0';
                scr_wc[0].op    = OP_EQ;
                scr_wc[0].value = &screen_id;
                scr_wc[0].logic = 0;

                ResultSet *scr_rs = db_select("screens", scr_wc, 1, NULL, 0);
                if (!scr_rs || scr_rs->row_count == 0) {
                    result_set_free(scr_rs);
                    continue;
                }

                int theatre_id = safe_int(scr_rs->rows, 0, 1);
                int screen_active = safe_int(scr_rs->rows, 0, 9);
                result_set_free(scr_rs);
                if (!screen_active) continue;

                WhereClause th_wc[2];
                strncpy(th_wc[0].col_name, "theatre_id", sizeof(th_wc[0].col_name) - 1);
                th_wc[0].col_name[sizeof(th_wc[0].col_name) - 1] = '\0';
                th_wc[0].op    = OP_EQ;
                th_wc[0].value = &theatre_id;
                th_wc[0].logic = 0;

                strncpy(th_wc[1].col_name, "is_active", sizeof(th_wc[1].col_name) - 1);
                th_wc[1].col_name[sizeof(th_wc[1].col_name) - 1] = '\0';
                th_wc[1].op    = OP_EQ;
                th_wc[1].value = &is_active_val;
                th_wc[1].logic = 0;

                ResultSet *th_rs = db_select("theatres", th_wc, 2, NULL, 0);
                if (!th_rs || th_rs->row_count == 0) {
                    result_set_free(th_rs);
                    continue;
                }

                int th_city_id = safe_int(th_rs->rows, 0, 2);
                result_set_free(th_rs);

                if (ctx->preferred_city_id > 0 && th_city_id != ctx->preferred_city_id)
                    continue;

                shows_today_in_city++;
            }
        }
        result_set_free(today_rs);
    }

    /* ── Header + movie info block ───────────────────────────────────────── */
    banner(title_local);
    printf("  \033[36m%d show(s) available today in %s\033[0m\n",
           shows_today_in_city, city_display);

    printf("  Genre      : %s\n", genre_local[0]   ? genre_local   : "—");
    printf("  Language   : %s\n", lang_local[0]    ? lang_local    : "—");
    printf("  Duration   : %d min\n", duration_min);
    printf("  Release    : %s\n", reldate_local[0] ? reldate_local : "—");
    printf("  Certificate: %s\n", rating_local[0]  ? rating_local  : "—");
    putchar('\n');

    if (synop_local[0]) {
        printf("  Synopsis:\n");
        /*
         * Word-wrap synopsis at ~70 chars per line.
         */
        const char *p   = synop_local;
        int         col = 0;
        printf("    ");
        while (*p) {
            const char *word_start = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            int word_len = (int)(p - word_start);

            if (col > 0 && col + 1 + word_len > 70) {
                printf("\n    ");
                col = 0;
            } else if (col > 0) {
                putchar(' ');
                col++;
            }

            fwrite(word_start, 1, (size_t)word_len, stdout);
            col += word_len;

            if (*p == '\n') { printf("\n    "); col = 0; }
            if (*p) p++;
        }
        printf("\n\n");
    }

    /* ── Cast ─────────────────────────────────────────────────────────────── */
    WhereClause cwc[1];
    strncpy(cwc[0].col_name, "movie_id", sizeof(cwc[0].col_name) - 1);
    cwc[0].col_name[sizeof(cwc[0].col_name) - 1] = '\0';
    cwc[0].op    = OP_EQ;
    cwc[0].value = &movie_id;
    cwc[0].logic = 0;

    ResultSet *cast_rs = db_select("cast_members", cwc, 1, NULL, 0);

    if (cast_rs && cast_rs->row_count > 0) {
        printf("  Cast\n");
        hr();
        /*
         * cast_members schema: 0=cast_id  1=movie_id  2=person_name
         *                      3=role_name  4=is_lead
         */
        /* Lead cast first */
        for (int i = 0; i < cast_rs->row_count; i++) {
            if (!safe_int(cast_rs->rows, i, 4)) continue;
            printf("  %-28s  %-28s  [LEAD]\n",
                   safe_str(cast_rs->rows, i, 2),
                   safe_str(cast_rs->rows, i, 3));
        }
        /* Supporting cast */
        for (int i = 0; i < cast_rs->row_count; i++) {
            if (safe_int(cast_rs->rows, i, 4)) continue;
            printf("  %-28s  %-28s\n",
                   safe_str(cast_rs->rows, i, 2),
                   safe_str(cast_rs->rows, i, 3));
        }
        hr();
    } else {
        printf("  No cast information available.\n");
        hr();
    }
    result_set_free(cast_rs);

    /* ── Streaming / Watch Online ───────────────────────────────────────── */
    printf("\n  \033[1mWatch Online\033[0m\n");
    hr();

    char *live_platforms = NULL;

    if (tmdb_id > 0 && g_tmdb_api_key[0] != '\0')
        live_platforms = tmdb_get_streaming_platforms(tmdb_id, g_tmdb_api_key);

    if (live_platforms) {
        printf("  Streaming on: \033[32m%s\033[0m\n", live_platforms);
        free(live_platforms);
        live_platforms = NULL;
    } else {
        const char *fb = fallback_platforms(title_local);
        if (fb)
            printf("  Streaming on: \033[33m%s\033[0m  (info may not be current)\n", fb);
        else
            printf("  No streaming info available.\n");
    }

    for (;;) {
        printf("\n  Upcoming Shows");
        if (ctx->preferred_city_id > 0) {
            int city_id_val = ctx->preferred_city_id;
            WhereClause city_wc[1];
            strncpy(city_wc[0].col_name, "city_id", sizeof(city_wc[0].col_name) - 1);
            city_wc[0].col_name[sizeof(city_wc[0].col_name) - 1] = '\0';
            city_wc[0].op    = OP_EQ;
            city_wc[0].value = &city_id_val;
            city_wc[0].logic = 0;
            ResultSet *city_rs = db_select("cities", city_wc, 1, NULL, 0);
            if (city_rs && city_rs->row_count > 0) {
                snprintf(city_display, sizeof(city_display), "%s", safe_str(city_rs->rows, 0, 1));
                printf(" — %s", city_display);
            }
            result_set_free(city_rs);
        }
        putchar('\n');
        hr();

        int is_active_val = 1;
        WhereClause swc[2];
        strncpy(swc[0].col_name, "movie_id",  sizeof(swc[0].col_name) - 1);
        swc[0].col_name[sizeof(swc[0].col_name) - 1] = '\0';
        swc[0].op    = OP_EQ;
        swc[0].value = &movie_id;
        swc[0].logic = 0;

        strncpy(swc[1].col_name, "is_active", sizeof(swc[1].col_name) - 1);
        swc[1].col_name[sizeof(swc[1].col_name) - 1] = '\0';
        swc[1].op    = OP_EQ;
        swc[1].value = &is_active_val;
        swc[1].logic = 0;

        ResultSet *show_rs = db_select("shows", swc, 2, NULL, 0);

        int   show_ids[MAX_SHOWS_DISPLAY];
        char  show_datetimes[MAX_SHOWS_DISPLAY][20];
        char  screen_names[MAX_SHOWS_DISPLAY][51];
        int   screen_types[MAX_SHOWS_DISPLAY];
        char  theatre_names[MAX_SHOWS_DISPLAY][151];
        float base_prices[MAX_SHOWS_DISPLAY];
        int   valid_count = 0;
        char  now_key[20];
        current_datetime_key(now_key, sizeof(now_key));

        if (show_rs && show_rs->row_count > 0) {
            for (int i = 0; i < show_rs->row_count && valid_count < MAX_SHOWS_DISPLAY; i++) {
                int         show_id_val = safe_int  (show_rs->rows, i, 0);
                int         screen_id   = safe_int  (show_rs->rows, i, 2);
                const char *sdt         = safe_str  (show_rs->rows, i, 3);
                float       base_price  = safe_float(show_rs->rows, i, 4);

                strncpy(show_datetimes[valid_count], sdt, sizeof(show_datetimes[0]) - 1);
                show_datetimes[valid_count][sizeof(show_datetimes[0]) - 1] = '\0';
                if (show_datetimes[valid_count][0] == '\0' ||
                    strcmp(show_datetimes[valid_count], now_key) < 0)
                    continue;

                WhereClause scr_wc[1];
                strncpy(scr_wc[0].col_name, "screen_id", sizeof(scr_wc[0].col_name) - 1);
                scr_wc[0].col_name[sizeof(scr_wc[0].col_name) - 1] = '\0';
                scr_wc[0].op    = OP_EQ;
                scr_wc[0].value = &screen_id;
                scr_wc[0].logic = 0;

                ResultSet *scr_rs = db_select("screens", scr_wc, 1, NULL, 0);
                if (!scr_rs || scr_rs->row_count == 0) {
                    result_set_free(scr_rs);
                    continue;
                }

                int         scr_active  = safe_int(scr_rs->rows, 0, 9);
                int         theatre_id  = safe_int(scr_rs->rows, 0, 1);
                const char *sname       = safe_str(scr_rs->rows, 0, 2);
                int         stype       = safe_int(scr_rs->rows, 0, 3);
                result_set_free(scr_rs);

                if (!scr_active) continue;

                WhereClause th_wc[2];
                strncpy(th_wc[0].col_name, "theatre_id", sizeof(th_wc[0].col_name) - 1);
                th_wc[0].col_name[sizeof(th_wc[0].col_name) - 1] = '\0';
                th_wc[0].op    = OP_EQ;
                th_wc[0].value = &theatre_id;
                th_wc[0].logic = 0;

                strncpy(th_wc[1].col_name, "is_active", sizeof(th_wc[1].col_name) - 1);
                th_wc[1].col_name[sizeof(th_wc[1].col_name) - 1] = '\0';
                th_wc[1].op    = OP_EQ;
                th_wc[1].value = &is_active_val;
                th_wc[1].logic = 0;

                ResultSet *th_rs = db_select("theatres", th_wc, 2, NULL, 0);
                if (!th_rs || th_rs->row_count == 0) {
                    result_set_free(th_rs);
                    continue;
                }

                int         th_city_id = safe_int(th_rs->rows, 0, 2);
                const char *th_name    = safe_str(th_rs->rows, 0, 1);

                if (ctx->preferred_city_id > 0 && th_city_id != ctx->preferred_city_id) {
                    result_set_free(th_rs);
                    continue;
                }

                show_ids[valid_count] = show_id_val;
                strncpy(screen_names[valid_count],   sname,  sizeof(screen_names[0])    - 1);
                screen_names[valid_count][sizeof(screen_names[0]) - 1] = '\0';
                screen_types[valid_count] = stype;
                strncpy(theatre_names[valid_count],  th_name, sizeof(theatre_names[0]) - 1);
                theatre_names[valid_count][sizeof(theatre_names[0]) - 1] = '\0';
                base_prices[valid_count] = base_price;
                valid_count++;

                result_set_free(th_rs);
            }
        }
        result_set_free(show_rs);

        if (valid_count == 0) {
            if (ctx->preferred_city_id > 0) {
                printf("  No shows in %s.\n", city_display);
                printf("  [C] Change city   [0] Back   Choice: ");
                read_line(input, (int)sizeof(input));

                if (toupper((unsigned char)input[0]) == 'C') {
                    pick_city(ctx);
                    continue;
                }
                return;
            }

            printf("  No upcoming shows available.\n");
            hr();
            printf("\n  Press Enter to go back...");
            read_line(input, (int)sizeof(input));
            return;
        }

        printf("  %-3s %-17s %-12s %-8s %-28s %-10s\n",
               "#", "Date & Time", "Screen", "Type", "Theatre", "Price");
        hr();

        for (int d = 0; d < valid_count; d++) {
            char price_txt[16];
            snprintf(price_txt, sizeof(price_txt), "%s%.2f",
                     ctx->currency_sym, base_prices[d]);

            printf("  %-3d %-17.17s %-12.12s %-8.8s %-28.28s %-10.10s\n",
                   d + 1,
                   show_datetimes[d],
                   screen_names[d],
                   screen_type_label(screen_types[d]),
                   theatre_names[d],
                   price_txt);
        }

        hr();
        printf("  %d show(s) available.\n\n", valid_count);

        for (;;) {
            printf("  Enter show number to book (0 to go back): ");
            read_line(input, (int)sizeof(input));

            if (input[0] == '\0' || input[0] == '0') return;

            char *endp = NULL;
            long choice = strtol(input, &endp, 10);

            if (endp == input || choice < 1 || choice > valid_count) {
                printf("  Invalid selection. Enter 1-%d or 0 to go back.\n", valid_count);
                continue;
            }

            run_booking_flow(show_ids[choice - 1], ctx);
            return;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * list_theatres_by_city
 *
 * Shows all active theatres in ctx->preferred_city_id with their screens.
 * ═══════════════════════════════════════════════════════════════════════════*/
void list_theatres_by_city(SessionContext *ctx)
{
    char input[32];

    if (ctx->preferred_city_id <= 0) {
        printf("\n  No preferred city set. Would you like to pick one now? (y/n): ");
        read_line(input, (int)sizeof(input));
        if (input[0] == 'y' || input[0] == 'Y') {
            pick_city(ctx);
            if (ctx->preferred_city_id <= 0) {
                printf("  No city selected. Returning.\n\n");
                return;
            }
        } else {
            return;
        }
    }

    /* Resolve city name */
    char city_display[128] = "Selected City";
    {
        int city_id_val = ctx->preferred_city_id;
        WhereClause city_wc[1];
        strncpy(city_wc[0].col_name, "city_id", sizeof(city_wc[0].col_name) - 1);
        city_wc[0].col_name[sizeof(city_wc[0].col_name) - 1] = '\0';
        city_wc[0].op    = OP_EQ;
        city_wc[0].value = &city_id_val;
        city_wc[0].logic = 0;
        ResultSet *city_rs = db_select("cities", city_wc, 1, NULL, 0);
        if (city_rs && city_rs->row_count > 0)
            snprintf(city_display, sizeof(city_display), "%s",
                     safe_str(city_rs->rows, 0, 1));
        result_set_free(city_rs);
    }

    char title_buf[200] = {0};
    snprintf(title_buf, sizeof(title_buf), "THEATRES \xe2\x80\x94 %.80s", city_display);
    banner(title_buf);

    int city_id_val   = ctx->preferred_city_id;
    int is_active_val = 1;

    WhereClause th_wc[2];
    strncpy(th_wc[0].col_name, "city_id",   sizeof(th_wc[0].col_name) - 1);
    th_wc[0].col_name[sizeof(th_wc[0].col_name) - 1] = '\0';
    th_wc[0].op    = OP_EQ;
    th_wc[0].value = &city_id_val;
    th_wc[0].logic = 0;

    strncpy(th_wc[1].col_name, "is_active", sizeof(th_wc[1].col_name) - 1);
    th_wc[1].col_name[sizeof(th_wc[1].col_name) - 1] = '\0';
    th_wc[1].op    = OP_EQ;
    th_wc[1].value = &is_active_val;
    th_wc[1].logic = 0;

    ResultSet *th_rs = db_select("theatres", th_wc, 2, NULL, 0);
    if (!th_rs || th_rs->row_count == 0) {
        printf("  No active theatres found in %s.\n\n", city_display);
        result_set_free(th_rs);
        return;
    }

    /*
     * theatres schema: 0=theatre_id  1=name  2=city_id  3=address  4=is_active
     */
    int th_count = th_rs->row_count;
    for (int t = 0; t < th_count; t++) {
        int         th_id   = safe_int(th_rs->rows, t, 0);
        const char *th_name = safe_str(th_rs->rows, t, 1);
        const char *address = safe_str(th_rs->rows, t, 3);

        printf("\n  %d. %s\n", t + 1, th_name);
        if (address[0])
            printf("     %s\n", address);

        WhereClause scr_wc[2];
        strncpy(scr_wc[0].col_name, "theatre_id", sizeof(scr_wc[0].col_name) - 1);
        scr_wc[0].col_name[sizeof(scr_wc[0].col_name) - 1] = '\0';
        scr_wc[0].op    = OP_EQ;
        scr_wc[0].value = &th_id;
        scr_wc[0].logic = 0;

        strncpy(scr_wc[1].col_name, "is_active", sizeof(scr_wc[1].col_name) - 1);
        scr_wc[1].col_name[sizeof(scr_wc[1].col_name) - 1] = '\0';
        scr_wc[1].op    = OP_EQ;
        scr_wc[1].value = &is_active_val;
        scr_wc[1].logic = 0;

        ResultSet *scr_rs = db_select("screens", scr_wc, 2, NULL, 0);

        if (!scr_rs || scr_rs->row_count == 0) {
            printf("     No screens.\n");
        } else {
            printf("     %-3s  %-20s  %-10s  %s\n",
                   "", "Screen Name", "Type", "Total Seats");
            printf("     ");
            for (int i = 0; i < 50; i++) putchar('-');
            putchar('\n');
            /*
             * screens schema: 0=screen_id  1=theatre_id  2=screen_name
             *                 3=screen_type  4=total_seats  ...  9=is_active
             */
            for (int s = 0; s < scr_rs->row_count; s++) {
                printf("     %-3d  %-20s  %-10s  %d seats\n",
                       s + 1,
                       safe_str(scr_rs->rows, s, 2),
                       screen_type_label(safe_int(scr_rs->rows, s, 3)),
                       safe_int(scr_rs->rows, s, 4));
            }
        }
        result_set_free(scr_rs);

        if (t < th_count - 1) {
            printf("  ");
            hr();
        }
    }

    result_set_free(th_rs);

    hr();
    printf("  %d theatre(s) in %s.\n\n", th_count, city_display);
    printf("  Press Enter to continue...");
    read_line(input, (int)sizeof(input));
}