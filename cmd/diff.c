#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* diff — working tree against the index, or with --cached, the index against
 * HEAD. Files whose stat still matches the index are skipped without being
 * read: the diff for an unchanged file is empty, and proving that from a
 * syscall is cheaper than proving it from its contents. */

typedef struct { oid id; char path[512]; } tent;
typedef struct { tent *e; size_t n, cap; } tset;

static int collect(const char *path, uint32_t mode, const oid *id, void *ctx) {
    (void)mode;
    tset *t = ctx;
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 128;
                          t->e = xrealloc(t->e, t->cap * sizeof *t->e); }
    t->e[t->n].id = *id;
    snprintf(t->e[t->n].path, sizeof t->e[t->n].path, "%s", path);
    t->n++;
    return 0;
}
static const tent *tfind(const tset *t, const char *p) {   /* collected in tree order */
    long lo = 0, hi = (long)t->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(t->e[mid].path, p);
        if (!c) return &t->e[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static void show(const char *path, const oid *from, const char *b, size_t blen) {
    char type[32]; size_t alen = 0; char *a = 0;
    if (from) a = object_read(from, type, sizeof type, &alen);
    diff_unified(stdout, path, a ? a : "", alen, b ? b : "", blen);
    free(a);
}

int cmd_main(int argc, char **argv) {
    int cached = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--cached") || !strcmp(argv[i], "--staged")) cached = 1;

    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    bit_index ix; if (index_read(&ix) < 0) die("cannot read index");

    tset head = {0, 0, 0};
    oid c;
    if (ref_head_oid(&c) == 0) {
        char type[32]; size_t len;
        char *body = object_read(&c, type, sizeof type, &len);
        if (body) {
            if (!strncmp(body, "tree ", 5)) {
                char hex[OID_HEX + 1]; memcpy(hex, body + 5, OID_HEX); hex[OID_HEX] = 0;
                oid t; if (!oid_from_hex(hex, &t)) tree_walk(&t, "", collect, &head);
            }
            free(body);
        }
    }

    for (size_t i = 0; i < ix.n; i++) {
        const bit_entry *e = &ix.e[i];
        const tent *h = tfind(&head, e->path);
        if (cached) {
            if (h && oid_eq(&h->id, &e->id)) continue;         /* identical, no read */
            char type[32]; size_t len;
            char *cur = object_read(&e->id, type, sizeof type, &len);
            if (cur) { show(e->path, h ? &h->id : 0, cur, len); free(cur); }
            continue;
        }
        char full[1300]; snprintf(full, sizeof full, "%s/%s", root, e->path);
        struct stat wst;
        if (lstat(full, &wst) < 0) { show(e->path, &e->id, "", 0); continue; }
        if (entry_matches_stat(e, full)) continue;             /* stat says unchanged */
        size_t len; char *cur = worktree_read(full, &len, 0);
        if (!cur) continue;
        show(e->path, &e->id, cur, len);
        free(cur);
    }
    free(head.e); index_free(&ix);
    return 0;
}
