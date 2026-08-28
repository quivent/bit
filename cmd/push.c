#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* push — send what the remote lacks, then move its branch. Fast-forward only:
 * the remote's current commit must be an ancestor of ours, or the push is
 * refused rather than silently discarding their work. */
int cmd_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bit push <remote-path>\n"); return 1; }
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) die("not a bit repository");
    char dst[1200];
    if (resolve_gitdir(argv[1], dst, sizeof dst) < 0) die("not a repository: %s", argv[1]);

    oid ours;
    if (ref_head_oid(&ours) != 0) die("no commits yet");
    char ref[512]; ref_head_name(ref, sizeof ref);
    const char *branch = strncmp(ref, "refs/heads/", 11) ? ref : ref + 11;

    char p[1400]; snprintf(p, sizeof p, "%s/refs/heads/%s", dst, branch);
    size_t l; char *s = slurp(p, &l);
    if (s) {                                   /* fast-forward check */
        oid theirs;
        if (oid_from_hex(s, &theirs) == 0 && !oid_eq(&theirs, &ours)) {
            oid base;
            if (merge_base(&ours, &theirs, &base) < 0 || !oid_eq(&base, &theirs)) {
                free(s);
                die("remote has commits we do not; fetch and merge first");
            }
        }
        free(s);
    }

    oid *objs = xmalloc(200000 * sizeof *objs);
    int n = reachable(&ours, objs, 200000);
    int sent = 0;
    for (int i = 0; i < n; i++) if (object_copy(git, dst, &objs[i]) == 1) sent++;
    free(objs);

    char hex[OID_HEX + 1]; oid_to_hex(&ours, hex);
    FILE *f = fopen(p, "w");
    if (!f) die("cannot update remote ref");
    fprintf(f, "%s\n", hex); fclose(f);

    printf("pushed %s: %d reachable, %d new, %d already present -> %.7s\n",
           branch, n, sent, n - sent, hex);
    return 0;
}
