#!/usr/bin/env bash
# Mechanised layer-boundary checks.
# Authoritative rules: README.md and AGENTS.md
# Run from anywhere: resolves the repo root from its own location.
set -u
cd "$(dirname "$0")/.." || exit 2

fail=0
err() {
    echo "boundary-check FAIL: $1" >&2
    echo "$2" | sed 's/^/    /' >&2
    fail=1
}

SRC_GLOBS=(--include='*.h' --include='*.cpp' --include='*.inl')

REQUIRED_DIRS=(ruvia-core ruvia-http ruvia-web)
REQUIRED_DOCS=(README.md AGENTS.md)
for d in "${REQUIRED_DIRS[@]}"; do
    [ -d "$d" ] || { echo "boundary-check FAIL: missing target dir $d (checks would be vacuous)" >&2; exit 2; }
done
for f in "${REQUIRED_DOCS[@]}"; do
    [ -f "$f" ] || { echo "boundary-check FAIL: missing doc $f (doc checks would be vacuous)" >&2; exit 2; }
done

if [ "${1:-}" = "--self-test" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/ruvia-http/src"
    printf '#include <asio.hpp>\nint x;\n' > "$tmp/ruvia-http/src/planted.h"
    if grep -rnE '#include[[:space:]]*[<"]asio|\basio::' "$tmp/ruvia-http" "${SRC_GLOBS[@]}" >/dev/null; then
        echo "boundary self-test OK (asio in ruvia-http is detected)"
        exit 0
    fi
    echo "boundary self-test FAIL: planted asio include was NOT detected" >&2
    exit 1
fi

# 1. ruvia-http is core/asio-free -- everything, src/client/ included. The
#    client protocol/policy half lives there; its asio/TLS runtime driver lives
#    in ruvia-web/src/client.
hits=$(grep -rnE '#include[[:space:]]*[<"]asio|\basio::' ruvia-http "${SRC_GLOBS[@]}" || true)
[ -n "$hits" ] && err "ruvia-http must not reference asio" "$hits"

# 2a. ruvia-http must not include core or web framework headers. Public http
#     headers may forward declare facade types, but the target must not include core.
hits=$(grep -rnE '#include[[:space:]]*"ruvia/(app/|memory/|detail/|router/|http/Context\.(h|inl))' ruvia-http "${SRC_GLOBS[@]}" || true)
[ -n "$hits" ] && err "ruvia-http must not include core/web headers" "$hits"

# 2b. ruvia-http must not link ruvia-core.
hits=$(grep -nE 'ruvia::core|ruvia-core' ruvia-http/CMakeLists.txt || true)
[ -n "$hits" ] && err "ruvia-http must not link/name ruvia-core in CMake" "$hits"

# 3. Docs must not carry stale ownership.
DOCS=(README.md AGENTS.md)
hits=$(grep -n 'Http2ServerSession' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs reference the deleted coroutine h2 server session" "$hits"
hits=$(grep -nE 'ruvia-web[[:space:]]*->[[:space:]]*ruvia-http[[:space:]]*->[[:space:]]*ruvia-core|http[[:space:]]*->[[:space:]]*core|http.*asio/TLS runtime driver|http.*socket/TLS runtime driver' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs contain stale dependency/runtime ownership" "$hits"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "layer boundaries OK"
