#include "banner.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void show_banner(void)
{
    const char *banner_lines[] = {
        "  ██████╗██╗███╗   ██╗███████╗██████╗  ██████╗  ██████╗ ██╗  ██╗",
        "  ██╔════╝██║████╗  ██║██╔════╝██╔══██╗██╔═══██╗██╔═══██╗██║ ██╔╝",
        "  ██║     ██║██╔██╗ ██║█████╗  ██████╔╝██║   ██║██║   ██║█████╔╝ ",
        "  ██║     ██║██║╚██╗██║██╔══╝  ██╔══██╗██║   ██║██║   ██║██╔═██╗ ",
        "  ╚██████╗██║██║ ╚████║███████╗██████╔╝╚██████╔╝╚██████╔╝██║  ██╗",
        "   ╚═════╝╚═╝╚═╝  ╚═══╝╚══════╝╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝",
        "",
        "  Terminal Cinema Booking System  |  BACSE104"
    };
    int num_lines = (int)(sizeof(banner_lines) / sizeof(banner_lines[0]));

    int has_lolcat = 0;
#ifdef _WIN32
    has_lolcat = (system("where lolcat >nul 2>&1") == 0);
#else
    has_lolcat = (system("command -v lolcat >/dev/null 2>&1") == 0);
#endif

    printf("\n");

    if (has_lolcat) {
        FILE *lolcat_pipe = NULL;
#ifdef _WIN32
        lolcat_pipe = _popen("lolcat -f 2>nul", "w");
#else
        lolcat_pipe = popen("lolcat -f 2>/dev/null", "w");
#endif
        if (lolcat_pipe) {
            for (int i = 0; i < num_lines; i++) {
                fprintf(lolcat_pipe, "%s\n", banner_lines[i]);
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
            printf("\n");
            return;
        }
    }

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
        printf("%s%s%s\n", colors[i % 8], banner_lines[i], reset);
        fflush(stdout);
#ifdef _WIN32
        Sleep(40);
#else
        usleep(40000);
#endif
    }
    printf("\n");
}