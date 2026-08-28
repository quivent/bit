#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* mv — rename in the index and on disk. The index is sorted, so this is a
 * binary search for the old path, one memmove out, and a sorted insert of the
 * new one; never a scan and never a re-sort. */
int cmd_main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bit mv <src> <dst>\n"); return 1; }
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    bit_index ix; if (index_read(&ix) < 0) die("cannot read index");

    long lo = 0, hi = (long)ix.n - 1, at = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(ix.e[mid].path, argv[1]);
        if (!c) { at = mid; break; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    if (at < 0) die("not tracked: %s", argv[1]);

    bit_entry e = ix.e[at];
    memmove(&ix.e[at], &ix.e[at + 1], (ix.n - (size_t)at - 1) * sizeof *ix.e);
    ix.n--;
    snprintf(e.path, sizeof e.path, "%s", argv[2]);

    char from[1300], to[1300];
    snprintf(from, sizeof from, "%s/%s", root, argv[1]);
    snprintf(to,   sizeof to,   "%s/%s", root, argv[2]);
    if (rename(from, to) < 0) die("cannot rename %s", argv[1]);

    struct stat st;                       /* inode changes; refresh the cache */
    if (stat(to, &st) == 0) {
        e.size = (uint32_t)st.st_size; e.mtime = (uint32_t)st.st_mtime;
        e.ctime = (uint32_t)st.st_ctime; e.ino = (uint32_t)st.st_ino;
    }
    index_upsert(&ix, &e);
    index_write(&ix);
    index_free(&ix);
    printf("renamed %s -> %s\n", argv[1], argv[2]);
    return 0;
}
