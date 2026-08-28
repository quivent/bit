#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* rm — drop paths from the index and the working tree. The index is sorted,
 * so removal is a binary search and one memmove, not a scan and a rebuild. */
int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit rm <path>...\n"); return 1; }
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    bit_index ix; if (index_read(&ix) < 0) die("cannot read index");

    int removed = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        long lo = 0, hi = (long)ix.n - 1, at = -1;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            int c = strcmp(ix.e[mid].path, argv[i]);
            if (!c) { at = mid; break; }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (at < 0) { fprintf(stderr, "bit: not tracked: %s\n", argv[i]); rc = 1; continue; }
        memmove(&ix.e[at], &ix.e[at + 1], (ix.n - (size_t)at - 1) * sizeof *ix.e);
        ix.n--;
        char full[1300]; snprintf(full, sizeof full, "%s/%s", root, argv[i]);
        unlink(full);
        printf("rm '%s'\n", argv[i]);
        removed++;
    }
    if (removed) index_write(&ix);
    index_free(&ix);
    return rc;
}
