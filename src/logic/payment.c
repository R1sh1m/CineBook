#include "payment.h"
#include "query.h"
#include "txn.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ─────────────────────────────────────────────────────────────────────────────
 * Local helpers
 * ───────────────────────────────────────────────────────────────────────────*/
static void read_line_local(char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    if (!fgets(buf, (int)sz, stdin)) {
        buf[0] = '\0';
        return;
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
}

static void format_inr_amount(float amount, char *out, size_t out_sz, int with_decimals)
{
    if (!out || out_sz == 0) return;

    long long paise = (long long)(amount * 100.0f + (amount >= 0.0f ? 0.5f : -0.5f));
    if (paise < 0) paise = 0;

    long long whole = paise / 100;
    int frac = (int)(paise % 100);

    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", whole);

    char rev[48];
    int r = 0;
    int grp = 0;
    for (int i = (int)strlen(raw) - 1; i >= 0 && r < (int)sizeof(rev) - 1; i--) {
        if (grp == 3) {
            rev[r++] = ',';
            grp = 0;
        }
        rev[r++] = raw[i];
        grp++;
    }
    rev[r] = '\0';

    char whole_fmt[48];
    int w = 0;
    for (int i = r - 1; i >= 0 && w < (int)sizeof(whole_fmt) - 1; i--) whole_fmt[w++] = rev[i];
    whole_fmt[w] = '\0';

    if (with_decimals) snprintf(out, out_sz, "%s.%02d", whole_fmt, frac);
    else snprintf(out, out_sz, "%s", whole_fmt);
}

static void get_current_datetime(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    if (!tm_now) {
        if (size) buf[0] = '\0';
        return;
    }
    strftime(buf, size, "%Y-%m-%d %H:%M", tm_now);
}

static int luhn_check(const char *card_num)
{
    if (!card_num || strlen(card_num) != 16) return 0;
    for (int i = 0; i < 16; i++) if (!isdigit((unsigned char)card_num[i])) return 0;

    int sum = 0;
    int is_second = 0;
    for (int i = 15; i >= 0; i--) {
        int digit = card_num[i] - '0';
        if (is_second) {
            digit *= 2;
            if (digit > 9) digit -= 9;
        }
        sum += digit;
        is_second = !is_second;
    }
    return (sum % 10 == 0);
}

static int validate_upi_vpa(const char *vpa)
{
    if (!vpa || strlen(vpa) < 3) return 0;

    const char *at = strchr(vpa, '@');
    if (!at || at == vpa || *(at + 1) == '\0') return 0;

    for (const char *p = vpa; p < at; p++) {
        if (!isdigit((unsigned char)*p) && !isalpha((unsigned char)*p) && *p != '.') return 0;
        if (isalpha((unsigned char)*p) && isupper((unsigned char)*p)) return 0;
    }
    for (const char *p = at + 1; *p; p++) {
        if (!isalpha((unsigned char)*p) || isupper((unsigned char)*p)) return 0;
    }
    return 1;
}

static int validate_expiry(const char *expiry)
{
    if (!expiry || strlen(expiry) != 5 || expiry[2] != '/') return 0;
    if (!isdigit((unsigned char)expiry[0]) || !isdigit((unsigned char)expiry[1]) ||
        !isdigit((unsigned char)expiry[3]) || !isdigit((unsigned char)expiry[4])) return 0;

    int month = (expiry[0] - '0') * 10 + (expiry[1] - '0');
    if (month < 1 || month > 12) return 0;

    int year = (expiry[3] - '0') * 10 + (expiry[4] - '0');

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    if (!tm_now) return 0;

    int current_year = tm_now->tm_year % 100;
    int current_month = tm_now->tm_mon + 1;

    if (year < current_year) return 0;
    if (year == current_year && month < current_month) return 0;
    return 1;
}

static void animated_spinner(const char *label, int seconds)
{
    const char frames[] = { '-', '\\', '|', '/' };
    int ticks = seconds * 10;
    if (ticks < 1) ticks = 10;

    for (int i = 0; i < ticks; i++) {
        printf("\r  %c  %s...", frames[i % 4], label ? label : "Processing");
        fflush(stdout);
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
    printf("\r  \xE2\x9C\x93  %s complete.          \n", label ? label : "Processing");
}

static int insert_payment_row(int booking_id, int method, float amount,
                              int status, const char *method_detail)
{
    char initiated_at[32];
    char completed_at[32];
    get_current_datetime(initiated_at, sizeof(initiated_at));
    strncpy(completed_at, initiated_at, sizeof(completed_at) - 1);
    completed_at[sizeof(completed_at) - 1] = '\0';

    int32_t payment_id_val = 0;
    int32_t booking_id_val = booking_id;
    int32_t method_val = method;
    int32_t status_val = status;
    const char *detail = method_detail ? method_detail : "";

    wal_begin();
    int payment_id = db_insert("payments",
                               (void *[]){ &payment_id_val, &booking_id_val, &method_val,
                                           &amount, &status_val, (void *)detail,
                                           (void *)initiated_at, (void *)completed_at });
    wal_commit();
    return payment_id;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API: formatted payment method menu
 * ───────────────────────────────────────────────────────────────────────────*/
void print_payment_menu(const SessionContext *ctx, float total)
{
    float wallet = ctx ? ctx->wallet_balance : 0.0f;
    char total_buf[32];
    char wallet_buf[32];
    format_inr_amount(total, total_buf, sizeof(total_buf), 0);
    format_inr_amount(wallet, wallet_buf, sizeof(wallet_buf), 0);

    printf("\n");
    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  SELECT PAYMENT METHOD               Total: Rs.%-8s │\n", total_buf);
    printf("  ├──────────────────────────────────────────────────────┤\n");

    if (wallet + 0.001f < total) {
        float shortfall = total - wallet;
        char shortfall_buf[32];
        format_inr_amount(shortfall, shortfall_buf, sizeof(shortfall_buf), 0);
        printf("  │  [ 1 ]  Wallet  \033[31mRs.%-3s — INSUFFICIENT (need Rs.%-5s more)\033[0m │\n",
               wallet_buf, shortfall_buf);
    } else {
        printf("  │  [ 1 ]  Wallet       Rs.%-10s balance  INSTANT     │\n", wallet_buf);
    }

    printf("  │  [ 2 ]  UPI          name@upi            2s           │\n");
    printf("  │  [ 3 ]  Card         16-digit PAN        3s           │\n");
    printf("  │  [ 4 ]  Net Banking  10 banks available  2s           │\n");
    printf("  │  [ 0 ]  Cancel booking                               │\n");
    printf("  └──────────────────────────────────────────────────────┘\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * process_payment
 * Returns PAY_STATUS_SUCCESS / PAY_STATUS_FAILED / PAY_STATUS_RETRY
 * ───────────────────────────────────────────────────────────────────────────*/
int process_payment(int booking_id, PaymentMethod method, SessionContext *ctx)
{
    if (!ctx || ctx->user_id == 0) return PAY_STATUS_FAILED;

    float grand_total = ctx->current_price_breakdown[11];

    if (method == PAY_WALLET) {
        if (ctx->wallet_balance + 0.001f < grand_total) {
            float shortfall = grand_total - ctx->wallet_balance;
            printf("\033[31m  ✗ Wallet payment unavailable.\033[0m\n");
            printf("  Current balance: Rs.%.2f\n", ctx->wallet_balance);
            printf("  Need Rs.%.2f more.\n", shortfall);
            return PAY_STATUS_FAILED;
        }

        float new_balance = ctx->wallet_balance - grand_total;

        wal_begin();
        int32_t user_id_val = ctx->user_id;
        WhereClause where[] = {
            {.col_name = "user_id", .op = OP_EQ, .value = &user_id_val, .logic = 0}
        };

        int success = db_update("users", where, 1, "wallet_balance", &new_balance);
        if (success <= 0) {
            wal_rollback();
            printf("\033[31m  ✗ Failed to update wallet.\033[0m\n");
            return PAY_STATUS_FAILED;
        }
        wal_commit();

        ctx->wallet_balance = new_balance;

        int payment_id = insert_payment_row(booking_id, (int)PAY_WALLET,
                                            grand_total, (int)PAY_STATUS_SUCCESS, "");
        if (payment_id < 0) {
            printf("\033[31m  ✗ Failed to insert payment record.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        ctx->active_payment_id = payment_id;
        printf("\033[32m  ✓ Wallet payment successful.\033[0m New balance: Rs.%.2f\n", new_balance);
        return PAY_STATUS_SUCCESS;
    }

    if (method == PAY_UPI) {
        char vpa[100];
        printf("\n  Enter your UPI ID: ");
        fflush(stdout);
        read_line_local(vpa, sizeof(vpa));

        if (!validate_upi_vpa(vpa)) {
            printf("\033[31m✗ Invalid UPI ID format.\033[0m\n");
            printf("  Expected format: yourname@bankname\n");
            printf("  Example: rishi@hdfc or 9876543210@paytm\n");
            return PAY_STATUS_FAILED;
        }

        animated_spinner("Verifying UPI", 2);

        static int seeded = 0;
        if (!seeded) {
            srand((unsigned int)time(NULL));
            seeded = 1;
        }

        if ((rand() % 20) == 0) {
            insert_payment_row(booking_id, (int)PAY_UPI, grand_total, (int)PAY_STATUS_FAILED, vpa);

            printf("\033[31m  ✗ Payment declined by UPI provider.\033[0m\n");
            char ans[8];
            printf("  Would you like to try a different method? (y/n): ");
            read_line_local(ans, sizeof(ans));
            if (ans[0] == 'y' || ans[0] == 'Y') return PAY_STATUS_RETRY;
            return PAY_STATUS_FAILED;
        }

        int payment_id = insert_payment_row(booking_id, (int)PAY_UPI,
                                            grand_total, (int)PAY_STATUS_SUCCESS, vpa);
        if (payment_id < 0) {
            printf("\033[31m  ✗ Failed to insert payment record.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        ctx->active_payment_id = payment_id;
        printf("\033[32m  ✓ UPI payment successful.\033[0m\n");
        return PAY_STATUS_SUCCESS;
    }

    if (method == PAY_CARD) {
        char pan[20];
        char cvv[10];
        char expiry[10];

        printf("\n  Enter 16-digit card number: ");
        read_line_local(pan, sizeof(pan));
        if (strlen(pan) != 16 || !luhn_check(pan)) {
            printf("\033[31m  ✗ Invalid card number (16 digits, Luhn check failed).\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        printf("  Enter CVV (3-4 digits): ");
        read_line_local(cvv, sizeof(cvv));
        if (strlen(cvv) < 3 || strlen(cvv) > 4) {
            printf("\033[31m  ✗ Invalid CVV (must be 3-4 digits).\033[0m\n");
            return PAY_STATUS_FAILED;
        }
        for (int i = 0; cvv[i]; i++) {
            if (!isdigit((unsigned char)cvv[i])) {
                printf("\033[31m  ✗ Invalid CVV (digits only).\033[0m\n");
                return PAY_STATUS_FAILED;
            }
        }

        printf("  Enter expiry (MM/YY): ");
        read_line_local(expiry, sizeof(expiry));
        if (!validate_expiry(expiry)) {
            printf("\033[31m  ✗ Invalid or expired card.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        animated_spinner("Processing Card", 3);

        char masked_pan[20];
        snprintf(masked_pan, sizeof(masked_pan), "XXXXXXXXXXXX%c%c%c%c",
                 pan[12], pan[13], pan[14], pan[15]);

        int payment_id = insert_payment_row(booking_id, (int)PAY_CARD,
                                            grand_total, (int)PAY_STATUS_SUCCESS, masked_pan);
        if (payment_id < 0) {
            printf("\033[31m  ✗ Failed to insert payment record.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        ctx->active_payment_id = payment_id;
        printf("\033[32m  ✓ Card payment successful.\033[0m\n");
        return PAY_STATUS_SUCCESS;
    }

    if (method == PAY_NETBANKING) {
        const char *banks[] = {
            "HDFC Bank", "ICICI Bank", "Axis Bank", "SBI", "Kotak Mahindra",
            "IndusInd Bank", "Yes Bank", "IDBI Bank", "Canara Bank", "Bank of Baroda"
        };

        printf("\n  Select your bank:\n");
        for (int i = 0; i < 10; i++) printf("    %d. %s\n", i + 1, banks[i]);
        printf("  Enter choice (1-10): ");

        char choice_buf[16];
        read_line_local(choice_buf, sizeof(choice_buf));
        int choice = atoi(choice_buf);
        if (choice < 1 || choice > 10) {
            printf("\033[31m  ✗ Invalid choice.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        const char *selected_bank = banks[choice - 1];
        animated_spinner("Contacting Bank", 2);

        int payment_id = insert_payment_row(booking_id, (int)PAY_NETBANKING,
                                            grand_total, (int)PAY_STATUS_SUCCESS, selected_bank);
        if (payment_id < 0) {
            printf("\033[31m  ✗ Failed to insert payment record.\033[0m\n");
            return PAY_STATUS_FAILED;
        }

        ctx->active_payment_id = payment_id;
        printf("\033[32m  ✓ NetBanking payment successful via %s.\033[0m\n", selected_bank);
        return PAY_STATUS_SUCCESS;
    }

    printf("\033[31m  ✗ Unknown payment method.\033[0m\n");
    return PAY_STATUS_FAILED;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wallet_topup — with quick amount shortcuts
 * ───────────────────────────────────────────────────────────────────────────*/
int wallet_topup(SessionContext *ctx)
{
    if (!ctx || ctx->user_id == 0) {
        printf("\033[31m  ✗ You must be logged in to top up wallet.\033[0m\n");
        return 0;
    }

    printf("\n┌─ Wallet Top-up ─\n");
    printf("  Quick amounts: [1] Rs.500  [2] Rs.1,000  [3] Rs.2,500  [4] Custom\n");
    printf("  Select option: ");

    char opt_buf[16];
    read_line_local(opt_buf, sizeof(opt_buf));
    int opt = atoi(opt_buf);

    float amount = 0.0f;
    if (opt == 1) amount = 500.0f;
    else if (opt == 2) amount = 1000.0f;
    else if (opt == 3) amount = 2500.0f;
    else if (opt == 4) {
        printf("  Minimum: Rs.100  |  Maximum: Rs.50,000\n");
        printf("  Enter amount to add: Rs.");
        char amount_buf[20];
        read_line_local(amount_buf, sizeof(amount_buf));
        amount = (float)atof(amount_buf);
    } else {
        printf("\033[31m  ✗ Invalid choice.\033[0m\n");
        return 0;
    }

    if (amount < 100.0f || amount > 50000.0f) {
        printf("\033[31m  ✗ Amount must be between Rs.100 and Rs.50,000.\033[0m\n");
        return 0;
    }

    float new_balance = ctx->wallet_balance + amount;

    wal_begin();
    int32_t user_id_val = ctx->user_id;
    WhereClause where[] = {
        {.col_name = "user_id", .op = OP_EQ, .value = &user_id_val, .logic = 0}
    };

    int success = db_update("users", where, 1, "wallet_balance", &new_balance);
    wal_commit();

    if (success <= 0) {
        printf("\033[31m  ✗ Failed to update wallet.\033[0m\n");
        return 0;
    }

    ctx->wallet_balance = new_balance;

    char bal_fmt[32];
    format_inr_amount(new_balance, bal_fmt, sizeof(bal_fmt), 1);
    printf("\033[32m  ✓ Wallet topped up. New balance: Rs.%s\033[0m\n", bal_fmt);
    printf("\033[36m  Your next booking has Rs.%s available.\033[0m\n", bal_fmt);
    return 1;
}