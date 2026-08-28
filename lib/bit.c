#include "bit.h"
#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>

void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("bit: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

static const char HEX[] = "0123456789abcdef";
void oid_to_hex(const oid *o, char out[OID_HEX + 1]) {
    for (int i = 0; i < OID_RAW; i++) {
        out[i*2]   = HEX[o->b[i] >> 4];
        out[i*2+1] = HEX[o->b[i] & 15];
    }
    out[OID_HEX] = 0;
}
static int nyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int oid_from_hex(const char *hex, oid *out) {
    for (int i = 0; i < OID_RAW; i++) {
        int h = nyb(hex[i*2]), l = nyb(hex[i*2+1]);
        if (h < 0 || l < 0) return -1;
        out->b[i] = (unsigned char)((h << 4) | l);
    }
    return 0;
}
int oid_eq(const oid *a, const oid *b) { return memcmp(a->b, b->b, OID_RAW) == 0; }

char *slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return 0; }
    char *buf = xmalloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return 0; }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n < 0) { free(buf); return 0; }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

/* ---- allocation ---- */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory allocating %zu bytes", n);
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory reallocating %zu bytes", n);
    return q;
}
void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory allocating %zu x %zu", n, sz);
    return p;
}
char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("out of memory duplicating a string");
    return p;
}

/* ---- oid set ----
 * Open addressing over a power-of-two table of indices into a dense array, so
 * membership is a probe and iteration is a walk. The dense array is what the
 * callers want; the table exists only to make `has` constant time. */

#define OIDSET_EMPTY ((size_t)-1)

static size_t oid_hash(const oid *o) {
    size_t h = 0;
    memcpy(&h, o->b, sizeof h < OID_RAW ? sizeof h : OID_RAW);
    return h;
}
static size_t *oidset_slots(const oidset *s) { return (size_t *)s->used; }

void oidset_init(oidset *s) {
    s->n = 0; s->cap = 0; s->v = 0; s->hn = 64;
    s->used = xmalloc(s->hn * sizeof(size_t));
    size_t *t = oidset_slots(s);
    for (size_t i = 0; i < s->hn; i++) t[i] = OIDSET_EMPTY;
}
static void oidset_rehash(oidset *s) {
    size_t nn = s->hn * 2;
    size_t *t = xmalloc(nn * sizeof(size_t));
    for (size_t i = 0; i < nn; i++) t[i] = OIDSET_EMPTY;
    for (size_t i = 0; i < s->n; i++) {
        size_t j = oid_hash(&s->v[i]) & (nn - 1);
        while (t[j] != OIDSET_EMPTY) j = (j + 1) & (nn - 1);
        t[j] = i;
    }
    free(s->used);
    s->used = (unsigned char *)t;
    s->hn = nn;
}
int oidset_has(const oidset *s, const oid *o) {
    if (!s->used) return 0;
    size_t *t = oidset_slots(s);
    size_t j = oid_hash(o) & (s->hn - 1);
    while (t[j] != OIDSET_EMPTY) {
        if (oid_eq(&s->v[t[j]], o)) return 1;
        j = (j + 1) & (s->hn - 1);
    }
    return 0;
}
int oidset_add(oidset *s, const oid *o) {
    if (!s->used) oidset_init(s);
    if (oidset_has(s, o)) return 0;
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64;
                          s->v = xrealloc(s->v, s->cap * sizeof *s->v); }
    s->v[s->n] = *o;
    size_t *t = oidset_slots(s);
    size_t j = oid_hash(o) & (s->hn - 1);
    while (t[j] != OIDSET_EMPTY) j = (j + 1) & (s->hn - 1);
    t[j] = s->n;
    s->n++;
    if (s->n * 2 >= s->hn) oidset_rehash(s);
    return 1;
}
void oidset_free(oidset *s) { free(s->v); free(s->used); s->v = 0; s->used = 0; s->n = s->cap = 0; }

char *worktree_read(const char *full, size_t *len, int *is_link) {
    struct stat st;
    if (lstat(full, &st) < 0) return 0;
    if (is_link) *is_link = S_ISLNK(st.st_mode);
    if (S_ISLNK(st.st_mode)) {
        char *buf = xmalloc(4097);
        ssize_t tl = readlink(full, buf, 4096);
        if (tl < 0) { free(buf); return 0; }
        buf[tl] = 0;
        if (len) *len = (size_t)tl;
        return buf;
    }
    return slurp(full, len);
}

/* ---- repository discovery ---- */
int repo_find(char *out, size_t cap) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) return -1;
    for (;;) {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/.git", cwd);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, cap, "%s", cwd);
            return 0;
        }
        char *slash = strrchr(cwd, '/');
        if (!slash || slash == cwd) return -1;
        *slash = 0;
    }
}
/* Cached. repo_find walks up the tree calling stat at each level, and the
   answer cannot change inside one command, but object_read called this once
   per object -- so reading n objects walked the directory tree n times. */
int repo_git_dir(char *out, size_t cap) {
    static char cached[1200];
    static int state;                            /* 0 unknown, 1 found, -1 not */
    if (state == 0) {
        char root[1024];
        if (repo_find(root, sizeof root) < 0) state = -1;
        else { snprintf(cached, sizeof cached, "%s/.git", root); state = 1; }
    }
    if (state < 0) return -1;
    snprintf(out, cap, "%s", cached);
    return 0;
}

/* ---- objects ---- */
static void object_path(const oid *o, char *out, size_t cap) {
    char git[1024], hex[OID_HEX + 1];
    if (repo_git_dir(git, sizeof git) < 0) die("not a bit repository");
    oid_to_hex(o, hex);
    snprintf(out, cap, "%s/objects/%.2s/%s", git, hex, hex + 2);
}

int object_exists(const oid *o) {
    char p[1200]; object_path(o, p, sizeof p);
    return access(p, F_OK) == 0;
}

int object_write(const char *type, const void *data, size_t len, int write_it, oid *out) {
    char hdr[64];
    int hlen = snprintf(hdr, sizeof hdr, "%s %zu", type, len) + 1;  /* include NUL */

    CC_SHA1_CTX c; CC_SHA1_Init(&c);
    CC_SHA1_Update(&c, hdr, (CC_LONG)hlen);
    CC_SHA1_Update(&c, data, (CC_LONG)len);
    CC_SHA1_Final(out->b, &c);
    if (!write_it) return 0;
    if (object_exists(out)) return 0;                 /* immutable; already there */

    size_t raw = (size_t)hlen + len;
    unsigned char *buf = xmalloc(raw);
    memcpy(buf, hdr, hlen);
    memcpy(buf + hlen, data, len);

    uLongf zcap = compressBound((uLong)raw);
    unsigned char *z = xmalloc(zcap);
    if (compress2(z, &zcap, buf, (uLong)raw, Z_DEFAULT_COMPRESSION) != Z_OK)
        die("compression failed");

    char p[1200]; object_path(out, p, sizeof p);
    char dir[1200]; snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/'); if (slash) *slash = 0;
    mkdir(dir, 0755);
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp%d", p, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) die("cannot write object: %s", strerror(errno));
    if (write(fd, z, zcap) != (ssize_t)zcap) die("short write");
    close(fd);
    rename(tmp, p);                                    /* atomic publish */
    free(buf); free(z);
    return 0;
}

void *object_read(const oid *o, char *type_out, size_t type_cap, size_t *len_out) {
    char p[1200]; object_path(o, p, sizeof p);
    size_t zlen; char *z = slurp(p, &zlen);
    if (!z) return pack_read(o, type_out, type_cap, len_out);   /* loose, then pack */

    size_t cap = zlen * 6 + 8192;
    unsigned char *buf = xmalloc(cap);
    uLongf got = (uLongf)cap;
    while (uncompress(buf, &got, (unsigned char *)z, (uLong)zlen) == Z_BUF_ERROR) {
        cap *= 4; buf = xrealloc(buf, cap); got = (uLongf)cap;
    }
    free(z);

    unsigned char *nul = memchr(buf, 0, got);
    if (!nul) { free(buf); return 0; }
    if (type_out) { size_t n = (size_t)(strchr((char *)buf, ' ') - (char *)buf);
                    if (n >= type_cap) n = type_cap - 1;
                    memcpy(type_out, buf, n); type_out[n] = 0; }
    size_t hlen = (size_t)(nul - buf) + 1;
    size_t clen = got - hlen;
    unsigned char *content = xmalloc(clen + 1);
    memcpy(content, buf + hlen, clen);
    content[clen] = 0;
    free(buf);
    if (len_out) *len_out = clen;
    return content;
}

/* ---- refs ---- */
int ref_head_name(char *out, size_t cap) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char p[1200]; snprintf(p, sizeof p, "%s/HEAD", git);
    size_t n; char *s = slurp(p, &n);
    if (!s) return -1;
    char *nl = strchr(s, '\n'); if (nl) *nl = 0;
    if (strncmp(s, "ref: ", 5) == 0) snprintf(out, cap, "%s", s + 5);
    else snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}
int ref_head_oid(oid *out) {
    char ref[512]; if (ref_head_name(ref, sizeof ref) < 0) return -1;
    char git[1024]; repo_git_dir(git, sizeof git);
    char p[1800]; snprintf(p, sizeof p, "%s/%s", git, ref);
    size_t n; char *s = slurp(p, &n);
    if (!s) return 1;                                  /* unborn branch */
    int rc = oid_from_hex(s, out);
    free(s);
    return rc < 0 ? -1 : 0;
}
int ref_update(const char *ref, const oid *o) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char p[1800]; snprintf(p, sizeof p, "%s/%s", git, ref);
    char dir[1800]; snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/'); if (slash) { *slash = 0; mkdir(dir, 0755); }
    char hex[OID_HEX + 1]; oid_to_hex(o, hex);
    FILE *f = fopen(p, "w"); if (!f) return -1;
    fprintf(f, "%s\n", hex); fclose(f);
    return 0;
}

/* ---- index, in git's v2 on-disk format ----
 *
 *   "DIRC" u32:version=2 u32:count
 *   entries, each 62 bytes of fixed fields + name + NUL padding to a
 *   multiple of 8, sorted by name
 *   SHA-1 of everything above
 *
 * Implementing git's real format rather than a private one is what lets
 * `git status` read a staging area that `bit add` wrote. */

static void put32(unsigned char **p, uint32_t v) {
    uint32_t n = htonl(v); memcpy(*p, &n, 4); *p += 4;
}
static uint32_t get32(const unsigned char **p) {
    uint32_t n; memcpy(&n, *p, 4); *p += 4; return ntohl(n);
}

void index_init(bit_index *ix) { ix->e = 0; ix->n = 0; ix->cap = 0; }
void index_free(bit_index *ix) { free(ix->e); index_init(ix); }

/* Keep the array sorted by inserting into position, rather than appending and
   re-sorting. The old form called qsort on every insert: 450 inserts each
   re-sorting an array of 552-byte elements. Callers that add in sorted order
   (a tree walk does) now memmove nothing at all. */
void index_upsert(bit_index *ix, const bit_entry *e) {
    long lo = 0, hi = (long)ix->n - 1, at = 0;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(ix->e[mid].path, e->path);
        if (c == 0) { ix->e[mid] = *e; return; }        /* replace in place */
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    at = lo;                                            /* sorted insertion point */
    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 64;
        ix->e = xrealloc(ix->e, ix->cap * sizeof *ix->e);
    }
    if ((size_t)at < ix->n)
        memmove(&ix->e[at + 1], &ix->e[at], (ix->n - (size_t)at) * sizeof *ix->e);
    ix->e[at] = *e;
    ix->n++;
}

/* The index's own mtime. An entry whose recorded mtime is not strictly older
 * than the index cannot be trusted from stat alone: a file rewritten in the
 * same second, to the same length, is indistinguishable from one that never
 * changed. git calls these entries "racily clean" and re-reads them. */
static uint32_t index_mtime;

int index_read(bit_index *ix) {
    index_init(ix);
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char p[1200]; snprintf(p, sizeof p, "%s/index", git);
    { struct stat ist; index_mtime = stat(p, &ist) == 0 ? (uint32_t)ist.st_mtime : 0; }
    size_t len; unsigned char *buf = (unsigned char *)slurp(p, &len);
    if (!buf) return 0;                                   /* no index yet */
    if (len < 12 || memcmp(buf, "DIRC", 4)) { free(buf); return 0; }
    const unsigned char *q = buf + 4;
    uint32_t ver = get32(&q), n = get32(&q);
    if (ver != 2) { free(buf); return 0; }
    for (uint32_t i = 0; i < n && (size_t)(q - buf) < len; i++) {
        const unsigned char *start = q;
        bit_entry e; memset(&e, 0, sizeof e);
        e.ctime = get32(&q); get32(&q);
        e.mtime = get32(&q); get32(&q);
        e.dev = get32(&q); e.ino = get32(&q); e.mode = get32(&q);
        e.uid = get32(&q); e.gid = get32(&q); e.size = get32(&q);
        memcpy(e.id.b, q, OID_RAW); q += OID_RAW;
        uint16_t flags; memcpy(&flags, q, 2); q += 2;
        uint16_t nlen = ntohs(flags) & 0x0fff;
        if (nlen >= sizeof e.path) nlen = sizeof e.path - 1;
        memcpy(e.path, q, nlen); e.path[nlen] = 0;
        q += nlen;
        size_t used = (size_t)(q - start);
        q += (8 - (used % 8)) ? (8 - (used % 8)) : 8;     /* at least one NUL */
        index_upsert(ix, &e);
    }
    free(buf);
    return 0;
}

int index_write(const bit_index *ix) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    size_t cap = 12 + ix->n * (62 + 512 + 8) + 20;
    unsigned char *buf = xcalloc(1, cap), *p = buf;
    memcpy(p, "DIRC", 4); p += 4;
    put32(&p, 2); put32(&p, (uint32_t)ix->n);
    for (size_t i = 0; i < ix->n; i++) {
        const bit_entry *e = &ix->e[i];
        unsigned char *start = p;
        put32(&p, e->ctime); put32(&p, 0);
        put32(&p, e->mtime); put32(&p, 0);
        put32(&p, e->dev); put32(&p, e->ino); put32(&p, e->mode);
        put32(&p, e->uid); put32(&p, e->gid); put32(&p, e->size);
        memcpy(p, e->id.b, OID_RAW); p += OID_RAW;
        size_t nlen = strlen(e->path);
        uint16_t flags = htons((uint16_t)(nlen < 0x0fff ? nlen : 0x0fff));
        memcpy(p, &flags, 2); p += 2;
        memcpy(p, e->path, nlen); p += nlen;
        size_t used = (size_t)(p - start);
        size_t pad = 8 - (used % 8); if (pad == 0) pad = 8;
        memset(p, 0, pad); p += pad;
    }
    unsigned char sum[OID_RAW];
    CC_SHA1(buf, (CC_LONG)(p - buf), sum);
    memcpy(p, sum, OID_RAW); p += OID_RAW;

    char path[1200]; snprintf(path, sizeof path, "%s/index", git);
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return -1; }
    ssize_t want = p - buf;
    if (write(fd, buf, (size_t)want) != want) { close(fd); free(buf); return -1; }
    close(fd); rename(tmp, path); free(buf);
    return 0;
}

/* ---- trees ----
 * Tree entry: "<mode> <name>\0" followed by 20 raw bytes. Subtrees use mode
 * 40000 with no leading zero. git sorts entries as though a subtree name ends
 * in '/', which changes the order and therefore the digest. */

static int tree_name_cmp(const char *a, int a_dir, const char *b, int b_dir) {
    char x[520], y[520];
    snprintf(x, sizeof x, "%s%s", a, a_dir ? "/" : "");
    snprintf(y, sizeof y, "%s%s", b, b_dir ? "/" : "");
    return strcmp(x, y);
}

/* Build the tree for `prefix` (empty for the root) from index entries. */
static int build_tree(const bit_index *ix, const char *prefix, oid *out) {
    size_t plen = strlen(prefix);
    /* On the heap, and grown as needed. This array was 4,096 fixed entries of
       ~540 bytes -- a 2.1 MB stack frame per recursion level, which overflowed
       an 8 MB stack at three levels of nesting. The test suite never caught it
       because every fixture nested only two. */
    typedef struct { char name[512]; int is_dir; oid id; uint32_t mode; } kid_t;
    size_t nk = 0, kcap = 64;
    kid_t *kids = xmalloc(kcap * sizeof *kids);
    if (!kids) return -1;

    for (size_t i = 0; i < ix->n; i++) {
        const char *path = ix->e[i].path;
        if (plen && (strncmp(path, prefix, plen) || path[plen] != '/')) continue;
        const char *rest = plen ? path + plen + 1 : path;
        const char *slash = strchr(rest, '/');
        char name[512];
        if (slash) { size_t n = (size_t)(slash - rest); memcpy(name, rest, n); name[n] = 0; }
        else snprintf(name, sizeof name, "%s", rest);

        size_t j = 0;
        for (; j < nk; j++) if (!strcmp(kids[j].name, name)) break;
        if (j < nk) continue;                              /* directory already seen */

        if (nk == kcap) {
            kcap *= 2;
            kid_t *grown = xrealloc(kids, kcap * sizeof *kids);
            if (!grown) { free(kids); return -1; }
            kids = grown;
        }
        snprintf(kids[nk].name, sizeof kids[nk].name, "%s", name);
        kids[nk].is_dir = slash != 0;
        if (slash) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s%s%s", prefix, plen ? "/" : "", name);
            if (build_tree(ix, sub, &kids[nk].id) < 0) { free(kids); return -1; }
            kids[nk].mode = 040000;
        } else {
            kids[nk].id = ix->e[i].id;
            kids[nk].mode = ix->e[i].mode;
        }
        nk++;
    }

    for (size_t a = 0; a + 1 < nk; a++)                    /* insertion sort, git order */
        for (size_t b = a + 1; b < nk; b++)
            if (tree_name_cmp(kids[a].name, kids[a].is_dir, kids[b].name, kids[b].is_dir) > 0) {
                kid_t t = kids[a]; kids[a] = kids[b]; kids[b] = t;
            }

    size_t cap = nk * 560 + 64, len = 0;
    unsigned char *buf = xmalloc(cap);
    for (size_t i = 0; i < nk; i++) {
        len += (size_t)snprintf((char *)buf + len, cap - len, "%o %s",
                                kids[i].mode, kids[i].name);
        buf[len++] = 0;
        memcpy(buf + len, kids[i].id.b, OID_RAW); len += OID_RAW;
    }
    int rc = object_write("tree", buf, len, 1, out);
    free(buf);
    free(kids);
    return rc;
}

int tree_from_index(const bit_index *ix, oid *out) { return build_tree(ix, "", out); }

int entry_matches_stat(const bit_entry *e, const char *full) {
    struct stat st;
    if (stat(full, &st) < 0) return 0;
    if (e->size  != (uint32_t)st.st_size)  return 0;
    if (e->mtime != (uint32_t)st.st_mtime) return 0;
    if (e->ino   != (uint32_t)st.st_ino)   return 0;
    /* Racily clean: the entry was stamped in the same second the index was
       written, so a later same-second, same-length rewrite would look identical.
       Refuse the shortcut and let the caller hash. */
    if (index_mtime && e->mtime >= index_mtime) return 0;
    return 1;
}

int tree_walk(const oid *tree, const char *prefix, tree_cb cb, void *ctx) {
    char type[32]; size_t len;
    unsigned char *d = object_read(tree, type, sizeof type, &len);
    if (!d || strcmp(type, "tree")) { free(d); return -1; }

    int rc = 0;
    unsigned char *p = d, *end = d + len;
    while (p < end && !rc) {
        char *sp = strchr((char *)p, ' ');
        if (!sp) break;
        uint32_t mode = (uint32_t)strtoul((char *)p, 0, 8);
        char *name = sp + 1;
        char *nul = name + strlen(name);
        oid id; memcpy(id.b, nul + 1, OID_RAW);

        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name);
        rc = (mode & 040000) ? tree_walk(&id, path, cb, ctx) : cb(path, mode, &id, ctx);
        p = (unsigned char *)nul + 1 + OID_RAW;
    }
    free(d);
    return rc;
}

struct delta_index_s;
struct delta_index_s *delta_index_build(const unsigned char *base, size_t blen);
void delta_index_free(struct delta_index_s *ix);
unsigned char *delta_create_indexed(const struct delta_index_s *ix,
                                    const unsigned char *base, size_t blen,
                                    const unsigned char *tgt, size_t tlen, size_t *dlen);
unsigned char *delta_create_max(const struct delta_index_s *ix,
                                const unsigned char *base, size_t blen,
                                const unsigned char *tgt, size_t tlen,
                                size_t max, size_t *dlen);

/* A pack is mapped, not read.
 *
 * Every lookup used to slurp the entire pack file to reach one object: a
 * malloc, a full read, and a free, per object, so reading a 200-byte blob out
 * of a gigabyte pack read a gigabyte. The bucket directory made the *location*
 * free to compute and then the read threw that away. Mapped once per process
 * and cached by path, a lookup touches only the pages the bucket walk lands on.
 *
 * The cache is process-lifetime, which is right for one-shot commands. The one
 * writer, pack_write, renames a new pack into place and drops the entry so a
 * later read in the same process does not see the file it replaced. */
#define MAPCACHE 4
static struct { char path[1400]; unsigned char *base; size_t len; } mapc[MAPCACHE];
static int mapc_n;

static const unsigned char *map_file(const char *path, size_t *len) {
#ifdef PACK_SLURP
    return (const unsigned char *)slurp(path, len);   /* the old whole-file read */
#endif
    for (int i = 0; i < mapc_n; i++)
        if (!strcmp(mapc[i].path, path)) { *len = mapc[i].len; return mapc[i].base; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return 0; }
    void *p = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);                                  /* the mapping keeps it alive */
    if (p == MAP_FAILED) return 0;
    *len = (size_t)st.st_size;
    if (mapc_n < MAPCACHE) {
        snprintf(mapc[mapc_n].path, sizeof mapc[mapc_n].path, "%s", path);
        mapc[mapc_n].base = p; mapc[mapc_n].len = *len; mapc_n++;
    }
    return p;
}

static void bcache_drop(void);
static void map_forget(const char *path) {
    bcache_drop();                              /* offsets refer to the old file */
    for (int i = 0; i < mapc_n; i++)
        if (!strcmp(mapc[i].path, path)) {
            munmap(mapc[i].base, mapc[i].len);
            mapc[i] = mapc[--mapc_n];
            return;
        }
}

/* ---- pack ---- */

/* Bounded: a pack is a file, and a file can be truncated or wrong. On a
   truncated or over-long encoding this parks *p at `end` and sets *bad, which
   every caller checks before using the value. */
static uint64_t get_varint(const unsigned char **p, const unsigned char *end, int *bad) {
    uint64_t v = 0; int shift = 0;
    for (;;) {
        if (*p >= end || shift > 63) { *p = end; *bad = 1; return 0; }
        unsigned char b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}
uint64_t get_varint_pub(const unsigned char **p, const unsigned char *end, int *bad) {
    return get_varint(p, end, bad);
}
static int type_code(const char *t) {
    return !strcmp(t,"commit") ? 1 : !strcmp(t,"tree") ? 2 : 3;
}
static const char *type_name(int c) {
    return c == 1 ? "commit" : c == 2 ? "tree" : "blob";
}

#ifndef DELTA_WIN
#define DELTA_WIN   32
#endif
#ifndef DELTA_DEPTH
#define DELTA_DEPTH 50
#endif

#define PACK_MAGIC "BITPACK3"
#define DIR_MAGIC  "BITDIR01"

/* --- where an object lives ---
 *
 * There is no index. An index is a record of where an arbitrary write order
 * happened to put things, and it only exists because the order was arbitrary.
 * Here the digest is the address: an object's leading bits choose its bucket,
 * objects are written grouped by bucket, and a lookup computes the bucket and
 * walks it. What is stored is one offset per bucket -- about one bucket per
 * eight objects -- rather than one digest and one offset per object.
 *
 * Walking a bucket needs to tell entries apart without reconstructing each
 * one, so an entry carries a single fingerprint byte, taken from the far end
 * of the digest so it is independent of the bits that chose the bucket. A
 * fingerprint match is still only a filter; the object is rehashed against all
 * 160 bits before it is returned, exactly as the digest prefix used to be.
 *
 *                     per object          600 objects
 *   digest + offset   12 B (was)          7,212 B
 *   bucket directory   0.4 B              304 B
 *   fingerprint        1 B                600 B  (inside the pack)
 */
#ifndef BUCKET_LOAD
/* Objects per bucket. The directory costs one offset per bucket and a walk
   parses one entry header per object in it, so this trades directory bytes
   against walk time -- and the trade is very lopsided. Measured on 5,052
   objects:
 *
 *      load    directory   walk      total addressing cost
 *         8      0.61 B     31 ns    1.61 B/object
 *        64      0.08 B    209 ns    1.08 B/object
 *       256      0.02 B  1,570 ns    1.02 B/object
 *
 * The +1 B in every total is the fingerprint byte each entry carries, which is
 * what makes a walk cheap, and past load 64 it is the whole cost -- the
 * directory has stopped mattering while the walk keeps growing. 209 ns is also
 * invisible next to the ~470 us a lookup currently spends reading the pack
 * file, so the walk is free at this end and the directory is not. */
#define BUCKET_LOAD 64
#endif
#define TC_STORED   0x08
#define TC_DELTA    7

static uint32_t oid_bucket(const oid *o, int bits) {
    if (bits <= 0) return 0;
    uint32_t v = ((uint32_t)o->b[0] << 24) | ((uint32_t)o->b[1] << 16)
               | ((uint32_t)o->b[2] << 8)  |  (uint32_t)o->b[3];
    return v >> (32 - bits);
}
static int bucket_bits_for(size_t n) {
    int bits = 0;
    while (((size_t)1 << bits) * BUCKET_LOAD < n && bits < 24) bits++;
    return bits;
}
#define FP_BYTE 19                         /* last digest byte; not a bucket bit */

/* Raw deflate, not zlib. A zlib stream wraps the deflate data in a two-byte
   header and a four-byte adler32 -- six bytes per object whose only job is to
   detect corruption that the object's own digest already detects, and detect
   it worse. On a history-shaped repository, where the median object is a
   fifty-byte tree, those six bytes were 13% of the entire pack. */
static int raw_deflate(const unsigned char *in, size_t ilen,
                       unsigned char *out, size_t *olen) {
    z_stream z; memset(&z, 0, sizeof z);
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    z.next_in = (Bytef *)in;   z.avail_in  = (uInt)ilen;
    z.next_out = out;          z.avail_out = (uInt)*olen;
    int r = deflate(&z, Z_FINISH);
    *olen = z.total_out;
    deflateEnd(&z);
    return r == Z_STREAM_END ? 0 : -1;
}
static int raw_inflate(const unsigned char *in, size_t ilen,
                       unsigned char *out, size_t olen) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (Bytef *)in;   z.avail_in  = (uInt)ilen;
    z.next_out = out;          z.avail_out = (uInt)olen;
    int r = inflate(&z, Z_FINISH);
    int ok = (r == Z_STREAM_END && z.total_out == olen);
    inflateEnd(&z);
    return ok ? 0 : -1;
}

/* Names only. This used to mkdir the directory as a side effect, which put a
   filesystem mutation in the path of every object read: a lookup spent more
   time creating a directory that already existed than it did reconstructing
   the object it was asked for. Only the writer needs the directory to exist,
   so only the writer creates it. */
static void pack_paths_in(const char *git, char *pack, size_t pc, char *idx, size_t ic) {
    // Not .git/objects/pack: this is not git's pack format, and git tries to
    // parse anything it finds there, reporting "non-monotonic index".
    snprintf(pack, pc, "%s/bitpack/bit.pack", git);
    snprintf(idx,  ic, "%s/bitpack/bit.dir",  git);
}
static void pack_dir_create(const char *git) {
    char dir[1200]; snprintf(dir, sizeof dir, "%s/bitpack", git);
    mkdir(dir, 0755);
}
static void pack_paths(char *pack, size_t pc, char *idx, size_t ic) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) die("not a bit repository");
    pack_paths_in(git, pack, pc, idx, ic);
}

/* The bucket directory, parsed. Nothing reads its bytes directly: the offset
   width and the bucket count are chosen per repository and recorded. */
typedef struct { const unsigned char *b; uint32_t n, nb; int bits, ow; } dirview;

#define DIR_HDR 18                         /* magic 8, count 4, buckets 4, bits 1, ow 1 */

static int dir_open(const unsigned char *d, size_t dlen, dirview *v) {
    if (!d || dlen < DIR_HDR || memcmp(d, DIR_MAGIC, 8)) return -1;
    memcpy(&v->n,  d + 8,  4);
    memcpy(&v->nb, d + 12, 4);
    v->bits = d[16]; v->ow = d[17];
    if (v->ow < 1 || v->ow > 8 || v->bits > 24) return -1;
    if (v->nb != (uint32_t)1 << v->bits) return -1;
    if (dlen < DIR_HDR + ((size_t)v->nb + 1) * (size_t)v->ow) return -1;
    v->b = d + DIR_HDR;
    return 0;
}
static size_t dir_at(const dirview *v, uint32_t i) {
    const unsigned char *p = v->b + (size_t)i * v->ow;
    size_t o = 0;
    for (int k = v->ow - 1; k >= 0; k--) o = (o << 8) | p[k];
    return o;
}

static int offw_for(size_t sz) { int w = 1; while (w < 8 && sz > ((size_t)1 << (8 * w))) w++; return w; }

static void put_off(FILE *f, size_t v, int w) {
    unsigned char b[8];
    for (int k = 0; k < w; k++) b[k] = (unsigned char)(v >> (8 * k));
    fwrite(b, 1, (size_t)w, f);
}

static void dvarint_w(unsigned char **p, uint64_t v) {
    do { unsigned char c = v & 0x7f; v >>= 7; if (v) c |= 0x80; *(*p)++ = c; } while (v);
}
static size_t varint_len(uint64_t v) { size_t n = 0; do { n++; v >>= 7; } while (v); return n; }

/* A candidate delta base, held while the representation of each object is
   being decided. `rec` is a position in the record array, not a file offset:
   at this point nothing has been placed. */
typedef struct { int tc; unsigned char *data; size_t len; long rec; int depth;
                 struct delta_index_s *ix; } win_t;

typedef struct { oid id; int tc; size_t size; } meta_t;
static int meta_cmp(const void *a, const void *b) {
    const meta_t *x = a, *y = b;
    if (x->tc != y->tc) return x->tc - y->tc;
    if (x->size != y->size) return x->size < y->size ? 1 : -1;   /* large first */
    return memcmp(x->id.b, y->id.b, OID_RAW);
}

/* One object, after its representation has been decided but before it has been
   placed. `base` is an index into this same array, not a file offset, because
   nothing has an offset yet. */
typedef struct {
    oid id; int tc; int is_delta; int stored;
    long base;                       /* index into recs, or -1 */
    size_t ulen;                     /* inflated length of the payload */
    unsigned char *pay; size_t paylen;
    size_t off;                      /* filled in when written */
    size_t basefield;                /* file offset of the 4-byte base field */
    int depth;
} rec_t;

static int rec_by_digest(const void *a, const void *b) {
    return memcmp(((const rec_t *)a)->id.b, ((const rec_t *)b)->id.b, OID_RAW);
}

static unsigned char *pack_at(const unsigned char *pk, size_t plen, size_t off,
                              int *tc_out, size_t *len_out, int depth);

/* Parse one entry header. Returns the offset just past it, or 0 on a malformed
   entry. Every field the walk needs comes from here, so a bucket can be scanned
   without reconstructing anything. */
static size_t entry_head(const unsigned char *pk, size_t plen, size_t off,
                         int *tc, int *stored, unsigned char *fp,
                         size_t *basefield, uint64_t *ulen, uint64_t *zlen) {
    if (off + 2 > plen) return 0;
    const unsigned char *pend = pk + plen, *p = pk + off;
    int raw = *p++;
    *stored = raw & TC_STORED;
    *tc = raw & ~TC_STORED;
    *fp = *p++;
    if (*tc == TC_DELTA) {
        if ((size_t)(p - pk) + 4 > plen) return 0;
        if (basefield) *basefield = (size_t)(p - pk);
        p += 4;
    } else if (*tc < 1 || *tc > 3) return 0;
    int bad = 0;
    *ulen = get_varint(&p, pend, &bad);
    *zlen = get_varint(&p, pend, &bad);
    if (bad || *ulen > (uint64_t)1 << 34) return 0;
    if ((size_t)(p - pk) + *zlen > plen) return 0;
    return (size_t)(p - pk);
}

int pack_write(int *n_out, size_t *pack_out, size_t *idx_out) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char objdir[1200]; snprintf(objdir, sizeof objdir, "%s/objects", git);
    char packp[1300], dirp[1300];
    pack_dir_create(git);                        /* the writer, and only it */
    pack_paths(packp, sizeof packp, dirp, sizeof dirp);

    /* Pass 1: everything that must end up in the new pack -- the loose store
       and whatever the current pack already holds. Packing is how a repository
       reclaims disk, so by the second pack the loose copies of the first
       pack's objects are usually gone; enumerating only the loose store wrote
       a pack containing the new objects alone and renamed it over the one
       holding the history. */
    meta_t *m = 0; size_t n = 0, cap = 0;
    oidset seen; oidset_init(&seen);
    #define ADD_META(O, TC, SZ) do { \
        if (oidset_add(&seen, &(O))) { \
            if (n == cap) { cap = cap ? cap * 2 : 256; m = xrealloc(m, cap * sizeof *m); } \
            m[n].id = (O); m[n].tc = (TC); m[n].size = (SZ); n++; \
        } \
    } while (0)

    DIR *d = opendir(objdir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strlen(de->d_name) != 2) continue;
            char sub[1400]; snprintf(sub, sizeof sub, "%s/%s", objdir, de->d_name);
            DIR *sd = opendir(sub); if (!sd) continue;
            struct dirent *se;
            while ((se = readdir(sd))) {
                if (strlen(se->d_name) != OID_HEX - 2) continue;
                char hex[OID_HEX + 1];
                snprintf(hex, sizeof hex, "%s%s", de->d_name, se->d_name);
                oid o; if (oid_from_hex(hex, &o) < 0) continue;
                char type[32]; size_t len;
                void *data = object_read(&o, type, sizeof type, &len);
                if (!data) continue;
                ADD_META(o, type_code(type), len);
                free(data);
            }
            closedir(sd);
        }
        closedir(d);
    }

    size_t oplen = 0; const unsigned char *oldpk = map_file(packp, &oplen);
    if (oldpk && oplen >= 12 && !memcmp(oldpk, PACK_MAGIC, 8)) {
        size_t dlen; const unsigned char *od = map_file(dirp, &dlen);
        dirview v;
        if (od && dir_open(od, dlen, &v) == 0) {
            size_t off = 12, endall = dir_at(&v, v.nb);
            while (off < endall && off < oplen) {
                int tc, st; unsigned char fp; uint64_t ul, zl;
                size_t body = entry_head(oldpk, oplen, off, &tc, &st, &fp, 0, &ul, &zl);
                if (!body) break;
                int rtc; size_t rlen;
                unsigned char *data = pack_at(oldpk, oplen, off, &rtc, &rlen, 0);
                if (data) {
                    oid o; object_write(type_name(rtc), data, rlen, 0, &o);
                    free(data);
                    if (o.b[FP_BYTE] == fp) ADD_META(o, rtc, rlen);   /* else corrupt */
                }
                off = body + zl;
            }
        }
    }
    oidset_free(&seen);
    #undef ADD_META

    /* Pass 2: decide each object's representation, in similarity order, so
       that objects likely to resemble each other are offered to each other as
       delta bases. Nothing is placed yet -- a base is recorded as a position
       in this array, not as a file offset. */
    qsort(m, n, sizeof *m, meta_cmp);
    rec_t *recs = xmalloc((n ? n : 1) * sizeof *recs);
    long *where = xmalloc((n ? n : 1) * sizeof *where);   /* meta index -> rec index */
    size_t nr = 0;

    win_t win[DELTA_WIN]; int nwin = 0, wnext = 0;
    memset(win, 0, sizeof win);

    for (size_t i = 0; i < n; i++) {
        where[i] = -1;
        char type[32]; size_t len;
        unsigned char *data = object_read(&m[i].id, type, sizeof type, &len);
        if (!data) continue;
        int tc = type_code(type);

        size_t zfull = compressBound((uLong)len) + 64;
        unsigned char *zf = xmalloc(zfull);
        int fstored = 0;
        if (raw_deflate(data, len, zf, &zfull) != 0 || zfull >= len) {
            memcpy(zf, data, len); zfull = len; fstored = 1;
        }

        unsigned char *bestz = 0; size_t bestzlen = 0, bestraw = 0;
        long bestbase = -1; int bestdepth = 0, beststored = 0;
        /* Newest first. Objects arrive in size order, so the most recently
           seen candidates are the closest in size and the likeliest to give a
           small delta -- which sets a tight ceiling early and lets every
           candidate after it be abandoned sooner. */
        /* The ceiling bounds the *raw* delta, so it starts at the object's raw
           length -- not at zfull, which is the deflated whole. Comparing a raw
           delta against a deflated size rejected deltas that would have won
           easily once compressed: on bit's own source it threw away half of
           them and cost 12% of the pack. The selection among survivors is
           still decided on deflated size. */
        size_t ceiling = len;
        for (int k = 0; k < nwin; k++) {
            int wi = (wnext - 1 - k + DELTA_WIN * 2) % DELTA_WIN;
            if (wi >= nwin && nwin < DELTA_WIN) continue;
            if (win[wi].tc != tc || win[wi].depth >= DELTA_DEPTH || !win[wi].ix) continue;
            size_t dl;
            unsigned char *dd = delta_create_max(win[wi].ix, win[wi].data,
                                                 win[wi].len, data, len, ceiling, &dl);
            if (!dd) continue;
            size_t zd = compressBound((uLong)dl) + 64;
            unsigned char *zz = xmalloc(zd);
            int dstored = 0;
            if (raw_deflate(dd, dl, zz, &zd) != 0 || zd >= dl) {
                memcpy(zz, dd, dl); zd = dl; dstored = 1;
            }
            free(dd);
            if (!bestz || zd < bestzlen) {
                free(bestz);
                bestz = zz; bestzlen = zd; bestraw = dl;
                bestbase = win[wi].rec; bestdepth = win[wi].depth + 1;
                beststored = dstored;
                if (dl < ceiling) ceiling = dl;        /* tightens as it improves */
            } else free(zz);
        }

        int use_delta = 0;
        if (bestz) {
            /* whole entries, headers included: a delta carries a 4-byte base */
            size_t dtot = 2 + 4 + varint_len(bestraw) + varint_len(bestzlen) + bestzlen;
            size_t ftot = 2 + varint_len(len) + varint_len(zfull) + zfull;
            if (dtot < ftot) use_delta = 1;
        }

        rec_t *r = &recs[nr];
        r->id = m[i].id; r->tc = tc; r->base = -1; r->basefield = 0;
        if (use_delta) {
            r->is_delta = 1; r->stored = beststored; r->base = bestbase;
            r->ulen = bestraw; r->pay = bestz; r->paylen = bestzlen;
            r->depth = bestdepth;
            free(zf);
        } else {
            r->is_delta = 0; r->stored = fstored;
            r->ulen = len; r->pay = zf; r->paylen = zfull;
            r->depth = 0;
            free(bestz);
        }
        where[i] = (long)nr;
        nr++;

        free(win[wnext].data); delta_index_free(win[wnext].ix);
        win[wnext].tc = tc; win[wnext].data = data; win[wnext].len = len;
        win[wnext].ix = delta_index_build(data, len);   /* built once, used W times */
        win[wnext].rec = (long)(nr - 1); win[wnext].depth = r->depth;
        wnext = (wnext + 1) % DELTA_WIN;
        if (nwin < DELTA_WIN) nwin++;
    }
    for (int wi = 0; wi < nwin; wi++) { free(win[wi].data); delta_index_free(win[wi].ix); }
    free(m); free(where);

    /* Pass 3: place. Sorting by digest puts every object in its bucket and the
       buckets in order, which is the whole point: the reader recomputes this
       from the digest instead of looking it up.
       Base links survive the reordering because they are array positions, not
       offsets. A base may now sit *after* the entry that references it, so the
       reader's guarantee of termination is the depth cap rather than a
       monotonic offset -- the links themselves are acyclic by construction,
       each having been chosen from an object decided earlier. */
    long *newpos = xmalloc((nr ? nr : 1) * sizeof *newpos);
    for (size_t i = 0; i < nr; i++) recs[i].off = i;      /* remember old position */
    rec_t *sorted = xmalloc((nr ? nr : 1) * sizeof *sorted);
    memcpy(sorted, recs, nr * sizeof *recs);
    qsort(sorted, nr, sizeof *sorted, rec_by_digest);
    for (size_t i = 0; i < nr; i++) newpos[sorted[i].off] = (long)i;
    for (size_t i = 0; i < nr; i++)
        if (sorted[i].base >= 0) sorted[i].base = newpos[sorted[i].base];
    free(recs); free(newpos);
    recs = sorted;

    int bits = bucket_bits_for(nr);
    uint32_t nb = (uint32_t)1 << bits;

    char tpackp[1400], tdirp[1400];
    snprintf(tpackp, sizeof tpackp, "%s.tmp", packp);
    snprintf(tdirp,  sizeof tdirp,  "%s.tmp", dirp);

    FILE *pf = fopen(tpackp, "wb");
    if (!pf) { for (size_t i = 0; i < nr; i++) free(recs[i].pay); free(recs); return -1; }
    fwrite(PACK_MAGIC, 1, 8, pf);
    uint32_t cnt = (uint32_t)nr; fwrite(&cnt, 4, 1, pf);

    size_t *bstart = xmalloc(((size_t)nb + 1) * sizeof *bstart);
    for (uint32_t b = 0; b <= nb; b++) bstart[b] = 0;
    uint32_t curb = 0;
    int overflow = 0;
    for (size_t i = 0; i < nr; i++) {
        long here = ftell(pf);
        if (here < 0 || here > 0xFFFFFFFFL) { overflow = 1; break; }
        recs[i].off = (size_t)here;
        uint32_t b = oid_bucket(&recs[i].id, bits);
        while (curb <= b) bstart[curb++] = (size_t)here;     /* empty buckets too */

        fputc((recs[i].is_delta ? TC_DELTA : recs[i].tc)
              | (recs[i].stored ? TC_STORED : 0), pf);
        fputc(recs[i].id.b[FP_BYTE], pf);
        if (recs[i].is_delta) {
            recs[i].basefield = (size_t)ftell(pf);
            unsigned char z4[4] = {0,0,0,0}; fwrite(z4, 1, 4, pf);   /* patched below */
        }
        unsigned char vb[24], *vp = vb;
        dvarint_w(&vp, recs[i].ulen);
        dvarint_w(&vp, recs[i].paylen);
        fwrite(vb, 1, (size_t)(vp - vb), pf);
        fwrite(recs[i].pay, 1, recs[i].paylen, pf);
    }
    size_t endall = (size_t)ftell(pf);
    while (curb <= nb) bstart[curb++] = endall;

    /* Pass 4: patch each delta's base offset, now that every object has one. */
    if (!overflow) {
        for (size_t i = 0; i < nr; i++) {
            if (!recs[i].is_delta) continue;
            uint32_t bo = (uint32_t)recs[recs[i].base].off;
            fseek(pf, (long)recs[i].basefield, SEEK_SET);
            unsigned char b4[4];
            for (int k = 0; k < 4; k++) b4[k] = (unsigned char)(bo >> (8 * k));
            fwrite(b4, 1, 4, pf);
        }
    }
    for (size_t i = 0; i < nr; i++) free(recs[i].pay);
    free(recs);

    if (overflow || ferror(pf)) { fclose(pf); unlink(tpackp); free(bstart); return -1; }
    fseek(pf, 0, SEEK_END);
    size_t psize = (size_t)ftell(pf);
    if (fclose(pf) != 0) { unlink(tpackp); free(bstart); return -1; }

    int ow = offw_for(psize);
    FILE *xf = fopen(tdirp, "wb");
    if (!xf) { unlink(tpackp); free(bstart); return -1; }
    fwrite(DIR_MAGIC, 1, 8, xf);
    fwrite(&cnt, 4, 1, xf);
    fwrite(&nb, 4, 1, xf);
    fputc(bits, xf); fputc(ow, xf);
    for (uint32_t b = 0; b <= nb; b++) put_off(xf, bstart[b], ow);
    size_t dsize = (size_t)ftell(xf);
    int xerr = ferror(xf); if (fclose(xf) != 0) xerr = 1;
    free(bstart);
    if (xerr) { unlink(tpackp); unlink(tdirp); return -1; }

    map_forget(packp); map_forget(dirp);        /* the mapped files are gone */
    if (rename(tpackp, packp) != 0) { unlink(tpackp); unlink(tdirp); return -1; }
    if (rename(tdirp, dirp) != 0) return -1;

    if (n_out) *n_out = (int)nr;
    if (pack_out) *pack_out = psize;
    if (idx_out) *idx_out = dsize;
    return 0;
}

int pack_stats(int *nobj, int *ndelta_out) {
    char packp[1300], dirp[1300];
    pack_paths(packp, sizeof packp, dirp, sizeof dirp);
    size_t plen; const unsigned char *pk = map_file(packp, &plen);
    size_t dlen; const unsigned char *dd = map_file(dirp, &dlen);
    dirview v;
    if (!pk || dir_open(dd, dlen, &v) < 0) return -1;
    size_t off = 12, endall = dir_at(&v, v.nb);
    int nd = 0;
    while (off < endall && off < plen) {
        int tc, st; unsigned char fp; uint64_t ul, zl;
        size_t body = entry_head(pk, plen, off, &tc, &st, &fp, 0, &ul, &zl);
        if (!body) break;
        if (tc == TC_DELTA) nd++;
        off = body + zl;
    }
    if (nobj) *nobj = (int)v.n;
    if (ndelta_out) *ndelta_out = nd;
    return 0;
}

/* Resolved delta bases, remembered for the life of the process.
 *
 * Reconstructing a delta means reconstructing its base, which may itself be a
 * delta: a chain of depth d costs d inflations. Without this, every lookup
 * walked its whole chain from the bottom, and a checkout of a repository where
 * 99% of objects are deltas re-resolved thousands of heavily overlapping
 * chains -- the same base rebuilt once per object that depends on it. git
 * keeps a delta base cache for exactly this reason.
 *
 * Keyed by pack offset, which is what the chain actually references, and
 * bounded by bytes rather than entries so one enormous object cannot evict
 * everything. Entries hand out copies; the cache owns its own. */
#define BASECACHE_SLOTS 1024
#define BASECACHE_BYTES (64u << 20)
typedef struct { size_t off; int tc; unsigned char *data; size_t len; } bcent;
static bcent bcache[BASECACHE_SLOTS];
static size_t bcache_bytes;

static void bcache_drop(void) {
    for (int i = 0; i < BASECACHE_SLOTS; i++) {
        free(bcache[i].data); bcache[i].data = 0; bcache[i].off = 0;
    }
    bcache_bytes = 0;
}
static unsigned char *bcache_get(size_t off, int *tc, size_t *len) {
    bcent *e = &bcache[(off * 2654435761u >> 4) & (BASECACHE_SLOTS - 1)];
    if (!e->data || e->off != off) return 0;
    unsigned char *copy = xmalloc(e->len + 1);
    memcpy(copy, e->data, e->len); copy[e->len] = 0;
    *tc = e->tc; *len = e->len;
    return copy;
}
static void bcache_put(size_t off, int tc, const unsigned char *data, size_t len) {
    if (len > BASECACHE_BYTES / 8) return;             /* never let one entry rule */
    if (bcache_bytes + len > BASECACHE_BYTES) bcache_drop();
    bcent *e = &bcache[(off * 2654435761u >> 4) & (BASECACHE_SLOTS - 1)];
    if (e->data) { bcache_bytes -= e->len; free(e->data); }
    e->data = xmalloc(len + 1);
    memcpy(e->data, data, len); e->data[len] = 0;
    e->off = off; e->tc = tc; e->len = len;
    bcache_bytes += len;
}

/* Reconstruct the object at `off`, following the base chain if it is a delta.
   Placement is by digest, so a base may sit either side of the entry that
   references it; the links are acyclic by construction and `depth` is what
   makes a corrupt file terminate. */
static unsigned char *pack_at(const unsigned char *pk, size_t plen, size_t off,
                              int *tc_out, size_t *len_out, int depth) {
    if (depth > DELTA_DEPTH + 2) return 0;
    int tc, stored; unsigned char fp; uint64_t ulen, zlen; size_t bf = 0;
    size_t body = entry_head(pk, plen, off, &tc, &stored, &fp, &bf, &ulen, &zlen);
    if (!body) return 0;
    const unsigned char *p = pk + body;

    if (tc != TC_DELTA) {
        unsigned char *out = xmalloc(ulen + 1);
        if (stored) {
            if (zlen != ulen) { free(out); return 0; }
            memcpy(out, p, (size_t)ulen);
        } else if (raw_inflate(p, (size_t)zlen, out, (size_t)ulen) != 0) {
            free(out); return 0;
        }
        out[ulen] = 0;
        if (tc_out) *tc_out = tc;
        if (len_out) *len_out = (size_t)ulen;
        return out;
    }

    uint32_t boff = 0;
    for (int k = 3; k >= 0; k--) boff = (boff << 8) | pk[bf + k];
    if (boff == off || boff >= plen) return 0;
    unsigned char *dz = xmalloc(ulen + 1);
    if (stored) {
        if (zlen != ulen) { free(dz); return 0; }
        memcpy(dz, p, (size_t)ulen);
    } else if (raw_inflate(p, (size_t)zlen, dz, (size_t)ulen) != 0) { free(dz); return 0; }

    int btc; size_t blen;
    unsigned char *base = bcache_get(boff, &btc, &blen);
    if (!base) {
        base = pack_at(pk, plen, boff, &btc, &blen, depth + 1);
        if (base) bcache_put(boff, btc, base, blen);
    }
    if (!base) { free(dz); return 0; }

    size_t olen;
    unsigned char *out = delta_apply(base, blen, dz, (size_t)ulen, &olen);
    free(base); free(dz);
    if (!out) return 0;
    if (tc_out) *tc_out = btc;                 /* a delta inherits its base's type */
    if (len_out) *len_out = olen;
    return out;
}

/* The lookup. The digest names the bucket; the bucket is walked. No stored
   digest, no stored per-object offset -- the fingerprint byte in each entry is
   what makes the walk cheap, and the rehash at the end is what makes it
   correct. */
void *pack_read_in(const char *gitdir, const oid *o, char *type_out,
                   size_t type_cap, size_t *len_out) {
    char packp[1400], dirp[1400];
    pack_paths_in(gitdir, packp, sizeof packp, dirp, sizeof dirp);
    size_t dlen; const unsigned char *dd = map_file(dirp, &dlen);
    dirview v;
    if (dir_open(dd, dlen, &v) < 0) return 0;
    uint32_t b = oid_bucket(o, v.bits);
    size_t off = dir_at(&v, b), stop = dir_at(&v, b + 1);
    if (stop <= off) return 0;

    size_t plen; const unsigned char *pk = map_file(packp, &plen);
    if (!pk) return 0;
    if (plen < 12 || memcmp(pk, PACK_MAGIC, 8)) return 0;
    if (stop > plen) stop = plen;

    while (off < stop) {
        int tc, st; unsigned char fp; uint64_t ul, zl;
        size_t body = entry_head(pk, plen, off, &tc, &st, &fp, 0, &ul, &zl);
        if (!body) break;
        if (fp == o->b[FP_BYTE]) {                 /* a filter, not an answer */
            int rtc; size_t rlen;
            unsigned char *out = pack_at(pk, plen, off, &rtc, &rlen, 0);
            if (out) {
                oid check; object_write(type_name(rtc), out, rlen, 0, &check);
                if (oid_eq(&check, o)) {
                    if (type_out) snprintf(type_out, type_cap, "%s", type_name(rtc));
                    if (len_out) *len_out = rlen;
                    return out;
                }
                free(out);
            }
        }
        off = body + zl;
    }
    return 0;
}

int pack_unpack(int *n_out) {
    char packp[1300], dirp[1300];
    pack_paths(packp, sizeof packp, dirp, sizeof dirp);
    size_t plen; const unsigned char *pk = map_file(packp, &plen);
    if (!pk) return -1;
    if (plen < 12 || memcmp(pk, PACK_MAGIC, 8)) {
        if (plen >= 8 && !memcmp(pk, "BITPACK", 7))
            fprintf(stderr, "bit: this pack is an earlier format (%.8s); this "
                            "build writes %s.\n", pk, PACK_MAGIC);
        return -1;
    }
    size_t dlen; const unsigned char *dd = map_file(dirp, &dlen);
    dirview v;
    if (dir_open(dd, dlen, &v) < 0) return -1;

    /* Every object is checked against the fingerprint recorded with it before
       it is written. Unpacking is the one path that turns pack bytes into
       repository state, and it used to trust them completely: a single flipped
       type byte made a tree land as a blob under a different name, the real
       tree vanish, and the command still report success. */
    int done = 0, bad = 0;
    size_t off = 12, endall = dir_at(&v, v.nb);
    if (endall > plen) endall = plen;
    while (off < endall) {
        int tc, st; unsigned char fp; uint64_t ul, zl;
        size_t body = entry_head(pk, plen, off, &tc, &st, &fp, 0, &ul, &zl);
        if (!body) { bad++; break; }
        int rtc; size_t rlen;
        unsigned char *out = pack_at(pk, plen, off, &rtc, &rlen, 0);
        if (!out) { bad++; off = body + zl; continue; }
        oid o;
        object_write(type_name(rtc), out, rlen, 0, &o);        /* hash, do not write */
        if (o.b[FP_BYTE] != fp) { free(out); bad++; off = body + zl; continue; }
        object_write(type_name(rtc), out, rlen, 1, &o);        /* now write it loose */
        free(out);
        done++;
        off = body + zl;
    }
    if (n_out) *n_out = done;
    if (bad) {
        fprintf(stderr, "bit: %d objects in the pack are corrupt and were not "
                        "written; the repository is incomplete\n", bad);
        return -1;
    }
    return 0;
}

void *pack_read(const oid *o, char *type_out, size_t type_cap, size_t *len_out) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) return 0;
    return pack_read_in(git, o, type_out, type_cap, len_out);
}

int pack_exists_in(const char *gitdir, const oid *o) {
    void *d = pack_read_in(gitdir, o, 0, 0, 0);
    if (!d) return 0;
    free(d);
    return 1;
}

int pack_exists(const oid *o) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) return 0;
    return pack_exists_in(git, o);
}

/* ---- diff ---- */

typedef struct { const char *p; size_t n; } line;

static line *split_lines(const char *s, size_t len, size_t *n_out) {
    size_t cap = 64, n = 0;
    line *v = xmalloc(cap * sizeof *v);
    const char *p = s, *end = s + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        if (n == cap) { cap *= 2; v = xrealloc(v, cap * sizeof *v); }
        v[n].p = p; v[n].n = l; n++;
        p += l;
    }
    *n_out = n;
    return v;
}
static int line_eq(const line *x, const line *y) {
    return x->n == y->n && memcmp(x->p, y->p, x->n) == 0;
}
static void emit(FILE *out, char sign, const line *l) {
    fputc(sign, out);
    fwrite(l->p, 1, l->n, out);
    if (l->n == 0 || l->p[l->n - 1] != '\n') fputs("\n\\ No newline at end of file\n", out);
}

/* Myers greedy, with the trace kept so the edit script can be recovered.
 * Operates on the trimmed middle only. */
static void myers(FILE *out, const line *a, int n, const line *b, int m) {
    int max = n + m;
    if (max == 0) return;
    int *v = xmalloc((size_t)(2 * max + 1) * sizeof *v);
    int **trace = xmalloc((size_t)(max + 1) * sizeof *trace);
    int off = max, depth = -1;
    memset(v, 0, (size_t)(2 * max + 1) * sizeof *v);

    for (int d = 0; d <= max; d++) {
        trace[d] = xmalloc((size_t)(2 * max + 1) * sizeof **trace);
        memcpy(trace[d], v, (size_t)(2 * max + 1) * sizeof *v);
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1]))
                    ? v[off + k + 1] : v[off + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && line_eq(&a[x], &b[y])) { x++; y++; }
            v[off + k] = x;
            if (x >= n && y >= m) { depth = d; goto done; }
        }
    }
done:
    if (depth < 0) depth = max;
    /* walk the trace backwards into an edit script, then replay it forwards */
    int *ops = xmalloc((size_t)(max + 1) * 2 * sizeof *ops), nops = 0;
    int x = n, y = m;
    for (int d = depth; d > 0; d--) {
        int *pv = trace[d];      /* V before expansion d = state after d-1 */
        int k = x - y;
        int pk = (k == -d || (k != d && pv[off + k - 1] < pv[off + k + 1])) ? k + 1 : k - 1;
        int px = pv[off + pk], py = px - pk;
        while (x > px && y > py) { ops[nops++] = 0; x--; y--; }      /* context */
        if (x == px) { ops[nops++] = 1; y--; }                        /* insert */
        else         { ops[nops++] = 2; x--; }                        /* delete */
    }
    while (x > 0 && y > 0) { ops[nops++] = 0; x--; y--; }
    for (int i = nops - 1, ax = 0, bx = 0; i >= 0; i--) {
        if (ops[i] == 0)      { emit(out, ' ', &a[ax]); ax++; bx++; }
        else if (ops[i] == 1) { emit(out, '+', &b[bx]); bx++; }
        else                  { emit(out, '-', &a[ax]); ax++; }
    }
    for (int d = 0; d <= depth; d++) free(trace[d]);
    free(trace); free(v); free(ops);
}

int diff_unified(FILE *out, const char *label,
                 const char *a, size_t alen, const char *b, size_t blen) {
    if (alen == blen && memcmp(a, b, alen) == 0) return 0;   /* cheapest test first */

    size_t na, nb;
    line *la = split_lines(a, alen, &na), *lb = split_lines(b, blen, &nb);

    /* Trim. D is what Myers is exponential in, and a typical edit leaves a
       handful of changed lines between two long identical runs. */
    size_t pre = 0;
    while (pre < na && pre < nb && line_eq(&la[pre], &lb[pre])) pre++;
    size_t suf = 0;
    while (suf < na - pre && suf < nb - pre &&
           line_eq(&la[na - 1 - suf], &lb[nb - 1 - suf])) suf++;
    /* Give back three common lines on each side. Unified diff shows context,
       and the trim exists to bound D, not to minimise output. */
    const size_t CTX = 3;
    pre = pre > CTX ? pre - CTX : 0;
    suf = suf > CTX ? suf - CTX : 0;

    fprintf(out, "--- a/%s\n+++ b/%s\n", label, label);
    size_t alen_h = na - pre - suf, blen_h = nb - pre - suf;
    fprintf(out, "@@ -%zu,%zu +%zu,%zu @@\n",
            alen_h ? pre + 1 : pre, alen_h,
            blen_h ? pre + 1 : pre, blen_h);   /* an empty range starts at 0 */
    myers(out, la + pre, (int)(na - pre - suf), lb + pre, (int)(nb - pre - suf));

    free(la); free(lb);
    return 1;
}

/* ---- refs and worktree ---- */

int commit_tree_oid(const oid *c, oid *tree_out) {
    char type[32]; size_t len;
    char *body = object_read(c, type, sizeof type, &len);
    if (!body) return -1;
    int rc = -1;
    if (!strcmp(type, "tree")) { *tree_out = *c; rc = 0; }
    else if (!strcmp(type, "commit") && !strncmp(body, "tree ", 5)) {
        char hex[OID_HEX + 1]; memcpy(hex, body + 5, OID_HEX); hex[OID_HEX] = 0;
        rc = oid_from_hex(hex, tree_out);
    }
    free(body);
    return rc;
}

typedef struct { const char *root; bit_index *ix; const bit_index *old; int n; } co_ctx;

static const bit_entry *ix_find(const bit_index *ix, const char *path) {
    long lo = 0, hi = (long)ix->n - 1;                 /* the index is sorted */
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(ix->e[mid].path, path);
        if (!c) return &ix->e[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static void stamp(bit_entry *e, const char *path, const oid *id, uint32_t mode, const struct stat *st) {
    memset(e, 0, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", path);
    e->id = *id;
    e->mode  = (mode & 0170000) == 0120000 ? 0120000
             : (mode & 0111) ? 0100755 : 0100644;
    e->size  = (uint32_t)st->st_size;  e->mtime = (uint32_t)st->st_mtime;
    e->ctime = (uint32_t)st->st_ctime; e->dev   = (uint32_t)st->st_dev;
    e->ino   = (uint32_t)st->st_ino;   e->uid   = (uint32_t)st->st_uid;
    e->gid   = (uint32_t)st->st_gid;
}

static void mkparents(const char *full) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s", full);
    for (char *p = strchr(tmp + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0; mkdir(tmp, 0755); *p = '/';
    }
}

static int co_put(const char *path, uint32_t mode, const oid *id, void *v) {
    co_ctx *c = v;
    char full[1300]; snprintf(full, sizeof full, "%s/%s", c->root, path);
    struct stat st;

    /* Already correct? Decide from the old index and a stat. Never read, and
       above all never inflate before knowing whether the write is needed. */
    const bit_entry *old = c->old ? ix_find(c->old, path) : 0;
    if (old && oid_eq(&old->id, id) && entry_matches_stat(old, full)
            && stat(full, &st) == 0) {
        bit_entry e; stamp(&e, path, id, mode, &st);
        index_upsert(c->ix, &e);
        c->n++;
        return 0;
    }

    char type[32]; size_t len;
    void *data = object_read(id, type, sizeof type, &len);
    if (!data) { fprintf(stderr, "bit: missing object for %s\n", path); return 1; }
    mkparents(full);
    if ((mode & 0170000) == 0120000) {        /* restore a symlink, not a file */
        char t[4097];
        size_t tl = len < sizeof t - 1 ? len : sizeof t - 1;
        memcpy(t, data, tl); t[tl] = 0;
        free(data);
        unlink(full);
        if (symlink(t, full) < 0) { fprintf(stderr, "bit: cannot link %s\n", path); return 1; }
        if (lstat(full, &st) == 0) { bit_entry e; stamp(&e, path, id, mode, &st); index_upsert(c->ix, &e); }
        c->n++;
        return 0;
    }
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, (mode & 0111) ? 0755 : 0644);
    if (fd < 0) { free(data); fprintf(stderr, "bit: cannot write %s\n", path); return 1; }
    ssize_t w = write(fd, data, len);
    close(fd); free(data);
    if (w != (ssize_t)len) return 1;
    if (lstat(full, &st) == 0) { bit_entry e; stamp(&e, path, id, mode, &st); index_upsert(c->ix, &e); }
    c->n++;
    return 0;
}

int checkout_tree(const oid *tree, const char *root, bit_index *ix, const bit_index *old, int *n_out) {
    co_ctx c = { root, ix, old, 0 };
    int rc = tree_walk(tree, "", co_put, &c);
    if (n_out) *n_out = c.n;
    return rc;
}

int ref_list(char names[][128], oid *ids, int cap) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return 0;
    char dir[1200]; snprintf(dir, sizeof dir, "%s/refs/heads", git);
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *de; int n = 0;
    while ((de = readdir(d)) && n < cap) {
        if (de->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        size_t len; char *s = slurp(p, &len);
        if (!s) continue;
        if (oid_from_hex(s, &ids[n]) == 0) { snprintf(names[n], 128, "%s", de->d_name); n++; }
        free(s);
    }
    closedir(d);
    return n;
}

int ref_delete(const char *ref) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char p[1800]; snprintf(p, sizeof p, "%s/%s", git, ref);
    return unlink(p);
}

/* ---- merge ---- */

int commit_parents(const oid *c, oid *p1, oid *p2) {
    char type[32]; size_t len;
    char *body = object_read(c, type, sizeof type, &len);
    if (!body || strcmp(type, "commit")) { free(body); return 0; }
    int n = 0;
    for (char *p = body; p && *p && *p != '\n'; ) {
        if (!strncmp(p, "parent ", 7)) {
            char hex[OID_HEX + 1]; memcpy(hex, p + 7, OID_HEX); hex[OID_HEX] = 0;
            oid o;
            if (oid_from_hex(hex, &o) == 0) {
                if (n == 0 && p1) *p1 = o;
                else if (n == 1 && p2) *p2 = o;
                n++;
            }
        }
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : 0;
    }
    free(body);
    return n;
}

/* Collect every ancestor of `a`, then walk `b`'s ancestry until one is hit.
 * Breadth-first on b, so the first hit is the nearest common ancestor. */
int merge_base(const oid *a, const oid *b, oid *out) {
    /* Every ancestor of `a`, then a breadth-first walk of `b` until one is hit,
       so the first hit is the nearest common ancestor. Both the frontier and
       the visited set grow; there is no ancestry depth at which this silently
       stops searching. Membership is a hash probe, not a scan. */
    oidset seen, visited;
    oidset_init(&seen); oidset_init(&visited);
    oid *q = 0; size_t qn = 0, qcap = 0, qh = 0;
    #define PUSH(x) do { if (qn == qcap) { qcap = qcap ? qcap * 2 : 64; \
                          q = xrealloc(q, qcap * sizeof *q); } q[qn++] = (x); } while (0)

    PUSH(*a);
    while (qh < qn) {
        oid c = q[qh++];
        if (!oidset_add(&seen, &c)) continue;
        oid p1, p2;
        int np = commit_parents(&c, &p1, &p2);
        if (np > 0) PUSH(p1);
        if (np > 1) PUSH(p2);
    }
    qn = qh = 0;
    PUSH(*b);
    int found = -1;
    while (qh < qn) {
        oid c = q[qh++];
        if (!oidset_add(&visited, &c)) continue;
        if (oidset_has(&seen, &c)) { *out = c; found = 0; break; }
        oid p1, p2;
        int np = commit_parents(&c, &p1, &p2);
        if (np > 0) PUSH(p1);
        if (np > 1) PUSH(p2);
    }
    #undef PUSH
    free(q); oidset_free(&seen); oidset_free(&visited);
    return found;
}

/* ---- transport ---- */

static void object_path_in(const char *gitdir, const oid *o, char *out, size_t cap) {
    char hex[OID_HEX + 1]; oid_to_hex(o, hex);
    snprintf(out, cap, "%s/objects/%.2s/%s", gitdir, hex, hex + 2);
}

int object_exists_in(const char *gitdir, const oid *o) {
    char p[1400]; object_path_in(gitdir, o, p, sizeof p);
    if (access(p, F_OK) == 0) return 1;
    return pack_exists_in(gitdir, o);           /* loose, then that repo's pack */
}

int object_copy(const char *src_git, const char *dst_git, const oid *o) {
    if (object_exists_in(dst_git, o)) return 0;         /* the negotiation */
    char sp[1400], dp[1400];
    object_path_in(src_git, o, sp, sizeof sp);
    object_path_in(dst_git, o, dp, sizeof dp);
    size_t len; char *data = slurp(sp, &len);
    if (!data) {
        /* Only in the source's pack. Reconstruct it, then write it loose in
           the destination -- the destination gets a normal git object, and the
           source's pack layout is not something the wire needs to know about. */
        char type[32]; size_t ulen;
        void *raw = pack_read_in(src_git, o, type, sizeof type, &ulen);
        if (!raw) return -1;
        char hdr[64];
        int hn = snprintf(hdr, sizeof hdr, "%s %zu", type, ulen) + 1;
        size_t rawlen = (size_t)hn + ulen;
        unsigned char *whole = xmalloc(rawlen);
        memcpy(whole, hdr, (size_t)hn);
        memcpy(whole + hn, raw, ulen);
        free(raw);
        uLongf zc = compressBound((uLong)rawlen);
        unsigned char *z = xmalloc(zc);
        compress2(z, &zc, whole, (uLong)rawlen, Z_DEFAULT_COMPRESSION);
        free(whole);
        data = (char *)z; len = (size_t)zc;
    }
    char dir[1400]; snprintf(dir, sizeof dir, "%s", dp);
    char *slash = strrchr(dir, '/'); if (slash) { *slash = 0; mkdir(dir, 0755); }
    char tmp[1500]; snprintf(tmp, sizeof tmp, "%s.tmp%d", dp, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) { free(data); return -1; }
    ssize_t w = write(fd, data, len);
    close(fd); free(data);
    if (w != (ssize_t)len) { unlink(tmp); return -1; }
    rename(tmp, dp);
    return 1;
}

/* Read an object from an arbitrary store, inflating it. */
static void *object_read_in(const char *gitdir, const oid *o, char *type_out,
                            size_t type_cap, size_t *len_out) {
    char p[1400]; object_path_in(gitdir, o, p, sizeof p);
    size_t zlen; char *z = slurp(p, &zlen);
    if (!z) return pack_read_in(gitdir, o, type_out, type_cap, len_out);
    size_t cap = zlen * 6 + 8192;
    unsigned char *buf = xmalloc(cap);
    uLongf got = (uLongf)cap;
    while (uncompress(buf, &got, (unsigned char *)z, (uLong)zlen) == Z_BUF_ERROR) {
        cap *= 4; buf = xrealloc(buf, cap); got = (uLongf)cap;
    }
    free(z);
    unsigned char *nul = memchr(buf, 0, got);
    if (!nul) { free(buf); return 0; }
    if (type_out) { size_t n = (size_t)(strchr((char *)buf, ' ') - (char *)buf);
                    if (n >= type_cap) n = type_cap - 1;
                    memcpy(type_out, buf, n); type_out[n] = 0; }
    size_t hlen = (size_t)(nul - buf) + 1, clen = got - hlen;
    unsigned char *c = xmalloc(clen + 1);
    memcpy(c, buf + hlen, clen); c[clen] = 0;
    free(buf);
    if (len_out) *len_out = clen;
    return c;
}

/* Records into the caller's array, but membership is tested against a set, so
   enumeration is linear rather than quadratic in object count. Overflowing the
   caller's capacity is reported, never silently truncated. */
static int seen_overflow;
static oidset seen_set;

static int seen_add(oid *out, int *n, int cap, const oid *o) {
    if (!oidset_add(&seen_set, o)) return 0;
    if (*n >= cap) { seen_overflow = 1; return 0; }
    out[(*n)++] = *o;
    return 1;
}

static void walk_tree_in(const char *gitdir, const oid *t, oid *out, int *n, int cap) {
    if (!seen_add(out, n, cap, t)) return;
    char type[32]; size_t len;
    unsigned char *d = object_read_in(gitdir, t, type, sizeof type, &len);
    if (!d || strcmp(type, "tree")) { free(d); return; }
    unsigned char *p = d, *end = d + len;
    while (p < end) {
        char *sp = strchr((char *)p, ' ');
        if (!sp) break;
        uint32_t mode = (uint32_t)strtoul((char *)p, 0, 8);
        char *nul = strchr(sp + 1, 0);
        oid id; memcpy(id.b, nul + 1, OID_RAW);
        if (mode & 040000) walk_tree_in(gitdir, &id, out, n, cap);
        else seen_add(out, n, cap, &id);
        p = (unsigned char *)nul + 1 + OID_RAW;
    }
    free(d);
}

int reachable_in(const char *gitdir, const oid *from, oid *out, int cap) {
    int n = 0;
    seen_overflow = 0;
    oidset_init(&seen_set);
    oid *queue = 0; size_t qn = 0, qcap = 0; int qh = 0;
    #define QPUSH(x) do { if (qn == qcap) { qcap = qcap ? qcap * 2 : 64; \
                           queue = xrealloc(queue, qcap * sizeof *queue); } \
                          queue[qn++] = (x); } while (0)
    QPUSH(*from);
    while ((size_t)qh < qn) {
        oid c = queue[qh++];
        if (!seen_add(out, &n, cap, &c)) continue;
        char type[32]; size_t len;
        char *body = object_read_in(gitdir, &c, type, sizeof type, &len);
        if (!body) continue;
        if (!strcmp(type, "commit")) {
            if (!strncmp(body, "tree ", 5)) {
                char hex[OID_HEX + 1]; memcpy(hex, body + 5, OID_HEX); hex[OID_HEX] = 0;
                oid t; if (!oid_from_hex(hex, &t)) walk_tree_in(gitdir, &t, out, &n, cap);
            }
            for (char *p = body; p && *p && *p != '\n'; ) {
                if (!strncmp(p, "parent ", 7)) {
                    char hex[OID_HEX + 1]; memcpy(hex, p + 7, OID_HEX); hex[OID_HEX] = 0;
                    oid pp;
                    if (!oid_from_hex(hex, &pp)) QPUSH(pp);
                }
                char *nl = strchr(p, '\n'); p = nl ? nl + 1 : 0;
            }
        }
        free(body);
    }
    #undef QPUSH
    free(queue);
    oidset_free(&seen_set);
    if (seen_overflow) die("more than %d objects reachable; raise the limit", cap);
    return n;
}

int reachable(const oid *from, oid *out, int cap) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) return 0;
    return reachable_in(git, from, out, cap);
}

int resolve_gitdir(const char *path, char *out, size_t cap) {
    char p[1300];
    snprintf(p, sizeof p, "%s/.git/HEAD", path);
    if (access(p, F_OK) == 0) { snprintf(out, cap, "%s/.git", path); return 0; }
    snprintf(p, sizeof p, "%s/HEAD", path);
    if (access(p, F_OK) == 0) { snprintf(out, cap, "%s", path); return 0; }
    return -1;
}

int do_fetch(const char *remote, oid *head_out, char *branch_out, size_t bcap,
             int *reach_out, int *sent_out) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) return -1;
    char src[1200];
    if (resolve_gitdir(remote, src, sizeof src) < 0) return -1;

    char branch[256] = "master";
    { char p[1400]; snprintf(p, sizeof p, "%s/HEAD", src);
      size_t l; char *s = slurp(p, &l);
      if (s) { char *nl = strchr(s, '\n'); if (nl) *nl = 0;
               if (!strncmp(s, "ref: refs/heads/", 16))
                   snprintf(branch, sizeof branch, "%s", s + 16);
               free(s); } }
    char p[1400]; snprintf(p, sizeof p, "%s/refs/heads/%s", src, branch);
    size_t l; char *s = slurp(p, &l);
    if (!s) return -1;
    oid head;
    if (oid_from_hex(s, &head) < 0) { free(s); return -1; }
    free(s);

    oid *objs = xmalloc(200000 * sizeof *objs);
    int n = reachable_in(src, &head, objs, 200000), sent = 0;
    for (int i = 0; i < n; i++) if (object_copy(src, git, &objs[i]) == 1) sent++;
    free(objs);

    char ref[256]; snprintf(ref, sizeof ref, "refs/remotes/origin/%s", branch);
    ref_update(ref, &head);
    snprintf(p, sizeof p, "%s/FETCH_HEAD", git);
    { char hex[OID_HEX + 1]; oid_to_hex(&head, hex);
      FILE *f = fopen(p, "w");
      if (f) { fprintf(f, "%s\t\tbranch '%s'\n", hex, branch); fclose(f); } }

    if (head_out) *head_out = head;
    if (branch_out) snprintf(branch_out, bcap, "%s", branch);
    if (reach_out) *reach_out = n;
    if (sent_out) *sent_out = sent;
    return 0;
}

/* ---- delta ---- */

#ifndef DELTA_BLK
#define DELTA_BLK 16                       /* match granularity */
#endif
#ifndef DELTA_STEP
/* Every byte, not every fourth. Indexing the base at a stride finds a match
   only where an aligned block coincides, so a stride of s needs a run of
   BLK + s - 1 bytes to guarantee a hit; at stride 1 the block size itself is
   the minimum, which is what git does.
 *
 * This was 4 for a long time, because the index was being rebuilt for every
 * (base, target) pair and a finer stride was unaffordable. Once the index is
 * built once per base and reused across the window, the finer stride is not
 * merely affordable but *faster*, since better matches leave fewer literal
 * bytes to encode and deflate:
 *
 *      stride   pack body        time
 *           4   1,640,221 B      3.79 s
 *           2   1,477,532 B      2.78 s
 *           1   1,380,006 B      2.74 s      <- and git is 1,484,589 B
 *
 * A parameter tuned around a defect stops being right when the defect is
 * fixed. Stride 4 was costing 19% of the pack to save time it no longer saved. */
#ifndef DELTA_STEP
#define DELTA_STEP 1                       /* base index stride */
#endif
#endif

static void dvarint(unsigned char **p, size_t v) {
    do { unsigned char b = v & 0x7f; v >>= 7; if (v) b |= 0x80; *(*p)++ = b; } while (v);
}
/* Bounded. These bytes come off disk or off a socket, so a varint whose
   continuation bit is set all the way to the end of the buffer must stop at
   the end rather than read past it. Returns 0 and leaves *p at `end` on a
   truncated or over-long encoding; callers treat that as a refusal. */
static size_t dvarint_get(const unsigned char **p, const unsigned char *end, int *bad) {
    size_t v = 0; int s = 0;
    for (;;) {
        if (*p >= end || s > 63) { *p = end; *bad = 1; return 0; }
        unsigned char b = *(*p)++;
        v |= (size_t)(b & 0x7f) << s;
        if (!(b & 0x80)) break;
        s += 7;
    }
    return v;
}

/* A rolling hash, so that advancing one byte costs a multiply and an add
   rather than a fresh pass over the window. The scan visits every byte of the
   target against every candidate base, so this factor is the whole cost of
   the search: rehashing 16 bytes per position made packing 12x slower than
   git for a 1% smaller result. */
#define RK_MUL 16777619u
#ifndef DELTA_CAND
#define DELTA_CAND 64                      /* chain entries examined per position */
#endif

static unsigned rk_pow(void) {
    unsigned p = 1;
    for (int i = 1; i < DELTA_BLK; i++) p *= RK_MUL;
    return p;
}
static unsigned blk_hash(const unsigned char *p) {
    unsigned h = 0;
    for (int i = 0; i < DELTA_BLK; i++) h = h * RK_MUL + p[i];
    return h;
}

/* The block index over a base, built once and reused.
 *
 * It used to be built inside delta_create, which meant that offering an object
 * to a window of W candidate bases rebuilt all W indexes for every target:
 * O(N * W * |base|) where O(N * |base|) is enough. With W = 32 that is thirty
 * two times the necessary work, and it was the whole of the gap against
 * `git gc`, which builds a base's index once and slides it across every target
 * that sees it. */
typedef struct delta_index_s { size_t *head, *next, nslots, nblk, blen; } delta_index;

void delta_index_free(delta_index *ix) {
    if (!ix) return;
    free(ix->head); free(ix->next); free(ix);
}

delta_index *delta_index_build(const unsigned char *base, size_t blen) {
    if (blen < DELTA_BLK || blen > 0xFFFFFFFFu) return 0;
    delta_index *ix = xmalloc(sizeof *ix);
    ix->blen = blen;
    ix->nblk = (blen - DELTA_BLK) / DELTA_STEP + 1;
    ix->nslots = 1; while (ix->nslots < ix->nblk * 2 + 16) ix->nslots <<= 1;
    ix->head = xmalloc(ix->nslots * sizeof *ix->head);
    ix->next = xmalloc(ix->nblk * sizeof *ix->next);
    for (size_t i = 0; i < ix->nslots; i++) ix->head[i] = (size_t)-1;
    /* Chained, not open-addressed. Real objects are full of repeated blocks --
       a Mach-O binary is largely runs of zeros -- and with linear probing those
       all land in one cluster that both insert and lookup walk end to end. */
    for (size_t k = 0; k < ix->nblk; k++) {
        size_t j = blk_hash(base + k * DELTA_STEP) & (ix->nslots - 1);
        ix->next[k] = ix->head[j]; ix->head[j] = k;
    }
    return ix;
}

/* `max` is a ceiling: the best delta found so far, or the cost of storing the
   object whole. Most candidates in a window produce a delta nobody will use,
   and encoding one to the last byte before discarding it is the bulk of what
   packing spends its time on. Abandoning a candidate the moment it exceeds the
   ceiling costs a comparison per instruction and skips the rest. */
unsigned char *delta_create_max(const delta_index *ix, const unsigned char *base,
                                size_t blen, const unsigned char *tgt,
                                size_t tlen, size_t max, size_t *dlen) {
    if (!ix || ix->blen != blen || tlen < DELTA_BLK) return 0;
    if (max > tlen) max = tlen;

    unsigned char *out = xmalloc(tlen + tlen / 8 + 64), *o = out;
    dvarint(&o, blen);
    dvarint(&o, tlen);

    const unsigned POW = rk_pow();
    const size_t nslots = ix->nslots;
    const unsigned char *ceiling = out + max;
    size_t pos = 0, lit = 0;
    unsigned h = 0; int have = 0;              /* h is valid for [pos, pos+BLK) */

    #define FLUSH_LIT() do { \
        while (lit) { size_t n = lit > 127 ? 127 : lit; \
                      *o++ = (unsigned char)n; \
                      memcpy(o, tgt + pos - lit, n); o += n; lit -= n; } \
    } while (0)

    while (pos < tlen) {
        if (o + lit >= ceiling) { free(out); return 0; }   /* already too big */
        size_t moff = 0, mlen = 0;
        if (pos + DELTA_BLK <= tlen) {
            if (!have) { h = blk_hash(tgt + pos); have = 1; }
            size_t j = h & (nslots - 1);
            int tries = 0;
            for (size_t k = ix->head[j]; k != (size_t)-1 && tries < DELTA_CAND;
                 k = ix->next[k], tries++) {
                size_t b = k * DELTA_STEP;
                if (memcmp(base + b, tgt + pos, DELTA_BLK)) continue;
                size_t n = DELTA_BLK;                           /* extend forward */
                while (b + n < blen && pos + n < tlen && base[b + n] == tgt[pos + n]) n++;
                if (n > mlen) { mlen = n; moff = b; }
                if (mlen > 4096) break;                         /* good enough */
            }
        }
        if (mlen >= DELTA_BLK) {
            FLUSH_LIT();
            while (mlen) {
                size_t n = mlen > 0xFFFF ? 0xFFFF : mlen;
                unsigned char *cmd = o++;
                unsigned char c = 0x80;
                if (moff & 0x0000ff) { *o++ = moff        & 0xff; c |= 0x01; }
                if (moff & 0x00ff00) { *o++ = (moff >> 8) & 0xff; c |= 0x02; }
                if (moff & 0xff0000) { *o++ = (moff >> 16)& 0xff; c |= 0x04; }
                if (moff & 0xff000000UL){*o++= (moff >> 24)& 0xff; c |= 0x08; }
                if (n & 0x00ff) { *o++ = n        & 0xff; c |= 0x10; }
                if (n & 0xff00) { *o++ = (n >> 8) & 0xff; c |= 0x20; }
                *cmd = c;
                moff += n; mlen -= n; pos += n;
            }
            have = 0;                          /* window jumped; rehash */
        } else {
            if (have && pos + DELTA_BLK < tlen)
                h = (h - tgt[pos] * POW) * RK_MUL + tgt[pos + DELTA_BLK];
            else
                have = 0;
            pos++; lit++;
        }
    }
    FLUSH_LIT();
    #undef FLUSH_LIT
    *dlen = (size_t)(o - out);
    if (*dlen >= max) { free(out); return 0; }       /* no saving; store whole */
    return out;
}

unsigned char *delta_create_indexed(const delta_index *ix, const unsigned char *base,
                                    size_t blen, const unsigned char *tgt,
                                    size_t tlen, size_t *dlen) {
    return delta_create_max(ix, base, blen, tgt, tlen, tlen, dlen);
}

/* The one-shot form, for callers with a single pair to compare. */
unsigned char *delta_create(const unsigned char *base, size_t blen,
                            const unsigned char *tgt, size_t tlen, size_t *dlen) {
    delta_index *ix = delta_index_build(base, blen);
    if (!ix) return 0;
    unsigned char *d = delta_create_indexed(ix, base, blen, tgt, tlen, dlen);
    delta_index_free(ix);
    return d;
}

unsigned char *delta_apply(const unsigned char *base, size_t blen,
                           const unsigned char *d, size_t dlen, size_t *olen) {
    const unsigned char *p = d, *end = d + dlen;
    int bad = 0;
    size_t bsize = dvarint_get(&p, end, &bad), tsize = dvarint_get(&p, end, &bad);
    if (bad || bsize != blen) return 0;
    if (tsize > (size_t)1 << 34) return 0;              /* refuse an absurd claim */
    unsigned char *out = xmalloc(tsize + 1), *o = out;
    /* Every length is checked against both ends before it is used: the source
       against the base, the destination against the size the header declared.
       A delta arrives from a file that may be corrupt or hostile, and the
       instruction stream is otherwise free to name any offset it likes. */
    while (p < end && (size_t)(o - out) < tsize) {
        unsigned char c = *p++;
        if (c & 0x80) {
            size_t off = 0, n = 0;
            if ((c & 0x01) && p < end) off  =  *p++;
            if ((c & 0x02) && p < end) off |= (size_t)*p++ << 8;
            if ((c & 0x04) && p < end) off |= (size_t)*p++ << 16;
            if ((c & 0x08) && p < end) off |= (size_t)*p++ << 24;
            if ((c & 0x10) && p < end) n    =  *p++;
            if ((c & 0x20) && p < end) n   |= (size_t)*p++ << 8;
            if (!n) n = 0x10000;
            if (off > blen || n > blen - off) { free(out); return 0; }
            if (n > tsize - (size_t)(o - out)) { free(out); return 0; }
            memcpy(o, base + off, n); o += n;
        } else if (c) {
            if ((size_t)(end - p) < c) { free(out); return 0; }
            if ((size_t)c > tsize - (size_t)(o - out)) { free(out); return 0; }
            memcpy(o, p, c); o += c; p += c;
        } else { free(out); return 0; }               /* 0x00 is reserved */
    }
    if ((size_t)(o - out) != tsize) { free(out); return 0; }   /* short stream */
    *o = 0;
    if (olen) *olen = (size_t)(o - out);
    return out;
}
