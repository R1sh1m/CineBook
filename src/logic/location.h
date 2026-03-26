#ifndef LOCATION_H
#define LOCATION_H

/*
 * location.h — City/country picker and theatre filter for CineBook
 *
 * Provides interactive city selection, city search, theatre lookup by city,
 * and country resolution. No external deps beyond query.h and session.h.
 */

#include "session.h"   /* SessionContext */

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * pick_city — Interactive country → city picker.
 *
 * Flow:
 *   1. Lists all countries from the countries table.
 *   2. User selects a country by number.
 *   3. Lists all cities in that country.
 *   4. User selects a city by number.
 *   5. Updates ctx->preferred_city_id.
 *   6. Persists the choice to users.preferred_city_id via db_update (skipped
 *      when ctx->user_id == 0).
 *   7. Prints a confirmation line.
 */
void pick_city(SessionContext *ctx);

/*
 * search_city — Case-insensitive substring match on city name.
 *
 * Scans all rows in the cities table; tests whether `query` appears anywhere
 * in the city name (both lowercased before comparison).
 *
 * Fills out_ids[0..n-1] with matching city_id values, up to max_results.
 * Returns the total count of matches found (may exceed max_results if
 * truncated). Returns 0 if query is NULL or empty.
 */
int search_city(const char *query, int *out_ids, int max_results);

/*
 * get_theatres_by_city — Retrieve active theatre IDs for a city.
 *
 * Queries theatres WHERE city_id=city_id AND is_active=1.
 * Fills out_theatre_ids[0..*out_count-1] up to max entries.
 * Sets *out_count to the number of results written.
 */
void get_theatres_by_city(int city_id,
                          int *out_theatre_ids,
                          int *out_count,
                          int  max);

/*
 * get_country_for_city — Return the country_id for a given city.
 *
 * Returns country_id on success, -1 if city not found.
 */
int get_country_for_city(int city_id);

#endif /* LOCATION_H */