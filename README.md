# bit

git's data model, reimplemented in 3,484 lines of C.

bit writes **byte-identical objects to git**. A repository bit creates is one
git reads: `git fsck` returns 0, `git log` reads bit's commits, and `git status`
reads a staging area `bit add` wrote. That is the specification, not a goal, and
`test/parity.sh` asserts it in fourteen places.

26 commands, covering 20 of git's 23 common ones. Faster than git on all 26
operations measured.

```sh
./build.sh
export PATH="$PWD/build/bin:$PATH"

init .
add .
commit -m "first"
status ; diff ; log --oneline
```

---

## Compatibility

| assertion | how it is checked |
|---|---|
| blob digests match | four `hash-object` comparisons: small, large, nested, executable |
| tree digests match | `write-tree` over nested directories, mixed 100644/100755 |
| git reads bit's index | `git status --short` after `bit add` |
| git reads bit's commits | `git log` after `bit commit` |
| the repository is valid | `git fsck` exits 0 |
| bit reads its own objects | `cat-file -t` on a bit-written commit |
| the histories agree | `bit log --oneline` equals `git log --oneline` |

Formats are git's exactly:

- **objects** — `zlib("<type> <len>\0" + content)` at `.git/objects/ab/cdef…`, named by SHA-1
- **trees** — `"<mode> <name>\0"` followed by 20 raw digest bytes
- **commits** — text, with 40-character hex digests
- **index** — git's binary v2 format, which is why `git status` reads what `bit add` staged

`bit pack` writes the denser representation; it does **not** remove the loose
objects it packed, so running it alone increases disk use. Reclaiming the space
means deleting the loose store, after which reads resolve through the pack and a
later `bit pack` carries the already-packed objects forward.

Two formats are bit's own and git cannot read them: `.git/bitpack/` and the
`refs/remotes/origin/*` that `bit fetch` writes. Neither is required, and
`bit unpack` restores full git readability losslessly.

## Commands

| | |
|---|---|
| repository | `init` `clone` |
| working tree | `add` `status` `diff` `restore` `checkout` `rm` `mv` `reset` |
| history | `commit` `log` `show` `tag` |
| branching | `branch` `switch` `merge` `rebase` |
| transport | `fetch` `push` `pull` |
| plumbing | `hash-object` `cat-file` `write-tree` `pack` `unpack` |

Not implemented: `grep`, `bisect`, `backfill`. Each duplicates something that
already exists — `grep` is a system tool, `bisect` is a driver loop over
`checkout`, `backfill` has meaning only for partial clones.

Each command builds twice: a standalone executable in `build/bin/`, and a shared
object in `build/` exporting `int cmd_main(int, char **)`. The same source runs
as a process or mapped into a host.

## Benchmarks

macOS 15.7.5, Apple silicon, 450 files. Trees asserted identical before any
timing is reported. Method and the full table in [BENCHMARK.md](BENCHMARK.md).

| operation | bit | git | |
|---|---|---|---|
| `init` | 2.24 ms | 12.64 ms | 5.64× |
| `hash-object -w` | 1.70 ms | 10.26 ms | 6.02× |
| `add`, 450 files | 3.10 ms | 11.81 ms | 3.81× |
| `write-tree` | 6.08 ms | 10.49 ms | 1.73× |
| `status` | 3.64 ms | 11.85 ms | 3.26× |
| `commit` | 8.20 ms | 16.90 ms | 2.06× |
| `checkout` | 3.40 ms | 13.42 ms | 3.95× |
| `diff` | 3.20 ms | 11.64 ms | 3.64× |
| `cat-file -e`, miss | 1.79 ms | 18.76 ms | 10.46× |
| `clone`, 100 files | 9.03 ms | 10.66 ms | 1.18× |
| `fetch`, nothing new | 1.86 ms | 26.16 ms | 14.10× |
| addressing | **1.08 B/object** | 28.05 B/object | 26× |

**How to read these.** The 5–6× on small operations is largely git's start-up:
git loads a 4 MB binary that parses configuration and discovers a repository
before doing anything. The honest rows are the ones where bit's own algorithms
carry the result — `write-tree` 1.73×, `reset` 1.87×, `commit` 2.06×, `clone`
1.18× — and those margins are much narrower. git also does more in several arms:
`.gitignore`, worktrees, submodules, hooks and pathspec magic, none of which bit
implements.

## Optimisations

Four of the five were defects in bit rather than clever ideas.

| change | effect |
|---|---|
| `readdir` instead of `popen("find")` per directory | `add` 20.1 → 12.2 ms |
| consult the index stat cache; skip unchanged files | 12.2 → 7.6 ms |
| binary search the sorted index instead of scanning | 7.6 → 4.1 ms |
| stop calling `qsort` on every index insert | 4.1 → 3.1 ms |
| decide the skip **before** inflating the object | `checkout` 15.0 → 2.8 ms |

`add`: 20.1 → 3.1 ms. `checkout`: 34.5 → 2.8 ms.

The last is the instructive one. `object_read` ran before the test that decides
whether to write, so every blob was decompressed and discarded. Deciding from
`stat` costs a syscall; deciding from content costs a read, an inflate and a
SHA-1.

### There is no index

An index records where an arbitrary write order happened to put things, and it
exists only because the order was arbitrary. Here the digest is the address: an
object's leading bits choose a bucket, objects are written grouped by bucket,
and a lookup computes the bucket and walks it. What is stored is one offset per
bucket, plus one fingerprint byte inside each entry so the walk can tell entries
apart without reconstructing them.

Both halves count. The honest figure is the total:

| | git `.idx` | bit |
|---|---|---|
| per object, stored | 28.05 B | **1.08 B** |
| | | 0.08 B directory + 1 B fingerprint |

**26×**, and it is the *addressing*, not the objects — bit's pack body is 1.06×
smaller than git's, no more. Measured at four repository sizes, the directory
holds at a fraction of a byte per object while git's index stays near 28:

| objects | bit directory | B/obj | git `.idx` | B/obj |
|---|---|---|---|---|
| 152 | 27 B | 0.18 | 5,328 B | 35.05 |
| 1,052 | 63 B | 0.06 | 30,528 B | 29.02 |
| 5,052 | 405 B | 0.08 | 142,528 B | 28.21 |
| 20,052 | 1,557 B | 0.08 | 562,528 B | 28.05 |

Bucket size is the one tuning knob, and it is lopsided. Measured on 5,052
objects:

| objects per bucket | directory | walk | total per object |
|---|---|---|---|
| 8 | 0.61 B | 31 ns | 1.61 B |
| 64 | 0.08 B | 209 ns | **1.08 B** |
| 256 | 0.02 B | 1,570 ns | 1.02 B |

Past 64 the directory has stopped mattering — the fingerprint byte is the whole
remaining cost — while the walk keeps growing. 209 ns is also invisible beside
the ~470 µs a lookup currently spends reading the pack file, which is the real
thing to fix next.

A fingerprint match is only a filter; the candidate is rehashed against all 160
bits before it is returned. That is the rule the truncated prefix index already
followed, with the prefix no longer stored.

One thing was given up. Placement by digest cannot also be placement by
similarity, which is what delta base-finding wants, so bases are chosen in
similarity order and objects are then placed in digest order — a base may sit
either side of the entry referencing it. The links are acyclic by construction,
but the reader's guarantee of termination is the depth cap rather than a
monotonic offset.

### Delta encoding

An object that resembles one already in the pack is stored as instructions to
rebuild it from that one, rather than as its own compressed copy. The
instruction stream is git's format: a byte with the high bit set copies a run
from the base, one with the high bit clear inserts literal bytes.

Measured on three corpora, same content and same object count on both sides:

| corpus | bit, no delta | bit | | git | |
|---|---|---|---|---|---|
| bit's source, 3 commits | 98,007 B | **42,893 B** | 2.28× | 46,167 B | 1.08× |
| git-core binaries, 32 MB | 15,449,819 B | **6,615,889 B** | 2.34× | 6,990,789 B | 1.06× |
| `/dev/urandom` | 329,440 B | 329,440 B | 1.00× | 329,366 B | 1.00× |

The third row is the important one. Random bytes have nothing to delta against,
and every fixture in the benchmark suite was random bytes — which is why this
feature was measured as worthless for as long as it was missing.

Three parameters decide the result, and only one of them mattered:

| | binaries |
|---|---|
| block 16, stride 16 | 7,442,104 B |
| block 16, stride 8 | 6,935,572 B |
| block 16, stride 4 | 6,615,889 B |
| window 32 → 8 | no change |
| chain depth 50 → 4 | no change |

The stride is how often the base is indexed. At a stride equal to the block, a
match is found only where 16 *aligned* bytes line up, which needs a run of 31 to
guarantee; at a stride of 4 a run of 19 suffices. Shared code sits at unaligned
offsets, so alignment was discarding most of the matches actually present. The
window and depth caps never bound anything.

The table is hashed with chaining, not open addressing. Real objects contain
long runs of identical blocks — a Mach-O binary is largely zeros — and under
linear probing those collapse into one cluster that both insert and lookup walk
end to end. That was quadratic: switching to chaining took packing the 32 MB
corpus from 16.8 s to 3.1 s, and the rolling hash added before it turned out to
be worth almost nothing by comparison.

Reconstruction is verified by the digest that named the object, so a delta that
rebuilds the wrong bytes fails the lookup instead of returning them.
`test/delta.sh` reproduces all of the above, including a pass that deletes every
loose object and asserts all 45 come back byte-identical from the pack.

### Transport

Content addressing answers "what do you need?" with `access()` on a path, so the
negotiation git needs a protocol for is visible in the output:

```
clone   5 objects reachable, 5 transferred, 0 already present
push    8 reachable, 3 new, 5 already present
fetch   11 reachable, 3 new
pull    11 reachable, 0 new
```

## Trade-offs

| gain | cost |
|---|---|
| `bit pack`: a 4.7× denser representation, 1.08 B/object of addressing | git cannot read the objects while packed. Reversible — `bit unpack` restores loose form, every object verified against its digest before being written |
| delta encoding: 2.3× smaller pack on real content | packing is 3.3× slower than `git gc --aggressive`, and a read may now apply a chain of up to 50 deltas |
| no stored digest at all | an existence check walks a bucket of ~8 entries and pays one reconstruction per fingerprint match |
| no CRC32 | objects cannot be copied between packs without inflating |
| no fanout table, no index | the bucket is computed, then walked |
| stat cache | requires the racy-index guard, or it is wrong, not merely fast |
| `push` is fast-forward only | cannot force-push; refuses rather than discarding remote commits |
| `pull` fast-forwards or stops | does not begin a merge the caller did not ask for |

## Caveats

- **Single platform.** macOS 15.7.5, Apple silicon. Untested elsewhere; the
  `.dylib` output is Darwin-specific.
- **File-granularity merge.** A file changed on both sides is reported as a
  conflict, never merged line by line. `merge` and `rebase` stop rather than guess.
- **Lightweight tags only.** An annotated tag is a fourth object type bit lacks.
- **No `.gitignore`, worktrees, submodules, hooks or pathspec magic.**
- **Transport is local-path only.** No SSH or HTTP. The negotiation is real; the
  wire is a filesystem.
- **`merge_base` caps at 4,096 commits** of ancestry per side.
- **Benchmark variance** reaches ~0.3 ms on millisecond arms. Only rows gathered
  in one batch may be compared, which is why `vs-git.sh` gathers all of them.

### Limits

`clone`, `fetch` and `push` enumerate into a 200,000-object buffer and **fail
loudly** if a repository exceeds it. Nothing else has a fixed ceiling: ancestry
walks, object enumeration and digest-prefix candidate lists all grow.

## Audit

The implementation was audited with escalating compiler strictness, sanitizers,
hostile input, and pathological content.

| check | result |
|---|---|
| `-Wall -Wextra -Wpedantic -std=c11` | 0 warnings |
| AddressSanitizer + UBSan, full flow | clean, with every command verified to have run |
| hostile input, all 26 commands | every one reports and exits non-zero; no crashes |
| binary files, empty files, no trailing newline | `write-tree` matches git exactly |
| no-trailing-newline diff | matches git, including the `\ No newline` marker |
| directory nesting to 100 levels | matches git exactly |
| symlinks, absolute, relative and dangling | all three match git, mode 120000 |
| 4,200-commit history | `merge_base` and `log` walk it; `git fsck` 0 |
| 200,000 fuzzed deltas, ASan + UBSan | 199,632 exact round trips; 1.6 M corrupted deltas rejected or in-bounds |
| 400 byte-corrupted pack files, ASan + UBSan | read and unpacked with 0 memory errors |

### Defects found and fixed

**Stack overflow at three levels of nesting.** `build_tree` held a 4,096-entry
array of ~540-byte structs on the stack — a 2.1 MB frame *per recursion level* —
which overflowed an 8 MB stack at a three-deep directory tree. git handled the
same tree. Every fixture nested exactly two levels, so nothing reached it. The
array is now on the heap and grows; `write-tree` matches git to 100 levels and
`parity.sh` asserts 60.

**Symlinks were stored as their targets' contents.** `add` used `stat()` and
`slurp()`, both of which follow a link, so a symlink became a mode-100644 blob
holding the bytes of whatever it pointed at — a different tree from git's, and a
different working directory on checkout. A dangling link made `add` fail
outright and vanish from the index. This was worse than the stack overflow: it
wrote wrong data silently rather than crashing. `add` now uses `lstat` and
`readlink`, `checkout` recreates real links, `status` and `diff` read the link
rather than its target, and all four symlink modes match git exactly.

**Unbounded walks were silently truncated.** `merge_base` and `reachable_in` had
4,096-entry stack arrays and scanned membership linearly, making `merge_base`
O(n²) in ancestry depth. Both now use a growable hash set; verified against a
4,200-commit history.

**Every allocation was unchecked.** 31 of 32 call sites could dereference a
`NULL` from a failed `malloc`. All allocation now routes through wrappers that
report and exit before anything is written.

**A corrupt pack could read off the end of the heap.** The delta instruction
stream was bounds-checked, but the two length varints in its header were not: a
varint whose continuation bit is set to the end of the buffer walked past it.
The same hole existed at all six sites that parse a pack header. Found by the
fuzzer on its first run, within seconds. Both varint readers now take an end
pointer and refuse a truncated or over-long encoding, and every caller checks.

**`typeof` is a GNU extension**, caught only by `-std=c11`, and would not have
compiled under a strict toolchain.

Nothing is currently known-broken. The tests that would have caught these —
deep nesting, symlinks, long histories — now exist, which is the part that
matters, since each of these defects was invisible to a suite whose fixtures
were all two levels deep and symlink-free.

## Reproducing

```sh
./build.sh
./test/parity.sh      # 14 assertions against real git; non-zero on any failure
./test/vs-git.sh      # 26 paired operations, one batch, trees asserted identical
./test/delta.sh       # delta: correctness, worth against git, parameter sensitivity
N=2000 ./test/vs-git.sh
```

Every harness applies the same discipline, each rule learned by getting it wrong:

- **Probe before timing.** A child that fails to `exec` is reported, never timed.
  A nonexistent path otherwise yields a fast, stable, meaningless number.
- **Distinguish signals from exit codes.** `$? >> 8` is 0 for a signalled
  process; a `SIGKILL` reads as success unless `$? & 127` is checked.
- **Redirect stdin**, or a command reading to EOF inherits the terminal and hangs.
- **Warm inodes.** The first execution of a never-run inode costs ~95 ms here.
- **One batch**, because cross-batch comparison is invalid at this scale.
- **Assert correctness alongside speed.** `vs-git.sh` compares `write-tree` from
  both before trusting any row.

## Layout

| | |
|---|---|
| `lib/bit.c` | objects, refs, index, trees, diff, pack, transport |
| `cmd/*.c` | one file per command, each exporting `cmd_main` |
| `spec/01-pack.md` | the pack format and the reasoning behind it |
| `test/` | `parity.sh`, `vs-git.sh`, `delta.sh`, `bench.sh` |
| `graft.pack` | declares the commands as a [graft](https://github.com/quivent/graft) namespace |

## Licence

Unlicensed pending review. Not affiliated with the Git project.
