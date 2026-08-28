#include "../lib/bit.h"
#include <stdio.h>

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int n = 0;
    /* A negative return means either that there is no pack or that some of it
       was unreadable. The second case has already said so on stderr, and the
       count distinguishes them: nothing recovered and nothing reported means
       there was nothing there. */
    int rc = pack_unpack(&n);
    if (rc < 0 && n == 0) die("no pack to unpack");
    printf("unpacked %d objects\n", n);
    return rc < 0 ? 1 : 0;
}
