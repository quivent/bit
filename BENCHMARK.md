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

## Caveats

- Random-content blobs. Delta compression, which git's pack does and ours does
  not, is worth nothing here and would matter on real source.
- `git commit` runs with `--allow-empty` in the commit arm, so it does slightly
  less work than bit's.
- git is doing more than bit in several arms: it consults `.gitignore`, supports
  worktrees, submodules, hooks and pathspec magic. bit implements none of that.
  The comparison is honest about what each *does*, not about what each *could*.
- Single machine, single platform, one tree shape.
