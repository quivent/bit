#include "../lib/bit.h"
#include <stdio.h>

int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit fetch <remote-path>\n"); return 1; }
    oid head; char branch[256]; int n, sent;
    if (do_fetch(argv[1], &head, branch, sizeof branch, &n, &sent) < 0)
        die("cannot fetch from %s", argv[1]);
    char hex[OID_HEX + 1]; oid_to_hex(&head, hex);
    printf("fetched %s: %d reachable, %d new -> refs/remotes/origin/%s (%.7s)\n",
           branch, n, sent, branch, hex);
    return 0;
}
