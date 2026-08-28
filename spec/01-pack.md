# The pack format

## Why it exists

A loose object is one file, and a file occupies a whole filesystem block. A
306-object repository measured **316,232 B of data in 1,277,952 B of disk —
75% padding**, because the average object is 949 B and the block is 4,096 B.

That is three times the logical size of everything stored, and it belongs to no
format. It is a consequence of one-file-per-object.

Packing solves two problems, and only one of them is compression:

1. N files collapse to 2, so per-object block padding disappears.
2. Objects can be stored adjacently, which enables cross-object delta. **We do
   not do this yet.** git does, and it is the remaining gap.

Measured on 203 objects: 860,160 B loose → 184,320 B packed, **4.7×**. The
logical bytes barely moved. Almost the entire win was problem 1.

## Layout

```
.git/bitpack/bit.pack
  "BITPACK1"                    8
  u32 count                     4
  per object, at a recorded offset, one of two entry shapes:

    whole object
      u8      type              1     1 commit, 2 tree, 3 blob
      varint  inflated length
      varint  compressed length
      bytes   zlib data

    delta against an earlier object
      u8      type              1     7
      varint  base offset             absolute, always < this entry's offset
      varint  inflated delta length
      varint  compressed length
      bytes   zlib delta

  A delta entry has no type of its own; it inherits the type of the object at
  the end of its base chain. Bases always precede the entries that reference
  them, so resolution walks backwards through the file and terminates.

.git/bitpack/bit.idx
  "BITIDX01"                    8
  u32 count                     4
  per object, sorted by prefix:
    u8[8]   digest prefix       8
    u32     offset into pack    4
```

Not `.git/objects/pack/`. git parses anything it finds there and reports
`non-monotonic index`.

## The delta stream

Reconstruction instructions, in git's encoding. A byte with the high bit set is
a copy from the base; its low bits say which of four offset bytes and two
length bytes follow, so a short copy near the start of the base costs three
bytes and a long copy far into it costs seven. A byte with the high bit clear
is a literal insert of that many bytes, so a run of new content costs one byte
per 127.

```
  varint  base size                   checked against the actual base
  varint  target size                 the output buffer is sized from this
  then, until the target size is reached:
    1xxxxxxx  copy    [off0][off1][off2][off3][len0][len1]
    0nnnnnnn  insert  n literal bytes, n in 1..127
    00000000  reserved; refused
```

Both the offsets and the lengths are checked against both ends before use: the
source against the base's actual length, the destination against the size the
header declared. A pack is a file, and a file can be truncated, corrupted on
disk, or written by something hostile. The first version of this checked the
instruction stream but not the two header varints, and a varint whose
continuation bit ran to the end of the buffer read past it — a heap overflow a
fuzzer found within seconds of first being pointed at it.

Nothing else needs to trust the delta. A reconstructed object is hashed and
compared against the digest that named it, so a chain that rebuilds the wrong
bytes fails the lookup rather than returning them.

## Choosing a base

Objects are written in (type, size-descending) order, which puts objects likely
to resemble each other next to each other, and each is offered the last 32
objects of its own type as a candidate base. The smallest resulting delta wins,
and only if the whole entry — header included — comes out smaller than storing
the object outright.

Base blocks are indexed every 4 bytes at a block size of 16. Those two numbers
are the entire result:

| block | stride | git-core binaries, 32 MB |
|---|---|---|
| 16 | 16 | 7,442,104 B |
| 16 | 8 | 6,935,572 B |
| 16 | 4 | **6,615,889 B** |
| 32 | 8 | 7,448,634 B |

At a stride equal to the block, a match is found only where 16 *aligned* bytes
line up, which needs a run of 31 bytes to guarantee; at a stride of 4, a run of
19 suffices. Shared code sits at unaligned offsets. Meanwhile the window (32)
and the chain depth cap (50) were measured at 8 and 4 with no change in output
at all — neither was ever binding.

The block index chains rather than probing. Real objects contain long runs of
identical blocks — a Mach-O binary is largely zeros — and under open addressing
those collapse into a single cluster that both insert and lookup traverse end
to end, which is quadratic. Chaining, with a cap of 64 chain entries examined
per position, took packing the 32 MB corpus from 16.8 s to 3.1 s.

## The index, against git's

git's `.idx` spends **31.5 B/object**; ours spends **12.0**.

| part | git | ours | why |
|---|---|---|---|
| fanout table | 1,024 B | 0 | 203 sorted entries is eight probes |
| digest | 20 B/obj | 8 B/obj | a prefix is a filter, not an identifier |
| CRC32 | 4 B/obj | 0 | the digest already proves the content |
| offset | 4 B/obj | 4 B/obj | unchanged |

The CRC exists so a packer can copy an object between packs without inflating
it. Nothing on the read path uses it.

## What truncating the digest costs

**Nothing in safety.** The 12 dropped bytes are not lost, they are recomputed:
a lookup inflates the candidate and rehashes it, then compares all 160 bits. A
filter can be arbitrarily leaky without ever being wrong.

Two distinct probabilities, which are easy to conflate:

| n objects | index holds *any* colliding pair, n²/2⁶⁵ | *a given lookup* hits a false positive, n/2⁶⁴ |
|---|---|---|
| 10⁶ | 2.7e-08 | 5.4e-14 |
| 10⁹ | 2.7e-02 | 5.4e-11 |

The first is the wrong metric — it says a pair exists somewhere, not that you
will touch it. The second is what costs work, and it is one extra inflate,
essentially never.

**A truncated-prefix index has no false negatives.** If the object is in the
pack, its prefix is in the index. So absence is exact and free; only presence
needs confirming. Measured in-process over 20,000 iterations:

| | |
|---|---|
| miss — prefix search only, exact | **13.86 µs** |
| hit — prefix, inflate, rehash | 39.40 µs |
| the confirming inflate | 25.54 µs |

This matters because the question asked most often — fetch negotiation, `cat-file
-e` — is overwhelmingly *misses*, and a miss reads a smaller index than git's.

Known defect: candidates sharing a prefix are collected into a fixed array of 8.
Balls-in-bins puts the maximum load at 10⁹ objects into 2⁶⁴ buckets at 1–2, so
it does not fill — but it is a hard limit in a format and should be dynamic.

## Packing is a mode, not a one-way door

While packed, the store is dense and **git cannot read it** (`git fsck` exits 2).
`bit unpack` restores the loose form and git reads it again (`exit 0`). The round
trip is lossless for a structural reason: an object's identity is its content, so
what comes out hashes to what went in. Verified — every object id identical
across pack → delete loose → unpack.

| | objects | disk | `git fsck` |
|---|---|---|---|
| loose | 203 | 860,160 B | 0 |
| packed | 0 loose | 184,320 B | 2 |
| unpacked | 203 | 1,019,904 B | 0 |
