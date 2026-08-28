#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>

int cmd_main(int argc, char **argv) {
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");
    oid c;
    if (argc > 1) { if (oid_from_hex(argv[1], &c) < 0) die("not an object name: %s", argv[1]); }
    else if (ref_head_oid(&c) != 0) die("no commits yet");

    oid tree;
    if (commit_tree_oid(&c, &tree) < 0) die("not a commit or tree");

    bit_index ix; index_init(&ix);
    bit_index old; index_read(&old);
    int n = 0;
    int rc = checkout_tree(&tree, root, &ix, &old, &n);
    if (rc == 0) index_write(&ix);
    index_free(&ix); index_free(&old);
    if (rc) return 1;
    printf("checked out %d file%s\n", n, n == 1 ? "" : "s");
    return 0;
}
