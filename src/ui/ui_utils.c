#include "ui_utils.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

void smart_clear(UIContext context) {
    switch (context) {
        case UI_CONTEXT_MENU:
        case UI_CONTEXT_ADMIN_PANEL:
        case UI_CONTEXT_BROWSING:
            // Clear screen and move cursor to top-left
            printf("\033[2J\033[H");
            fflush(stdout);
            break;
        
        case UI_CONTEXT_BOOKING:
        case UI_CONTEXT_RECEIPT:
        case UI_CONTEXT_FORM_INPUT:
            // Do NOT clear - preserve critical data
            // Instead, add a visual section break
            draw_section_break();
            break;
    }
}

void preserve_and_clear(int lines_to_preserve) {
    if (lines_to_preserve <= 0) {
        printf("\033[2J\033[H");
        fflush(stdout);
        return;
    }
    
    // Scroll up to preserve lines, then clear
    for (int i = 0; i < lines_to_preserve; i++) {
        printf("\n");
    }
    
    // Move cursor back up
    printf("\033[%dA", lines_to_preserve);
    fflush(stdout);
}

void soft_clear(void) {
    // Clear visible screen area but preserve scrollback buffer
    // This is what Ctrl+L typically does in terminals
    
#ifdef _WIN32
    // Windows: Clear console buffer
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD coordScreen = {0, 0};
    DWORD cCharsWritten;
    DWORD dwConSize;
    
    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        dwConSize = csbi.dwSize.X * csbi.dwSize.Y;
        FillConsoleOutputCharacter(hConsole, ' ', dwConSize, coordScreen, &cCharsWritten);
        GetConsoleScreenBufferInfo(hConsole, &csbi);
        FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConSize, coordScreen, &cCharsWritten);
        SetConsoleCursorPosition(hConsole, coordScreen);
    }
#else
    // Unix/Linux: Use ANSI escape to clear visible area only
    // \033[H moves cursor to top-left
    // \033[2J clears entire screen (but preserves scrollback on most terminals)
    printf("\033[H\033[2J");
    fflush(stdout);
#endif
}

void draw_separator(const char *title) {
    // Draw a separator with optional title using box-drawing characters
    const int width = 80;
    
    printf("\n");
    
    if (title && strlen(title) > 0) {
        int title_len = strlen(title);
        int padding = (width - title_len - 4) / 2;
        
        // Top line with title
        printf("  ");
        for (int i = 0; i < padding; i++) printf("─");
        printf(" %s ", title);
        for (int i = 0; i < padding; i++) printf("─");
        if ((width - title_len - 4) % 2) printf("─"); // Handle odd widths
        printf("\n");
    } else {
        // Simple horizontal rule
        printf("  ");
        for (int i = 0; i < width - 4; i++) printf("─");
        printf("\n");
    }
    
    fflush(stdout);
}

void draw_section_break(void) {
    // Light visual separator instead of clearing
    printf("\n\n");
    printf("  \033[90m"); // Dim gray color
    for (int i = 0; i < 76; i++) printf("·");
    printf("\033[0m\n\n"); // Reset color
    fflush(stdout);
}
