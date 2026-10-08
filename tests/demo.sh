#!/bin/bash
# End-to-end demo / smoke test. Usage: bash tests/demo.sh  (run from project root after `make`)
set -e
MG="$(pwd)/minigit"
T=$(mktemp -d); cd "$T"
export MINIGIT_AUTHOR="Demo User"
run() { echo; echo "\$ minigit $*"; "$MG" "$@"; }

run init
printf 'hello\nworld\n' > a.txt
mkdir src && printf 'int main(){}\n' > src/b.txt
run status
run add .
run commit -m "Initial commit"
printf 'hello\nmini-git\nworld\n' > a.txt
run diff
run status
run add a.txt
run commit -m "Edit a.txt"
run branch feature
run checkout feature
printf 'feature work\n' > f.txt
run add f.txt
run commit -m "Add feature file"
run log --oneline
run html --no-open
run checkout main
ls
run merge feature
ls
run branch
echo; echo "--- error handling ---"
run add nofile.txt || true
run commit -m "again" || true
run checkout nope || true
run branch feature || true
