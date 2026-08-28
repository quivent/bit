#include "../lib/bit.h"
#include <stdio.h>

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int n; size_t p, i;
    if (pack_write(&n, &p, &i) < 0) die("could not write pack");
    printf("packed %d objects\n", n);
    printf("  pack %8zu B  %6.1f B/object\n", p, n ? (double)p / n : 0);
    printf("  idx  %8zu B  %6.1f B/object\n", i, n ? (double)i / n : 0);
    return 0;
}
