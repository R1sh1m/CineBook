/*
 * location.c — City/country picker and theatre filter for CineBook
 *
 * Depends on: query.h (→ schema.h → record.h), session.h, location.h
 * Standard libs: stdio.h, string.h, stdlib.h, ctype.h
 *
 * Schema column indices (0-based, per decisions.md):
 *   countries  : [0] country_id  [1] name  [2] currency_sym  [3] currency_code
 *   cities     : [0] city_id     [1] name  [2] country_id
 *   theatres   : [0] theatre_id  [1] name  [2] city_id  [3] address  [4] is_active
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "query.h"
#include "session.h"
#include "location.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────────*/

/* Safe fgets wrapper: reads one line, strips trailing newline. */
static void loc_readline(char *buf, int size)
{
    if (fgets(buf, size, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
}

/* Parse the first token of buf as a positive integer; returns 0 on failure. */
static int loc_parse_int(const char *buf)
{
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || v <= 0 || v > 65535)
        return 0;
    return (int)v;
}

/* In-place ASCII lowercase of str. */
static void loc_strlower(char *str)
{
    for (; *str; str++)
        *str = (char)tolower((unsigned char)*str);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * pick_city
 * ───────────────────────────────────────────────────────────────────────────*/
void pick_city(SessionContext *ctx)
{
    char input[16];

    /* ── Step 1: list all countries ── */
    ResultSet *countries = db_select("countries", NULL, 0, NULL, 0);
    if (!countries || countries->row_count == 0) {
        printf("  [location] No countries found in database.\n");
        result_set_free(countries);
        return;
    }

    printf("\n  ┌─ Select Country ──────────────────────────\n");
    for (int i = 0; i < countries->row_count; i++) {
        /* col[1] = name (CHAR) */
        const char *cname = (const char *)countries->rows[i][1];
        printf("  │  %2d. %s\n", i + 1, cname ? cname : "(unknown)");
    }
    printf("  └───────────────────────────────────────────\n");
    printf("  Country> ");
    loc_readline(input, sizeof(input));

    int country_choice = loc_parse_int(input);
    if (country_choice < 1 || country_choice > countries->row_count) {
        printf("  [location] Invalid selection.\n");
        result_set_free(countries);
        return;
    }

    /* Capture the selected country_id (col[0] = INT) */
    int selected_country_id = *(int *)countries->rows[country_choice - 1][0];
    const char *country_name = (const char *)countries->rows[country_choice - 1][1];
    result_set_free(countries);

    /* ── Step 2: list cities in the chosen country ── */
    int cid_val = selected_country_id;
    WhereClause w_country = {
        .col_name = "country_id",
        .op       = OP_EQ,
        .value    = &cid_val,
        .logic    = 0
    };

    ResultSet *cities = db_select("cities", &w_country, 1, NULL, 0);
    if (!cities || cities->row_count == 0) {
        printf("  [location] No cities found for %s.\n", country_name);
        result_set_free(cities);
        return;
    }

    printf("\n  ┌─ Select City in %s ", country_name);
    /* pad to fixed width */
    int pad = 28 - (int)strlen(country_name);
    for (int p = 0; p < pad; p++) fputs("\xe2\x94\x80", stdout);  // ✅ UTF-8 bytes
    printf("\n");

    for (int i = 0; i < cities->row_count; i++) {
        const char *city_name = (const char *)cities->rows[i][1];
        printf("  │  %2d. %s\n", i + 1, city_name ? city_name : "(unknown)");
    }
    printf("  └───────────────────────────────────────────\n");
    printf("  City> ");
    loc_readline(input, sizeof(input));

    int city_choice = loc_parse_int(input);
    if (city_choice < 1 || city_choice > cities->row_count) {
        printf("  [location] Invalid selection.\n");
        result_set_free(cities);
        return;
    }

    int new_city_id   = *(int *)cities->rows[city_choice - 1][0];
    const char *cname = (const char *)cities->rows[city_choice - 1][1];
    result_set_free(cities);

    /* ── Step 3: update ctx ── */
    ctx->preferred_city_id = new_city_id;

    /* ── Step 4: persist to DB (skip for guests) ── */
    if (ctx->user_id != 0) {
        int uid = ctx->user_id;
        WhereClause w_user = {
            .col_name = "user_id",
            .op       = OP_EQ,
            .value    = &uid,
            .logic    = 0
        };
        int rows = db_update("users", &w_user, 1,
                             "preferred_city_id", &new_city_id);
        if (rows <= 0) {
            printf("  [location] Warning: city preference could not be saved.\n");
        }
    }

    printf("  [location] City set to: %s\n", cname ? cname : "(unknown)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * search_city
 * ───────────────────────────────────────────────────────────────────────────*/
int search_city(const char *query, int *out_ids, int max_results)
{
    if (!query || query[0] == '\0' || !out_ids || max_results <= 0)
        return 0;

    /* Build a lowercase copy of the search query. */
    char q_lower[128];
    strncpy(q_lower, query, sizeof(q_lower) - 1);
    q_lower[sizeof(q_lower) - 1] = '\0';
    loc_strlower(q_lower);

    ResultSet *cities = db_select("cities", NULL, 0, NULL, 0);
    if (!cities)
        return 0;

    int found = 0;
    int written = 0;

    for (int i = 0; i < cities->row_count; i++) {
        const char *city_name = (const char *)cities->rows[i][1];
        if (!city_name)
            continue;

        /* Lowercase copy of city name for comparison. */
        char name_lower[128];
        strncpy(name_lower, city_name, sizeof(name_lower) - 1);
        name_lower[sizeof(name_lower) - 1] = '\0';
        loc_strlower(name_lower);

        if (strstr(name_lower, q_lower) != NULL) {
            found++;
            if (written < max_results) {
                out_ids[written] = *(int *)cities->rows[i][0];
                written++;
            }
        }
    }

    result_set_free(cities);
    return found;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * get_theatres_by_city
 * ───────────────────────────────────────────────────────────────────────────*/
void get_theatres_by_city(int city_id,
                          int *out_theatre_ids,
                          int *out_count,
                          int  max)
{
    if (!out_theatre_ids || !out_count || max <= 0) {
        if (out_count) *out_count = 0;
        return;
    }

    *out_count = 0;

    int cid   = city_id;
    int active = 1;

    WhereClause where[2];
    /* WHERE city_id = ? */
    where[0].op    = OP_EQ;
    where[0].value = &cid;
    where[0].logic = 0;
    strncpy(where[0].col_name, "city_id", sizeof(where[0].col_name) - 1);
    where[0].col_name[sizeof(where[0].col_name) - 1] = '\0';

    /* AND is_active = 1 */
    where[1].op    = OP_EQ;
    where[1].value = &active;
    where[1].logic = 0;
    strncpy(where[1].col_name, "is_active", sizeof(where[1].col_name) - 1);
    where[1].col_name[sizeof(where[1].col_name) - 1] = '\0';

    ResultSet *theatres = db_select("theatres", where, 2, NULL, 0);
    if (!theatres) return;

    int written = 0;
    for (int i = 0; i < theatres->row_count && written < max; i++) {
        out_theatre_ids[written] = *(int *)theatres->rows[i][0];
        written++;
    }

    *out_count = written;
    result_set_free(theatres);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * get_country_for_city
 * ───────────────────────────────────────────────────────────────────────────*/
int get_country_for_city(int city_id)
{
    int cid = city_id;
    WhereClause w = {
        .col_name = "city_id",
        .op       = OP_EQ,
        .value    = &cid,
        .logic    = 0
    };

    ResultSet *rs = db_select("cities", &w, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        result_set_free(rs);
        return -1;
    }

    /* col[2] = country_id */
    int country_id = *(int *)rs->rows[0][2];
    result_set_free(rs);
    return country_id;
}