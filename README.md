# bit

git's data model, reimplemented in 2,486 lines of C.

bit writes **byte-identical objects to git**. A repository bit creates is one
git reads: `git fsck` returns 0, `git log` reads bit's commits, and `git status`
reads a staging area `bit add` wrote. That is the specification, not a goal, and
`test/parity.sh` asserts it in ten places.

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
| pack index | **12.0 B/object** | 32.5 B/object | 2.70× |

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

### The pack index

git's `.idx` spends 31.5 B/object; bit spends 12.0.

| part | git | bit | why |
|---|---|---|---|
| fanout table | 1,024 B | 0 | 300 sorted entries is eight probes |
| digest | 20 B/obj | 8 B/obj | a prefix is a filter, not an identifier |
| CRC32 | 4 B/obj | 0 | the digest already proves the content |
| offset | 4 B/obj | 4 B/obj | unchanged |

Truncating costs **no safety**: a hit is confirmed by rehashing against all 160
bits, and a prefix index has no false negatives, so absence is exact and free.
Only a positive check pays a decompression — measured at 13.86 µs for a miss
against 39.40 µs for a hit.

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
| `bit pack`: 12.0 B/object index, 4.7× less disk | git cannot read the objects while packed. Reversible — `bit unpack` restores loose form, every object id verified identical |
| 8-byte digest prefix | a *positive* existence check pays one decompression |
| no CRC32 | objects cannot be copied between packs without inflating |
| no fanout table | eight probes rather than ~4 |
| stat cache | requires the racy-index guard, or it is wrong, not merely fast |
| `push` is fast-forward only | cannot force-push; refuses rather than discarding remote commits |
| `pull` fast-forwards or stops | does not begin a merge the caller did not ask for |

## Caveats

- **Single platform.** macOS 15.7.5, Apple silicon. Untested elsewhere; the
  `.dylib` output is Darwin-specific.
- **No delta encoding.** git's pack stores similar objects as reconstruction
  instructions. bit compresses each independently — worth nothing on the random
  blobs benchmarked, a great deal on real source.
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

**`typeof` is a GNU extension**, caught only by `-std=c11`, and would not have
compiled under a strict toolchain.

Nothing is currently known-broken. The tests that would have caught these —
deep nesting, symlinks, long histories — now exist, which is the part that
matters, since each of these defects was invisible to a suite whose fixtures
were all two levels deep and symlink-free.

## Reproducing

```sh
./build.sh
./test/parity.sh      # 10 assertions against real git; non-zero on any failure
./test/vs-git.sh      # 26 paired operations, one batch, trees asserted identical
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
| `test/` | `parity.sh`, `vs-git.sh`, `bench.sh` |
| `graft.pack` | declares the commands as a [graft](https://github.com/quivent/graft) namespace |

## Licence

Unlicensed pending review. Not affiliated with the Git project.
