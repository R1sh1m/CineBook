#include "messages.h"
#include <stdio.h>
#include <string.h>

#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"

#define ICON_ERROR    "✗"
#define ICON_WARNING  "⚠"
#define ICON_INFO     "ℹ"
#define ICON_SUCCESS  "✓"

static const char* get_color(MessageSeverity severity) {
    switch (severity) {
        case MSG_ERROR:   return COLOR_RED;
        case MSG_WARNING: return COLOR_YELLOW;
        case MSG_INFO:    return COLOR_CYAN;
        case MSG_SUCCESS: return COLOR_GREEN;
        default:          return COLOR_RESET;
    }
}

static const char* get_icon(MessageSeverity severity) {
    switch (severity) {
        case MSG_ERROR:   return ICON_ERROR;
        case MSG_WARNING: return ICON_WARNING;
        case MSG_INFO:    return ICON_INFO;
        case MSG_SUCCESS: return ICON_SUCCESS;
        default:          return "";
    }
}

static void print_separator(void) {
    printf("%s───────────────────────────────────────%s\n", COLOR_BOLD, COLOR_RESET);
}

void show_message(MessageSeverity severity, const char *title, 
                  const char *message, const char **suggested_actions, 
                  int action_count) {
    show_message_with_code(severity, title, message, suggested_actions, action_count, NULL);
}

void show_message_with_code(MessageSeverity severity, const char *title,
                            const char *message, const char **suggested_actions,
                            int action_count, const char *error_code) {
    const char *color = get_color(severity);
    const char *icon = get_icon(severity);
    
    printf("\n");
    print_separator();
    
    printf("%s%s %s%s\n", color, icon, title, COLOR_RESET);
    print_separator();
    
    if (message && strlen(message) > 0) {
        printf("%s\n", message);
        printf("\n");
    }
    
    if (suggested_actions && action_count > 0) {
        printf("%sWhat to do:%s\n", COLOR_BOLD, COLOR_RESET);
        for (int i = 0; i < action_count; i++) {
            printf("  %s\n", suggested_actions[i]);
        }
        printf("\n");
    }
    
    print_separator();
    if (severity == MSG_ERROR || severity == MSG_WARNING) {
        printf("Need help? Check the troubleshooting guide:\n");
        printf("%shttps://github.com/yourusername/cinebook#troubleshooting%s\n", 
               COLOR_CYAN, COLOR_RESET);
        if (error_code) {
            printf("Error code: %s%s%s\n", COLOR_BOLD, error_code, COLOR_RESET);
        }
        print_separator();
    }
    printf("\n");
}

void show_tmdb_auth_error(void) {
    const char *actions[] = {
        "1. Visit https://www.themoviedb.org/settings/api",
        "2. Generate a new API key",
        "3. Run 'Admin Settings > TMDB Configuration' to update your key"
    };
    show_message_with_code(MSG_ERROR, "Invalid TMDB API Key",
                          "Your TMDB API key is invalid or has expired.",
                          actions, 3, ERR_TMDB_001);
}

void show_tmdb_network_error(void) {
    const char *actions[] = {
        "1. Check your internet connection",
        "2. Verify proxy settings if behind a firewall",
        "3. Try again in a few moments"
    };
    show_message_with_code(MSG_ERROR, "Network Connection Failed",
                          "Unable to connect to TMDB servers. Please check your network.",
                          actions, 3, ERR_TMDB_002);
}

void show_tmdb_ssl_error(void) {
    const char *actions[] = {
        "1. Check your system time is correct",
        "2. Install ca-certificates package if missing",
        "3. Update curl/openssl to the latest version"
    };
    show_message_with_code(MSG_ERROR, "SSL Certificate Error",
                          "Could not verify TMDB SSL certificate. Your connection may be insecure.",
                          actions, 3, ERR_TMDB_003);
}

void show_tmdb_rate_limit_error(void) {
    const char *actions[] = {
        "1. Wait 10 seconds before trying again",
        "2. Reduce the frequency of TMDB API calls",
        "3. Consider upgrading your TMDB API plan if needed"
    };
    show_message_with_code(MSG_WARNING, "TMDB Rate Limit Exceeded",
                          "You've made too many requests to TMDB. Please wait a moment.",
                          actions, 3, ERR_TMDB_004);
}

void show_tmdb_not_found_error(const char *resource_type) {
    char message[256];
    snprintf(message, sizeof(message), "The requested %s was not found in TMDB database.", 
             resource_type ? resource_type : "resource");
    
    const char *actions[] = {
        "1. Check the movie/show ID is correct",
        "2. Try searching with a different title",
        "3. Verify the resource exists on themoviedb.org"
    };
    show_message_with_code(MSG_WARNING, "Resource Not Found",
                          message, actions, 3, ERR_TMDB_005);
}

void show_tmdb_server_error(void) {
    const char *actions[] = {
        "1. Try again in a few minutes",
        "2. Check TMDB status at https://status.themoviedb.org",
        "3. If problem persists, report to TMDB support"
    };
    show_message_with_code(MSG_ERROR, "TMDB Server Error",
                          "TMDB servers are experiencing issues. This is not a problem with Cinebook.",
                          actions, 3, ERR_TMDB_006);
}

void show_db_transaction_error(void) {
    const char *actions[] = {
        "1. Try the operation again",
        "2. Check if the database file is corrupted",
        "3. Restart the application if problem persists"
    };
    show_message_with_code(MSG_ERROR, "Database Transaction Failed",
                          "The database operation could not be completed.",
                          actions, 3, ERR_DB_001);
}

void show_db_connection_error(void) {
    const char *actions[] = {
        "1. Ensure the database file exists in data/",
        "2. Check file permissions (should be read/write)",
        "3. Verify disk is not full",
        "4. Run database integrity check"
    };
    show_message_with_code(MSG_ERROR, "Database Connection Failed",
                          "Cannot open or connect to the database.",
                          actions, 4, ERR_DB_003);
}

void show_db_corruption_error(void) {
    const char *actions[] = {
        "1. IMMEDIATELY backup your database file",
        "2. Run: sqlite3 data/cinebook.db '.recover' > recovered.sql",
        "3. Contact support with the error details",
        "4. Restore from backup if available"
    };
    show_message_with_code(MSG_ERROR, "Database Corruption Detected",
                          "CRITICAL: The database appears to be corrupted!",
                          actions, 4, ERR_DB_004);
}

void show_auth_invalid_credentials_error(void) {
    const char *actions[] = {
        "1. Verify your phone number (must be 10 digits)",
        "2. Check your password (case-sensitive)",
        "3. Use 'Forgot Password?' if you can't remember it"
    };
    show_message_with_code(MSG_ERROR, "Login Failed",
                          "Invalid phone number or password. Please check your credentials.",
                          actions, 3, ERR_AUTH_001);
}

void show_auth_session_expired_error(void) {
    const char *actions[] = {
        "1. Log in again to continue",
        "2. Your data has been saved automatically"
    };
    show_message_with_code(MSG_WARNING, "Session Expired",
                          "Your session has expired for security. Please log in again.",
                          actions, 2, ERR_AUTH_002);
}

void show_auth_invalid_phone_error(void) {
    const char *actions[] = {
        "1. Enter exactly 10 digits (no spaces or dashes)",
        "2. Example: 9876543210",
        "3. Don't include country code (+91)"
    };
    show_message_with_code(MSG_ERROR, "Invalid Phone Number",
                          "Phone number must be exactly 10 digits.",
                          actions, 3, ERR_AUTH_004);
}

void show_auth_weak_password_error(void) {
    const char *actions[] = {
        "1. Use at least 8 characters",
        "2. Include uppercase and lowercase letters",
        "3. Add at least one number",
        "4. Include at least one special character (!@#$%^&*)"
    };
    show_message_with_code(MSG_ERROR, "Password Too Weak",
                          "Your password doesn't meet security requirements.",
                          actions, 4, ERR_AUTH_005);
}

void show_auth_user_exists_error(void) {
    const char *actions[] = {
        "1. Try logging in instead of registering",
        "2. Use 'Forgot Password?' to reset your password",
        "3. Contact support if you believe this is an error"
    };
    show_message_with_code(MSG_ERROR, "Account Already Exists",
                          "An account with this phone number already exists.",
                          actions, 3, ERR_AUTH_006);
}

// Booking errors
void show_booking_seat_unavailable_error(const char *seat) {
    char message[256];
    snprintf(message, sizeof(message), 
             "Seat %s is already booked or being held by another user.", 
             seat ? seat : "");
    
    const char *actions[] = {
        "1. Choose a different seat from the seating chart",
        "2. Refresh the seat availability",
        "3. Try a different showtime if all seats are taken"
    };
    show_message_with_code(MSG_WARNING, "Seat Not Available",
                          message, actions, 3, ERR_BOOK_001);
}

void show_booking_insufficient_balance_error(double balance, double required) {
    char message[512];
    double needed = required - balance;
    snprintf(message, sizeof(message),
             "Wallet balance: ₹%.2f\nRequired: ₹%.2f\nPlease add ₹%.2f to continue.",
             balance, required, needed);
    
    const char *actions[] = {
        "1. Add money to your wallet from Account menu",
        "2. Choose seats in a lower price category",
        "3. Apply any available coupons or discounts"
    };
    show_message_with_code(MSG_ERROR, "Insufficient Wallet Balance",
                          message, actions, 3, ERR_BOOK_002);
}

void show_booking_show_unavailable_error(void) {
    const char *actions[] = {
        "1. Check if the show time has already passed",
        "2. Browse available shows from the main menu",
        "3. Try a different date or time"
    };
    show_message_with_code(MSG_WARNING, "Show Not Available",
                          "This show is no longer available for booking.",
                          actions, 3, ERR_BOOK_003);
}

void show_booking_payment_failed_error(void) {
    const char *actions[] = {
        "1. Verify your wallet has sufficient balance",
        "2. Try the booking again",
        "3. Contact support if the issue persists"
    };
    show_message_with_code(MSG_ERROR, "Payment Failed",
                          "Could not process your payment. No money has been deducted.",
                          actions, 3, ERR_BOOK_006);
}

// Validation errors
void show_validation_error(const char *field, const char *expected) {
    char message[256];
    snprintf(message, sizeof(message), 
             "The %s you entered is not valid.\nExpected format: %s",
             field ? field : "input", expected ? expected : "see requirements");
    
    const char *actions[] = {
        "1. Check the input format",
        "2. Remove any special characters if not allowed",
        "3. Try again with the correct format"
    };
    show_message_with_code(MSG_ERROR, "Invalid Input",
                          message, actions, 3, ERR_VAL_001);
}

// Simple message helpers
void show_success(const char *message) {
    const char *color = get_color(MSG_SUCCESS);
    const char *icon = get_icon(MSG_SUCCESS);
    printf("\n%s%s %s%s\n\n", color, icon, message, COLOR_RESET);
}

void show_warning(const char *message) {
    const char *color = get_color(MSG_WARNING);
    const char *icon = get_icon(MSG_WARNING);
    printf("\n%s%s %s%s\n\n", color, icon, message, COLOR_RESET);
}

void show_info(const char *message) {
    const char *color = get_color(MSG_INFO);
    const char *icon = get_icon(MSG_INFO);
    printf("\n%s%s %s%s\n\n", color, icon, message, COLOR_RESET);
}
