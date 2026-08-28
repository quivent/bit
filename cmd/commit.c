#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* git's identity line: "Name <email> <unix seconds> <+HHMM>" */
static void ident(char *out, size_t cap, const char *who) {
    char envname[64], envmail[64];
    snprintf(envname, sizeof envname, "GIT_%s_NAME", who);
    snprintf(envmail, sizeof envmail, "GIT_%s_EMAIL", who);
    const char *n = getenv(envname); if (!n) n = getenv("GIT_AUTHOR_NAME");
    const char *m = getenv(envmail); if (!m) m = getenv("GIT_AUTHOR_EMAIL");
    if (!n) n = "bit"; if (!m) m = "bit@localhost";

    time_t now = time(0);
    struct tm lt; localtime_r(&now, &lt);
    long off = lt.tm_gmtoff;
    int sign = off < 0 ? -1 : 1; off *= sign;
    snprintf(out, cap, "%s <%s> %ld %c%02ld%02ld",
             n, m, (long)now, sign < 0 ? '-' : '+', off / 3600, (off % 3600) / 60);
}

int cmd_main(int argc, char **argv) {
    const char *msg = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-m") && i + 1 < argc) msg = argv[++i];
    if (!msg) { fprintf(stderr, "usage: bit commit -m <message>\n"); return 1; }

    bit_index ix;
    if (index_read(&ix) < 0) die("not a bit repository");
    if (ix.n == 0) die("nothing staged");
    oid tree;
    if (tree_from_index(&ix, &tree) < 0) die("could not write tree");
    index_free(&ix);

    oid parent; int has_parent = (ref_head_oid(&parent) == 0);
    char thex[OID_HEX + 1], phex[OID_HEX + 1];
    oid_to_hex(&tree, thex);
    if (has_parent) oid_to_hex(&parent, phex);

    char au[256], co[256];
    ident(au, sizeof au, "AUTHOR"); ident(co, sizeof co, "COMMITTER");

    char body[8192];
    int n = snprintf(body, sizeof body, "tree %s\n", thex);
    if (has_parent) n += snprintf(body + n, sizeof body - n, "parent %s\n", phex);
    n += snprintf(body + n, sizeof body - n,
                  "author %s\ncommitter %s\n\n%s\n", au, co, msg);

    oid c;
    object_write("commit", body, (size_t)n, 1, &c);
    char ref[512]; ref_head_name(ref, sizeof ref);
    ref_update(ref, &c);

    char chex[OID_HEX + 1]; oid_to_hex(&c, chex);
    printf("[%s %.7s] %s\n", ref + 11, chex, msg);
    return 0;
}
