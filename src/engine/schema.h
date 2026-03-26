#ifndef SCHEMA_H
#define SCHEMA_H

#include "record.h"   /* brings in ResultSet, INT_NULL_SENTINEL, etc.        */
                      /* record.h has: typedef struct Schema Schema;          */

/* ─────────────────────────────────────────────────────────────────────────────
 * Compile-time limits
 * ───────────────────────────────────────────────────────────────────────────*/
#define MAX_COLUMNS  32
#define MAX_TABLES   32

/* ─────────────────────────────────────────────────────────────────────────────
 * ColumnType enum
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    COL_INT   = 0,
    COL_FLOAT = 1,
    COL_CHAR  = 2,
    COL_DATE  = 3
} ColumnType;

/* ─────────────────────────────────────────────────────────────────────────────
 * Column struct
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    char       name[64];
    ColumnType type;
    int        char_len;    /* COL_CHAR only; 0 otherwise                     */
    int        size;        /* on-disk byte width                             */
    int        is_pk;
    int        is_fk;
    int        is_not_null;
    int        offset;      /* byte offset within the serialised record       */
} Column;

/* ─────────────────────────────────────────────────────────────────────────────
 * Schema struct
 * record.h forward-declares `typedef struct Schema Schema;` — this full
 * definition of `struct Schema` is compatible with that forward declaration.
 * ───────────────────────────────────────────────────────────────────────────*/
struct Schema {
    char   table_name[64];
    Column columns[MAX_COLUMNS];
    int    col_count;
    int    record_size;
};

/* ─────────────────────────────────────────────────────────────────────────────
 * Global store (definitions live in schema.c)
 * ───────────────────────────────────────────────────────────────────────────*/
extern Schema g_schemas[MAX_TABLES];
extern int    g_schema_count;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/
void    schema_load(const char *path);
Schema *get_schema(const char *table_name);
void    schema_print_all(void);

#endif /* SCHEMA_H */