#ifndef MESSAGES_H
#define MESSAGES_H

typedef enum {
    MSG_ERROR,    // Red - critical errors
    MSG_WARNING,  // Yellow - warnings
    MSG_INFO,     // Blue/Cyan - informational
    MSG_SUCCESS   // Green - success messages
} MessageSeverity;

// Error code constants - TMDB API errors
#define ERR_TMDB_001 "ERR_TMDB_001"  // Invalid API key
#define ERR_TMDB_002 "ERR_TMDB_002"  // Network timeout
#define ERR_TMDB_003 "ERR_TMDB_003"  // SSL certificate error
#define ERR_TMDB_004 "ERR_TMDB_004"  // Rate limit exceeded
#define ERR_TMDB_005 "ERR_TMDB_005"  // Resource not found
#define ERR_TMDB_006 "ERR_TMDB_006"  // Server error
#define ERR_TMDB_007 "ERR_TMDB_007"  // Invalid request
#define ERR_TMDB_008 "ERR_TMDB_008"  // Network unavailable

// Database errors
#define ERR_DB_001 "ERR_DB_001"      // Transaction failed
#define ERR_DB_002 "ERR_DB_002"      // Constraint violation
#define ERR_DB_003 "ERR_DB_003"      // Connection failed
#define ERR_DB_004 "ERR_DB_004"      // Corruption detected
#define ERR_DB_005 "ERR_DB_005"      // Lock timeout
#define ERR_DB_006 "ERR_DB_006"      // Disk full
#define ERR_DB_007 "ERR_DB_007"      // Permission denied

// Authentication errors
#define ERR_AUTH_001 "ERR_AUTH_001"  // Invalid credentials
#define ERR_AUTH_002 "ERR_AUTH_002"  // Session expired
#define ERR_AUTH_003 "ERR_AUTH_003"  // Account locked
#define ERR_AUTH_004 "ERR_AUTH_004"  // Phone number invalid
#define ERR_AUTH_005 "ERR_AUTH_005"  // Password requirements not met
#define ERR_AUTH_006 "ERR_AUTH_006"  // User already exists

// Booking errors
#define ERR_BOOK_001 "ERR_BOOK_001"  // Seat unavailable
#define ERR_BOOK_002 "ERR_BOOK_002"  // Insufficient balance
#define ERR_BOOK_003 "ERR_BOOK_003"  // Show not available
#define ERR_BOOK_004 "ERR_BOOK_004"  // Invalid seat selection
#define ERR_BOOK_005 "ERR_BOOK_005"  // Booking limit exceeded
#define ERR_BOOK_006 "ERR_BOOK_006"  // Payment failed

// Validation errors
#define ERR_VAL_001 "ERR_VAL_001"    // Invalid input format
#define ERR_VAL_002 "ERR_VAL_002"    // Required field missing
#define ERR_VAL_003 "ERR_VAL_003"    // Value out of range
#define ERR_VAL_004 "ERR_VAL_004"    // Invalid date/time

// Display a formatted message with optional suggested actions
void show_message(MessageSeverity severity, const char *title, 
                  const char *message, const char **suggested_actions, 
                  int action_count);

// Display a message with an error code
void show_message_with_code(MessageSeverity severity, const char *title,
                            const char *message, const char **suggested_actions,
                            int action_count, const char *error_code);

// Template functions for common error scenarios
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

// Success messages
void show_success(const char *message);
void show_warning(const char *message);
void show_info(const char *message);

#endif // MESSAGES_H
