#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* show — a commit's header and the diff against its first parent. For a
 * non-commit object it prints the object, like cat-file -p. */

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
static const tent *find(const tset *t, const char *p) {
    long lo = 0, hi = (long)t->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(t->e[mid].path, p);
        if (!c) return &t->e[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static void body_of(const oid *id, char **buf, size_t *len) {
    char type[32];
    *buf = object_read(id, type, sizeof type, len);
    if (!*buf) *len = 0;
}

int cmd_main(int argc, char **argv) {
    oid c;
    if (argc > 1) { if (oid_from_hex(argv[1], &c) < 0) die("not an object: %s", argv[1]); }
    else if (ref_head_oid(&c) != 0) die("no commits yet");

    char type[32]; size_t len;
    char *body = object_read(&c, type, sizeof type, &len);
    if (!body) die("no such object");
    if (strcmp(type, "commit")) { fwrite(body, 1, len, stdout); free(body); return 0; }

    char hex[OID_HEX + 1]; oid_to_hex(&c, hex);
    printf("commit %s\n", hex);
    for (char *p = body; p && *p && *p != '\n'; ) {
        if (!strncmp(p, "author ", 7)) { char *nl = strchr(p, '\n'); printf("Author: %.*s\n", (int)(nl - p - 7), p + 7); }
        char *nl = strchr(p, '\n'); p = nl ? nl + 1 : 0;
    }
    char *msg = strstr(body, "\n\n");
    if (msg) printf("\n    %s\n", msg + 2);

    oid p1, p2, tree;
    int np = commit_parents(&c, &p1, &p2);
    commit_tree_oid(&c, &tree);
    tset now = {0,0,0}, before = {0,0,0};
    tree_walk(&tree, "", collect, &now);
    if (np > 0) { oid pt; if (commit_tree_oid(&p1, &pt) == 0) tree_walk(&pt, "", collect, &before); }

    for (size_t i = 0; i < now.n; i++) {
        const tent *b = find(&before, now.e[i].path);
        if (b && oid_eq(&b->id, &now.e[i].id)) continue;      /* unchanged, no read */
        char *ab = 0, *bb = 0; size_t al = 0, bl = 0;
        if (b) body_of(&b->id, &ab, &al);
        body_of(&now.e[i].id, &bb, &bl);
        diff_unified(stdout, now.e[i].path, ab ? ab : "", al, bb ? bb : "", bl);
        free(ab); free(bb);
    }
    for (size_t i = 0; i < before.n; i++)
        if (!find(&now, before.e[i].path)) {
            char *ab = 0; size_t al = 0; body_of(&before.e[i].id, &ab, &al);
            diff_unified(stdout, before.e[i].path, ab ? ab : "", al, "", 0);
            free(ab);
        }
    free(now.e); free(before.e); free(body);
    return 0;
}
