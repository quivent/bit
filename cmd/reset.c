#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* reset — rebuild the index from HEAD, discarding staged changes. The working
 * tree is untouched: this is `git reset` without --hard. */
typedef struct { const char *root; bit_index *ix; int n; } ctx;

static int put(const char *path, uint32_t mode, const oid *id, void *v) {
    ctx *c = v;
    char full[1300]; snprintf(full, sizeof full, "%s/%s", c->root, path);
    struct stat st;
    bit_entry e; memset(&e, 0, sizeof e);
    snprintf(e.path, sizeof e.path, "%s", path);
    e.id = *id;
    e.mode = (mode & 0111) ? 0100755 : 0100644;
    /* Carry the stat only when the file on disk really is this object. Copying
       a live stat next to HEAD's oid would make `status` believe a modified
       file was clean -- the stat cache is only valid if it describes the same
       content the oid names. Left zeroed, status falls back to hashing, which
       is exactly what it should do for a file whose state is now unknown. */
    if (stat(full, &st) == 0) {
        size_t len; char *cur = slurp(full, &len);
        if (cur) {
            oid now; object_write("blob", cur, len, 0, &now);
            free(cur);
            if (oid_eq(&now, id)) {
                e.size  = (uint32_t)st.st_size;  e.mtime = (uint32_t)st.st_mtime;
                e.ctime = (uint32_t)st.st_ctime; e.dev   = (uint32_t)st.st_dev;
                e.ino   = (uint32_t)st.st_ino;   e.uid   = (uint32_t)st.st_uid;
                e.gid   = (uint32_t)st.st_gid;
            }
        }
    }
    index_upsert(c->ix, &e);
    c->n++;
    return 0;
}

int cmd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    oid c;
    if (ref_head_oid(&c) != 0) die("no commits yet");
    oid tree;
    if (commit_tree_oid(&c, &tree) < 0) die("bad commit");
    bit_index ix; index_init(&ix);
    ctx x = { root, &ix, 0 };
    if (tree_walk(&tree, "", put, &x) != 0) return 1;
    index_write(&ix);
    index_free(&ix);
    printf("index reset to HEAD (%d entries)\n", x.n);
    return 0;
}
