#!/bin/bash
# One-command setup: build Mini-Git, then create a demo repo with a first commit.
# Usage: bash setup.sh
set -e
cd "$(dirname "$0")"

echo "Building..."
g++ -std=c++17 -Iinclude src/*.cpp -o minigit
MG="$(pwd)/minigit"

DEMO="$(pwd)/../minigit-demo"
rm -rf "$DEMO" && mkdir "$DEMO" && cd "$DEMO"

echo "hello" > a.txt
"$MG" init
"$MG" add a.txt
"$MG" commit -m "first commit"
"$MG" log --oneline

echo
echo "Done. Demo repo: $DEMO"
echo "Run commands there with: $MG <command>"
