#include "../lib/bit.h"
#include <stdio.h>

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    bit_index ix;
    if (index_read(&ix) < 0) die("not a bit repository");
    if (ix.n == 0) die("nothing staged");
    oid t;
    if (tree_from_index(&ix, &t) < 0) die("could not write tree");
    char hex[OID_HEX + 1]; oid_to_hex(&t, hex);
    printf("%s\n", hex);
    index_free(&ix);
    return 0;
}
