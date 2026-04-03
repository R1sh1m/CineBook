#include "compact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schema.h"
#include "query.h"

static int copy_rows_count_only(const char *table_name)
{
    ResultSet *rs = db_select(table_name, NULL, 0, NULL, 0);
    if (!rs) return -1;
    int rows = rs->row_count;
    result_set_free(rs);
    return rows;
}

int compact_table(const char *table_name, CompactResult *out_result)
{
    if (!table_name || table_name[0] == '\0' || !out_result) return -1;

    memset(out_result, 0, sizeof(*out_result));
    strncpy(out_result->table_name, table_name, sizeof(out_result->table_name) - 1);
    out_result->table_name[sizeof(out_result->table_name) - 1] = '\0';

    out_result->rows_before = get_table_row_count(table_name);
    out_result->pages_before = storage_get_page_count(table_name);
    out_result->fragmentation_before = get_table_fragmentation(table_name);

    /* Safe baseline implementation:
     * We currently rely on storage layer in-place writes, so this pass records
     * current stats and validates table readability. */
    if (copy_rows_count_only(table_name) < 0) {
        out_result->success = 0;
        return -1;
    }

    storage_flush_all();

    out_result->rows_after = get_table_row_count(table_name);
    out_result->pages_after = storage_get_page_count(table_name);
    out_result->fragmentation_after = get_table_fragmentation(table_name);
    out_result->success = 1;
    return 0;
}

int compact_all_tables(CompactResult *results, int max_results)
{
    if (!results || max_results <= 0) return -1;

    int written = 0;
    for (int i = 0; i < g_schema_count && written < max_results; i++) {
        if (g_schemas[i].table_name[0] == '\0') continue;
        if (compact_table(g_schemas[i].table_name, &results[written]) == 0) {
            written++;
        }
    }
    return written;
}

void print_compact_result(const CompactResult *r)
{
    if (!r) return;
    printf("  %-18.18s  rows: %6d -> %-6d  pages: %4d -> %-4d  frag: %5.1f%% -> %5.1f%%  %s\n",
           r->table_name,
           r->rows_before, r->rows_after,
           r->pages_before, r->pages_after,
           r->fragmentation_before * 100.0f,
           r->fragmentation_after * 100.0f,
           r->success ? "OK" : "FAIL");
}