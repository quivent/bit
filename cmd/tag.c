#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

/* tag — a name under refs/tags holding one digest. Lightweight tags only;
 * an annotated tag would be a fourth object type this does not have. */
int cmd_main(int argc, char **argv) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) die("not a bit repository");

    if (argc > 2 && !strcmp(argv[1], "-d")) {
        char ref[256]; snprintf(ref, sizeof ref, "refs/tags/%s", argv[2]);
        if (ref_delete(ref) < 0) die("no such tag: %s", argv[2]);
        printf("deleted tag %s\n", argv[2]);
        return 0;
    }
    if (argc > 1) {
        oid c;
        if (argc > 2) { if (oid_from_hex(argv[2], &c) < 0) die("not an object: %s", argv[2]); }
        else if (ref_head_oid(&c) != 0) die("no commits yet");
        char ref[256]; snprintf(ref, sizeof ref, "refs/tags/%s", argv[1]);
        if (ref_update(ref, &c) < 0) die("cannot create tag");
        char hex[OID_HEX + 1]; oid_to_hex(&c, hex);
        printf("%s -> %.7s\n", argv[1], hex);
        return 0;
    }
    char dir[1200]; snprintf(dir, sizeof dir, "%s/refs/tags", git);
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) if (de->d_name[0] != '.') printf("%s\n", de->d_name);
    closedir(d);
    return 0;
}
