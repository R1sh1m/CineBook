/*
 * ui_account.c — Account Settings UI for CineBook
 *
 * Public surface:
 *   void account_menu(SessionContext *ctx)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query.h"     /* db_select, db_update, db_join, WhereClause, ResultSet */
#include "session.h"   /* SessionContext, session_get_currency_sym               */
#include "auth.h"      /* sha256_hex, upgrade_to_student, UserRole              */
#include "location.h"  /* pick_city                                              */
#include "txn.h"       /* wal_begin, wal_commit, wal_rollback                    */

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────────*/

static void strip_newline(char *s)
{
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n')
        s[len - 1] = '\0';
}

/* Read a char* column from a single-row ResultSet; returns "" on any error. */
static const char *rs_str(ResultSet *rs, int row, int col)
{
    if (!rs || row >= rs->row_count || col >= rs->col_count) return "";
    if (!rs->rows[row][col]) return "";
    return (const char *)rs->rows[row][col];
}

static void print_card_row(const char *text)
{
    /* 56 chars inner width; avoid precision truncation to keep ANSI escapes intact */
    printf("║ %-56s║\n", text ? text : "");
}

/* Fetch "City, Country" using cities JOIN countries ON country_id */
static void fetch_city_country_display(int city_id, char *out_buf, int max_len)
{
    if (city_id <= 0) {
        strncpy(out_buf, "(not set)", (size_t)max_len - 1);
        out_buf[max_len - 1] = '\0';
        return;
    }

    WhereClause w[1];
    memset(w, 0, sizeof(w));
    strncpy(w[0].col_name, "city_id", sizeof(w[0].col_name) - 1);
    w[0].op = OP_EQ;
    w[0].value = &city_id;
    w[0].logic = 0;

    /* joined schema:
       cities    : [0]=city_id [1]=name [2]=country_id
       countries : [3]=country_id [4]=name [5]=currency_sym [6]=currency_code */
    ResultSet *rs = db_join("cities", "countries", "country_id", "country_id", w, 1);
    if (rs && rs->row_count > 0) {
        const char *city_name = rs_str(rs, 0, 1);
        const char *country_name = rs_str(rs, 0, 4);

        if (city_name[0] != '\0' && country_name[0] != '\0') {
            snprintf(out_buf, (size_t)max_len, "%s, %s", city_name, country_name);
        } else if (city_name[0] != '\0') {
            snprintf(out_buf, (size_t)max_len, "%s", city_name);
        } else {
            strncpy(out_buf, "(unknown)", (size_t)max_len - 1);
            out_buf[max_len - 1] = '\0';
        }
    } else {
        strncpy(out_buf, "(unknown)", (size_t)max_len - 1);
        out_buf[max_len - 1] = '\0';
    }

    if (rs) result_set_free(rs);
}

static int password_strength_score(const char *pw)
{
    int has_digit = 0, has_upper = 0, has_special = 0;
    size_t len = strlen(pw);

    for (size_t i = 0; pw[i] != '\0'; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (isdigit(c)) has_digit = 1;
        else if (isupper(c)) has_upper = 1;
        else if (!isalnum(c)) has_special = 1;
    }

    int score = 0;
    if (len > 8) score++;
    if (has_digit) score++;
    if (has_upper) score++;
    if (has_special) score++;
    return score; /* 0..4 */
}

static void print_password_strength_meter(int score)
{
    const char *bars = "██░░░░░░░░";
    const char *level = "WEAK";
    const char *color = "\033[31m";

    if (score >= 4) {
        bars = "████████░░";
        level = "STRONG";
        color = "\033[32m";
    } else if (score == 3) {
        bars = "██████░░░░";
        level = "FAIR";
        color = "\033[33m";
    } else if (score == 2) {
        bars = "████░░░░░░";
    } else if (score <= 0) {
        bars = "██░░░░░░░░";
    }

    printf("%s  Strength: %s  %s\033[0m\n", color, bars, level);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * print_account_header — premium account card
 * ───────────────────────────────────────────────────────────────────────────*/
static void print_account_header(SessionContext *ctx)
{
    char city_display[160];
    char line[256];
    char wallet_raw[64];
    char wallet_colored[128];
    const char *currency = session_get_currency_sym(ctx);
    if (!currency || currency[0] == '\0') currency = "Rs.";

    fetch_city_country_display(ctx->preferred_city_id, city_display, sizeof(city_display));

    const char *role_display = "Unknown";
    if (ctx->role == ROLE_STUDENT) {
        role_display = "\033[36mStudent  ✓ 12% discount active\033[0m";
    } else if (ctx->role == ROLE_USER) {
        role_display = "User  \033[33m○ Add academic email to unlock discount\033[0m";
    } else if (ctx->role == ROLE_ADMIN) {
        role_display = "\033[31mAdministrator\033[0m";
    }

    snprintf(wallet_raw, sizeof(wallet_raw), "%s%.2f", currency, ctx->wallet_balance);
    if (ctx->wallet_balance > 500.0f) {
        snprintf(wallet_colored, sizeof(wallet_colored), "\033[32m%s\033[0m", wallet_raw);
    } else if (ctx->wallet_balance >= 100.0f) {
        snprintf(wallet_colored, sizeof(wallet_colored), "\033[33m%s\033[0m", wallet_raw);
    } else {
        snprintf(wallet_colored, sizeof(wallet_colored), "\033[31m%s\033[0m", wallet_raw);
    }

    printf("\n");
    printf("╔════════════════════════════════════════════════════════╗\n");
    print_card_row(" ACCOUNT SETTINGS");
    printf("╠════════════════════════════════════════════════════════╣\n");

    snprintf(line, sizeof(line), " Name      : %s", ctx->name);
    print_card_row(line);

    snprintf(line, sizeof(line), " Role      : %s", role_display);
    print_card_row(line);

    snprintf(line, sizeof(line), " City      : %s", city_display);
    print_card_row(line);

    if (ctx->email[0] != '\0') {
        snprintf(line, sizeof(line), " Email     : %s", ctx->email);
    } else {
        snprintf(line, sizeof(line), " Email     : \033[90m(not set)\033[0m");
    }
    print_card_row(line);

    snprintf(line, sizeof(line), " Wallet    : %s", wallet_colored);
    print_card_row(line);

    printf("╠════════════════════════════════════════════════════════╣\n");
    print_card_row(" [ 1 ]  Change city");
    print_card_row(" [ 2 ]  Change password");

    if (ctx->role == ROLE_USER) {
        print_card_row(" [ 3 ]  Add email  \033[33m→ unlock student discount\033[0m");
    } else if (ctx->role == ROLE_STUDENT) {
        print_card_row(" [ 3 ]  Update email");
    }

    print_card_row(" [ 0 ]  Back");
    printf("╚════════════════════════════════════════════════════════╝\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * option_change_city
 * ───────────────────────────────────────────────────────────────────────────*/
static void option_change_city(SessionContext *ctx)
{
    char old_city[160];
    char new_city[160];

    printf("\n  ── Change City ─────────────────────────────────\n");

    fetch_city_country_display(ctx->preferred_city_id, old_city, sizeof(old_city));
    printf("  Current city: %s\n\n", old_city);

    /* pick_city() handles interactive flow and persistence */
    pick_city(ctx);

    fetch_city_country_display(ctx->preferred_city_id, new_city, sizeof(new_city));
    printf("\n  ✓ City updated to: %s\n", new_city);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * option_change_password
 * ───────────────────────────────────────────────────────────────────────────*/
static void option_change_password(SessionContext *ctx)
{
    printf("\n  ── Change Password ─────────────────────────────\n");

    /* ── 1. Fetch current password_hash from DB ── */
    int uid = ctx->user_id;
    WhereClause w[1];
    w[0].op    = OP_EQ;
    w[0].value = &uid;
    w[0].logic = 0;
    strncpy(w[0].col_name, "user_id", 63);
    w[0].col_name[63] = '\0';

    /* users schema: user_id(0) name(1) phone(2) email(3) password_hash(4) ... */
    ResultSet *user_rs = db_select("users", w, 1, NULL, 0);
    if (!user_rs || user_rs->row_count == 0) {
        printf("  [!] Could not load account data. Please try again.\n");
        if (user_rs) result_set_free(user_rs);
        return;
    }

    char stored_hash[65];
    strncpy(stored_hash, rs_str(user_rs, 0, 4), 64);
    stored_hash[64] = '\0';
    result_set_free(user_rs);

    /* ── 2. Prompt current password and verify ── */
    char current_pw[129];
    printf("  Current password: ••••••••\n");
    printf("  Enter current password: ");
    fflush(stdout);
    if (!fgets(current_pw, sizeof(current_pw), stdin)) return;
    strip_newline(current_pw);

    char current_hash[65];
    sha256_hex(current_pw, current_hash);

    if (strcmp(current_hash, stored_hash) != 0) {
        printf("\n  [!] Incorrect current password. Password not changed.\n");
        return;
    }

    /* ── 3. Prompt new password and evaluate strength ── */
    char new_pw[129];
    printf("  New password         : ");
    fflush(stdout);
    if (!fgets(new_pw, sizeof(new_pw), stdin)) return;
    strip_newline(new_pw);

    int score = password_strength_score(new_pw);
    print_password_strength_meter(score);

    if (score <= 2) {
        printf("  \033[31m[!] Password is WEAK. Minimum required strength is FAIR.\033[0m\n");
        return;
    }

    /* ── 4. Confirm new password ── */
    char confirm_pw[129];
    printf("  Confirm new password : ");
    fflush(stdout);
    if (!fgets(confirm_pw, sizeof(confirm_pw), stdin)) return;
    strip_newline(confirm_pw);

    if (strcmp(new_pw, confirm_pw) != 0) {
        printf("\n  [!] Passwords do not match. Password not changed.\n");
        return;
    }

    /* Reject if same as current */
    char new_hash[65];
    sha256_hex(new_pw, new_hash);
    if (strcmp(new_hash, stored_hash) == 0) {
        printf("\n  [!] New password must differ from the current password.\n");
        return;
    }

    /* ── 5. Persist via WAL-wrapped db_update ── */
    wal_begin();

    WhereClause upd_where[1];
    upd_where[0].op    = OP_EQ;
    upd_where[0].value = &uid;
    upd_where[0].logic = 0;
    strncpy(upd_where[0].col_name, "user_id", 63);
    upd_where[0].col_name[63] = '\0';

    int rows = db_update("users", upd_where, 1, "password_hash", new_hash);

    if (rows > 0) {
        wal_commit();
        printf("\n  ✓ Password updated successfully.\n");
    } else {
        wal_rollback();
        printf("\n  [!] Failed to update password in database. No changes made.\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * option_update_email
 * ───────────────────────────────────────────────────────────────────────────*/
static void option_update_email(SessionContext *ctx)
{
    printf("\n  ── Add / Update Email & Student Upgrade ────────\n");

    if (ctx->role == ROLE_STUDENT) {
        printf("\033[32m  ✓ Student status confirmed. Academic email on file: %s\033[0m\n",
               (ctx->email[0] != '\0') ? ctx->email : "(not set)");
        printf("  You can update your email below, but your discount is already active.\n");
    } else {
        printf("  Adding an academic email (.ac.in / .edu domain)\n");
        printf("  will upgrade your account to Student status,\n");
        printf("  unlocking the 12%% student discount on bookings.\n");
    }

    /* Handles prompt + validation + db updates + ctx refresh */
    upgrade_to_student(ctx);

    if (ctx->email[0] != '\0') {
        printf("\n  [i] Email on file: %s\n", ctx->email);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * account_menu — public entry point
 * ───────────────────────────────────────────────────────────────────────────*/
void account_menu(SessionContext *ctx)
{
    if (!ctx || ctx->user_id == 0) {
        printf("\n  [!] You must be logged in to access account settings.\n");
        return;
    }

    char input_buf[8];

    while (1) {
        int max_choice = (ctx->role == ROLE_ADMIN) ? 2 : 3;

        print_account_header(ctx);

        printf("  Choice: ");
        fflush(stdout);

        if (!fgets(input_buf, sizeof(input_buf), stdin)) break;
        strip_newline(input_buf);

        char *endptr = NULL;
        long choice = strtol(input_buf, &endptr, 10);

        if (endptr == input_buf || *endptr != '\0') {
            printf("\n  [!] Invalid input. Please enter 0–%d.\n", max_choice);
            continue;
        }

        switch (choice) {
            case 0:
                printf("\n  Returning to main menu.\n\n");
                return;

            case 1:
                option_change_city(ctx);
                break;

            case 2:
                option_change_password(ctx);
                break;

            case 3:
                if (ctx->role == ROLE_ADMIN) {
                    printf("\n  [!] Please enter 0, 1, or 2.\n");
                } else {
                    option_update_email(ctx);
                }
                break;

            default:
                printf("\n  [!] Please enter a valid option (0–%d).\n", max_choice);
                break;
        }

        printf("\n  Press Enter to continue...");
        fflush(stdout);
        char dummy[64];
        fgets(dummy, sizeof(dummy), stdin);
    }
}