# bit vs git

The comparison that is apples-to-apples. bit and git write byte-identical
objects, so storage is tied by construction and what remains is code and speed.
Every run asserts the trees match before any timing is reported.

Platform: macOS 15.7.5, Apple silicon. Reproduce with `test/vs-git.sh`.

## Method

Each arm is probed once and its success confirmed before timing begins. This
is not ceremony — four earlier rounds of this measurement were invalidated by
timing something that never ran, and one reported a repository state that had
been rewritten between calls.

- **Probe before timing.** A child that fails to `exec` returns a sentinel and
  the arm is reported `EXECFAIL`, never as a fast result.
- **Signals are distinguished from exit codes.** In perl `$? >> 8` is 0 for a
  signalled process; a `SIGKILL` looks like success unless `$? & 127` is checked.
- **stdin is redirected.** A command that reads to EOF otherwise inherits the
  terminal and hangs forever.
- **Inodes are warmed.** The first execution of a never-run inode costs ~95 ms
  on this platform, flat with binary size.
- **One batch.** Run-to-run variation reaches ~0.3 ms on millisecond arms, and
  one arm measured 2.52 ms in one batch and 3.79 ms in another. Only rows
  gathered together may be compared.
- **Correctness alongside speed.** `write-tree` output from both is compared
  before the rest of the table is trusted.

## Results, 450 files

| operation | bit | git | |
|---|---|---|---|
| `init` | 2.281 ms | 15.643 ms | **6.86×** |
| `hash-object`, 3 KB | 2.919 ms | 12.706 ms | **4.35×** |
| `hash-object`, 1.2 KB | 2.469 ms | 11.845 ms | **4.80×** |
| `hash-object -w` | 2.160 ms | 12.636 ms | **5.85×** |
| `add`, 450 files | 3.125 ms | 12.052 ms | **3.86×** |
| `write-tree`, 450 entries | 6.205 ms | 13.716 ms | **2.21×** |
| `status`, clean | 3.216 ms | 11.093 ms | **3.45×** |
| `status`, 1 modified 1 untracked | 3.719 ms | 11.167 ms | **3.00×** |
| `commit` | 7.941 ms | 19.551 ms | **2.46×** |
| `cat-file -t` | 1.649 ms | 10.525 ms | **6.38×** |
| `cat-file -s` | 2.221 ms | 14.573 ms | **6.56×** |
| `cat-file -p` | 2.330 ms | 11.692 ms | **5.02×** |
| `cat-file -e`, hit | 2.036 ms | 9.268 ms | **4.55×** |
| `cat-file -e`, miss | 1.577 ms | 9.046 ms | **5.74×** |
| `log --oneline` | 2.196 ms | 10.653 ms | **4.85×** |
| `checkout` | 4.571 ms | 11.298 ms | **2.47×** |

Trees identical every run. bit is ahead on all sixteen.

| packing | bit | git |
|---|---|---|
| pack body | 496,816 B | 495,407 B |
| index | 5,784 B | 15,612 B |
| **index per object** | **12.0 B** | **32.5 B** |

## How the two slowest got fast

Both losses in the first full run traced to the same thing, and it is worth
recording because the fix was never a better algorithm.

### `add`: 20.1 ms → 3.1 ms

| change | result |
|---|---|
| baseline, `popen("find")` per directory | 20.1 ms |
| `readdir` instead of a subprocess | 12.2 ms |
| consult the index stat cache, skip unchanged files | 7.6 ms |
| binary search that lookup instead of scanning | 4.1 ms |
| `index_upsert` stops calling `qsort` on every insert | **3.1 ms** |

### `checkout`: 34.5 ms → 4.6 ms

| change | result |
|---|---|
| baseline, rewrite every file unconditionally | 34.5 ms |
| skip files already correct — decided by **hashing** them | 17.5 ms |
| binary search the old index instead of scanning it | 15.0 ms |
| decide from `stat`, and skip **before** inflating the object | **2.8–4.6 ms** |

The third row is the one that mattered. `object_read` was being called before
the skip test, so every blob was decompressed and then discarded. Deciding from
`stat` costs a syscall; deciding from content costs a read, an inflate and a
SHA-1.

## What was wrong in my own code, found by benchmarking

- **A subprocess per directory.** `popen("find")` to obtain names the kernel
  hands over directly: a fork, an exec, a shell to parse the command, a pipe.
  38% of staging.
- **Three linear scans that should have been binary searches.** The index is
  kept sorted; `add`, `checkout` and `index_upsert` each scanned it. At 450
  entries that is 202,500 `strcmp` calls per operation.
- **`qsort` on every insert.** 450 inserts, each re-sorting an array of
  552-byte elements.
- **Work done before the test that would have avoided it.** Inflating an object
  and then deciding not to write it.

## The field I called bloat

The index carries `ctime`, `mtime`, `dev`, `ino`, `uid`, `gid` and `size` per
entry. Measured across 300 entries, nine of eleven fields held exactly one
distinct value — 45% of the file — and it was written up as redundancy.

It is not redundancy. It is the cache that lets `status`, `add` and `checkout`
decide "unchanged" from a `stat` without reading, let alone hashing, the file.
Three of the four optimisations above are simply *using* it. The measurement was
right and the conclusion was wrong: a field that is constant is not thereby
useless.

## Packing, on content that is not random

Every fixture above is `/dev/urandom`, which is the right choice for timing the
working-tree arms and the wrong one for measuring a pack: random bytes have
nothing to delta against, so a pack of them measures compression and nothing
else. That fixture is the reason delta encoding read as worthless for as long as
bit did not have it.

`test/delta.sh` measures it on content that is not random. Same content, same
object count, both sides packed:

| corpus | bit, delta off | bit | | git | bit/git |
|---|---|---|---|---|---|
| bit's own source, 3 commits of small edits | 98,007 B | **42,893 B** | 2.28× | 46,167 B | 1.076× |
| git-core binaries, 22 files, 32 MB | 15,449,819 B | **6,615,889 B** | 2.34× | 6,990,789 B | 1.057× |
| 40 × 8 KB of `/dev/urandom` | 329,440 B | 329,440 B | 1.00× | 329,366 B | 1.000× |

The third row is the control. It confirms both that delta finds nothing in
random bytes and that it costs nothing to try.

The cost is time. Packing the 32 MB corpus takes bit 4.4 s against
`git gc --aggressive` at 1.4 s — **3.3× slower** — because each of the 32
candidate bases rebuilds a hash index of itself for every target object. That
is `TASK-013`; building it once per base is the obvious fix and has not been
done.

### What actually moved the number

| change | 32 MB corpus |
|---|---|
| base indexed every 16 B (= block size) | 7,442,104 B |
| base indexed every 8 B | 6,935,572 B |
| base indexed every 4 B | **6,615,889 B** |
| block size 32 instead of 16 | 7,448,634 B |
| candidate window 32 → 8 | no change |
| chain depth 50 → 4 | no change |

Only the stride. The window and depth caps, the two parameters that look like
the important ones, never bound anything on this corpus.

Separately, and not visible in output size at all: the block index originally
used open addressing. Objects contain long runs of identical blocks — a Mach-O
binary is largely zeros — and those all cluster into one probe chain that both
insert and lookup walk end to end. Switching to chaining took the same pack from
16.8 s to 3.1 s. A rolling hash added just before that, on the theory that the
scan was the cost, was worth 0.8 s of the 16.8.

## The index, deleted

The pack index was 12.0 B/object: an eight-byte digest prefix and a four-byte
offset. Measured against the objects it pointed at, on a 600-object history:

| kind | count | B each | index entry, as a share of it |
|---|---|---|---|
| blob | 123 | 8.4 | **142%** |
| delta | 120 | 58.9 | 20% |
| tree | 356 | 43.6 | 28% |
| commit | 1 | 132.0 | 9% |

For the most numerous small object the pointer was larger than the object. The
index existed only because the write order was arbitrary; objects are now
written grouped by a bucket taken from the digest's leading bits, so the
location is computed rather than recorded. What is stored is one offset per
bucket, about one bucket per eight objects.

| | before | after |
|---|---|---|
| 600 objects | pack 22,969 B + index 7,212 B | pack 23,763 B + directory 276 B |
| total | 30,181 B | **24,039 B** |

The pack grew by 794 B -- one fingerprint byte per object, which is what lets a
bucket be walked without reconstructing every entry in it, plus fixed-width base
offsets in delta entries. The directory shrank by 6,936 B.

## Caveats

- The working-tree arms use random-content blobs, which is why packing is
  measured separately above.
- `git commit` runs with `--allow-empty` in the commit arm, so it does slightly
  less work than bit's.
- git is doing more than bit in several arms: it consults `.gitignore`, supports
  worktrees, submodules, hooks and pathspec magic. bit implements none of that.
  The comparison is honest about what each *does*, not about what each *could*.
- Single machine, single platform, one tree shape.
