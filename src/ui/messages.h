#ifndef MESSAGES_H
#define MESSAGES_H

typedef enum {
    MSG_ERROR,
    MSG_WARNING,
    MSG_INFO,
    MSG_SUCCESS
} MessageSeverity;

#define ERR_TMDB_001 "ERR_TMDB_001"
#define ERR_TMDB_002 "ERR_TMDB_002"
#define ERR_TMDB_003 "ERR_TMDB_003"
#define ERR_TMDB_004 "ERR_TMDB_004"
#define ERR_TMDB_005 "ERR_TMDB_005"
#define ERR_TMDB_006 "ERR_TMDB_006"
#define ERR_TMDB_007 "ERR_TMDB_007"
#define ERR_TMDB_008 "ERR_TMDB_008"

#define ERR_DB_001 "ERR_DB_001"
#define ERR_DB_002 "ERR_DB_002"
#define ERR_DB_003 "ERR_DB_003"
#define ERR_DB_004 "ERR_DB_004"
#define ERR_DB_005 "ERR_DB_005"
#define ERR_DB_006 "ERR_DB_006"
#define ERR_DB_007 "ERR_DB_007"

#define ERR_AUTH_001 "ERR_AUTH_001"
#define ERR_AUTH_002 "ERR_AUTH_002"
#define ERR_AUTH_003 "ERR_AUTH_003"
#define ERR_AUTH_004 "ERR_AUTH_004"
#define ERR_AUTH_005 "ERR_AUTH_005"
#define ERR_AUTH_006 "ERR_AUTH_006"

#define ERR_BOOK_001 "ERR_BOOK_001"
#define ERR_BOOK_002 "ERR_BOOK_002"
#define ERR_BOOK_003 "ERR_BOOK_003"
#define ERR_BOOK_004 "ERR_BOOK_004"
#define ERR_BOOK_005 "ERR_BOOK_005"
#define ERR_BOOK_006 "ERR_BOOK_006"

#define ERR_VAL_001 "ERR_VAL_001"
#define ERR_VAL_002 "ERR_VAL_002"
#define ERR_VAL_003 "ERR_VAL_003"
#define ERR_VAL_004 "ERR_VAL_004"

void show_message(MessageSeverity severity, const char *title, 
                  const char *message, const char **suggested_actions, 
                  int action_count);

void show_message_with_code(MessageSeverity severity, const char *title,
                            const char *message, const char **suggested_actions,
                            int action_count, const char *error_code);

void show_tmdb_auth_error(void);
void show_tmdb_network_error(void);
void show_tmdb_ssl_error(void);
void show_tmdb_rate_limit_error(void);
void show_tmdb_not_found_error(const char *resource_type);
void show_tmdb_server_error(void);

void show_db_transaction_error(void);
void show_db_connection_error(void);
void show_db_corruption_error(void);

void show_auth_invalid_credentials_error(void);
void show_auth_session_expired_error(void);
void show_auth_invalid_phone_error(void);
void show_auth_weak_password_error(void);
void show_auth_user_exists_error(void);

void show_booking_seat_unavailable_error(const char *seat);
void show_booking_insufficient_balance_error(double balance, double required);
void show_booking_show_unavailable_error(void);
void show_booking_payment_failed_error(void);

void show_validation_error(const char *field, const char *expected);

void show_success(const char *message);
void show_warning(const char *message);
void show_info(const char *message);

#endif /* MESSAGES_H */
