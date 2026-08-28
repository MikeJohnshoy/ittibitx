// hw_settings.c
//
// minimal reader for data/hw_settings.ini. That file is the same file 
// that sbitx uses, so 'bfo_freq' and 'scale' settings will carry over
// to minibitx.  If hw_settings.ini is not found, minibitx just uses
// its own default settings.

#include "hw_settings.h"
#include "radio.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HW_SETTINGS_PATH "data/hw_settings.ini"

void hw_settings_load(void) {
    FILE *f = fopen(HW_SETTINGS_PATH, "r");
    if (!f) {
        printf("init: %s not found, using compiled-in defaults\n", HW_SETTINGS_PATH);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;

        if (*p == '[')          // first [section] line: nothing below this
            break;               // point is a top-level key - stop here
        if (*p == '#' || *p == '\0' || *p == '\n')
            continue;            // comment or blank line

        char key[64];
        long value;
        if (sscanf(p, "%63[^=]=%ld", key, &value) == 2 && !strcmp(key, "bfo_freq")) {
            bfo_freq = (int)value;
            printf("init: bfo_freq loaded from %s: %d Hz\n", HW_SETTINGS_PATH, bfo_freq);
        }
        // ssb_val and any other top-level keys: read past, not applied yet.
    }

    fclose(f);
}
