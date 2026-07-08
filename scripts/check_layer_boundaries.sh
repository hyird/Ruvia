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

# 1. ruvia-http is asio-free (covers Http2Connection.h / WsConnection.h transitively:
#    the whole target carries no asio include or symbol).
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
hits=$(grep -n 'ruvia-http/src/client' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs claim a client under ruvia-http/src/client" "$hits"
hits=$(grep -nE 'client belongs to .ruvia-http|client 固定归属 .?ruvia-http' "${DOCS[@]}" || true)
[ -n "$hits" ] && err "docs claim the client runtime belongs to ruvia-http" "$hits"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "layer boundaries OK"
