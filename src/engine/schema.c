/*
 * schema.c — CineBook RDBMS Engine, Conversation 04
 * Parses data/schema.cat into g_schemas[] at startup.
 * No dependency on storage.c; uses plain fopen/fgets/sscanf.
 * C11 standard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "schema.h"
#include "storage.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Global definitions
 * ───────────────────────────────────────────────────────────────────────────*/
Schema g_schemas[MAX_TABLES];
int    g_schema_count = 0;

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────────*/

/* Trim trailing newline / carriage-return in place. */
static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = '\0';
}

/* Return 1 if the line (after trimming) is blank or a comment (#). */
static int is_skip_line(const char *line)
{
    while (*line && isspace((unsigned char)*line))
        ++line;
    return (*line == '\0' || *line == '#');
}

/*
 * parse_column_line
 * Format: <name> <TYPE> <size> <pk> <fk> <nn> <offset>
 * TYPE token: INT | FLOAT | CHAR | DATE
 * Populates *col in full.
 * Returns 1 on success, 0 on parse failure.
 */
static int parse_column_line(const char *line, Column *col)
{
    char type_token[16];
    int  size, pk, fk, nn, offset;

    memset(col, 0, sizeof(*col));

    int matched = sscanf(line, "%63s %15s %d %d %d %d %d",
                         col->name, type_token,
                         &size, &pk, &fk, &nn, &offset);
    if (matched != 7)
        return 0;

    col->size        = size;
    col->is_pk       = pk;
    col->is_fk       = fk;
    col->is_not_null = nn;
    col->offset      = offset;
    col->char_len    = 0;

    if (strcmp(type_token, "INT") == 0) {
        col->type = COL_INT;
    } else if (strcmp(type_token, "FLOAT") == 0) {
        col->type = COL_FLOAT;
    } else if (strcmp(type_token, "CHAR") == 0) {
        col->type     = COL_CHAR;
        col->char_len = size;
    } else if (strcmp(type_token, "DATE") == 0) {
        col->type = COL_DATE;
    } else {
        fprintf(stderr, "[schema] WARNING: unknown column type '%s' for column '%s'; skipping\n",
                type_token, col->name);
        return 0;
    }

    return 1;
}

static int validate_table_schema(const Schema *s, int lineno)
{
    if (!s) return 0;

    if (s->col_count <= 0 || s->record_size <= 0) {
        fprintf(stderr, "[schema] ERROR: line %d: table '%s' has invalid shape "
                        "(cols=%d record_size=%d)\n",
                lineno, s->table_name, s->col_count, s->record_size);
        return 0;
    }

    if (s->record_size > (int)PAGE_DATA_SIZE) {
        fprintf(stderr, "[schema] ERROR: table '%s' record_size=%d exceeds page data size=%u\n",
                s->table_name, s->record_size, PAGE_DATA_SIZE);
        return 0;
    }

    int expected_offset = 0;
    for (int i = 0; i < s->col_count; i++) {
        const Column *c = &s->columns[i];

        if (c->size <= 0 || c->offset < 0) {
            fprintf(stderr, "[schema] ERROR: table '%s' column '%s' has invalid size/offset\n",
                    s->table_name, c->name);
            return 0;
        }

        if (c->offset != expected_offset) {
            fprintf(stderr, "[schema] ERROR: table '%s' column offset drift at '%s': "
                            "expected %d got %d\n",
                    s->table_name, c->name, expected_offset, c->offset);
            return 0;
        }

        if (c->offset + c->size > s->record_size) {
            fprintf(stderr, "[schema] ERROR: table '%s' column '%s' overflows record bounds\n",
                    s->table_name, c->name);
            return 0;
        }

        if ((c->type == COL_INT || c->type == COL_FLOAT) && c->size != 4) {
            fprintf(stderr, "[schema] ERROR: table '%s' column '%s' has invalid scalar size=%d\n",
                    s->table_name, c->name, c->size);
            return 0;
        }

        if (c->type == COL_DATE && c->size != TYPE_DATE_SIZE) {
            fprintf(stderr, "[schema] ERROR: table '%s' column '%s' DATE size must be %d\n",
                    s->table_name, c->name, TYPE_DATE_SIZE);
            return 0;
        }

        if (c->type == COL_CHAR && (c->char_len <= 0 || c->char_len != c->size)) {
            fprintf(stderr, "[schema] ERROR: table '%s' column '%s' invalid CHAR size/len\n",
                    s->table_name, c->name);
            return 0;
        }

        for (int j = i + 1; j < s->col_count; j++) {
            if (strcmp(c->name, s->columns[j].name) == 0) {
                fprintf(stderr, "[schema] ERROR: table '%s' duplicate column '%s'\n",
                        s->table_name, c->name);
                return 0;
            }
        }

        expected_offset += c->size;
    }

    if (expected_offset != s->record_size) {
        fprintf(stderr, "[schema] ERROR: table '%s' record_size mismatch: "
                        "computed=%d declared=%d\n",
                s->table_name, expected_offset, s->record_size);
        return 0;
    }

    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * schema_load
 * ───────────────────────────────────────────────────────────────────────────*/
void schema_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[schema] FATAL: cannot open schema file '%s'\n", path);
        exit(1);
    }

    g_schema_count = 0;

    char    line[512];
    Schema *cur     = NULL;
    int     lineno  = 0;

    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        trim_newline(line);

        if (is_skip_line(line))
            continue;

        /* ── TABLE <name> record_size <N> ── */
        if (strncmp(line, "TABLE ", 6) == 0) {
            if (cur != NULL) {
                fprintf(stderr, "[schema] WARNING: line %d: nested TABLE without END_TABLE; "
                                "closing previous table '%s'\n", lineno, cur->table_name);
            }
            if (g_schema_count >= MAX_TABLES) {
                fprintf(stderr, "[schema] WARNING: line %d: MAX_TABLES (%d) reached; "
                                "ignoring table\n", lineno, MAX_TABLES);
                cur = NULL;
                continue;
            }

            cur = &g_schemas[g_schema_count];
            memset(cur, 0, sizeof(*cur));

            char tname[64];
            int  rsz = 0;
            if (sscanf(line, "TABLE %63s record_size %d", tname, &rsz) != 2) {
                fprintf(stderr, "[schema] WARNING: line %d: malformed TABLE line: '%s'\n",
                        lineno, line);
                cur = NULL;
                continue;
            }
            strncpy(cur->table_name, tname, 63);
            cur->table_name[63] = '\0';
            cur->record_size    = rsz;
            cur->col_count      = 0;
            continue;
        }

        /* ── END_TABLE ── */
        if (strcmp(line, "END_TABLE") == 0) {
            if (cur == NULL) {
                fprintf(stderr, "[schema] WARNING: line %d: END_TABLE without matching TABLE\n",
                        lineno);
            } else {
                if (validate_table_schema(cur, lineno)) {
                    ++g_schema_count;
                } else {
                    fprintf(stderr, "[schema] FATAL: invalid schema for table '%s'\n",
                            cur->table_name);
                    fclose(f);
                    exit(1);
                }
                cur = NULL;
            }
            continue;
        }

        /* ── Column line (inside a TABLE block) ── */
        if (cur != NULL) {
            if (cur->col_count >= MAX_COLUMNS) {
                fprintf(stderr, "[schema] WARNING: line %d: table '%s' exceeds MAX_COLUMNS (%d); "
                                "column ignored\n", lineno, cur->table_name, MAX_COLUMNS);
                continue;
            }

            Column col;
            if (parse_column_line(line, &col)) {
                cur->columns[cur->col_count++] = col;
            } else {
                fprintf(stderr, "[schema] WARNING: line %d: could not parse column line: '%s'\n",
                        lineno, line);
            }
            continue;
        }

        fprintf(stderr, "[schema] WARNING: line %d: unexpected line outside TABLE block: '%s'\n",
                lineno, line);
    }

    if (cur != NULL) {
        fprintf(stderr, "[schema] WARNING: EOF reached inside unclosed TABLE block '%s'\n",
                cur->table_name);
        if (validate_table_schema(cur, lineno)) {
            ++g_schema_count;
        } else {
            fprintf(stderr, "[schema] FATAL: invalid schema for table '%s'\n",
                    cur->table_name);
            fclose(f);
            exit(1);
        }
    }

    fclose(f);

    /* ── Boot diagnostic → stderr so it never appears in the UI ── */
    fprintf(stderr, "[schema] Loaded %d table schema(s) from '%s'\n",
            g_schema_count, path);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * get_schema
 * ───────────────────────────────────────────────────────────────────────────*/
Schema *get_schema(const char *table_name)
{
    for (int i = 0; i < g_schema_count; ++i) {
        if (strcmp(g_schemas[i].table_name, table_name) == 0)
            return &g_schemas[i];
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * schema_print_all  (debug / admin only — writes to stderr)
 * ───────────────────────────────────────────────────────────────────────────*/
void schema_print_all(void)
{
    fprintf(stderr, "=== Schema Dump (%d tables) ===\n", g_schema_count);
    for (int i = 0; i < g_schema_count; ++i) {
        const Schema *s = &g_schemas[i];
        fprintf(stderr, "  [%02d] %-30s  cols=%-3d  record_size=%d\n",
               i, s->table_name, s->col_count, s->record_size);
        for (int j = 0; j < s->col_count; ++j) {
            const Column *c = &s->columns[j];
            const char *tname = "?";
            switch (c->type) {
                case COL_INT:   tname = "INT";   break;
                case COL_FLOAT: tname = "FLOAT"; break;
                case COL_CHAR:  tname = "CHAR";  break;
                case COL_DATE:  tname = "DATE";  break;
            }
            fprintf(stderr, "         %-28s  %-5s  sz=%-4d off=%-4d  pk=%d fk=%d nn=%d",
                   c->name, tname, c->size, c->offset,
                   c->is_pk, c->is_fk, c->is_not_null);
            if (c->type == COL_CHAR)
                fprintf(stderr, "  char_len=%d", c->char_len);
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "================================\n");
}