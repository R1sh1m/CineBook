#include "promos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <txn.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: uppercase a string in-place
 * ───────────────────────────────────────────────────────────────────────────*/
static void str_toupper_inplace(char *s) {
    if (!s) return;
    for (int i = 0; s[i]; i++) {
        s[i] = (char)toupper((unsigned char)s[i]);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: get today's date as YYYY-MM-DD string
 * ───────────────────────────────────────────────────────────────────────────*/
static void get_today(char buf[20]) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    strncpy(buf, tmp, 19);
    buf[19] = '\0';
}

/* ─────────────────────────────────────────────────────────────────────────────
 * validate_promo — Multi-condition promo code validation
 * ───────────────────────────────────────────────────────────────────────────*/
Promo *validate_promo(const char *code, UserRole role, int seat_count,
                      SessionContext *ctx) {
    (void)ctx;   // ADD — suppresses unused-parameter warning
    
    if (!code || !code[0]) {
        printf("Error: promo code cannot be empty.\n");
        return NULL;
    }

    /* Uppercase the input code for case-insensitive lookup */
    char code_upper[20];
    strncpy(code_upper, code, sizeof(code_upper) - 1);
    code_upper[sizeof(code_upper) - 1] = '\0';
    str_toupper_inplace(code_upper);

    /* Query promos WHERE code = code_upper */
    WhereClause where = {
        .col_name = "code",
        .op = OP_EQ,
        .value = code_upper,
        .logic = 0
    };

    ResultSet *rs = db_select("promos", &where, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("Error: promo code '%s' not found.\n", code);
        if (rs) result_set_free(rs);
        return NULL;
    }

    /* Extract fields from first result row */
    void **row = rs->rows[0];
    Promo *p = (Promo *)malloc(sizeof(Promo));

    p->promo_id        = *(int32_t *)row[0];
    strncpy(p->code, (const char *)row[1], sizeof(p->code) - 1);
    p->code[sizeof(p->code) - 1] = '\0';
    p->discount_type   = *(int32_t *)row[2];
    p->discount_value  = *(float *)row[3];
    p->max_discount_cap = *(float *)row[4];
    p->min_seats       = *(int32_t *)row[5];
    p->role_mask       = *(int32_t *)row[6];
    strncpy(p->valid_from, (const char *)row[7], sizeof(p->valid_from) - 1);
    p->valid_from[sizeof(p->valid_from) - 1] = '\0';
    strncpy(p->valid_until, (const char *)row[8], sizeof(p->valid_until) - 1);
    p->valid_until[sizeof(p->valid_until) - 1] = '\0';
    p->max_uses        = *(int32_t *)row[9];
    p->current_uses    = *(int32_t *)row[10];
    p->is_active       = *(int32_t *)row[11];

    result_set_free(rs);

    /* ── Validation checks ── */

    /* Check 1: is_active */
    if (p->is_active != 1) {
        printf("Error: promo code '%s' is not active.\n", code);
        free(p);
        return NULL;
    }

    /* Check 2: date range (YYYY-MM-DD string comparison is safe) */
    char today[20];
    get_today(today);
    if (strcmp(today, p->valid_from) < 0) {
        printf("Error: promo code '%s' is not yet valid (starts %s).\n",
               code, p->valid_from);
        free(p);
        return NULL;
    }
    if (strcmp(today, p->valid_until) > 0) {
        printf("Error: promo code '%s' has expired (ended %s).\n",
               code, p->valid_until);
        free(p);
        return NULL;
    }

    /* Check 3: max_uses limit */
    if (p->max_uses > 0 && p->current_uses >= p->max_uses) {
        printf("Error: promo code '%s' has reached max usage limit (%d).\n",
               code, p->max_uses);
        free(p);
        return NULL;
    }

    /* Check 4: minimum seat count */
    if (seat_count < p->min_seats) {
        printf("Error: promo code '%s' requires at least %d seats (you have %d).\n",
               code, p->min_seats, seat_count);
        free(p);
        return NULL;
    }

    /* Check 5: role eligibility (bitmask check) */
    if (!((p->role_mask >> role) & 1)) {
        printf("Error: promo code '%s' is not available for your role.\n", code);
        free(p);
        return NULL;
    }

    /* All checks passed */
    return p;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * apply_promo — Compute discount amount
 * ───────────────────────────────────────────────────────────────────────────*/
float apply_promo(const Promo *promo, float base_amount) {
    if (!promo || base_amount <= 0.0f) {
        return 0.0f;
    }

    float discount = 0.0f;

    if (promo->discount_type == 0) {
        /* PERCENT type */
        discount = base_amount * (promo->discount_value / 100.0f);

        /* Apply cap if set */
        if (promo->max_discount_cap > 0.0f && discount > promo->max_discount_cap) {
            discount = promo->max_discount_cap;
        }
    } else {
        /* FLAT type */
        discount = promo->discount_value;

        /* Cap at base_amount (cannot discount more than the price) */
        if (discount > base_amount) {
            discount = base_amount;
        }
    }

    return discount;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * increment_promo_uses — Increment redemption counter
 * ───────────────────────────────────────────────────────────────────────────*/
void increment_promo_uses(int promo_id) {
    /* Read current value */
    WhereClause where = {
        .col_name = "promo_id",
        .op = OP_EQ,
        .value = &promo_id,
        .logic = 0
    };

    ResultSet *rs = db_select("promos", &where, 1, NULL, 0);
    if (!rs || rs->row_count == 0) {
        printf("Error: promo_id %d not found in increment_promo_uses.\n", promo_id);
        if (rs) result_set_free(rs);
        return;
    }

    int current_uses = *(int32_t *)rs->rows[0][10];
    result_set_free(rs);

    int new_uses = current_uses + 1;

    /* Update with transaction */
    wal_begin();
    int affected = db_update("promos", &where, 1, "current_uses", &new_uses);
    wal_commit();

    if (affected <= 0) {
        printf("Warning: failed to increment promo_uses for promo_id %d.\n", promo_id);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * create_promo — Admin: interactive promo creation
 * ───────────────────────────────────────────────────────────────────────────*/
void create_promo(void) {
    printf("\n=== CREATE NEW PROMO CODE ===\n");

    char code[20];
    printf("Promo code (max 20 chars, e.g., SAVE20): ");
    if (!fgets(code, sizeof(code), stdin)) {
        printf("Error reading input.\n");
        return;
    }
    /* Trim newline */
    size_t len = strlen(code);
    if (len > 0 && code[len - 1] == '\n') code[len - 1] = '\0';
    if (!code[0]) {
        printf("Error: code cannot be empty.\n");
        return;
    }
    str_toupper_inplace(code);

    int discount_type;
    printf("Discount type (0=PERCENT / 1=FLAT): ");
    scanf("%d", &discount_type);
    getchar(); /* consume newline */
    if (discount_type != 0 && discount_type != 1) {
        printf("Error: discount_type must be 0 or 1.\n");
        return;
    }

    float discount_value;
    if (discount_type == 0) {
        printf("Discount percentage (e.g., 15 for 15%%): ");
    } else {
        printf("Discount amount in INR (e.g., 500): ");
    }
    scanf("%f", &discount_value);
    getchar();
    if (discount_value <= 0.0f) {
        printf("Error: discount_value must be > 0.\n");
        return;
    }

    float max_discount_cap = 0.0f;
    if (discount_type == 0) {
        printf("Max discount cap in INR (0 = no cap): ");
        scanf("%f", &max_discount_cap);
        getchar();
    }

    int min_seats;
    printf("Minimum seats to qualify (default 1): ");
    scanf("%d", &min_seats);
    getchar();
    if (min_seats < 1) min_seats = 1;

    int role_mask;
    printf("Role mask (1=USER, 2=STUDENT, 4=ADMIN; sum for multiple, e.g., 7=all): ");
    scanf("%d", &role_mask);
    getchar();
    if (role_mask < 1 || role_mask > 7) {
        printf("Error: role_mask must be 1-7.\n");
        return;
    }

    char valid_from[20];
    printf("Valid from (YYYY-MM-DD): ");
    if (!fgets(valid_from, sizeof(valid_from), stdin)) {
        printf("Error reading date.\n");
        return;
    }
    len = strlen(valid_from);
    if (len > 0 && valid_from[len - 1] == '\n') valid_from[len - 1] = '\0';
    if (strlen(valid_from) != 10) {
        printf("Error: date format must be YYYY-MM-DD.\n");
        return;
    }

    char valid_until[20];
    printf("Valid until (YYYY-MM-DD): ");
    if (!fgets(valid_until, sizeof(valid_until), stdin)) {
        printf("Error reading date.\n");
        return;
    }
    len = strlen(valid_until);
    if (len > 0 && valid_until[len - 1] == '\n') valid_until[len - 1] = '\0';
    if (strlen(valid_until) != 10) {
        printf("Error: date format must be YYYY-MM-DD.\n");
        return;
    }

    int max_uses;
    printf("Max uses (0 = unlimited): ");
    scanf("%d", &max_uses);
    getchar();
    if (max_uses < 0) max_uses = 0;

    int is_active = 1;
    printf("Active immediately? (1=yes / 0=no): ");
    scanf("%d", &is_active);
    getchar();

    /* Prepare field values */
    int promo_id = 0; /* Will be auto-incremented by db_insert */
    int current_uses = 0;

    void *field_values[12] = {
        &promo_id,
        code,
        &discount_type,
        &discount_value,
        &max_discount_cap,
        &min_seats,
        &role_mask,
        valid_from,
        valid_until,
        &max_uses,
        &current_uses,
        &is_active
    };

    wal_begin();
    int new_id = db_insert("promos", field_values);
    wal_commit();

    if (new_id > 0) {
        printf("✓ Promo code created successfully! (ID: %d, Code: %s)\n", new_id, code);
    } else {
        printf("✗ Failed to create promo code.\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * deactivate_promo — Admin: deactivate a promo
 * ───────────────────────────────────────────────────────────────────────────*/
void deactivate_promo(int promo_id) {
    int is_active_val = 0;

    WhereClause where = {
        .col_name = "promo_id",
        .op = OP_EQ,
        .value = &promo_id,
        .logic = 0
    };

    wal_begin();
    int affected = db_update("promos", &where, 1, "is_active", &is_active_val);
    wal_commit();

    if (affected > 0) {
        printf("✓ Promo %d deactivated.\n", promo_id);
    } else {
        printf("✗ Promo %d not found or already inactive.\n", promo_id);
    }
}