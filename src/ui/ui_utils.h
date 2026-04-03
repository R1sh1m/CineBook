#ifndef UI_UTILS_H
#define UI_UTILS_H

typedef enum {
    UI_CONTEXT_MENU,           // Clear freely (admin menus, navigation)
    UI_CONTEXT_BOOKING,        // Never clear (preserve seat map, bill)
    UI_CONTEXT_RECEIPT,        // Never clear (preserve confirmation)
    UI_CONTEXT_ADMIN_PANEL,    // Clear between options (TMDB imports)
    UI_CONTEXT_BROWSING,       // Moderate clearing (movie lists)
    UI_CONTEXT_FORM_INPUT      // Don't clear during input
} UIContext;

void smart_clear(UIContext context);

void preserve_and_clear(int lines_to_preserve);

void soft_clear(void);

void draw_separator(const char *title);

void draw_section_break(void);

#endif // UI_UTILS_H
