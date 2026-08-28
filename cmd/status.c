#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* status — three-way comparison: HEAD tree, index, working tree.
 *
 * Output matches `git status --short`: two columns, staged then unstaged.
 *   A_ added to index      _M modified on disk      ?? untracked
 *   M_ modified in index   _D deleted from disk
 *
 * The working-tree column is decided by stat alone wherever possible. A file
 * whose size, mtime and inode match the index is unchanged and is never read. */

typedef struct { oid id; char path[512]; } head_ent;
typedef struct { head_ent *e; size_t n, cap; } head_set;

static int collect(const char *path, uint32_t mode, const oid *id, void *ctx) {
    (void)mode;
    head_set *h = ctx;
    if (h->n == h->cap) { h->cap = h->cap ? h->cap * 2 : 128;
                          h->e = xrealloc(h->e, h->cap * sizeof *h->e); }
    h->e[h->n].id = *id;
    snprintf(h->e[h->n].path, sizeof h->e[h->n].path, "%s", path);
    h->n++;
    return 0;
}
static int head_cmp(const void *a, const void *b) {
    return strcmp(((const head_ent *)a)->path, ((const head_ent *)b)->path);
}
/* Binary search, over a set sorted once. This was a linear scan called once
   per index entry, so status was O(entries * head entries): at eight thousand
   files that is sixty-four million string comparisons, and it was most of what
   status spent its time doing. */
static const head_ent *head_find(const head_set *h, const char *p) {
    long lo = 0, hi = (long)h->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(h->e[mid].path, p);
        if (c == 0) return &h->e[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
/* The index is kept sorted by path, so the same applies to it. */
static int index_has(const bit_index *ix, const char *p) {
    long lo = 0, hi = (long)ix->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(ix->e[mid].path, p);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* untracked: anything on disk the index does not name */
static void scan(const char *root, const char *rel, const bit_index *ix) {
    char full[1200];
    snprintf(full, sizeof full, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
        if (!strcmp(n, ".git")) continue;
        char child[1024];
        snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", n);
        char cfull[1300];
        snprintf(cfull, sizeof cfull, "%s/%s", root, child);
        struct stat st;
        if (stat(cfull, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) { scan(root, child, ix); continue; }
        if (!index_has(ix, child)) printf("?? %s\n", child);
    }
    closedir(d);
}

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    bit_index ix; if (index_read(&ix) < 0) die("cannot read index");

    head_set h = {0, 0, 0};
    oid c;
    if (ref_head_oid(&c) == 0) {
        char type[32]; size_t len;
        char *body = object_read(&c, type, sizeof type, &len);
        if (body) {
            char hex[OID_HEX + 1];
            if (!strncmp(body, "tree ", 5)) {
                memcpy(hex, body + 5, OID_HEX); hex[OID_HEX] = 0;
                oid t; if (!oid_from_hex(hex, &t)) tree_walk(&t, "", collect, &h);
            }
            free(body);
        }
    }

    qsort(h.e, h.n, sizeof *h.e, head_cmp);              /* sorted once, searched n times */

    for (size_t i = 0; i < ix.n; i++) {
        const bit_entry *e = &ix.e[i];
        const head_ent *he = head_find(&h, e->path);
        char staged = he ? (oid_eq(&he->id, &e->id) ? ' ' : 'M') : 'A';

        char full[1300]; snprintf(full, sizeof full, "%s/%s", root, e->path);
        char work = ' ';
        struct stat wst;
        if (lstat(full, &wst) < 0) work = 'D';            /* a dangling link exists */
        else if (!entry_matches_stat(e, full)) {          /* stat differs: now hash */
            size_t len; char *data = worktree_read(full, &len, 0);
            if (data) { oid now; object_write("blob", data, len, 0, &now);
                        if (!oid_eq(&now, &e->id)) work = 'M';
                        free(data); }
        }
        if (staged != ' ' || work != ' ') printf("%c%c %s\n", staged, work, e->path);
    }

    for (size_t i = 0; i < h.n; i++)                     /* in HEAD, gone from index */
        if (!index_has(&ix, h.e[i].path)) printf("D  %s\n", h.e[i].path);

    scan(root, "", &ix);
    free(h.e); index_free(&ix);
    return 0;
}
