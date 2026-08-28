#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bit cat-file (-t|-s|-p) <object>\n"); return 1; }
    const char *mode = argv[1], *hex = argv[2];
    oid o;
    if (oid_from_hex(hex, &o) < 0) { fprintf(stderr, "bit: bad object name %s\n", hex); return 1; }
    char type[32]; size_t len;
    void *data = object_read(&o, type, sizeof type, &len);
    if (!data) { fprintf(stderr, "bit: no such object %s\n", hex); return 1; }

    if (!strcmp(mode, "-e")) {                    /* existence only */
        int present = object_exists(&o) || pack_exists(&o);
        return present ? 0 : 1;
    }
    if (!strcmp(mode, "-t")) printf("%s\n", type);
    else if (!strcmp(mode, "-s")) printf("%zu\n", len);
    else if (!strcmp(mode, "-p")) {
        if (!strcmp(type, "tree")) {                    /* trees are binary */
            unsigned char *p = data, *end = p + len;
            while (p < end) {
                char *sp = strchr((char *)p, ' ');
                char *nul = strchr(sp + 1, 0);
                oid e; memcpy(e.b, nul + 1, OID_RAW);
                char ehex[OID_HEX + 1]; oid_to_hex(&e, ehex);
                unsigned mode_bits = (unsigned)strtoul((char *)p, 0, 8);
                printf("%06o %s %s\t%s\n", mode_bits,
                       (mode_bits & 040000) ? "tree" : "blob", ehex, sp + 1);
                p = (unsigned char *)nul + 1 + OID_RAW;
            }
        } else fwrite(data, 1, len, stdout);
    } else { fprintf(stderr, "bit: unknown mode %s\n", mode); return 1; }
    free(data);
    return 0;
}
