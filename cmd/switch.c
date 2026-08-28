#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* switch — point HEAD at another branch and materialise its tree.
 * The checkout skips every file already correct, so switching between two
 * branches that differ in one file writes one file. */
int cmd_main(int argc, char **argv) {
    int create = 0, i = 1;
    if (argc > 1 && !strcmp(argv[1], "-c")) { create = 1; i = 2; }
    if (i >= argc) { fprintf(stderr, "usage: bit switch [-c] <branch>\n"); return 1; }
    const char *name = argv[i];

    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    char ref[256]; snprintf(ref, sizeof ref, "refs/heads/%s", name);

    oid target;
    if (create) {
        if (ref_head_oid(&target) != 0) die("no commits yet");
        if (ref_update(ref, &target) < 0) die("cannot create %s", name);
    }

    char git[1024]; repo_git_dir(git, sizeof git);
    char p[1800]; snprintf(p, sizeof p, "%s/%s", git, ref);
    size_t len; char *s = slurp(p, &len);
    if (!s) die("no such branch: %s", name);
    if (oid_from_hex(s, &target) < 0) { free(s); die("bad ref: %s", name); }
    free(s);

    char headp[1200]; snprintf(headp, sizeof headp, "%s/HEAD", git);
    FILE *h = fopen(headp, "w");
    if (!h) die("cannot update HEAD");
    fprintf(h, "ref: %s\n", ref);
    fclose(h);

    oid tree;
    if (commit_tree_oid(&target, &tree) < 0) die("bad commit");
    bit_index ix; index_init(&ix);
    bit_index old; index_read(&old);
    int n = 0;
    int rc = checkout_tree(&tree, root, &ix, &old, &n);
    if (rc == 0) index_write(&ix);
    index_free(&ix); index_free(&old);
    if (rc) return 1;
    printf("switched to %s (%d file%s)\n", name, n, n == 1 ? "" : "s");
    return 0;
}
