#ifndef COMPACT_H
#define COMPACT_H

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char table_name[TABLE_NAME_MAX];
    int rows_before;
    int rows_after;
    int pages_before;
    int pages_after;
    float fragmentation_before;
    float fragmentation_after;
    int success;
} CompactResult;

/* Compact one table by rewriting records into a fresh file image.
 * Returns 0 on success, -1 on error. */
int compact_table(const char *table_name, CompactResult *out_result);

/* Compact all schema tables; returns number of successfully compacted tables. */
int compact_all_tables(CompactResult *results, int max_results);

/* Print a compact summary row in admin-friendly format. */
void print_compact_result(const CompactResult *r);

#ifdef __cplusplus
}
#endif

#endif /* COMPACT_H */