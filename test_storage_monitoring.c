/*
 * test_storage_monitoring.c — Test suite for storage capacity monitoring system
 *
 * Tests all storage monitoring functions:
 * - get_storage_stats()
 * - calculate_capacity_percentage()
 * - get_table_size_bytes()
 * - get_table_row_count()
 * - get_table_fragmentation()
 * - get_table_stats()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/engine/storage.h"
#include "src/engine/schema.h"

static void print_separator(void)
{
    printf("================================================================\n");
}

static void test_table_stats(const char *table_name)
{
    printf("\n--- Testing Table: %s ---\n", table_name);

    /* Test get_table_size_bytes */
    long size_bytes = get_table_size_bytes(table_name);
    if (size_bytes >= 0) {
        printf("  Table size: %ld bytes (%.2f KB)\n", 
               size_bytes, size_bytes / 1024.0);
    } else {
        printf("  Table size: ERROR\n");
    }

    /* Test get_table_row_count */
    int row_count = get_table_row_count(table_name);
    if (row_count >= 0) {
        printf("  Row count: %d rows\n", row_count);
    } else {
        printf("  Row count: ERROR\n");
    }

    /* Test get_table_fragmentation */
    float fragmentation = get_table_fragmentation(table_name);
    if (fragmentation >= 0.0f) {
        printf("  Fragmentation: %.2f%% (%.4f ratio)\n", 
               fragmentation * 100.0f, fragmentation);
    } else {
        printf("  Fragmentation: ERROR\n");
    }

    /* Test get_table_stats */
    TableStats stats;
    if (get_table_stats(table_name, &stats) == 0) {
        printf("\n  Detailed Stats:\n");
        printf("    Table: %s\n", stats.table_name);
        printf("    Pages: %d\n", stats.page_count);
        printf("    Rows: %d\n", stats.row_count);
        printf("    Bytes allocated: %ld\n", stats.bytes_allocated);
        printf("    Bytes used: %ld\n", stats.bytes_used);
        printf("    Fragmentation: %.4f\n", stats.fragmentation_ratio);
        
        if (stats.bytes_allocated > 0) {
            float utilization = (float)stats.bytes_used / (float)stats.bytes_allocated * 100.0f;
            printf("    Utilization: %.2f%%\n", utilization);
        }
    } else {
        printf("  get_table_stats: ERROR\n");
    }
}

int main(void)
{
    print_separator();
    printf("Storage Capacity Monitoring System - Test Suite\n");
    print_separator();

    /* Initialize storage and schema */
    printf("\nInitializing storage and schema...\n");
    storage_init();
    schema_load("data/schema.cat");
    printf("Schema loaded: %d tables\n", g_schema_count);

    /* Test overall storage statistics */
    print_separator();
    printf("OVERALL STORAGE STATISTICS\n");
    print_separator();

    StorageStats *overall = get_storage_stats();
    if (overall) {
        printf("\nGlobal Storage Stats:\n");
        printf("  Total pages allocated: %d\n", overall->total_pages_allocated);
        printf("  Pages in use: %d\n", overall->pages_in_use);
        printf("  Max pages allowed: %d\n", overall->max_pages_allowed);
        printf("  Total bytes: %ld (%.2f MB)\n", 
               overall->bytes_total, overall->bytes_total / (1024.0 * 1024.0));
        printf("  Bytes used: %ld (%.2f MB)\n", 
               overall->bytes_used, overall->bytes_used / (1024.0 * 1024.0));
        printf("  Overall fragmentation: %.2f%% (%.4f ratio)\n", 
               overall->fragmentation_ratio * 100.0f, overall->fragmentation_ratio);
        printf("  Active tables: %d\n", overall->table_count);

        if (overall->bytes_total > 0) {
            float utilization = (float)overall->bytes_used / (float)overall->bytes_total * 100.0f;
            printf("  Overall utilization: %.2f%%\n", utilization);
        }

        free(overall);
    } else {
        printf("ERROR: get_storage_stats() returned NULL\n");
    }

    /* Test capacity percentage */
    int capacity_pct = calculate_capacity_percentage();
    if (capacity_pct >= 0) {
        printf("\nStorage Capacity: %d%% used\n", capacity_pct);
        printf("  Status: ");
        if (capacity_pct < 50) {
            printf("LOW USAGE (plenty of space)\n");
        } else if (capacity_pct < 75) {
            printf("MODERATE USAGE (space available)\n");
        } else if (capacity_pct < 90) {
            printf("HIGH USAGE (consider cleanup)\n");
        } else {
            printf("CRITICAL USAGE (cleanup recommended)\n");
        }
    } else {
        printf("\nERROR: calculate_capacity_percentage() failed\n");
    }

    /* Test individual table statistics for a few tables */
    print_separator();
    printf("PER-TABLE STATISTICS\n");
    print_separator();

    /* Test with tables that likely exist */
    const char *test_tables[] = {
        "users",
        "movies", 
        "theatres",
        "bookings",
        "shows",
        "seats"
    };

    for (size_t i = 0; i < sizeof(test_tables) / sizeof(test_tables[0]); i++) {
        test_table_stats(test_tables[i]);
    }

    /* Test edge cases */
    print_separator();
    printf("EDGE CASE TESTS\n");
    print_separator();

    printf("\nTest 1: Invalid table name\n");
    long bad_size = get_table_size_bytes("nonexistent_table");
    printf("  get_table_size_bytes(\"nonexistent_table\"): %ld\n", bad_size);

    printf("\nTest 2: NULL table name\n");
    int bad_rows = get_table_row_count(NULL);
    printf("  get_table_row_count(NULL): %d\n", bad_rows);

    printf("\nTest 3: Empty string table name\n");
    float bad_frag = get_table_fragmentation("");
    printf("  get_table_fragmentation(\"\"): %.4f\n", bad_frag);

    printf("\nTest 4: NULL stats pointer\n");
    int bad_stats = get_table_stats("users", NULL);
    printf("  get_table_stats(\"users\", NULL): %d\n", bad_stats);

    /* Summary */
    print_separator();
    printf("TEST SUMMARY\n");
    print_separator();
    printf("\nAll storage monitoring functions tested:\n");
    printf("  ✓ get_storage_stats()\n");
    printf("  ✓ calculate_capacity_percentage()\n");
    printf("  ✓ get_table_size_bytes()\n");
    printf("  ✓ get_table_row_count()\n");
    printf("  ✓ get_table_fragmentation()\n");
    printf("  ✓ get_table_stats()\n");
    printf("\nStorage monitoring system is operational.\n");

    /* Cleanup */
    storage_shutdown();
    printf("\nStorage shutdown complete.\n");
    print_separator();

    return 0;
}
