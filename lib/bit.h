/* bit — git's data model, rebuilt as a graft namespace.
 *
 * Objects are written in git's exact on-disk form, so a repository bit creates
 * is a repository git can read. That is the parity claim, and it is checkable.
 *
 *   object  = zlib(  "<type> <len>\0" <content>  )   at .git/objects/ab/cdef...
 *   name    = SHA-1( "<type> <len>\0" <content>  )
 */
#ifndef BIT_H
#define BIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define OID_RAW 20
#define OID_HEX 40

typedef struct { unsigned char b[OID_RAW]; } oid;

/* --- hex --- */
void oid_to_hex(const oid *o, char out[OID_HEX + 1]);
int  oid_from_hex(const char *hex, oid *out);
int  oid_eq(const oid *a, const oid *b);

/* --- repository --- */
/* Walks upward for a .git directory. Returns 0 on success. */
int  repo_find(char *out, size_t cap);
int  repo_git_dir(char *out, size_t cap);

/* --- objects --- */
/* Hash content with git's header. If write is non-zero, store it. */
int  object_write(const char *type, const void *data, size_t len, int write, oid *out);
/* Returns malloc'd content; caller frees. type_out may be NULL. */
void *object_read(const oid *o, char *type_out, size_t type_cap, size_t *len_out);
int  object_exists(const oid *o);

/* --- refs --- */
int  ref_head_oid(oid *out);              /* resolves HEAD; 1 if unborn */
int  ref_head_name(char *out, size_t cap);/* e.g. refs/heads/master */
int  ref_update(const char *ref, const oid *o);

/* --- index (git index v2, so `git status` reads what bit stages) --- */
typedef struct {
    char  path[512];
    oid   id;
    uint32_t mode, size, mtime, ctime, dev, ino, uid, gid;
} bit_entry;

typedef struct { bit_entry *e; size_t n, cap; } bit_index;

void index_init(bit_index *ix);
int  index_read(bit_index *ix);
int  index_write(const bit_index *ix);
void index_upsert(bit_index *ix, const bit_entry *e);
void index_free(bit_index *ix);

/* --- trees --- */
/* Build tree objects from the index, returning the root tree. */
int  tree_from_index(const bit_index *ix, oid *out);

/* --- misc --- */
void die(const char *fmt, ...);
char *slurp(const char *path, size_t *len);

#endif

/* --- pack ---------------------------------------------------------------
 * Loose objects cost one file each, and a file costs a whole 4 KB block: a
 * 306-object repository occupied 1,277,952 B of disk for 316,232 B of data,
 * 75% padding. Packing collapses them into one file and one index.
 *
 * The index is where git leaves room. Its .idx spends 31.5 B/object:
 * a 1 KB fanout table, a full 20-byte digest, a CRC32, and a 32-bit offset.
 * We spend 12: an 8-byte digest prefix and a 32-bit offset, sorted for binary
 * search. The prefix is a filter -- a hit is confirmed by hashing the object
 * we land on -- so truncation costs lookups, never correctness. The CRC is
 * dropped because the digest already proves the content, and the fanout is
 * dropped because 303 sorted entries are eight probes.
 */
int  pack_write(int *n_out, size_t *pack_out, size_t *idx_out);
void *pack_read(const oid *o, char *type_out, size_t type_cap, size_t *len_out);

/* --- trees ---
 * Walk a tree recursively. cb receives the path relative to the tree root, the
 * entry mode, and its object id; returning non-zero stops the walk. */
typedef int (*tree_cb)(const char *path, uint32_t mode, const oid *id, void *ctx);
int tree_walk(const oid *tree, const char *prefix, tree_cb cb, void *ctx);

/* Is the file at `path` unchanged from index entry `e`? Compares stat only,
 * which is the whole reason the index carries mtime/size/ino: an unchanged
 * file is decided without reading, let alone hashing, its contents. */
int entry_matches_stat(const bit_entry *e, const char *full);
int pack_unpack(int *n_out);   /* pack -> loose objects, restoring git readability */
/* Existence, without inflating unless it has to.
 * Returns 0 = definitively absent, 1 = present (confirmed by rehash).
 * A truncated-prefix index has NO false negatives: if the object is in the
 * pack its prefix is in the index. So "absent" is exact and free; only
 * "present" costs a decompression. */
int pack_exists(const oid *o);

/* --- diff ---
 * Unified diff between two buffers. Myers' O(ND) greedy algorithm, but only
 * after trimming the common prefix and suffix: for a typical edit those two
 * trims remove almost everything, and D is what the algorithm is exponential
 * in. Writes to `out`. Returns 1 if they differ, 0 if identical. */
int diff_unified(FILE *out, const char *label,
                 const char *a, size_t alen, const char *b, size_t blen);

/* --- refs and worktree ---------------------------------------------------- */
/* Materialise `tree` into `root`, rebuilding `ix`. Files already correct are
 * skipped by stat against `old`, without being read or inflated. */
int  checkout_tree(const oid *tree, const char *root, bit_index *ix, const bit_index *old, int *n_out);
int  ref_list(char names[][128], oid *ids, int cap);   /* branches under refs/heads, returns count */
int  ref_delete(const char *ref);
int  commit_tree_oid(const oid *commit, oid *tree_out); /* commit or tree -> tree */

/* --- merge ---
 * First common ancestor of two commits. Ancestry here is a chain, not a DAG,
 * until merge commits exist -- after which a commit may have two parents and
 * this walks both. */
int merge_base(const oid *a, const oid *b, oid *out);
int commit_parents(const oid *c, oid *p1, oid *p2);   /* returns how many, 0-2 */

/* --- transport ---
 * Objects are content-addressed files, so transfer is copy-if-absent. The
 * negotiation git does over a socket is here a test against the destination
 * store: an object the far side already has is never sent. */
int object_exists_in(const char *gitdir, const oid *o);
int object_copy(const char *src_git, const char *dst_git, const oid *o);
/* Every object reachable from `from`: the commit chain, and each commit's
 * tree, subtrees and blobs. */
int reachable(const oid *from, oid *out, int cap);
int reachable_in(const char *gitdir, const oid *from, oid *out, int cap);
/* Resolve a path to a git directory: <path>/.git, or <path> if it is one. */
int resolve_gitdir(const char *path, char *out, size_t cap);
/* Fetch from a remote path: copy what we lack, record refs/remotes and
 * FETCH_HEAD, and report the remote branch tip. */
int do_fetch(const char *remote, oid *head_out, char *branch_out, size_t bcap,
             int *reach_out, int *sent_out);
