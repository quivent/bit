#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_main(int argc, char **argv) {
    int write_it = 0, stdin_mode = 0;
    const char *path = 0, *type = "blob";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) write_it = 1;
        else if (!strcmp(argv[i], "--stdin")) stdin_mode = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else path = argv[i];
    }
    char *data; size_t len;
    if (stdin_mode) {
        size_t cap = 1 << 16; len = 0; data = malloc(cap);
        ssize_t n;
        while ((n = read(0, data + len, cap - len)) > 0) {
            len += (size_t)n;
            if (len == cap) { cap *= 2; data = realloc(data, cap); }
        }
    } else if (path) {
        data = slurp(path, &len);
        if (!data) { fprintf(stderr, "bit: cannot read %s\n", path); return 1; }
    } else { fprintf(stderr, "usage: bit hash-object [-w] [-t type] <file>|--stdin\n"); return 1; }

    oid o;
    object_write(type, data, len, write_it, &o);
    char hex[OID_HEX + 1]; oid_to_hex(&o, hex);
    printf("%s\n", hex);
    free(data);
    return 0;
}
