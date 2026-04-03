/*
 * storage_monitor_demo.c — Demonstration of storage capacity monitoring usage
 * 
 * Shows practical usage of the storage monitoring API for database administrators.
 */

#include <stdio.h>
#include <stdlib.h>
#include "src/engine/storage.h"
#include "src/engine/schema.h"

void print_header(const char *title)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║ %-62s ║\n", title);
    printf("╚════════════════════════════════════════════════════════════════╝\n");
}

void print_storage_dashboard(void)
{
    print_header("CineBook Storage Capacity Dashboard");

    StorageStats *stats = get_storage_stats();
    if (!stats) {
        printf("ERROR: Unable to retrieve storage statistics\n");
        return;
    }

    int capacity_pct = calculate_capacity_percentage();
    
    printf("\n📊 GLOBAL STORAGE METRICS\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    printf("  Capacity Used:        %d%% ", capacity_pct);
    
    /* Visual bar */
    printf("[");
    for (int i = 0; i < 50; i++) {
        if (i < capacity_pct / 2) {
            printf("█");
        } else {
            printf("░");
        }
    }
    printf("]\n");
    
    printf("  Pages Allocated:      %d / %d\n", 
           stats->total_pages_allocated, stats->max_pages_allowed);
    printf("  Total Space:          %.2f MB\n", 
           stats->bytes_total / (1024.0 * 1024.0));
    printf("  Space Used:           %.2f MB\n", 
           stats->bytes_used / (1024.0 * 1024.0));
    printf("  Space Available:      %.2f MB\n", 
           (stats->max_pages_allowed * 4096 - stats->bytes_total) / (1024.0 * 1024.0));
    printf("  Overall Fragmentation: %.2f%%\n", 
           stats->fragmentation_ratio * 100.0f);
    printf("  Active Tables:        %d\n", stats->table_count);
    
    float utilization = stats->bytes_total > 0 ? 
                        (float)stats->bytes_used / (float)stats->bytes_total * 100.0f : 0.0f;
    printf("  Data Utilization:     %.2f%%\n", utilization);

    /* Status indicator */
    printf("\n  Status: ");
    if (capacity_pct < 50) {
        printf("🟢 HEALTHY - Plenty of storage available\n");
    } else if (capacity_pct < 75) {
        printf("🟡 MODERATE - Monitor storage growth\n");
    } else if (capacity_pct < 90) {
        printf("🟠 WARNING - Consider cleanup or expansion\n");
    } else {
        printf("🔴 CRITICAL - Immediate action required\n");
    }

    free(stats);
}

void print_table_summary(void)
{
    print_header("Per-Table Storage Summary");
    
    printf("\n%-20s %8s %10s %10s %12s\n", 
           "TABLE", "PAGES", "ROWS", "SIZE(KB)", "FRAG(%)");
    printf("─────────────────────────────────────────────────────────────────\n");

    for (int i = 0; i < g_schema_count; i++) {
        Schema *schema = &g_schemas[i];
        TableStats stats;
        
        if (get_table_stats(schema->table_name, &stats) == 0 && stats.page_count > 0) {
            printf("%-20s %8d %10d %10.2f %11.2f%%\n",
                   stats.table_name,
                   stats.page_count,
                   stats.row_count,
                   stats.bytes_allocated / 1024.0,
                   stats.fragmentation_ratio * 100.0f);
        }
    }
}

void identify_fragmented_tables(void)
{
    print_header("Fragmentation Analysis");
    
    printf("\n⚠️  Tables with >20%% fragmentation (candidates for optimization):\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    
    int found_fragmented = 0;
    for (int i = 0; i < g_schema_count; i++) {
        Schema *schema = &g_schemas[i];
        float frag = get_table_fragmentation(schema->table_name);
        
        if (frag > 0.20f) {  /* More than 20% fragmentation */
            TableStats stats;
            if (get_table_stats(schema->table_name, &stats) == 0) {
                printf("  • %-20s : %.1f%% fragmented (%d rows, %d pages)\n",
                       schema->table_name,
                       frag * 100.0f,
                       stats.row_count,
                       stats.page_count);
                
                long wasted_bytes = stats.bytes_allocated - stats.bytes_used;
                printf("    └─ Wasted space: %.2f KB\n", wasted_bytes / 1024.0);
                found_fragmented = 1;
            }
        }
    }
    
    if (!found_fragmented) {
        printf("  ✓ No heavily fragmented tables detected\n");
    }
}

void show_largest_tables(void)
{
    print_header("Largest Tables by Storage");
    
    /* Simple array to track top 5 tables */
    typedef struct {
        char name[TABLE_NAME_MAX];
        long bytes;
        int rows;
    } TableSize;
    
    TableSize top_tables[5] = {0};
    
    /* Find top 5 largest tables */
    for (int i = 0; i < g_schema_count; i++) {
        Schema *schema = &g_schemas[i];
        long bytes = get_table_size_bytes(schema->table_name);
        int rows = get_table_row_count(schema->table_name);
        
        if (bytes > 0) {
            /* Insert into sorted top 5 */
            for (int j = 0; j < 5; j++) {
                if (bytes > top_tables[j].bytes) {
                    /* Shift down */
                    for (int k = 4; k > j; k--) {
                        top_tables[k] = top_tables[k-1];
                    }
                    /* Insert */
                    strncpy(top_tables[j].name, schema->table_name, TABLE_NAME_MAX - 1);
                    top_tables[j].name[TABLE_NAME_MAX - 1] = '\0';
                    top_tables[j].bytes = bytes;
                    top_tables[j].rows = rows;
                    break;
                }
            }
        }
    }
    
    printf("\n");
    for (int i = 0; i < 5 && top_tables[i].bytes > 0; i++) {
        printf("  %d. %-20s : %8.2f KB  (%d rows)\n",
               i + 1,
               top_tables[i].name,
               top_tables[i].bytes / 1024.0,
               top_tables[i].rows);
    }
}

int main(void)
{
    /* Initialize */
    storage_init();
    schema_load("data/schema.cat");

    /* Display comprehensive dashboard */
    print_storage_dashboard();
    print_table_summary();
    identify_fragmented_tables();
    show_largest_tables();

    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Storage monitoring complete. Use these metrics to optimize your\n");
    printf("  database performance and plan for capacity expansion.\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("\n");

    /* Cleanup */
    storage_shutdown();
    
    return 0;
}
