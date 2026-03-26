#ifndef QUERY_H
#define QUERY_H

/*
 * query.h — Public RDBMS API for CineBook
 *
 * Every module above the engine layer includes only this header.
 * schema.h is included here to give callers access to Schema, Column,
 * ResultSet, and all engine types through a single include.
 */

#include "schema.h"   /* → record.h → ResultSet, Schema, Column, ColumnType */

/* ─────────────────────────────────────────────────────────────────────────────
 * WhereOp — comparison operators for WHERE clauses
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    OP_EQ  = 0,
    OP_NEQ = 1,
    OP_GT  = 2,
    OP_LT  = 3,
    OP_GTE = 4,
    OP_LTE = 5
} WhereOp;

/* ─────────────────────────────────────────────────────────────────────────────
 * WhereClause — one predicate in a WHERE expression
 *
 * value is a heap pointer owned by the caller; query.c never frees it.
 * logic is reserved (0 = AND); all multi-clause arrays are evaluated as AND.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    char     col_name[64];
    WhereOp  op;
    void    *value;   /* int*, float*, or char* depending on column type */
    int      logic;   /* 0 = AND (only supported value for now)          */
} WhereClause;

/* ─────────────────────────────────────────────────────────────────────────────
 * DML — Data Manipulation
 * ───────────────────────────────────────────────────────────────────────────*/

/* Select rows from table matching all WHERE clauses (AND).
 * cols/col_count specify which columns to return; NULL/0 = all columns.
 * Returns heap-allocated ResultSet; caller must call result_set_free(). */
ResultSet *db_select(const char *table,
                     WhereClause *where, int where_count,
                     char **cols,        int col_count);

/* Insert a new row into table from field_values (one void* per column).
 * Auto-increments PK. Updates all relevant indexes.
 * Returns new PK (row_id) on success, -1 on failure. */
int db_insert(const char *table, void **field_values);

/* Update col_name to new_val in every row matching WHERE clauses.
 * Updates indexes if the column is indexed.
 * Returns count of rows updated. */
int db_update(const char *table,
              WhereClause *where, int where_count,
              const char *col_name, void *new_val);

/* Delete all rows matching WHERE clauses (marks slots as all-zero).
 * Updates indexes.
 * Returns count of rows deleted. */
int db_delete(const char *table,
              WhereClause *where, int where_count);

/* Nested-loop join on t1.col1 = t2.col2; uses index on col2 when available.
 * post_filter WHERE clauses are applied on the combined row after joining.
 * Returns ResultSet with col_count = t1.col_count + t2.col_count. */
ResultSet *db_join(const char *t1,    const char *t2,
                   const char *col1,  const char *col2,
                   WhereClause *post_filter, int filter_count);

/* Count rows in table matching all WHERE clauses.
 * Uses index when available.
 * Returns count (>= 0), or -1 on error. */
int db_count(const char *table,
             WhereClause *where, int where_count);

/* ─────────────────────────────────────────────────────────────────────────────
 * DDL — Data Definition
 * ───────────────────────────────────────────────────────────────────────────*/

/* Create a new table: adds schema, creates .db file, creates PK .idx file.
 * Returns 1 on success, 0 on failure. */
int db_create_table(const char *table,
                    Column *cols, int col_count,
                    int record_size);

/* Drop a table: removes schema, deletes .db file.
 * Returns 1 on success, 0 on failure. */
int db_drop_table(const char *table);

/* Add a column to an existing table's in-memory schema.
 * Existing serialised records will have zeroed bytes for the new column.
 * Returns 1 on success, 0 on failure. */
int db_add_column(const char *table, Column *col);

/* Remove a column from an existing table's in-memory schema.
 * Returns 1 on success, 0 on failure. */
int db_drop_column(const char *table, const char *col_name);

#endif /* QUERY_H */