#!/bin/sh
# parity — prove bit writes objects byte-identical to git's.
#
# Runs bit ALONE in a fresh directory. No git command is allowed to mutate the
# index or refs during the run: an earlier harness interleaved `git add` with
# bit's calls, which rewrote .git/index underneath bit and produced a failure
# that looked like a bit defect. git is used only to READ and to compute
# reference hashes from content.
set -e
B=${B:-$(cd "$(dirname "$0")/.." && pwd)/build/bin}
T=$(mktemp -d /tmp/bit-parity.XXXXXX)
trap 'rm -rf "$T"' EXIT
cd "$T"

fail=0
ok()   { printf "  \033[32mPASS\033[0m  %s\n" "$1"; }
bad()  { printf "  \033[31mFAIL\033[0m  %s\n     bit: %s\n     git: %s\n" "$1" "$2" "$3"; fail=1; }
cmp2() { [ -n "$2" ] && [ "$2" = "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }

mkdir -p src/deep docs
printf 'hello\n'            > a.txt
printf 'nested content\n'   > src/deep/b.txt
printf '# doc\n'            > docs/c.md
head -c 4000 /dev/zero | tr '\0' 'x' > big.txt
chmod +x a.txt                        # exercise mode 100755 vs 100644

"$B/init" . >/dev/null

# 1. blob hashing, several shapes
for f in a.txt src/deep/b.txt docs/c.md big.txt; do
  cmp2 "hash-object $f" "$("$B/hash-object" "$f")" "$(git hash-object "$f")"
done

# 2. staging, then the tree git computes from bit's own index
"$B/add" a.txt src docs big.txt >/dev/null
cmp2 "write-tree (nested, mixed modes)" "$("$B/write-tree")" "$(git write-tree)"

# 3. staged set as git reads bit's index
n=$(git status --short | wc -l | tr -d ' ')
[ "$n" = "4" ] && ok "git reads bit's index ($n entries)" || bad "git reads bit's index" "$n" "4"

# 4. commit, then git must read it
GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t "$B/commit" -m "parity" >/dev/null
subj=$(git log --format=%s -1 2>/dev/null)
[ "$subj" = "parity" ] && ok "git log reads bit's commit" || bad "git log reads commit" "$subj" "parity"
git fsck >/dev/null 2>&1 && ok "git fsck exits 0" || bad "git fsck" "nonzero" "0"

# 5. bit reads back what it wrote
t=$("$B/cat-file" -t "$(git rev-parse HEAD)")
[ "$t" = "commit" ] && ok "bit cat-file -t on its own commit" || bad "cat-file -t" "$t" "commit"

# 6. deep nesting. A regression test: build_tree once held a 2.1 MB array on the
# stack per recursion level and overflowed at three levels. Every fixture here
# nested only two, so nothing caught it.
deep=d; n=1
while [ $n -lt 60 ]; do deep="$deep/l$n"; n=$((n+1)); done
mkdir -p "$deep" && printf 'bottom\n' > "$deep/f.txt"
"$B/add" d >/dev/null
cmp2 "write-tree at 60 levels" "$("$B/write-tree")" "$(git write-tree)"

# 7. bit's own log agrees with git's

cmp2 "bit log == git log" "$("$B/log" --oneline)" "$(git log --oneline)"

[ $fail = 0 ] && printf "\n  parity: ALL PASS\n" || printf "\n  parity: FAILURES\n"
exit $fail
