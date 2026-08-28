#include "../lib/bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract a header field, or the message body when field is NULL. */
static char *field(char *commit, const char *name) {
    if (!name) { char *blank = strstr(commit, "\n\n"); return blank ? blank + 2 : 0; }
    size_t n = strlen(name);
    for (char *p = commit; p && *p; ) {
        if (*p == '\n') break;                              /* headers ended */
        if (!strncmp(p, name, n) && p[n] == ' ') return p + n + 1;
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : 0;
    }
    return 0;
}

int cmd_main(int argc, char **argv) {
    int oneline = 0, limit = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--oneline")) oneline = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
    }
    oid cur;
    int rc = ref_head_oid(&cur);
    if (rc < 0) die("not a bit repository");
    if (rc == 1) { fprintf(stderr, "bit: no commits yet\n"); return 1; }

    int shown = 0, root = 1;
    for (;;) {
        char type[32]; size_t len;
        char *body = object_read(&cur, type, sizeof type, &len);
        /* A commit that names a parent the repository does not have is a
           truncated history, not the end of one. Ending the walk quietly here
           made a pack that had lost its older objects look like a repository
           whose history simply began later. */
        if (!body || strcmp(type, "commit")) {
            free(body);
            if (!root) {
                char h[OID_HEX + 1]; oid_to_hex(&cur, h);
                fprintf(stderr, "bit: history stops at %s, which is missing\n", h);
                return 1;
            }
            break;
        }
        root = 0;
        char hex[OID_HEX + 1]; oid_to_hex(&cur, hex);
        char *msg = field(body, 0);
        char *nl = msg ? strchr(msg, '\n') : 0; if (nl) *nl = 0;

        if (oneline) printf("%.7s %s\n", hex, msg ? msg : "");
        else {
            char *au = field(body, "author");
            char *aunl = au ? strchr(au, '\n') : 0; if (aunl) *aunl = 0;
            printf("commit %s\nAuthor: %s\n\n    %s\n\n", hex, au ? au : "?", msg ? msg : "");
        }
        if (++shown == limit) { free(body); break; }

        char *p = field(body, "parent");
        if (!p) { free(body); break; }              /* a real root commit */
        root = 0;
        char phex[OID_HEX + 1]; memcpy(phex, p, OID_HEX); phex[OID_HEX] = 0;
        free(body);
        if (oid_from_hex(phex, &cur) < 0) break;
    }
    return 0;
}
