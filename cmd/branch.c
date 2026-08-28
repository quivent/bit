#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>

/* branch — list, create, delete. A branch is a file holding one hex digest. */
int cmd_main(int argc, char **argv) {
    char names[128][128]; oid ids[128];
    char cur[512]; ref_head_name(cur, sizeof cur);
    const char *curname = strncmp(cur, "refs/heads/", 11) ? cur : cur + 11;

    if (argc > 2 && (!strcmp(argv[1], "-d") || !strcmp(argv[1], "-D"))) {
        if (!strcmp(argv[2], curname)) die("cannot delete the current branch");
        char ref[256]; snprintf(ref, sizeof ref, "refs/heads/%s", argv[2]);
        if (ref_delete(ref) < 0) die("no such branch: %s", argv[2]);
        printf("deleted branch %s\n", argv[2]);
        return 0;
    }
    if (argc > 1) {
        oid c;
        if (ref_head_oid(&c) != 0) die("no commits yet");
        char ref[256]; snprintf(ref, sizeof ref, "refs/heads/%s", argv[1]);
        if (ref_update(ref, &c) < 0) die("cannot create %s", argv[1]);
        char hex[OID_HEX + 1]; oid_to_hex(&c, hex);
        printf("created %s at %.7s\n", argv[1], hex);
        return 0;
    }
    int n = ref_list(names, ids, 128);
    for (int i = 0; i < n; i++) {
        char hex[OID_HEX + 1]; oid_to_hex(&ids[i], hex);
        printf("%c %-20s %.7s\n", strcmp(names[i], curname) ? ' ' : '*', names[i], hex);
    }
    return 0;
}
