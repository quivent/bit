#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* rebase — replay this branch's commits onto another.
 *
 * For each commit in base..HEAD, take what it changed relative to its own
 * parent and overlay that onto the accumulating tree. File granularity, like
 * merge: a path changed by the replayed commit and also different in the new
 * base is reported rather than guessed at. */

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
static tent *find(tset *t, const char *p) {
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
static void setpath(tset *t, const char *path, const oid *id, uint32_t mode) {
    tent *e = find(t, path);
    if (e) { e->id = *id; e->mode = mode; return; }
    collect(path, mode, id, t);
    for (size_t i = t->n - 1; i > 0; i--) {          /* keep it sorted */
        if (strcmp(t->e[i - 1].path, t->e[i].path) <= 0) break;
        tent tmp = t->e[i - 1]; t->e[i - 1] = t->e[i]; t->e[i] = tmp;
    }
}
static void delpath(tset *t, const char *path) {
    tent *e = find(t, path);
    if (!e) return;
    size_t at = (size_t)(e - t->e);
    memmove(&t->e[at], &t->e[at + 1], (t->n - at - 1) * sizeof *t->e);
    t->n--;
}
static void ident(char *out, size_t cap) {
    const char *n = getenv("GIT_AUTHOR_NAME");  if (!n) n = "bit";
    const char *m = getenv("GIT_AUTHOR_EMAIL"); if (!m) m = "bit@localhost";
    time_t now = time(0); struct tm lt; localtime_r(&now, &lt);
    long off = lt.tm_gmtoff; int s = off < 0 ? -1 : 1; off *= s;
    snprintf(out, cap, "%s <%s> %ld %c%02ld%02ld", n, m, (long)now,
             s < 0 ? '-' : '+', off / 3600, (off % 3600) / 60);
}
static char *message_of(const oid *c) {
    char type[32]; size_t len;
    char *b = object_read(c, type, sizeof type, &len);
    if (!b) return 0;
    char *m = strstr(b, "\n\n");
    char *out = strdup(m ? m + 2 : "replayed");
    free(b);
    char *nl = strchr(out, '\n'); if (nl) *nl = 0;
    return out;
}

int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit rebase <upstream>\n"); return 1; }
    char root[1024]; if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    char git[1024]; repo_git_dir(git, sizeof git);

    oid ours; if (ref_head_oid(&ours) != 0) die("no commits yet");
    char p[1800]; snprintf(p, sizeof p, "%s/refs/heads/%s", git, argv[1]);
    size_t len; char *s = slurp(p, &len);
    if (!s) die("no such branch: %s", argv[1]);
    oid upstream;
    if (oid_from_hex(s, &upstream) < 0) { free(s); die("bad ref"); }
    free(s);

    oid base;
    if (merge_base(&ours, &upstream, &base) < 0) die("no common ancestor");
    if (oid_eq(&base, &upstream)) { printf("already up to date with %s\n", argv[1]); return 0; }
    if (oid_eq(&base, &ours)) {
        char ref[512]; ref_head_name(ref, sizeof ref);
        ref_update(ref, &upstream);
        printf("fast-forwarded onto %s\n", argv[1]);
        return 0;
    }

    /* commits base..HEAD, oldest first */
    oid chain[1024]; int nc = 0;
    for (oid c = ours; !oid_eq(&c, &base) && nc < 1024; ) {
        chain[nc++] = c;
        oid p1, p2;
        if (commit_parents(&c, &p1, &p2) == 0) break;
        c = p1;
    }

    tset cur = {0,0,0};
    load(&upstream, &cur);
    oid parent = upstream;
    int replayed = 0, conflicts = 0;

    for (int i = nc - 1; i >= 0; i--) {
        tset now = {0,0,0}, before = {0,0,0};
        load(&chain[i], &now);
        oid p1, p2;
        if (commit_parents(&chain[i], &p1, &p2) > 0) load(&p1, &before);

        for (size_t j = 0; j < now.n; j++) {
            tent *b = find(&before, now.e[j].path);
            if (b && oid_eq(&b->id, &now.e[j].id)) continue;      /* untouched */
            tent *c = find(&cur, now.e[j].path);
            if (c && b && !oid_eq(&c->id, &b->id) && !oid_eq(&c->id, &now.e[j].id)) {
                fprintf(stderr, "CONFLICT: %s changed both upstream and in the replayed commit\n",
                        now.e[j].path);
                conflicts++;
                continue;
            }
            setpath(&cur, now.e[j].path, &now.e[j].id, now.e[j].mode);
        }
        for (size_t j = 0; j < before.n; j++)
            if (!find(&now, before.e[j].path)) delpath(&cur, before.e[j].path);
        free(now.e); free(before.e);
        if (conflicts) break;

        bit_index ix; index_init(&ix);
        for (size_t j = 0; j < cur.n; j++) {
            bit_entry e; memset(&e, 0, sizeof e);
            snprintf(e.path, sizeof e.path, "%s", cur.e[j].path);
            e.id = cur.e[j].id;
            e.mode = (cur.e[j].mode & 0111) ? 0100755 : 0100644;
            index_upsert(&ix, &e);
        }
        oid tree;
        if (tree_from_index(&ix, &tree) < 0) die("could not write tree");
        index_free(&ix);

        char thex[OID_HEX+1], phex[OID_HEX+1], au[256];
        oid_to_hex(&tree, thex); oid_to_hex(&parent, phex); ident(au, sizeof au);
        char *msg = message_of(&chain[i]);
        char body[8192];
        int n = snprintf(body, sizeof body,
            "tree %s\nparent %s\nauthor %s\ncommitter %s\n\n%s\n",
            thex, phex, au, au, msg ? msg : "replayed");
        free(msg);
        object_write("commit", body, (size_t)n, 1, &parent);
        replayed++;
    }

    if (conflicts) { fprintf(stderr, "%d conflict%s; rebase aborted\n",
                             conflicts, conflicts == 1 ? "" : "s"); free(cur.e); return 1; }

    char ref[512]; ref_head_name(ref, sizeof ref);
    ref_update(ref, &parent);
    oid tree; commit_tree_oid(&parent, &tree);
    bit_index ix; index_init(&ix);
    bit_index old; index_read(&old);
    int nf = 0;
    checkout_tree(&tree, root, &ix, &old, &nf);
    index_write(&ix);
    index_free(&ix); index_free(&old); free(cur.e);

    char hex[OID_HEX + 1]; oid_to_hex(&parent, hex);
    printf("rebased %d commit%s onto %s (%.7s)\n",
           replayed, replayed == 1 ? "" : "s", argv[1], hex);
    return 0;
}
