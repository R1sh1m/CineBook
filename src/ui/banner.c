#include "banner.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int has_lolcat_binary(void)
{
#ifdef _WIN32
    return (system("where lolcat >nul 2>&1") == 0);
#else
    return (system("command -v lolcat >/dev/null 2>&1") == 0);
#endif
}

static int print_lines_with_lolcat(const char *const *lines, int num_lines)
{
    FILE *lolcat_pipe = NULL;
#ifdef _WIN32
    lolcat_pipe = _popen("lolcat -f 2>nul", "w");
#else
    lolcat_pipe = popen("lolcat -f 2>/dev/null", "w");
#endif
    if (!lolcat_pipe) return 0;

    for (int i = 0; i < num_lines; i++) {
        fprintf(lolcat_pipe, "%s\n", lines[i]);
        fflush(lolcat_pipe);
#ifdef _WIN32
        Sleep(40);
#else
        usleep(40000);
#endif
    }

#ifdef _WIN32
    _pclose(lolcat_pipe);
#else
    pclose(lolcat_pipe);
#endif
    return 1;
}

int print_rainbow_lines(const char *const *lines, int num_lines, int add_trailing_blank)
{
    if (!lines || num_lines <= 0) return 0;

    if (has_lolcat_binary() && print_lines_with_lolcat(lines, num_lines)) {
        if (add_trailing_blank) printf("\n");
        return 1;
    }

    {
        const char *colors[] = {
            "\033[38;5;196m",
            "\033[38;5;208m",
            "\033[38;5;226m",
            "\033[38;5;82m",
            "\033[38;5;21m",
            "\033[38;5;93m",
            "\033[38;5;201m",
            "\033[38;5;51m"
        };
        const char *reset = "\033[0m";

        for (int i = 0; i < num_lines; i++) {
            printf("%s%s%s\n", colors[i % 8], lines[i], reset);
            fflush(stdout);
#ifdef _WIN32
            Sleep(40);
#else
            usleep(40000);
#endif
        }
        if (add_trailing_blank) printf("\n");
    }

    return 1;
}

void show_banner(void)
{
    const char *banner_lines[] = {
        "   ██████╗██╗███╗   ██╗███████╗██████╗  ██████╗  ██████╗ ██╗  ██╗",
        "  ██╔════╝██║████╗  ██║██╔════╝██╔══██╗██╔═══██╗██╔═══██╗██║ ██╔╝",
        "  ██║     ██║██╔██╗ ██║█████╗  ██████╔╝██║   ██║██║   ██║█████╔╝ ",
        "  ██║     ██║██║╚██╗██║██╔══╝  ██╔══██╗██║   ██║██║   ██║██╔═██╗ ",
        "  ╚██████╗██║██║ ╚████║███████╗██████╔╝╚██████╔╝╚██████╔╝██║  ██╗",
        "   ╚═════╝╚═╝╚═╝  ╚═══╝╚══════╝╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝",
        "",
        "  Terminal Cinema Booking System "
    };
    int num_lines = (int)(sizeof(banner_lines) / sizeof(banner_lines[0]));

    printf("\n");
    (void)print_rainbow_lines(banner_lines, num_lines, 1);
}