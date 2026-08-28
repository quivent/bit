#include "../lib/bit.h"
#include <stdio.h>
#include <string.h>

/* pull — fetch, then fast-forward. git defines pull as fetch plus merge; where
 * a merge is genuinely required this says so and stops, rather than starting
 * one the caller did not ask for. */
int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit pull <remote-path>\n"); return 1; }
    char root[1024];
    if (repo_find(root, sizeof root) < 0) die("not a bit repository");

    oid theirs; char branch[256]; int n, sent;
    if (do_fetch(argv[1], &theirs, branch, sizeof branch, &n, &sent) < 0)
        die("cannot fetch from %s", argv[1]);
    printf("fetched %s: %d reachable, %d new\n", branch, n, sent);

    oid ours;
    if (ref_head_oid(&ours) != 0) die("no commits yet");
    if (oid_eq(&ours, &theirs)) { printf("already up to date\n"); return 0; }

    oid base;
    if (merge_base(&ours, &theirs, &base) < 0) die("no common ancestor");
    if (!oid_eq(&base, &ours)) {
        fprintf(stderr, "diverged from %s; run: bit merge <branch>\n", branch);
        return 1;
    }
    oid tree;
    if (commit_tree_oid(&theirs, &tree) < 0) die("bad commit");
    bit_index ix; index_init(&ix);
    bit_index old; index_read(&old);
    int nf = 0;
    if (checkout_tree(&tree, root, &ix, &old, &nf) != 0) return 1;
    index_write(&ix);
    char ref[512]; ref_head_name(ref, sizeof ref);
    ref_update(ref, &theirs);
    index_free(&ix); index_free(&old);
    char hex[OID_HEX + 1]; oid_to_hex(&theirs, hex);
    printf("fast-forwarded to %.7s (%d file%s)\n", hex, nf, nf == 1 ? "" : "s");
    return 0;
}
