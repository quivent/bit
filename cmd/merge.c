#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* merge — three-way, at file granularity.
 *
 * For each path, given the version at the merge base, ours and theirs:
 *   ours == theirs        take either
 *   base == ours          they changed it; take theirs
 *   base == theirs        we changed it; take ours
 *   all three differ      conflict
 *
 * Content-level merging of a single file is not attempted: a file changed on
 * both sides is reported rather than guessed at. */

typedef struct { oid id; uint32_t mode; char path[512]; } tent;
typedef struct { tent *e; size_t n, cap; } tset;

static int collect(const char *path, uint32_t mode, const oid *id, void *ctx) {
    tset *t = ctx;
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 128;
                          t->e = realloc(t->e, t->cap * sizeof *t->e); }
    t->e[t->n].id = *id; t->e[t->n].mode = mode;
    snprintf(t->e[t->n].path, sizeof t->e[t->n].path, "%s", path);
    t->n++;
    return 0;
}
static const tent *find(const tset *t, const char *p) {   /* tree order is sorted */
    long lo = 0, hi = (long)t->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(t->e[mid].path, p);
        if (!c) return &t->e[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static void load(const oid *commit, tset *t) {
    oid tree;
    if (commit_tree_oid(commit, &tree) == 0) tree_walk(&tree, "", collect, t);
}
static void ident(char *out, size_t cap) {
    const char *n = getenv("GIT_AUTHOR_NAME");  if (!n) n = "bit";
    const char *m = getenv("GIT_AUTHOR_EMAIL"); if (!m) m = "bit@localhost";
    time_t now = time(0); struct tm lt; localtime_r(&now, &lt);
    long off = lt.tm_gmtoff; int sign = off < 0 ? -1 : 1; off *= sign;
    snprintf(out, cap, "%s <%s> %ld %c%02ld%02ld", n, m, (long)now,
             sign < 0 ? '-' : '+', off / 3600, (off % 3600) / 60);
}

int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit merge <branch>\n"); return 1; }
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    char git[1024]; repo_git_dir(git, sizeof git);

    oid ours;
    if (ref_head_oid(&ours) != 0) die("no commits yet");
    char p[1800]; snprintf(p, sizeof p, "%s/refs/heads/%s", git, argv[1]);
    size_t len; char *s = slurp(p, &len);
    if (!s) die("no such branch: %s", argv[1]);
    oid theirs;
    if (oid_from_hex(s, &theirs) < 0) { free(s); die("bad ref"); }
    free(s);

    oid base;
    if (merge_base(&ours, &theirs, &base) < 0) die("no common ancestor");
    if (oid_eq(&base, &theirs)) { printf("already up to date\n"); return 0; }

    char ref[512]; ref_head_name(ref, sizeof ref);
    bit_index ix; index_init(&ix);
    bit_index old; index_read(&old);

    if (oid_eq(&base, &ours)) {                       /* fast-forward */
        oid tree; commit_tree_oid(&theirs, &tree);
        int n = 0;
        if (checkout_tree(&tree, root, &ix, &old, &n) != 0) return 1;
        index_write(&ix); ref_update(ref, &theirs);
        index_free(&ix); index_free(&old);
        char hex[OID_HEX + 1]; oid_to_hex(&theirs, hex);
        printf("fast-forward to %.7s (%d file%s)\n", hex, n, n == 1 ? "" : "s");
        return 0;
    }

    tset B = {0,0,0}, O = {0,0,0}, T = {0,0,0};
    load(&base, &B); load(&ours, &O); load(&theirs, &T);

    tset merged = {0,0,0};
    int conflicts = 0;
    for (size_t i = 0; i < O.n; i++) {
        const tent *o = &O.e[i];
        const tent *b = find(&B, o->path), *t = find(&T, o->path);
        if (!t)                              continue;               /* deleted by them */
        if (oid_eq(&o->id, &t->id))          { collect(o->path, o->mode, &o->id, &merged); continue; }
        if (b && oid_eq(&b->id, &o->id))     { collect(o->path, t->mode, &t->id, &merged); continue; }
        if (b && oid_eq(&b->id, &t->id))     { collect(o->path, o->mode, &o->id, &merged); continue; }
        fprintf(stderr, "CONFLICT: both changed %s\n", o->path);
        conflicts++;
    }
    for (size_t i = 0; i < T.n; i++)                                  /* added by them */
        if (!find(&O, T.e[i].path)) collect(T.e[i].path, T.e[i].mode, &T.e[i].id, &merged);

    if (conflicts) { fprintf(stderr, "%d conflict%s; merge aborted\n",
                             conflicts, conflicts == 1 ? "" : "s"); return 1; }

    for (size_t i = 0; i < merged.n; i++) {
        bit_entry e; memset(&e, 0, sizeof e);
        snprintf(e.path, sizeof e.path, "%s", merged.e[i].path);
        e.id = merged.e[i].id;
        e.mode = (merged.e[i].mode & 0111) ? 0100755 : 0100644;
        index_upsert(&ix, &e);
    }
    oid tree;
    if (tree_from_index(&ix, &tree) < 0) die("could not write tree");

    char thex[OID_HEX+1], ohex[OID_HEX+1], xhex[OID_HEX+1], au[256];
    oid_to_hex(&tree, thex); oid_to_hex(&ours, ohex); oid_to_hex(&theirs, xhex);
    ident(au, sizeof au);
    char body[8192];
    int n = snprintf(body, sizeof body,
        "tree %s\nparent %s\nparent %s\nauthor %s\ncommitter %s\n\nMerge %s\n",
        thex, ohex, xhex, au, au, argv[1]);          /* two parents */
    oid mc;
    object_write("commit", body, (size_t)n, 1, &mc);
    ref_update(ref, &mc);

    bit_index fresh; index_init(&fresh);
    int nf = 0;
    checkout_tree(&tree, root, &fresh, &old, &nf);
    index_write(&fresh);
    index_free(&fresh); index_free(&ix); index_free(&old);
    free(B.e); free(O.e); free(T.e); free(merged.e);

    char mhex[OID_HEX + 1]; oid_to_hex(&mc, mhex);
    printf("merge %.7s (%zu files)\n", mhex, merged.n);
    return 0;
}
