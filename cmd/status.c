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
                          h->e = realloc(h->e, h->cap * sizeof *h->e); }
    h->e[h->n].id = *id;
    snprintf(h->e[h->n].path, sizeof h->e[h->n].path, "%s", path);
    h->n++;
    return 0;
}
static const head_ent *head_find(const head_set *h, const char *p) {
    for (size_t i = 0; i < h->n; i++) if (!strcmp(h->e[i].path, p)) return &h->e[i];
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
        int known = 0;
        for (size_t i = 0; i < ix->n; i++) if (!strcmp(ix->e[i].path, child)) { known = 1; break; }
        if (!known) printf("?? %s\n", child);
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

    for (size_t i = 0; i < ix.n; i++) {
        const bit_entry *e = &ix.e[i];
        const head_ent *he = head_find(&h, e->path);
        char staged = he ? (oid_eq(&he->id, &e->id) ? ' ' : 'M') : 'A';

        char full[1300]; snprintf(full, sizeof full, "%s/%s", root, e->path);
        char work = ' ';
        if (access(full, F_OK) < 0) work = 'D';
        else if (!entry_matches_stat(e, full)) {          /* stat differs: now hash */
            size_t len; char *data = slurp(full, &len);
            if (data) { oid now; object_write("blob", data, len, 0, &now);
                        if (!oid_eq(&now, &e->id)) work = 'M';
                        free(data); }
        }
        if (staged != ' ' || work != ' ') printf("%c%c %s\n", staged, work, e->path);
    }

    for (size_t i = 0; i < h.n; i++) {                    /* in HEAD, gone from index */
        int in_ix = 0;
        for (size_t j = 0; j < ix.n; j++) if (!strcmp(ix.e[j].path, h.e[i].path)) { in_ix = 1; break; }
        if (!in_ix) printf("D  %s\n", h.e[i].path);
    }

    scan(root, "", &ix);
    free(h.e); index_free(&ix);
    return 0;
}
