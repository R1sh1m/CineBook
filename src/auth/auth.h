#ifndef AUTH_H
#define AUTH_H

/*
 * auth.h — Authentication module for CineBook
 *
 * Declares the UserRole enum, a forward declaration of SessionContext,
 * and all public auth functions.
 *
 * NOTE: session.h is NOT included here to avoid circular dependencies.
 *       auth.c includes session.h internally to access the full struct.
 */

/* ─────────────────────────────────────────────────────────────────────────────
 * UserRole — integer-backed enum stored in users.role
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    ROLE_GUEST   = 0,
    ROLE_USER    = 1,
    ROLE_STUDENT = 2,
    ROLE_ADMIN   = 3
} UserRole;

/* ─────────────────────────────────────────────────────────────────────────────
 * Forward declaration — full definition lives in session.h
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct SessionContext SessionContext;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * show_auth_menu — display the top-level auth loop.
 * Shows: 1=Login / 2=Sign Up / 3=Exit.
 * Loops until the user authenticates successfully or chooses Exit.
 * On successful auth ctx is fully populated via session_set_user().
 */
void show_auth_menu(SessionContext *ctx);

/*
 * login — prompt phone + password, compare sha256_hex(entered) to stored hash.
 * Calls session_set_user(ctx, user_id) on success.
 * Returns 1 on success, 0 after 3 failed attempts.
 */
int login(SessionContext *ctx);

/*
 * signup — collect name, phone, password (confirm), optional email.
 * Domain match → ROLE_STUDENT, else ROLE_USER.
 * Calls session_set_user(ctx, new_user_id) on success.
 * Returns 1 on success, 0 on any validation or db error.
 */
int signup(SessionContext *ctx);

/*
 * sha256_hex — pure C FIPS 180-4 SHA-256, no external crypto lib.
 * Writes lowercase 64-char hex + null terminator into out_buf[65].
 */
void sha256_hex(const char *input, char out_buf[65]);

/*
 * upgrade_to_student — prompt for email if not already set, check
 * academic_domains, db_update email + role, refresh ctx->role.
 * Returns 1 if upgraded, 0 if domain not found / already student.
 */
int upgrade_to_student(SessionContext *ctx);

#endif /* AUTH_H */