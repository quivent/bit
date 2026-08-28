#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* clone -- copy every object reachable from the source's HEAD, then materialise
 * it. Transfer is copy-if-absent, so cloning into a directory that already
 * shares objects sends only what is missing. */

int cmd_main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bit clone <src> <dst>\n"); return 1; }
    char src[1200];
    if (resolve_gitdir(argv[1], src, sizeof src) < 0) die("not a repository: %s", argv[1]);

    char branch[256] = "master", refpath[1400];
    { char p[1400]; snprintf(p, sizeof p, "%s/HEAD", src);
      size_t l; char *s = slurp(p, &l);
      if (!s) die("cannot read source HEAD");
      char *nl = strchr(s, '\n'); if (nl) *nl = 0;
      if (!strncmp(s, "ref: refs/heads/", 16)) snprintf(branch, sizeof branch, "%s", s + 16);
      free(s); }
    snprintf(refpath, sizeof refpath, "%s/refs/heads/%s", src, branch);
    size_t l; char *s = slurp(refpath, &l);
    if (!s) die("source branch %s has no commits", branch);
    oid head;
    if (oid_from_hex(s, &head) < 0) { free(s); die("bad ref"); }
    free(s);

    char dst[1200], dg[1300], p[1400];
    snprintf(dst, sizeof dst, "%s", argv[2]);
    mkdir(dst, 0755);
    snprintf(dg, sizeof dg, "%s/.git", dst);
    mkdir(dg, 0755);
    snprintf(p, sizeof p, "%s/objects", dg);    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/refs", dg);       mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/refs/heads", dg); mkdir(p, 0755);

    oid *objs = xmalloc(200000 * sizeof *objs);
    int n = reachable_in(src, &head, objs, 200000);
    int sent = 0;
    for (int i = 0; i < n; i++) if (object_copy(src, dg, &objs[i]) == 1) sent++;
    free(objs);

    snprintf(p, sizeof p, "%s/refs/heads/%s", dg, branch);
    { char hex[OID_HEX + 1]; oid_to_hex(&head, hex);
      FILE *f = fopen(p, "w"); if (!f) die("cannot write ref"); fprintf(f, "%s\n", hex); fclose(f); }
    snprintf(p, sizeof p, "%s/HEAD", dg);
    { FILE *f = fopen(p, "w"); if (!f) die("cannot write HEAD");
      fprintf(f, "ref: refs/heads/%s\n", branch); fclose(f); }

    if (chdir(dst) < 0) die("cannot enter %s", dst);
    char root[1024]; repo_find(root, sizeof root);
    oid tree;
    if (commit_tree_oid(&head, &tree) < 0) die("bad commit");
    bit_index ix; index_init(&ix);
    int nf = 0;
    checkout_tree(&tree, root, &ix, 0, &nf);
    index_write(&ix); index_free(&ix);

    printf("cloned %s into %s\n", argv[1], argv[2]);
    printf("  %d objects reachable, %d transferred, %d already present\n", n, sent, n - sent);
    printf("  %d file%s checked out on %s\n", nf, nf == 1 ? "" : "s", branch);
    return 0;
}
