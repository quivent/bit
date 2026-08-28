#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cmd_main(int argc, char **argv) {
    const char *where = argc > 1 ? argv[1] : ".";
    char git[1200]; snprintf(git, sizeof git, "%s/.git", where);
    char p[1400];
    if (mkdir(git, 0755) < 0 && access(git, F_OK) != 0) { perror("bit init"); return 1; }
    snprintf(p, sizeof p, "%s/objects", git);    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/refs", git);       mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/refs/heads", git); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/HEAD", git);
    FILE *f = fopen(p, "w");
    if (!f) { perror("bit init"); return 1; }
    fputs("ref: refs/heads/master\n", f);        /* git reads this unchanged */
    fclose(f);
    printf("initialised empty bit repository in %s\n", git);
    return 0;
}
