#include "bit.h"
#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return 0; }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n < 0) { free(buf); return 0; }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
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
int repo_git_dir(char *out, size_t cap) {
    char root[1024];
    if (repo_find(root, sizeof root) < 0) return -1;
    snprintf(out, cap, "%s/.git", root);
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
    unsigned char *buf = malloc(raw);
    memcpy(buf, hdr, hlen);
    memcpy(buf + hlen, data, len);

    uLongf zcap = compressBound((uLong)raw);
    unsigned char *z = malloc(zcap);
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
    unsigned char *buf = malloc(cap);
    uLongf got = (uLongf)cap;
    while (uncompress(buf, &got, (unsigned char *)z, (uLong)zlen) == Z_BUF_ERROR) {
        cap *= 4; buf = realloc(buf, cap); got = (uLongf)cap;
    }
    free(z);

    unsigned char *nul = memchr(buf, 0, got);
    if (!nul) { free(buf); return 0; }
    if (type_out) { size_t n = (size_t)(strchr((char *)buf, ' ') - (char *)buf);
                    if (n >= type_cap) n = type_cap - 1;
                    memcpy(type_out, buf, n); type_out[n] = 0; }
    size_t hlen = (size_t)(nul - buf) + 1;
    size_t clen = got - hlen;
    unsigned char *content = malloc(clen + 1);
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
        ix->e = realloc(ix->e, ix->cap * sizeof *ix->e);
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
    unsigned char *buf = calloc(1, cap), *p = buf;
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
    kid_t *kids = malloc(kcap * sizeof *kids);
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
            kid_t *grown = realloc(kids, kcap * sizeof *kids);
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
                typeof(kids[0]) t = kids[a]; kids[a] = kids[b]; kids[b] = t;
            }

    size_t cap = nk * 560 + 64, len = 0;
    unsigned char *buf = malloc(cap);
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

/* ---- pack ---- */

#define PACK_MAGIC "BITPACK1"
#define IDX_MAGIC  "BITIDX01"
#define IDX_PREFIX 8                       /* bytes of digest kept in the index */
#define IDX_SLOT   (IDX_PREFIX + 4)

static void pack_paths(char *pack, size_t pc, char *idx, size_t ic) {
    char git[1024];
    if (repo_git_dir(git, sizeof git) < 0) die("not a bit repository");
    // Not .git/objects/pack: this is not git's pack format, and git tries to
    // parse anything it finds there, reporting "non-monotonic index".
    char dir[1200]; snprintf(dir, sizeof dir, "%s/bitpack", git);
    mkdir(dir, 0755);
    snprintf(pack, pc, "%s/bit.pack", dir);
    snprintf(idx,  ic, "%s/bit.idx",  dir);
}

static void put_varint(FILE *f, uint64_t v) {
    do { unsigned char b = v & 0x7f; v >>= 7; if (v) b |= 0x80; fputc(b, f); } while (v);
}
static uint64_t get_varint(const unsigned char **p) {
    uint64_t v = 0; int shift = 0;
    for (;;) { unsigned char b = *(*p)++; v |= (uint64_t)(b & 0x7f) << shift;
               if (!(b & 0x80)) break; shift += 7; }
    return v;
}
static int type_code(const char *t) {
    return !strcmp(t,"commit") ? 1 : !strcmp(t,"tree") ? 2 : 3;
}
static const char *type_name(int c) {
    return c == 1 ? "commit" : c == 2 ? "tree" : "blob";
}

typedef struct { oid id; uint32_t off; } idx_ent;
static int idx_cmp(const void *a, const void *b) {
    return memcmp(((const idx_ent *)a)->id.b, ((const idx_ent *)b)->id.b, IDX_PREFIX);
}

int pack_write(int *n_out, size_t *pack_out, size_t *idx_out) {
    char git[1024]; if (repo_git_dir(git, sizeof git) < 0) return -1;
    char objdir[1200]; snprintf(objdir, sizeof objdir, "%s/objects", git);

    idx_ent *ents = 0; size_t n = 0, cap = 0;
    char packp[1300], idxp[1300];
    pack_paths(packp, sizeof packp, idxp, sizeof idxp);
    FILE *pf = fopen(packp, "wb");
    if (!pf) return -1;
    fwrite(PACK_MAGIC, 1, 8, pf);
    uint32_t zero = 0; fwrite(&zero, 4, 1, pf);        /* count, patched at the end */

    DIR *d = opendir(objdir);
    if (!d) { fclose(pf); return -1; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strlen(de->d_name) != 2) continue;         /* only the 2-hex shards */
        char sub[1400]; snprintf(sub, sizeof sub, "%s/%s", objdir, de->d_name);
        DIR *s = opendir(sub); if (!s) continue;
        struct dirent *se;
        while ((se = readdir(s))) {
            if (strlen(se->d_name) != OID_HEX - 2) continue;
            char hex[OID_HEX + 1];
            snprintf(hex, sizeof hex, "%s%s", de->d_name, se->d_name);
            oid o; if (oid_from_hex(hex, &o) < 0) continue;
            char type[32]; size_t len;
            void *data = object_read(&o, type, sizeof type, &len);
            if (!data) continue;

            if (n == cap) { cap = cap ? cap * 2 : 256; ents = realloc(ents, cap * sizeof *ents); }
            ents[n].id = o;
            ents[n].off = (uint32_t)ftell(pf);
            n++;

            uLongf zcap = compressBound((uLong)len);
            unsigned char *z = malloc(zcap);
            compress2(z, &zcap, data, (uLong)len, Z_DEFAULT_COMPRESSION);
            fputc(type_code(type), pf);
            put_varint(pf, len);
            put_varint(pf, zcap);
            fwrite(z, 1, zcap, pf);
            free(z); free(data);
        }
        closedir(s);
    }
    closedir(d);
    fseek(pf, 8, SEEK_SET); uint32_t cnt = (uint32_t)n; fwrite(&cnt, 4, 1, pf);
    fseek(pf, 0, SEEK_END); size_t psize = (size_t)ftell(pf);
    fclose(pf);

    qsort(ents, n, sizeof *ents, idx_cmp);
    FILE *xf = fopen(idxp, "wb");
    if (!xf) { free(ents); return -1; }
    fwrite(IDX_MAGIC, 1, 8, xf);
    fwrite(&cnt, 4, 1, xf);
    for (size_t i = 0; i < n; i++) {
        fwrite(ents[i].id.b, 1, IDX_PREFIX, xf);       /* prefix only */
        fwrite(&ents[i].off, 4, 1, xf);
    }
    size_t isize = (size_t)ftell(xf);
    fclose(xf); free(ents);

    if (n_out) *n_out = (int)n;
    if (pack_out) *pack_out = psize;
    if (idx_out) *idx_out = isize;
    return 0;
}

void *pack_read(const oid *o, char *type_out, size_t type_cap, size_t *len_out) {
    char packp[1300], idxp[1300];
    pack_paths(packp, sizeof packp, idxp, sizeof idxp);
    size_t ilen; unsigned char *ix = (unsigned char *)slurp(idxp, &ilen);
    if (!ix) return 0;
    if (ilen < 12 || memcmp(ix, IDX_MAGIC, 8)) { free(ix); return 0; }
    uint32_t n; memcpy(&n, ix + 8, 4);

    /* binary search on the 8-byte prefix; no fanout table */
    long lo = 0, hi = (long)n - 1; long hit = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = memcmp(ix + 12 + mid * IDX_SLOT, o->b, IDX_PREFIX);
        if (c == 0) { hit = mid; break; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    if (hit < 0) { free(ix); return 0; }
    /* Entries sharing a prefix are adjacent, because the index is sorted on it.
       Rewind to the first, then try each until one rehashes to the full oid. */
    while (hit > 0 && memcmp(ix + 12 + (hit - 1) * IDX_SLOT, o->b, IDX_PREFIX) == 0) hit--;
    uint32_t cand[8]; int ncand = 0;
    for (long j = hit; j < (long)n && ncand < 8; j++) {
        if (memcmp(ix + 12 + j * IDX_SLOT, o->b, IDX_PREFIX) != 0) break;
        memcpy(&cand[ncand++], ix + 12 + j * IDX_SLOT + IDX_PREFIX, 4);
    }
    free(ix);

    size_t plen; unsigned char *pk = (unsigned char *)slurp(packp, &plen);
    if (!pk) return 0;
    for (int k = 0; k < ncand; k++) {
        uint32_t off = cand[k];
        if (off + 1 >= plen) continue;
        const unsigned char *p = pk + off;
        int tc = *p++;
        uint64_t ulen = get_varint(&p), zlen = get_varint(&p);
        unsigned char *out = malloc(ulen + 1);
        uLongf got = (uLongf)ulen;
        if (uncompress(out, &got, p, (uLong)zlen) != Z_OK) { free(out); continue; }
        out[ulen] = 0;
        /* the prefix was a filter; the rehash decides */
        oid check; object_write(type_name(tc), out, (size_t)ulen, 0, &check);
        if (!oid_eq(&check, o)) { free(out); continue; }
        free(pk);
        if (type_out) snprintf(type_out, type_cap, "%s", type_name(tc));
        if (len_out) *len_out = (size_t)ulen;
        return out;
    }
    free(pk);
    return 0;
}

/* ---- tree walking ---- */

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

/* Restore every packed object to loose form. Packing is a mode, not a
 * one-way door: while packed the store is dense and git cannot read it;
 * unpacked it is larger and git can. Round-tripping is lossless because an
 * object's identity is its content, so what comes out hashes to what went in. */
int pack_unpack(int *n_out) {
    char packp[1300], idxp[1300];
    pack_paths(packp, sizeof packp, idxp, sizeof idxp);
    size_t plen; unsigned char *pk = (unsigned char *)slurp(packp, &plen);
    if (!pk) return -1;
    if (plen < 12 || memcmp(pk, PACK_MAGIC, 8)) { free(pk); return -1; }
    uint32_t n; memcpy(&n, pk + 8, 4);

    const unsigned char *p = pk + 12;
    int done = 0;
    for (uint32_t i = 0; i < n && (size_t)(p - pk) < plen; i++) {
        int tc = *p++;
        uint64_t ulen = get_varint(&p), zlen = get_varint(&p);
        unsigned char *out = malloc(ulen + 1);
        uLongf got = (uLongf)ulen;
        if (uncompress(out, &got, p, (uLong)zlen) != Z_OK) { free(out); break; }
        p += zlen;
        oid o;
        object_write(type_name(tc), out, (size_t)ulen, 1, &o);   /* writes loose */
        free(out);
        done++;
    }
    free(pk);
    if (n_out) *n_out = done;
    return 0;
}

int pack_exists(const oid *o) {
    char packp[1300], idxp[1300];
    pack_paths(packp, sizeof packp, idxp, sizeof idxp);
    size_t ilen; unsigned char *ix = (unsigned char *)slurp(idxp, &ilen);
    if (!ix) return 0;
    if (ilen < 12 || memcmp(ix, IDX_MAGIC, 8)) { free(ix); return 0; }
    uint32_t n; memcpy(&n, ix + 8, 4);
    long lo = 0, hi = (long)n - 1, hit = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = memcmp(ix + 12 + mid * IDX_SLOT, o->b, IDX_PREFIX);
        if (c == 0) { hit = mid; break; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    free(ix);
    if (hit < 0) return 0;                    /* exact: no false negatives */
    void *d = pack_read(o, 0, 0, 0);          /* only a hit pays the inflate */
    if (!d) return 0;
    free(d);
    return 1;
}

/* ---- diff ---- */

typedef struct { const char *p; size_t n; } line;

static line *split_lines(const char *s, size_t len, size_t *n_out) {
    size_t cap = 64, n = 0;
    line *v = malloc(cap * sizeof *v);
    const char *p = s, *end = s + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
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
    int *v = malloc((size_t)(2 * max + 1) * sizeof *v);
    int **trace = malloc((size_t)(max + 1) * sizeof *trace);
    int off = max, depth = -1;
    memset(v, 0, (size_t)(2 * max + 1) * sizeof *v);

    for (int d = 0; d <= max; d++) {
        trace[d] = malloc((size_t)(2 * max + 1) * sizeof **trace);
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
    int *ops = malloc((size_t)(max + 1) * 2 * sizeof *ops), nops = 0;
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
    e->mode  = (mode & 0111) ? 0100755 : 0100644;
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
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, (mode & 0111) ? 0755 : 0644);
    if (fd < 0) { free(data); fprintf(stderr, "bit: cannot write %s\n", path); return 1; }
    ssize_t w = write(fd, data, len);
    close(fd); free(data);
    if (w != (ssize_t)len) return 1;
    if (stat(full, &st) == 0) { bit_entry e; stamp(&e, path, id, mode, &st); index_upsert(c->ix, &e); }
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
    oid seen[4096]; size_t nseen = 0;
    oid queue[4096]; size_t qh = 0, qt = 0;
    queue[qt++] = *a;
    while (qh < qt && nseen < 4096) {
        oid c = queue[qh++];
        int dup = 0;
        for (size_t i = 0; i < nseen; i++) if (oid_eq(&seen[i], &c)) { dup = 1; break; }
        if (dup) continue;
        seen[nseen++] = c;
        oid p1, p2;
        int np = commit_parents(&c, &p1, &p2);
        if (np > 0 && qt < 4096) queue[qt++] = p1;
        if (np > 1 && qt < 4096) queue[qt++] = p2;
    }
    qh = qt = 0;
    queue[qt++] = *b;
    oid visited[4096]; size_t nv = 0;
    while (qh < qt) {
        oid c = queue[qh++];
        int dup = 0;
        for (size_t i = 0; i < nv; i++) if (oid_eq(&visited[i], &c)) { dup = 1; break; }
        if (dup) continue;
        if (nv < 4096) visited[nv++] = c;
        for (size_t i = 0; i < nseen; i++)
            if (oid_eq(&seen[i], &c)) { *out = c; return 0; }
        oid p1, p2;
        int np = commit_parents(&c, &p1, &p2);
        if (np > 0 && qt < 4096) queue[qt++] = p1;
        if (np > 1 && qt < 4096) queue[qt++] = p2;
    }
    return -1;
}

/* ---- transport ---- */

static void object_path_in(const char *gitdir, const oid *o, char *out, size_t cap) {
    char hex[OID_HEX + 1]; oid_to_hex(o, hex);
    snprintf(out, cap, "%s/objects/%.2s/%s", gitdir, hex, hex + 2);
}

int object_exists_in(const char *gitdir, const oid *o) {
    char p[1400]; object_path_in(gitdir, o, p, sizeof p);
    return access(p, F_OK) == 0;
}

int object_copy(const char *src_git, const char *dst_git, const oid *o) {
    if (object_exists_in(dst_git, o)) return 0;         /* the negotiation */
    char sp[1400], dp[1400];
    object_path_in(src_git, o, sp, sizeof sp);
    object_path_in(dst_git, o, dp, sizeof dp);
    size_t len; char *data = slurp(sp, &len);
    if (!data) return -1;
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
    if (!z) return 0;
    size_t cap = zlen * 6 + 8192;
    unsigned char *buf = malloc(cap);
    uLongf got = (uLongf)cap;
    while (uncompress(buf, &got, (unsigned char *)z, (uLong)zlen) == Z_BUF_ERROR) {
        cap *= 4; buf = realloc(buf, cap); got = (uLongf)cap;
    }
    free(z);
    unsigned char *nul = memchr(buf, 0, got);
    if (!nul) { free(buf); return 0; }
    if (type_out) { size_t n = (size_t)(strchr((char *)buf, ' ') - (char *)buf);
                    if (n >= type_cap) n = type_cap - 1;
                    memcpy(type_out, buf, n); type_out[n] = 0; }
    size_t hlen = (size_t)(nul - buf) + 1, clen = got - hlen;
    unsigned char *c = malloc(clen + 1);
    memcpy(c, buf + hlen, clen); c[clen] = 0;
    free(buf);
    if (len_out) *len_out = clen;
    return c;
}

static int seen_add(oid *out, int *n, int cap, const oid *o) {
    for (int i = 0; i < *n; i++) if (oid_eq(&out[i], o)) return 0;
    if (*n >= cap) return 0;
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
    oid queue[4096]; int qh = 0, qt = 0;
    queue[qt++] = *from;
    while (qh < qt) {
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
                    if (!oid_from_hex(hex, &pp) && qt < 4096) queue[qt++] = pp;
                }
                char *nl = strchr(p, '\n'); p = nl ? nl + 1 : 0;
            }
        }
        free(body);
    }
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

    oid *objs = malloc(200000 * sizeof *objs);
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
