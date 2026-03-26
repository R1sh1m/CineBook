#ifndef RECORD_H
#define RECORD_H

#include <stdint.h>
#include <stddef.h>

/* Forward-declare Schema (defined in schema.h, Conversation 4). */
typedef struct Schema Schema;

/* ─────────────────────────────────────────────────────────────────────────────
 * NULL sentinels
 * ───────────────────────────────────────────────────────────────────────────*/
#define INT_NULL_SENTINEL   ((int32_t)0x80000000)
#define FLOAT_NULL_BITS     (0xFFFFFFFFu)

/* Fixed byte-widths for supported column types. */
#define TYPE_INT_SIZE    4
#define TYPE_FLOAT_SIZE  4
#define TYPE_DATE_SIZE   20   /* "YYYY-MM-DD HH:MM\0" + 3 pad bytes */

/* ─────────────────────────────────────────────────────────────────────────────
 * ResultSet
 * rows[i][j] is a heap-allocated copy of column j of row i.
 * Allocation size per field follows the column's type / char_len.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    void ***rows;     /* [row_count][col_count] heap-allocated field copies */
    int   row_count;
    int   col_count;
} ResultSet;

/* ─────────────────────────────────────────────────────────────────────────────
 * Core serialisation API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Serialise typed C values in fields[] into the raw byte buffer out. */
void record_serialize(const Schema *s, void **fields, uint8_t *out);

/* Deserialise raw byte buffer in into heap-allocated copies in out[]. */
void record_deserialize(const Schema *s, const uint8_t *in, void **out);

/* Free all memory owned by a ResultSet including the struct itself. */
void result_set_free(ResultSet *rs);

/* ─────────────────────────────────────────────────────────────────────────────
 * NULL helpers
 * ───────────────────────────────────────────────────────────────────────────*/

/* Return 1 if v equals the INT NULL sentinel (0x80000000). */
int  field_is_null_int(int32_t v);

/* Return 1 if the float's raw bit pattern is all-ones (FLOAT NULL). */
int  field_is_null_float(float v);

/* Return 1 if the first byte of the CHAR/DATE field is 0x00 (NULL). */
int  field_is_null_char(const char *v);

/* Write the INT NULL sentinel into the 4-byte buffer at out. */
void field_set_null_int(void *out);

/* Write the FLOAT NULL bit pattern (0xFFFFFFFF) into the 4-byte buffer at out. */
void field_set_null_float(void *out);

/* Zero n bytes at out, marking a CHAR/DATE field as NULL. */
void field_set_null_char(void *out, size_t n);

#endif /* RECORD_H */