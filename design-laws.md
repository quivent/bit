# Design laws — bit

Applied to every iteration of performance work. Scored explicitly, PASS or FAIL.

- **LAW-1 Byte identity.** Every object bit writes has the same SHA-1 as git's for the
  same input. Checked by direct comparison, not inference.
- **LAW-2 git accepts the repository.** `git fsck` exits 0 and `git log` reads bit's
  commits.
- **LAW-3 No unverified measurement.** A timing may only be reported for a command whose
  success was confirmed on that input. Empty output is a failure, not a fast result.
- **LAW-4 Clean build.** Zero warnings at `-Wall -Wextra`.
- **LAW-5 Paired measurement.** A claimed speedup is measured before and after, on the
  same input, in the same batch. Cross-batch comparison is not evidence.
- **LAW-6 No capability traded for speed.** Nothing is removed or weakened to make a
  number better.
