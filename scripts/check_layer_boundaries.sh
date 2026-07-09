#!/usr/bin/env bash
# Mechanised layer-boundary checks.
# Authoritative rules: docs/superpowers/specs/2026-07-08-ruvia-layer-boundaries.md
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

# Preflight: fail CLOSED. Every rule below is `grep || true`, so a renamed/missing
# target directory or doc would make its check pass vacuously -- assert the inputs
# exist first so the guard cannot silently degrade to a no-op after a reorg.
REQUIRED_DIRS=(ruvia-core ruvia-http ruvia-web ruvia-edge)
REQUIRED_DOCS=(README.md AGENTS.md docs/superpowers/specs/2026-07-08-ruvia-layer-boundaries.md)
for d in "${REQUIRED_DIRS[@]}"; do
    [ -d "$d" ] || { echo "boundary-check FAIL: missing target dir $d (checks would be vacuous)" >&2; exit 2; }
done
for f in "${REQUIRED_DOCS[@]}"; do
    [ -f "$f" ] || { echo "boundary-check FAIL: missing doc $f (doc checks would be vacuous)" >&2; exit 2; }
done

# Self-test mode: plant a fixture that violates rule 1 (an asio include under a fake
# ruvia-http tree) and assert the asio grep fires, proving the check is not vacuous.
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

# 1. ruvia-http is asio-free -- EVERYTHING, src/client/ included (the client's pure
#    protocol/policy half lives there; its asio runtime driver is ruvia-web/src/client).
#    Covers Http2Connection.h / WsConnection.h / Http1Connection.h transitively.
hits=$(grep -rnE '#include[[:space:]]*[<"]asio|\basio::' ruvia-http "${SRC_GLOBS[@]}" || true)
[ -n "$hits" ] && err "ruvia-http must not reference asio" "$hits"

# 2. ruvia-http must not reach the web framework. Core's ruvia/app/Task.h and
#    ruvia/app/detail/ are runtime (allowed); everything else under ruvia/app/,
#    all of ruvia/router/, and Context.h are web-owned.
hits=$(grep -rnE '#include[[:space:]]*"ruvia/(router/|http/Context\.(h|inl)|app/)' ruvia-http "${SRC_GLOBS[@]}" \
    | grep -vE 'ruvia/app/(Task\.h|detail/)' || true)
[ -n "$hits" ] && err "ruvia-http must not include web framework headers" "$hits"

# 3a. ruvia-edge must not link web.
hits=$(grep -nE 'ruvia(-|::)?web' ruvia-edge/CMakeLists.txt || true)
[ -n "$hits" ] && err "ruvia-edge must not link/name ruvia-web in CMake" "$hits"

# 3b. ruvia-edge sources must not include web framework headers.
hits=$(grep -rnE '#include[[:space:]]*"(ruvia/router/|ruvia/http/Context\.(h|inl)|ruvia/app/App\.h|router/|http/ContextServices\.h|app/AppAccess\.h)' ruvia-edge "${SRC_GLOBS[@]}" || true)
[ -n "$hits" ] && err "ruvia-edge must not include web framework headers" "$hits"

# 4. Docs must not carry stale ownership: no coroutine h2 server session by name, no
#    client attributed to ruvia-http.
DOCS=(README.md AGENTS.md docs/superpowers/specs/2026-07-08-ruvia-layer-boundaries.md)
hits=$(grep -n 'Http2ServerSession' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs reference the deleted coroutine h2 server session" "$hits"
hits=$(grep -nE 'client.*(belongs to|归属)[^。.]*web|HttpClient\.h[^。.]*(installed by[^.。]*web|由[^。.]*web[^。.]*安装)' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs attribute the outbound client's ownership to web (it is http-owned; only the runtime driver lives in ruvia-web/src/client)" "$hits"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "layer boundaries OK"
