#include "../lib/bit.h"
#include <stdio.h>

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int n;
    if (pack_unpack(&n) < 0) die("no pack to unpack");
    printf("unpacked %d objects\n", n);
    return 0;
}
