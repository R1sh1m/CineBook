/*
 * auth.c — Authentication module for CineBook
 * C11. No OpenSSL. No scanf for strings. No SQLite. No threads.
 *
 * Dependencies (include order matters for circular-dep avoidance):
 *   query.h  → schema.h → record.h   (the full RDBMS API)
 *   auth.h                            (our own header, forward-decls SessionContext)
 *   session.h                         (full SessionContext definition — internal only)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "query.h"
#include "session.h"
#include "location.h"
#include "txn.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — SHA-256  (FIPS 180-4, pure C, ~85 lines)
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Initial hash values H0..H7 — first 32 bits of the fractional parts of the
 * square roots of the first 8 primes. */
static const uint32_t SHA256_H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

/* Round constants K[0..63] — first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes. */
static const uint32_t SHA256_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROTR32(x, n)  ( ((x) >> (n)) | ((x) << (32u - (n))) )
#define CH(e,f,g)     ( ((e) & (f)) ^ (~(e) & (g)) )
#define MAJ(a,b,c)    ( ((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)) )
#define EP0(a)        ( ROTR32(a,2)  ^ ROTR32(a,13) ^ ROTR32(a,22) )
#define EP1(e)        ( ROTR32(e,6)  ^ ROTR32(e,11) ^ ROTR32(e,25) )
#define SIG0(x)       ( ROTR32(x,7)  ^ ROTR32(x,18) ^ ((x) >> 3)  )
#define SIG1(x)       ( ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10) )

void sha256_hex(const char *input, char out_buf[65])
{
    /* --- 1. Length and bit count ---------------------------------------- */
    size_t msg_len   = strlen(input);
    uint64_t bit_len = (uint64_t)msg_len * 8u;

    /* Padded message length: msg_len + 1 (0x80) + zeros + 8 bytes bit_len,
     * rounded up to a multiple of 64. */
    size_t padded = msg_len + 1;
    while (padded % 64 != 56) padded++;
    padded += 8;   /* add the 64-bit big-endian length */

    /* --- 2. Build padded message on the heap ----------------------------- */
    uint8_t *m = (uint8_t *)calloc(padded, 1);
    if (!m) { memset(out_buf, '0', 64); out_buf[64] = '\0'; return; }

    memcpy(m, input, msg_len);
    m[msg_len] = 0x80u;
    /* bit_len in big-endian at the last 8 bytes */
    for (int i = 0; i < 8; i++) {
        m[padded - 8 + i] = (uint8_t)((bit_len >> (56u - 8u * (unsigned)i)) & 0xFFu);
    }

    /* --- 3. Initialise hash state --------------------------------------- */
    uint32_t h[8];
    memcpy(h, SHA256_H0, sizeof(h));

    /* --- 4. Process each 512-bit (64-byte) chunk ----------------------- */
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[64];
        /* Prepare message schedule */
        for (int i = 0; i < 16; i++) {
            w[i]  = ((uint32_t)m[chunk + i*4 + 0] << 24u)
                  | ((uint32_t)m[chunk + i*4 + 1] << 16u)
                  | ((uint32_t)m[chunk + i*4 + 2] <<  8u)
                  | ((uint32_t)m[chunk + i*4 + 3]       );
        }
        for (int i = 16; i < 64; i++) {
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
        }
        /* Working variables */
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3],
                 e=h[4], f=h[5], g=h[6], hh=h[7];
        /* 64 rounds */
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e,f,g) + SHA256_K[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a,b,c);
            hh = g; g = f; f = e; e = d + t1;
            d  = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    free(m);

    /* --- 5. Produce hex output ------------------------------------------ */
    for (int i = 0; i < 8; i++) {
        snprintf(out_buf + i*8, 9, "%08x", h[i]);
    }
    out_buf[64] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — Input helpers
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Read a line from stdin into buf[size], strip trailing newline.
 * Returns 1 on success, 0 on EOF/error. */
static int read_line(char *buf, int size)
{
    if (!fgets(buf, size, stdin)) return 0;
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    if (len > 0 && buf[len-1] == '\r') buf[--len] = '\0';
    return 1;
}

/* Validate phone: exactly 10 ASCII digit characters, nothing else. */
static int validate_phone(const char *phone)
{
    if (strlen(phone) != 10) return 0;
    for (int i = 0; i < 10; i++) {
        if (!isdigit((unsigned char)phone[i])) return 0;
    }
    return 1;
}

/* Validate email: at least one char before exactly one '@',
 * at least one char after '@', and a '.' somewhere in the domain. */
static int validate_email(const char *email)
{
    const char *at = strchr(email, '@');
    if (!at)               return 0;   /* no @ */
    if (at == email)       return 0;   /* empty local part */
    if (strchr(at+1, '@')) return 0;   /* second @ */
    const char *domain = at + 1;
    if (strlen(domain) < 3) return 0;  /* too short to be valid */
    if (!strchr(domain, '.')) return 0; /* no dot in domain */
    return 1;
}

/* Password must include at least one digit. */
static int password_has_digit(const char *password)
{
    for (int i = 0; password[i]; i++) {
        if (isdigit((unsigned char)password[i])) return 1;
    }
    return 0;
}

/* Resolve city name for summary card. */
static void get_city_name_by_id(int city_id, char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    strncpy(out, "Unknown", (size_t)(out_size - 1));
    out[out_size - 1] = '\0';

    WhereClause w;
    memset(&w, 0, sizeof(w));
    strncpy(w.col_name, "city_id", sizeof(w.col_name)-1);
    w.op = OP_EQ;
    w.value = &city_id;
    w.logic = 0;

    ResultSet *rs = db_select("cities", &w, 1, NULL, 0);
    if (rs && rs->row_count > 0) {
        char *name = (char *)rs->rows[0][1];
        if (name && name[0] != '\0') {
            strncpy(out, name, (size_t)(out_size - 1));
            out[out_size - 1] = '\0';
        }
    }
    if (rs) result_set_free(rs);
}

/* Extract the domain part (everything after '@') into out[out_size]. */
static void extract_domain(const char *email, char *out, int out_size)
{
    const char *at = strchr(email, '@');
    if (!at) { out[0] = '\0'; return; }
    strncpy(out, at + 1, (size_t)(out_size - 1));
    out[out_size - 1] = '\0';
    /* lowercase for case-insensitive comparison */
    for (int i = 0; out[i]; i++) out[i] = (char)tolower((unsigned char)out[i]);
}

/* Check domain against academic_domains table.
 * Returns 1 if found and active, 0 otherwise. */
static int domain_is_academic(const char *domain)
{
    char dom_lower[128];
    strncpy(dom_lower, domain, sizeof(dom_lower)-1);
    dom_lower[sizeof(dom_lower)-1] = '\0';
    for (int i = 0; dom_lower[i]; i++)
        dom_lower[i] = (char)tolower((unsigned char)dom_lower[i]);

    WhereClause w;
    memset(&w, 0, sizeof(w));
    strncpy(w.col_name, "domain", sizeof(w.col_name)-1);
    w.op    = OP_EQ;
    w.value = dom_lower;
    w.logic = 0;

    ResultSet *rs = db_select("academic_domains", &w, 1, NULL, 0);
    int found = (rs && rs->row_count > 0) ? 1 : 0;
    if (rs) result_set_free(rs);
    return found;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — login()
 * ═══════════════════════════════════════════════════════════════════════════*/

int login(SessionContext *ctx)
{
    char phone[32];
    char password[256];
    char hash_buf[65];

    printf("\033[2J\033[H");
    printf("  ── Sign In ──────────────────────────────────────\n");

    for (int attempt = 1; attempt <= 3; attempt++) {
        printf("\n  Phone number : ");
        fflush(stdout);
        if (!read_line(phone, sizeof(phone))) return 0;

        if (!validate_phone(phone)) {
            printf("  \033[31m✗ Phone must be exactly 10 digits.\033[0m\n");
            continue;
        }

        printf("  \033[32m✓ Phone accepted\033[0m\n");
        printf("  Password     : ");
        fflush(stdout);
        if (!read_line(password, sizeof(password))) return 0;

        sha256_hex(password, hash_buf);
        memset(password, 0, sizeof(password));

        WhereClause w;
        memset(&w, 0, sizeof(w));
        strncpy(w.col_name, "phone", sizeof(w.col_name)-1);
        w.op    = OP_EQ;
        w.value = phone;
        w.logic = 0;

        ResultSet *rs = db_select("users", &w, 1, NULL, 0);
        if (!rs || rs->row_count == 0) {
            if (rs) result_set_free(rs);
            printf("  \033[31m✗ No account found for that phone number.\033[0m\n");
            continue;
        }

        int   is_active   = *((int32_t *)rs->rows[0][10]);
        char *stored_hash = (char *)rs->rows[0][4];
        int   user_id     = *((int32_t *)rs->rows[0][0]);

        if (!is_active) {
            result_set_free(rs);
            printf("  \033[31m✗ This account is deactivated. Contact support.\033[0m\n");
            continue;
        }

        if (strncmp(hash_buf, stored_hash, 64) != 0) {
            result_set_free(rs);
            printf("  \033[33m✗ Incorrect password  (attempt %d of 3)\033[0m\n", attempt);
            if (attempt == 3) {
                printf("  \033[31m✗ Account locked for this session. Please try again.\033[0m\n");
                sleep(2);
                return 0;
            }
            continue;
        }

        result_set_free(rs);
        session_set_user(ctx, user_id);

        if (ctx->preferred_city_id == 0) {
            pick_city(ctx);
        }

        printf("\033[2J\033[H");
        printf("\033[32m\n  ✓ Authentication successful\033[0m\n");
        printf("\n  Welcome back, \033[1m%s\033[0m!\n", ctx->name);
        return 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — signup()
 * ═══════════════════════════════════════════════════════════════════════════*/

int signup(SessionContext *ctx)
{
    char name[101];
    char phone[32];
    char password[256];
    char confirm[256];
    char email[151];
    char hash_buf[65];
    char created_at[32];
    char domain[128];
    char city_name[64];

    printf("\033[2J\033[H");
    printf("  ── Create Account ─────────────────────────────────\n");

    printf("  Step 1 of 5 — Tell us your name\n");
    printf("  Full name    : ");
    fflush(stdout);
    if (!read_line(name, sizeof(name))) return 0;
    if (strlen(name) < 2) {
        printf("  \033[31m✗ Name must be at least 2 characters.\033[0m\n");
        return 0;
    }
    printf("  \033[32m✓ Name saved\033[0m\n");

    printf("\n  Step 2 of 5 — Set password\n");
    printf("  Password     : ");
    fflush(stdout);
    if (!read_line(password, sizeof(password))) return 0;
    if (strlen(password) < 6) {
        memset(password, 0, sizeof(password));
        printf("  \033[31m✗ Password must be at least 6 characters.\033[0m\n");
        return 0;
    }
    if (!password_has_digit(password)) {
        memset(password, 0, sizeof(password));
        printf("  \033[31m✗ Password must contain at least one number.\033[0m\n");
        return 0;
    }

    printf("  Confirm pwd  : ");
    fflush(stdout);
    if (!read_line(confirm, sizeof(confirm))) {
        memset(password, 0, sizeof(password));
        return 0;
    }
    if (strcmp(password, confirm) != 0) {
        memset(password, 0, sizeof(password));
        memset(confirm, 0, sizeof(confirm));
        printf("  \033[31m✗ Passwords do not match.\033[0m\n");
        return 0;
    }
    memset(confirm, 0, sizeof(confirm));
    sha256_hex(password, hash_buf);
    memset(password, 0, sizeof(password));
    printf("  \033[32m✓ Password secured\033[0m\n");

    printf("\n  Step 3 of 5 — Add mobile number\n");
    int phone_verified = 0;
    for (int phone_attempt = 1; phone_attempt <= 3; phone_attempt++) {
        printf("  Phone number : ");
        fflush(stdout);
        if (!read_line(phone, sizeof(phone))) return 0;

        if (!validate_phone(phone)) {
            printf("  \033[31m✗ Phone must be exactly 10 digits.\033[0m\n");
            if (phone_attempt == 3) {
                printf("  \033[31m✗ Too many phone attempts. Signup cancelled.\033[0m\n");
                return 0;
            }
            continue;
        }

        WhereClause w;
        memset(&w, 0, sizeof(w));
        strncpy(w.col_name, "phone", sizeof(w.col_name)-1);
        w.op    = OP_EQ;
        w.value = phone;
        w.logic = 0;

        int exists = db_count("users", &w, 1);
        if (exists > 0) {
            printf("  \033[31m✗ That phone number is already registered.\033[0m\n");
            printf("    (Tip: demo accounts use 9876543210, 9845123456, etc.)\n");
            printf("    Please use a different number.\n");
            if (phone_attempt == 3) {
                printf("  \033[31m✗ Too many phone attempts. Signup cancelled.\033[0m\n");
                return 0;
            }
            continue;
        }

        phone_verified = 1;
        break;
    }

    if (!phone_verified) {
        return 0;
    }

    printf("  \033[32m✓ Phone verified\033[0m\n");

    printf("\n  Step 4 of 5 — Add email (optional)\n");
    printf("  Email (optional, press Enter to skip): ");
    fflush(stdout);
    if (!read_line(email, sizeof(email))) return 0;

    int has_email = (strlen(email) > 0);
    int role = ROLE_USER;

    if (has_email) {
        if (!validate_email(email)) {
            printf("  \033[31m✗ Invalid email format.\033[0m\n");
            return 0;
        }
        printf("  \033[32m✓ Email saved\033[0m\n");
        extract_domain(email, domain, sizeof(domain));
        if (domain_is_academic(domain)) {
            role = ROLE_STUDENT;
            printf("  \033[36m\033[1m✓ Academic domain detected! Student discount unlocked.\033[0m\n");
        }
    } else {
        printf("  \033[32m✓ Skipped email for now\033[0m\n");
    }

    printf("\n  Step 5 of 5 — Choose your city\n");
    SessionContext tmp_ctx;
    session_init(&tmp_ctx);
    pick_city(&tmp_ctx);

    int preferred_city_id = tmp_ctx.preferred_city_id;
    if (preferred_city_id == 0) preferred_city_id = 1;
    printf("  \033[32m✓ City preference recorded\033[0m\n");

    int country_id = get_country_for_city(preferred_city_id);
    if (country_id < 0) country_id = 1;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(created_at, sizeof(created_at), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    int32_t v_user_id         = 0;
    char    v_name[101];
    char    v_phone[16];
    char    v_email[151];
    char    v_hash[65];
    int32_t v_role            = (int32_t)role;
    float   v_wallet          = 0.0f;
    int32_t v_city            = (int32_t)preferred_city_id;
    int32_t v_country         = (int32_t)country_id;
    char    v_created_at[21];
    int32_t v_is_active       = 1;

    strncpy(v_name, name, sizeof(v_name)-1); v_name[sizeof(v_name)-1] = '\0';
    strncpy(v_phone, phone, sizeof(v_phone)-1); v_phone[sizeof(v_phone)-1] = '\0';
    strncpy(v_email, has_email ? email : "", sizeof(v_email)-1); v_email[sizeof(v_email)-1] = '\0';
    strncpy(v_hash, hash_buf, sizeof(v_hash)-1); v_hash[sizeof(v_hash)-1] = '\0';
    strncpy(v_created_at, created_at, sizeof(v_created_at)-1); v_created_at[sizeof(v_created_at)-1] = '\0';

    void *fields[11] = {
        &v_user_id,
        v_name,
        v_phone,
        v_email,
        v_hash,
        &v_role,
        &v_wallet,
        &v_city,
        &v_country,
        v_created_at,
        &v_is_active
    };

    wal_begin();
    int new_id = db_insert("users", fields);
    if (new_id < 0) {
        wal_rollback();
        printf("  \033[31m✗ Failed to create account. Please try again.\033[0m\n");
        return 0;
    }
    wal_commit();

    session_set_user(ctx, new_id);
    get_city_name_by_id(ctx->preferred_city_id, city_name, sizeof(city_name));

    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║      Account Created Successfully    ║\n");
    printf("  ╠══════════════════════════════════════╣\n");
    printf("  ║  Name   : %-27.27s║\n", ctx->name);
    printf("  ║  Role   : %-27.27s║\n", (ctx->role == ROLE_STUDENT) ? "STUDENT" : "USER");
    printf("  ║  City   : %-27.27s║\n", city_name);
    printf("  ╚══════════════════════════════════════╝\n");

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — show_auth_menu()
 * ═══════════════════════════════════════════════════════════════════════════*/

void show_auth_menu(SessionContext *ctx)
{
    char buf[16];

    for (;;) {
        printf("\033[2J\033[H");
        fflush(stdout);

        printf("  ╔═════════════════════════════════════════════════════════════════╗\n");
        printf("  ║                                                                 ║\n");
        printf("  ║   ██████╗██╗███╗   ██╗███████╗██████╗  ██████╗  ██████╗ ██╗  ██╗║\n");
        printf("  ║  ██╔════╝██║████╗  ██║██╔════╝██╔══██╗██╔═══██╗██╔═══██╗██║ ██╔╝║\n");
        printf("  ║  ██║     ██║██╔██╗ ██║█████╗  ██████╔╝██║   ██║██║   ██║█████╔╝ ║\n");
        printf("  ║  ██║     ██║██║╚██╗██║██╔══╝  ██╔══██╗██║   ██║██║   ██║██╔═██╗ ║\n");
        printf("  ║  ╚██████╗██║██║ ╚████║███████╗██████╔╝╚██████╔╝╚██████╔╝██║  ██╗║\n");
        printf("  ║   ╚═════╝╚═╝╚═╝  ╚═══╝╚══════╝╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝║\n");
        printf("  ║                                                                 ║\n");
        printf("  ║               Your Cinema. Your Seats. Your Story.              ║\n");
        printf("  ╚═════════════════════════════════════════════════════════════════╝\n");

        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │                                                                 │\n");
        printf("  │    [ 1 ]  Login                     (If Already have an account)│\n");
        printf("  │                                                                 │\n");
        printf("  │    [ 2 ]  Create an Account         (Join CineBook today)       │\n");
        printf("  │                                                                 │\n");
        printf("  │    [ 3 ]  Exit                                                  │\n");
        printf("  │                                                                 │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");
        printf("                          Choose an Option: ");
        fflush(stdout);

        if (!read_line(buf, sizeof(buf))) continue;

        int choice = atoi(buf);
        switch (choice) {
            case 1:
                if (login(ctx)) return;
                break;
            case 2:
                if (signup(ctx)) return;
                break;
            case 3:
                ctx->user_id = 0;
                return;
            default:
                printf("  \033[31m✗ Enter 1, 2, or 3.\033[0m\n");
                sleep(1);
                break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — upgrade_to_student()
 * ═══════════════════════════════════════════════════════════════════════════*/

int upgrade_to_student(SessionContext *ctx)
{
    if (ctx->role == ROLE_STUDENT) {
        printf("  Your account is already a Student account.\n");
        return 0;
    }

    char email[151];
    char domain[128];

    /* If user has no email on file, prompt for one */
    if (strlen(ctx->email) == 0) {
        printf("  Enter your academic email address: ");
        fflush(stdout);
        if (!read_line(email, sizeof(email))) return 0;

        if (!validate_email(email)) {
            printf("  ✗ Invalid email format.\n");
            return 0;
        }
    } else {
        strncpy(email, ctx->email, sizeof(email)-1);
        email[sizeof(email)-1] = '\0';
        printf("  Checking email on file: %s\n", email);
    }

    extract_domain(email, domain, sizeof(domain));

    if (!domain_is_academic(domain)) {
        printf("  ✗ Domain '%s' is not registered as an academic institution.\n", domain);
        return 0;
    }

    /* Update email column */
    int32_t uid = (int32_t)ctx->user_id;
    WhereClause w_uid;
    memset(&w_uid, 0, sizeof(w_uid));
    strncpy(w_uid.col_name, "user_id", sizeof(w_uid.col_name)-1);
    w_uid.op    = OP_EQ;
    w_uid.value = &uid;
    w_uid.logic = 0;

    char v_email[151];
    strncpy(v_email, email, sizeof(v_email)-1);
    v_email[sizeof(v_email)-1] = '\0';

    db_update("users", &w_uid, 1, "email", v_email);

    /* Update role column */
    int32_t new_role = (int32_t)ROLE_STUDENT;
    db_update("users", &w_uid, 1, "role", &new_role);

    /* Refresh session */
    ctx->role = ROLE_STUDENT;
    strncpy(ctx->email, email, sizeof(ctx->email)-1);
    ctx->email[sizeof(ctx->email)-1] = '\0';

    printf("  ✓ Upgraded to Student account. −12%% discount is now active.\n");
    return 1;
}