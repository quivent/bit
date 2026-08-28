#!/bin/sh
# bench — measure bit against git on identical input, in one batch.
# LAW-3: every timed command is run once first and its success confirmed.
set -e
B=${B:-$(cd "$(dirname "$0")/.." && pwd)/build/bin}
N=${N:-450}
T=$(mktemp -d /tmp/bit-bench.XXXXXX); trap 'rm -rf "$T"' EXIT; cd "$T"
mkdir -p src/a src/b docs
i=0; while [ $i -lt $((N/2)) ]; do head -c 900 /dev/urandom | base64 > src/a/f$i.txt; i=$((i+1)); done
i=0; while [ $i -lt $((N/2 - 25)) ]; do head -c 900 /dev/urandom | base64 > src/b/g$i.txt; i=$((i+1)); done
i=0; while [ $i -lt 25 ]; do head -c 3000 /dev/urandom | base64 > docs/d$i.md; i=$((i+1)); done
FILES=$(find . -type f | wc -l | tr -d ' ')
echo "  tree: $FILES files"
"$B/init" . >/dev/null
LIST=$(find . -type f -not -path './.git/*' | sed 's|^\./||')

verify() { "$@" >/dev/null 2>&1 || { echo "  ABORT: '$*' failed"; exit 1; }; }
t() {  # t <label> <reps> <cmd...>
  lbl=$1; reps=$2; shift 2
  verify "$@"
  s=$(perl -MTime::HiRes=time -e 'print time')
  n=0; while [ $n -lt $reps ]; do "$@" >/dev/null 2>&1; n=$((n+1)); done
  e=$(perl -MTime::HiRes=time -e 'print time')
  perl -e 'printf("  %-40s %8.1f ms\n", $ARGV[0], ($ARGV[2]-$ARGV[1])*1000/$ARGV[3])' "$lbl" "$s" "$e" "$reps"
}
echo
echo "  --- staging $FILES files ---"
t "bit add <dir>   (popen find per dir)" 5 "$B/add" src docs
H1=$("$B/write-tree")
rm -f .git/index
# shellcheck disable=SC2086
t "bit add <explicit list, no popen)" 5 "$B/add" $LIST
H2=$("$B/write-tree")
rm -f .git/index
t "git add <dir>" 5 git add src docs
H3=$(git write-tree)
echo
echo "  --- tree from index ---"
t "bit write-tree" 20 "$B/write-tree"
t "git write-tree" 20 git write-tree
echo
echo "  --- single blob ---"
t "bit hash-object -w" 30 "$B/hash-object" -w docs/d0.md
t "git hash-object -w" 30 git hash-object -w docs/d0.md
echo
echo "  identical trees? bit(dir)=$H1"
echo "                   bit(list)=$H2"
echo "                   git=$H3"
[ "$H1" = "$H2" ] && [ "$H2" = "$H3" ] && echo "  YES" || echo "  NO -- results void"
