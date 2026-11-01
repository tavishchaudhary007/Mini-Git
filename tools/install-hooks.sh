#!/usr/bin/env bash
set -e

# Install git pre-push hook to run scripts/verify.sh before every git push
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

HOOKS_DIR="$(git rev-parse --git-path hooks 2>/dev/null || echo .git/hooks)"
mkdir -p "$HOOKS_DIR"

PRE_PUSH_HOOK="$HOOKS_DIR/pre-push"

cat > "$PRE_PUSH_HOOK" << 'EOF'
#!/usr/bin/env bash
set -e

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

echo "============================================================"
echo "          Mini-Git Pre-Push Safeguard Hook                  "
echo "============================================================"

if [ -f "$REPO_ROOT/scripts/verify.sh" ]; then
    bash "$REPO_ROOT/scripts/verify.sh"
    STATUS=$?
    if [ $STATUS -ne 0 ]; then
        echo "" >&2
        echo "[ERROR] Pre-push verification failed with status $STATUS." >&2
        echo "Push aborted. Fix all verification failures before pushing." >&2
        exit 1
    fi
else
    echo "[WARN] scripts/verify.sh not found; proceeding with push."
fi

exit 0
EOF

chmod +x "$PRE_PUSH_HOOK"

echo "Pre-push hook successfully installed to $PRE_PUSH_HOOK"
