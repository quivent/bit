#!/bin/sh
# Delta compression: does it work, and what is it worth.
#
# Three questions, in the order they matter:
#   1. correctness -- with every loose object deleted, does each one come back
#      from the pack byte-identical, and can git read the result?
#   2. worth -- same content, same object count, bit against git.
#   3. sensitivity -- which search parameter actually moved the number.
#
# Corpus 1 is real source with three commits of small edits. Corpus 2 is the
# git-core binaries, which are 22 builds of largely the same code and so the
# hardest available test of base-finding. A third corpus of /dev/urandom is run
# to show the case where delta is worth nothing, because that fixture is what
# hid the absence of this feature for as long as it did.
#
# Run: sh test/delta.sh
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
[ -x build/bin/pack ] || ./build.sh >/dev/null
B="$ROOT/build/bin"
W=${TMPDIR:-/tmp}/bit-delta.$$
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# a no-delta build of the same source, so the baseline is one code path
printf 'int cmd_main(int,char**);\nint main(int a,char**v){return cmd_main(a,v);}\n' > "$W/main.c"
cc -O2 -DDELTA_DEPTH=0 -o "$W/pack-nodelta" "$ROOT/cmd/pack.c" "$ROOT/lib/bit.c" "$W/main.c" -lz 2>/dev/null

g() { git -c user.email=t@t -c user.name=t "$@"; }
size() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
packsz() { awk '/^  pack/{print $2}'; }

# ---------------------------------------------------------------- corpus 1
echo "corpus 1: bit's own source, 3 commits of small edits"
mkdir -p "$W/c1b" "$W/c1g"
seed1() {
  cp "$ROOT"/lib/*.c "$ROOT"/lib/*.h "$ROOT"/cmd/*.c "$1"/
}
seed1 "$W/c1b"; seed1 "$W/c1g"

( cd "$W/c1b"; $B/init >/dev/null; $B/add . >/dev/null; $B/commit -m one >/dev/null
  sed -i.x 's/xmalloc/xmalloc_/g' bit.c; rm -f *.x
  $B/add . >/dev/null; $B/commit -m two >/dev/null
  sed -i.x 's/static/static  /g' *.c; rm -f *.x
  $B/add . >/dev/null; $B/commit -m three >/dev/null )

( cd "$W/c1g"; git init -q .; git add -A; g commit -qm one
  sed -i.x 's/xmalloc/xmalloc_/g' bit.c; rm -f *.x; git add -A; g commit -qm two
  sed -i.x 's/static/static  /g' *.c; rm -f *.x; git add -A; g commit -qm three
  git gc -q --aggressive 2>/dev/null )

# --- correctness, on this corpus ---
(cd "$W/c1b"
find .git/objects -type d -name '??' | sed 's|.*/||' | while read d; do
  ls ".git/objects/$d" | sed "s|^|$d|"
done | sort > "$W/oids"
NOBJ=$(wc -l < "$W/oids" | tr -d ' ')
while read o; do
  printf '%s %s\n' "$o" "$($B/cat-file -p "$o" 2>/dev/null | shasum | cut -d' ' -f1)"
done < "$W/oids" > "$W/before"
$B/pack > "$W/c1.on"
find .git/objects -type d -name '??' -exec rm -rf {} +      # loose gone; pack only
while read o; do
  printf '%s %s\n' "$o" "$($B/cat-file -p "$o" 2>/dev/null | shasum | cut -d' ' -f1)"
done < "$W/oids" > "$W/after"
if cmp -s "$W/before" "$W/after"; then
  echo "  PASS  $NOBJ/$NOBJ objects byte-identical read back from a delta pack"
else
  echo "  FAIL  objects differ after packing:"; diff "$W/before" "$W/after" | head; exit 1
fi
$B/unpack >/dev/null                                         # restores the loose store
if git fsck --no-dangling >/dev/null 2>&1; then
  echo "  PASS  git fsck accepts the unpacked objects"
else
  echo "  FAIL  git rejects the unpacked objects"; exit 1
fi
$B/pack > "$W/c1.on"
"$W/pack-nodelta" > "$W/c1.off" )

# ---------------------------------------------------------------- corpus 2
echo "corpus 2: git-core binaries, deduplicated"
mkdir -p "$W/c2b" "$W/c2g"
find "$(git --exec-path)" -maxdepth 1 -type f -perm +111 2>/dev/null |
  while read f; do echo "$(shasum "$f" | cut -c1-40)|$f"; done |
  sort -u -t'|' -k1,1 | cut -d'|' -f2 |
  while read f; do cp "$f" "$W/c2b/"; cp "$f" "$W/c2g/"; done
( cd "$W/c2b"; $B/init >/dev/null; $B/add . >/dev/null; $B/commit -m bins >/dev/null )
now() { python3 -c 'import time;print(time.time())'; }
T0=$(now); ( cd "$W/c2b"; $B/pack ) > "$W/c2.on"; T1=$(now)
( cd "$W/c2b"; "$W/pack-nodelta" ) > "$W/c2.off"
( cd "$W/c2g"; git init -q .; git add -A; g commit -qm bins )
T2=$(now); ( cd "$W/c2g"; git gc -q --aggressive 2>/dev/null ); T3=$(now)

# ---------------------------------------------------------------- corpus 3
echo "corpus 3: /dev/urandom -- the case delta cannot help"
mkdir -p "$W/c3b" "$W/c3g"
i=0; while [ $i -lt 40 ]; do
  dd if=/dev/urandom of="$W/c3b/f$i" bs=4096 count=2 2>/dev/null
  cp "$W/c3b/f$i" "$W/c3g/f$i"; i=$((i+1))
done
( cd "$W/c3b"; $B/init >/dev/null; $B/add . >/dev/null; $B/commit -m r >/dev/null; $B/pack ) > "$W/c3.on"
( cd "$W/c3b"; "$W/pack-nodelta" ) > "$W/c3.off"
( cd "$W/c3g"; git init -q .; git add -A; g commit -qm r; git gc -q --aggressive 2>/dev/null )

# ---------------------------------------------------------------- results
gpack() { size "$1"/.git/objects/pack/*.pack; }
gidx()  { size "$1"/.git/objects/pack/*.idx;  }
row() { # name off on gitdir
  OFF=$(packsz < "$W/$2.off"); ON=$(packsz < "$W/$2.on"); GP=$(gpack "$3")
  ND=$(sed -n 's/.*(\([0-9]*\) as.*/\1/p' "$W/$2.on")
  NO=$(sed -n 's/packed \([0-9]*\) .*/\1/p' "$W/$2.on")
  awk -v n="$1" -v o="$OFF" -v c="$ON" -v gp="$GP" -v nd="$ND" -v no="$NO" 'BEGIN{
    printf "  %-22s %3s obj %3s delta %10d %10d %6.2fx %10d %7.3fx\n", n,no,nd,o,c,o/c,gp,gp/c }'
}
echo
echo "                          objects        bit off     bit on   ratio      git   bit/git"
row "source, 3 commits" c1 "$W/c1g"
row "git-core binaries" c2 "$W/c2g"
row "random bytes"      c3 "$W/c3g"
echo
echo "  time to pack corpus 2 (32 MB):"
python3 -c "print('    bit %.1fs   git gc --aggressive %.1fs   bit is %.1fx slower' % ($T1-$T0, $T3-$T2, ($T1-$T0)/($T3-$T2)))"
echo
echo "  index size, same object sets:"
for c in c1 c2 c3; do
  BI=$(awk '/^  idx/{print $2}' "$W/$c.on"); GI=$(gidx "$W/${c}g")
  awk -v c="$c" -v b="$BI" -v g="$GI" 'BEGIN{printf "    %s  bit %7d B   git %7d B   %5.2fx\n", c,b,g,g/b}'
done

# ---------------------------------------------------------------- sensitivity
echo
echo "sensitivity, corpus 2 -- one parameter at a time"
set +e
printf '32 50 16 16\n32 50 16 8\n32 50 32 8\n32 50 8 4\n8 50 16 8\n32 4 16 8\n' > "$W/sweep"
while read WI DE BK ST; do
  cc -O2 -DDELTA_WIN=$WI -DDELTA_DEPTH=$DE -DDELTA_BLK=$BK -DDELTA_STEP=$ST \
     -o "$W/pk" "$ROOT/cmd/pack.c" "$ROOT/lib/bit.c" "$W/main.c" -lz 2>"$W/cc.err"
  if [ $? -ne 0 ]; then
    printf "    win=%-3s depth=%-3s block=%-3s stride=%-3s  build failed: %s\n" \
           "$WI" "$DE" "$BK" "$ST" "$(head -1 "$W/cc.err")"
    continue
  fi
  cd "$W/c2b"; "$W/pk" > "$W/sw.out"; cd "$ROOT"
  SZ=$(packsz < "$W/sw.out")
  ND=$(sed -n 's/.*(\([0-9]*\) as.*/\1/p' "$W/sw.out")
  printf "    win=%-3s depth=%-3s block=%-3s stride=%-3s  %10s B  %s deltas\n" \
         "$WI" "$DE" "$BK" "$ST" "$SZ" "$ND"
done < "$W/sweep"
set -e
printf "    git gc --aggressive%25s%10s B\n" "" "$(gpack "$W/c2g")"
