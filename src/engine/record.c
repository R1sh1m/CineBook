/*
 * record.c — Serialise/deserialise typed C values to/from raw page-slot bytes.
 *
 * Column definitions come from Schema (schema.h, built in Conversation 4).
 * This file never opens files; it only transforms memory buffers.
 *
 * Type encoding:
 *   INT   → little-endian int32_t (4 bytes). NULL = 0x80000000
 *   FLOAT → little-endian IEEE-754 float (4 bytes). NULL = 0xFFFFFFFF bits
 *   CHAR  → NUL-padded, n bytes. NULL = first byte 0x00
 *   DATE  → 20-byte ASCII "YYYY-MM-DD HH:MM\0" + 3 pad. NULL = first byte 0x00
 */

#include "record.h"

/* schema.h will be available after Conversation 4; it defines Schema and
 * Column. The Column struct is expected to have at minimum:
 *   char  name[64];
 *   int   type;          // COL_INT | COL_FLOAT | COL_CHAR | COL_DATE
 *   int   char_len;      // valid when type == COL_CHAR; 0 otherwise
 *   int   offset;        // byte offset of this field within a serialised record
 *   int   nullable;      // 1 = column accepts NULL values
 * Schema has:
 *   char    table_name[64];
 *   Column  columns[MAX_COLUMNS];
 *   int     col_count;
 *   int     record_size; // total serialised size in bytes
 *
 * We include schema.h here so the compiler can resolve Column.
 */
#include "schema.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers: portably read/write little-endian 32-bit values.
 * On all architectures used for this project (x86/ARM, little-endian), a
 * direct memcpy is equivalent to a little-endian write; we use memcpy to
 * avoid strict-aliasing UB.
 * ───────────────────────────────────────────────────────────────────────────*/

/* Write a 32-bit pattern p into dst (no alignment requirement). */
static void write32(void *dst, uint32_t p)
{
    memcpy(dst, &p, 4);
}

/* Read a 32-bit pattern from src into *out (no alignment requirement). */
static void read32(const void *src, uint32_t *out)
{
    memcpy(out, src, 4);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * NULL helpers — public
 * ───────────────────────────────────────────────────────────────────────────*/

/* Return 1 if v is the INT NULL sentinel. */
int field_is_null_int(int32_t v)
{
    return v == INT_NULL_SENTINEL;
}

/* Return 1 if the float's bit pattern is all-ones. */
int field_is_null_float(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return bits == FLOAT_NULL_BITS;
}

/* Return 1 if first byte of the CHAR/DATE field is 0x00. */
int field_is_null_char(const char *v)
{
    return v == NULL || (uint8_t)v[0] == 0x00u;
}

/* Write INT NULL sentinel into the 4-byte buffer at out. */
void field_set_null_int(void *out)
{
    uint32_t sentinel = (uint32_t)INT_NULL_SENTINEL;
    write32(out, sentinel);
}

/* Write FLOAT NULL bit pattern into the 4-byte buffer at out. */
void field_set_null_float(void *out)
{
    write32(out, FLOAT_NULL_BITS);
}

/* Zero n bytes at out (CHAR/DATE NULL). */
void field_set_null_char(void *out, size_t n)
{
    memset(out, 0, n);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Static helpers: write_field / read_field
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * write_field — encode one field value into the correct position of out[].
 * col  : column descriptor (type, offset, char_len).
 * value: pointer to the C value to encode; NULL means write the NULL sentinel.
 * out  : base pointer of the record byte buffer (record_size bytes).
 */
static void write_field(const Column *col, void *value, uint8_t *out)
{
    uint8_t *dst = out + col->offset;

    switch (col->type) {

    case COL_INT: {
        if (value == NULL) {
            field_set_null_int(dst);
        } else {
            /* Caller passes int* or int32_t*. */
            int32_t v;
            memcpy(&v, value, 4);
            uint32_t bits;
            memcpy(&bits, &v, 4);
            write32(dst, bits);
        }
        break;
    }

    case COL_FLOAT: {
        if (value == NULL) {
            field_set_null_float(dst);
        } else {
            float f;
            memcpy(&f, value, 4);
            uint32_t bits;
            memcpy(&bits, &f, 4);
            write32(dst, bits);
        }
        break;
    }

    case COL_CHAR: {
        /* char_len bytes, NUL-padded. NULL = zero entire slot. */
        int n = col->char_len > 0 ? col->char_len : 1;
        if (value == NULL) {
            field_set_null_char(dst, (size_t)n);
        } else {
            const char *src = (const char *)value;
            size_t slen = strnlen(src, (size_t)n);
            memcpy(dst, src, slen);
            if (slen < (size_t)n)
                memset(dst + slen, 0, (size_t)n - slen);
        }
        break;
    }

    case COL_DATE: {
        /* Fixed 20-byte slot. NULL = zero the slot. */
        if (value == NULL) {
            memset(dst, 0, TYPE_DATE_SIZE);
        } else {
            const char *src = (const char *)value;
            size_t slen = strnlen(src, TYPE_DATE_SIZE);
            if (slen >= TYPE_DATE_SIZE) slen = TYPE_DATE_SIZE - 1;
            memcpy(dst, src, slen);
            /* NUL-pad the remainder (including the 3 pad bytes). */
            memset(dst + slen, 0, TYPE_DATE_SIZE - slen);
        }
        break;
    }

    default:
        /* Unknown type: zero the storage to avoid leaking junk bytes. */
        break;
    }
}

/*
 * read_field — decode one field from the raw record buffer and allocate a
 * heap copy that the caller owns.
 * col : column descriptor.
 * in  : base pointer of the record byte buffer.
 * out : pointer to the void* slot that will receive the heap allocation.
 *       Set to NULL if caller passed NULL for this column (skip it).
 */
static void read_field(const Column *col, const uint8_t *in, void **out)
{
    const uint8_t *src = in + col->offset;

    /* If the caller's out slot itself is NULL they don't want this column. */
    if (out == NULL)
        return;

    switch (col->type) {

    case COL_INT: {
        int32_t *buf = (int32_t *)malloc(4);
        if (!buf) { *out = NULL; return; }
        uint32_t bits;
        read32(src, &bits);
        memcpy(buf, &bits, 4);
        *out = buf;
        break;
    }

    case COL_FLOAT: {
        float *buf = (float *)malloc(4);
        if (!buf) { *out = NULL; return; }
        uint32_t bits;
        read32(src, &bits);
        memcpy(buf, &bits, 4);
        *out = buf;
        break;
    }

    case COL_CHAR: {
        int n = col->char_len > 0 ? col->char_len : 1;
        /* Allocate n+1 to guarantee NUL terminator even if field is full. */
        char *buf = (char *)malloc((size_t)n + 1);
        if (!buf) { *out = NULL; return; }
        memcpy(buf, src, (size_t)n);
        buf[n] = '\0';
        *out = buf;
        break;
    }

    case COL_DATE: {
        /* Allocate TYPE_DATE_SIZE+1 for safety. */
        char *buf = (char *)malloc(TYPE_DATE_SIZE + 1);
        if (!buf) { *out = NULL; return; }
        memcpy(buf, src, TYPE_DATE_SIZE);
        buf[TYPE_DATE_SIZE] = '\0';
        *out = buf;
        break;
    }

    default:
        *out = NULL;
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * record_serialize — encode all columns of a row into the flat byte buffer out.
 * fields[i] is a void* to the value for column i; pass NULL for SQL NULL.
 * out must be at least s->record_size bytes.
 */
void record_serialize(const Schema *s, void **fields, uint8_t *out)
{
    /* Zero the entire record first so unused/pad bytes are deterministic. */
    memset(out, 0, (size_t)s->record_size);

    for (int i = 0; i < s->col_count; i++) {
        write_field(&s->columns[i], fields ? fields[i] : NULL, out);
    }
}

/*
 * record_deserialize — decode the flat byte buffer in into heap copies in out[].
 * out must be an array of col_count void* pointers, pre-set by the caller:
 *   out[i] == NULL → skip (caller doesn't need column i)
 *   out[i] != NULL → overwrite with a fresh heap allocation (caller must free)
 * After this call, each non-skipped out[i] points to a heap buffer owned by
 * the caller.
 */
// always populate every column
void record_deserialize(const Schema *s, const uint8_t *in, void **out)
{
    /* FIX: always populate every column.
     * The old "skip if out[i]==NULL" design caused table_scan to produce
     * all-NULL rows after the first row's ownership transfer set every
     * fields[i] to NULL — the next slot skipped all deserialization,
     * causing a segfault in any caller that read the row fields. */
    if (!out) return;
    for (int i = 0; i < s->col_count; i++) {
        read_field(&s->columns[i], in, &out[i]);
    }
}

/*
 * result_set_free — release all memory owned by a ResultSet.
 * Frees each field copy, each row array, the rows array, then the struct.
 */
void result_set_free(ResultSet *rs)
{
    if (!rs) return;

    if (rs->rows) {
        for (int i = 0; i < rs->row_count; i++) {
            if (rs->rows[i]) {
                for (int j = 0; j < rs->col_count; j++) {
                    free(rs->rows[i][j]);
                    rs->rows[i][j] = NULL;
                }
                free(rs->rows[i]);
                rs->rows[i] = NULL;
            }
        }
        free(rs->rows);
        rs->rows = NULL;
    }

    free(rs);
}