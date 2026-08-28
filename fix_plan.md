# fix_plan — bit

## Done

- [x] TASK-001 Isolated parity harness. `test/parity.sh`, 10 assertions.
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
- [ ] TASK-009 The candidate array in `pack_read` is fixed at 8. Make it dynamic.
- [ ] TASK-010 mmap the pack index rather than `slurp`ing it per call. The 13.86 µs
      miss is dominated by reading a 2,448 B file, not by eight probes.
- [ ] TASK-011 Delta encoding in the pack. The one thing git's `.pack` does that
      ours does not.
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
