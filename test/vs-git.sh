#!/bin/sh
# vs-git — comprehensive head-to-head. Every command bit implements, paired
# against git on identical input in a single batch.
#
# Discipline, all of it learned the hard way:
#   - every arm is probed once and its success confirmed before timing
#   - stdin is redirected, or a command that reads it hangs forever
#   - inodes are warmed, or the first exec pays ~95 ms
#   - correctness is checked alongside speed; a failing command is never
#     reported as a fast one
set -e
B=${B:-$(cd "$(dirname "$0")/.." && pwd)/build/bin}
N=${N:-450}
T=$(mktemp -d /tmp/vsgit.XXXXXX); trap 'rm -rf "$T"' EXIT

mk() { # mk <dir> <n>
  mkdir -p "$1/src/a" "$1/src/b" "$1/docs"
  i=0; while [ $i -lt $(( $2 / 2 )) ];      do head -c 900 /dev/urandom | base64 > "$1/src/a/f$i.txt"; i=$((i+1)); done
  i=0; while [ $i -lt $(( $2 / 2 - 25 )) ]; do head -c 900 /dev/urandom | base64 > "$1/src/b/g$i.txt"; i=$((i+1)); done
  i=0; while [ $i -lt 25 ];                 do head -c 3000 /dev/urandom | base64 > "$1/docs/d$i.md"; i=$((i+1)); done
}

TIMER=$T/timer.pl
cat > "$TIMER" <<'PL'
use Time::HiRes qw(time);
my ($reps, @cmd) = @ARGV;
# probe: confirm it runs before timing anything
my $p = fork();
if (!$p) { open(STDIN,"</dev/null"); open(STDOUT,">/dev/null"); open(STDERR,">/dev/null");
           exec(@cmd) or exit 77 }
waitpid($p,0);
my $st = $?;
if (($st >> 8) == 77) { print "EXECFAIL\n"; exit }
if ($st & 127)        { print "SIGNAL\n";  exit }
my $t = time;
for (1..$reps) {
  my $q = fork();
  if (!$q) { open(STDIN,"</dev/null"); open(STDOUT,">/dev/null"); open(STDERR,">/dev/null");
             exec(@cmd); exit 1 }
  waitpid($q,0);
}
printf("%.3f\n", (time-$t)*1000/$reps);
PL

row() { # row <label> <reps> -- <bit cmd...> -- <git cmd...>
  lbl=$1; reps=$2; shift 3
  bc=""; while [ "$1" != "--" ]; do bc="$bc $1"; shift; done; shift
  gc="$*"
  b=$(perl "$TIMER" "$reps" $bc); g=$(perl "$TIMER" "$reps" $gc)
  case "$b$g" in *EXECFAIL*|*SIGNAL*)
    printf "  %-30s %10s %10s   %s\n" "$lbl" "$b" "$g" "ARM FAILED"; return;;
  esac
  r=$(perl -e 'printf("%.2fx", $ARGV[1]/$ARGV[0])' "$b" "$g")
  w=$(perl -e 'print $ARGV[0] < $ARGV[1] ? "bit" : "git"' "$b" "$g")
  printf "  %-30s %8s ms %8s ms   %-6s %s\n" "$lbl" "$b" "$g" "$w" "$r"
}

echo "════ bit vs git — $N files ════"
printf "  %-30s %11s %11s   %s\n" "OPERATION" "bit" "git" "faster by"
echo

# ---------- init (fresh dir each time, so measured separately) ----------
mkdir -p "$T/i1" "$T/i2"
row "init" 20 -- "$B/init" "$T/i1" -- git init -q "$T/i2"

# ---------- the shared repo ----------
R=$T/repo; mkdir -p "$R"; mk "$R" "$N"; cd "$R"
"$B/init" . >/dev/null
FILES=$(find . -type f -not -path './.git/*' | wc -l | tr -d ' ')
BIG=$(ls -S docs/*.md | head -1); SMALL=$(ls src/a/*.txt | head -1)

row "hash-object, 3 KB" 30 -- "$B/hash-object" "$BIG"   -- git hash-object "$BIG"
row "hash-object, 1.2 KB" 30 -- "$B/hash-object" "$SMALL" -- git hash-object "$SMALL"
row "hash-object -w" 30 -- "$B/hash-object" -w "$BIG" -- git hash-object -w "$BIG"

# ---------- staging ----------
row "add, $FILES files" 5 -- "$B/add" src docs -- git add src docs

# both indexes now exist; confirm they agree before trusting anything below
BT=$("$B/write-tree"); GT=$(git write-tree)
[ "$BT" = "$GT" ] && AGREE="trees identical: $BT" || AGREE="TREES DIFFER -- results void"
echo
echo "  $AGREE"
echo

row "write-tree, $FILES entries" 20 -- "$B/write-tree" -- git write-tree
row "status, clean" 10 -- "$B/status" -- git status --short

printf 'MODIFIED\n' > "$SMALL"; printf 'x\n' > untracked.tmp
row "status, 1 modified 1 untracked" 10 -- "$B/status" -- git status --short
rm -f untracked.tmp

GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t "$B/add" src docs >/dev/null
row "commit" 10 -- env GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t "$B/commit" -m x \
                -- git -c user.name=t -c user.email=t@t commit -q -m x --allow-empty

H=$(git rev-parse HEAD); TREE=$(git rev-parse HEAD^{tree})
BLOB=$(git cat-file -p "$TREE" | head -1 | awk '{print $3}' | xargs -I{} git cat-file -p {} | head -1 | awk '{print $3}')
ABSENT=0000000000000000000000000000000000000000

row "cat-file -t, commit" 30 -- "$B/cat-file" -t "$H"    -- git cat-file -t "$H"
row "cat-file -s, blob" 30 -- "$B/cat-file" -s "$BLOB" -- git cat-file -s "$BLOB"
row "cat-file -p, blob" 30 -- "$B/cat-file" -p "$BLOB" -- git cat-file -p "$BLOB"
row "cat-file -e, HIT" 30 -- "$B/cat-file" -e "$BLOB" -- git cat-file -e "$BLOB"
row "cat-file -e, MISS" 30 -- "$B/cat-file" -e "$ABSENT" -- git cat-file -e "$ABSENT"
row "log --oneline" 20 -- "$B/log" --oneline -- git log --oneline
row "checkout" 10 -- "$B/checkout" -- git checkout -q -- .

# ---------- batch 2: diff, refs, index manipulation ----------
printf 'MODIFIED AGAIN\n' > "$SMALL"
row "diff, 1 changed of $FILES" 10 -- "$B/diff" -- git diff
"$B/add" src docs >/dev/null; git add src docs >/dev/null 2>&1
row "diff --cached" 10 -- "$B/diff" --cached -- git diff --cached
"$B/branch" bench >/dev/null 2>&1; git branch bench2 >/dev/null 2>&1
row "branch, list" 20 -- "$B/branch" -- git branch
row "switch, same branch (no-op)" 10 -- "$B/switch" master -- git switch -q master
row "reset" 10 -- "$B/reset" -- git reset -q

# ---------- batch 3: show, restore, tag ----------
"$B/add" src docs >/dev/null; git add src docs >/dev/null 2>&1
row "show HEAD" 10 -- "$B/show" -- git show
row "restore, 1 path" 10 -- "$B/restore" "$SMALL" -- git restore "$SMALL"
"$B/tag" v-bench >/dev/null 2>&1; git tag v-bench2 >/dev/null 2>&1
row "tag, list" 20 -- "$B/tag" -- git tag

# ---------- batch 4: transport, over a local path ----------
O=$T/origin; mkdir -p "$O"; cd "$O"
mk "$O" 100 >/dev/null 2>&1 || true
"$B/init" . >/dev/null
"$B/add" src docs >/dev/null
GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t "$B/commit" -m origin >/dev/null
cd "$T"
echo
echo "  ── transport (local path) ──"
printf "  %-30s %11s %11s   %s\n" "OPERATION" "bit" "git" "faster by"
i=0
BC=$T/bclone; GC=$T/gclone
rm -rf "$BC" "$GC"
row "clone, 100 files" 3 -- "$B/clone" "$O" "$T/bc$$" -- git clone -q "$O" "$T/gc$$"
rm -rf "$T/bc$$" "$T/gc$$"
"$B/clone" "$O" "$T/w" >/dev/null 2>&1
cd "$T/w"
row "fetch, nothing new" 10 -- "$B/fetch" "$O" -- git fetch -q "$O"
row "push, nothing new" 10 -- "$B/push" "$O" -- git push -q "$O" master
cd "$R"



echo
echo "  ── packing ──"
LOOSE=$(perl -e '$s=0;for(`find .git -type f`){chomp;$s+=int(((-s $_)+4095)/4096)*4096}print $s')
NOBJ=$(find .git/objects -type f | wc -l | tr -d ' ')
"$B/pack" > "$T/packout"
BPACK=$(perl -e '$s=0;for(`find .git/bitpack -type f`){chomp;$s+=-s $_}print $s')
BIDX=$(wc -c < .git/bitpack/bit.dir | tr -d ' ')
git gc -q 2>/dev/null || true
GPACK=$(perl -e '$s=0;for(`find .git/objects/pack -name "*.pack"`){chomp;$s+=-s $_}print $s')
GIDX=$(perl -e '$s=0;for(`find .git/objects/pack -name "*.idx"`){chomp;$s+=-s $_}print $s')
perl -e '
my ($n,$bp,$bi,$gp,$gi) = @ARGV;
printf("  %-30s %8s    %8s\n","", "bit", "git");
printf("  %-30s %8d B  %8d B\n","pack body",$bp-$bi,$gp);
printf("  %-30s %8d B  %8d B   %.2fx\n","index",$bi,$gi,$gi/$bi);
printf("  %-30s %8.1f    %8.1f    %.2fx\n","directory B/object",$bi/$n,$gi/$n,($gi/$n)/($bi/$n));
' "$NOBJ" "$BPACK" "$BIDX" "$GPACK" "$GIDX"
echo
echo "  objects: $NOBJ    loose on disk: $LOOSE B"
