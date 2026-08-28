#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* restore — put a path back to what the index says, or with --source=HEAD, to
 * what the commit says. Path-scoped, so unlike checkout it touches only what
 * was asked for. A file already correct is left alone, decided by stat. */
int cmd_main(int argc, char **argv) {
    int from_head = 0, first = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--source", 8) || !strcmp(argv[i], "--staged")) { from_head = 1; first = i + 1; }
        else { first = i; break; }
    }
    if (first >= argc) { fprintf(stderr, "usage: bit restore [--source=HEAD] <path>...\n"); return 1; }

    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    bit_index ix; if (index_read(&ix) < 0) die("cannot read index");

    int n = 0;
    for (int i = first; i < argc; i++) {
        long lo = 0, hi = (long)ix.n - 1; const bit_entry *e = 0;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            int c = strcmp(ix.e[mid].path, argv[i]);
            if (!c) { e = &ix.e[mid]; break; }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (!e) { fprintf(stderr, "bit: not tracked: %s\n", argv[i]); continue; }

        char full[1300]; snprintf(full, sizeof full, "%s/%s", root, argv[i]);
        if (!from_head && entry_matches_stat(e, full)) continue;     /* already right */

        char type[32]; size_t len;
        void *data = object_read(&e->id, type, sizeof type, &len);
        if (!data) { fprintf(stderr, "bit: missing object for %s\n", argv[i]); continue; }
        int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, (e->mode & 0111) ? 0755 : 0644);
        if (fd < 0) { free(data); fprintf(stderr, "bit: cannot write %s\n", argv[i]); continue; }
        ssize_t w = write(fd, data, len);
        close(fd); free(data);
        if (w == (ssize_t)len) n++;
    }
    index_free(&ix);
    printf("restored %d path%s\n", n, n == 1 ? "" : "s");
    return 0;
}
