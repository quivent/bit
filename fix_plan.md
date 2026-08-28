# fix_plan — bit

## Done

- [x] TASK-001 Isolated parity harness. `test/parity.sh`, 14 assertions.
- [x] TASK-002 Profile `add`. The `popen("find")` was 7.6 ms of 20.1 ms.
- [x] TASK-003 Replace it with `readdir`. 20.1 → 12.2 ms, no capability lost.
- [x] TASK-004 Byte-count analysis. See `spec/01-pack.md` and the notes below.
- [x] TASK-006 `status` — three-way HEAD/index/worktree, matches `git status --short`.
- [x] TASK-007 `checkout` — materialise a commit, rebuild the index.
- [x] TASK-008 `pack` / `unpack` — 4.7× on disk, 12.0 B/object index, lossless round trip.

## Open

- [x] TASK-005 index_upsert no longer qsorts per insert: binary search plus an
      in-place insert. add 4.1 -> 3.1 ms, checkout 15.0 -> 2.8 ms.
      re-sorting an array of 520-byte elements. Append, sort once at write.
- [x] TASK-009 The candidate array in `pack_read` grows rather than capping at 8.
      A fixed array dropped candidates silently, and the dropped one could be
      the object asked for.
- [ ] TASK-010 mmap the pack index rather than `slurp`ing it per call. The 13.86 µs
      miss is dominated by reading a 2,448 B file, not by eight probes.
- [x] TASK-011 Delta encoding in the pack. Done. Same content and object count,
      bit's pack is now 1.08x smaller than git's on source and 1.06x smaller on
      the git-core binaries; it was 1.61x larger before. Packing costs 3.3x
      git's time. `test/delta.sh` reproduces it.
      Three things were learned and are worth not re-learning:
        - the base index stride, not the window or the chain depth, is what
          decides the result. Window 32 -> 8 and depth 50 -> 4 changed nothing;
          stride 16 -> 4 was worth 826 KB.
        - open addressing is quadratic here. Objects contain long runs of
          identical blocks, and they all cluster. Chaining: 16.8 s -> 3.1 s.
        - every benchmark fixture was /dev/urandom, where delta is worth
          exactly zero. That is why the feature read as worthless for as long as
          it was absent. Fixture-blindness again, after the symlink and the
          nesting bugs.
- [ ] TASK-013 Pack time. 4.4 s against `git gc --aggressive` at 1.4 s on the
      32 MB corpus. The remaining cost is that each of the 32 window candidates
      rebuilds a hash index of its own base for every target. Building it once
      per base and keeping it with the window entry is the obvious fix.
- [ ] TASK-012 `diff`. Then `branch`/`switch`, then `merge`.

## Notes worth keeping

- git stores a digest **three different ways in one repository**: raw 20 bytes in
  trees, raw 20 bytes in the index, and **40 ASCII hex characters in commits**.
  A commit spends 80 bytes on tree+parent where 40 would do.
- The index's stat fields — `ctime`, `mtime`, `dev`, `ino`, `uid`, `gid`, `size` —
  measured 9-of-11 constant across 300 entries and looked like 45% waste. They
  are not waste. They are what lets `status` decide "unchanged" from a `stat`
  without reading, let alone hashing, the file. Implementing `status` vindicated
  the field I had called bloat.
- Digests do not compress. gzip 0.994×, zstd 0.998×, xz 0.989×, bzip2 0.927× —
  every compressor makes them **larger**. A cryptographic hash is maximally
  entropic by construction. They can only be avoided, never squeezed.
