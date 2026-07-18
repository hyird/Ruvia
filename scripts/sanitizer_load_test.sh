#!/usr/bin/env bash
# Drive the example server under a sanitizer with real concurrent load and fail
# on any sanitizer report. The unit-test sanitizer jobs (see .github/workflows)
# exercise correctness single- and lightly-multi-threaded; this covers the
# under-load path -- connection churn, HTTP/2 multiplexing, worker interleaving
# -- where memory-safety and concurrency bugs actually surface.
#
# Usage: scripts/sanitizer_load_test.sh <asan|tsan> [duration_seconds] [http|https]
#
# The https mode generates a throwaway self-signed cert and drives the TLS
# handshake path (connection churn + h2-over-TLS), which the http mode never
# touches.
#
# Requires a C++ toolchain, CMake+Ninja, VCPKG_ROOT, and a load generator
# (wrk and/or h2load). Missing load generators are reported, not silently
# skipped. ASan finds use-after-free / overflow; TSan finds data races. The
# tsan mode builds everything with clang and builds mimalloc itself with
# MI_DEBUG_TSAN (scripts/vcpkg-triplets/x64-linux-tsan.cmake), so the
# allocator's internal synchronization is visible to TSan and no suppression
# file is needed -- every report this script surfaces is a real finding.
set -euo pipefail

sanitizer="${1:-}"
duration="${2:-15}"
scheme="${3:-http}"
case "$sanitizer" in
    asan) flags="-fsanitize=address,undefined" ;;
    tsan) flags="-fsanitize=thread" ;;
    *)
        echo "usage: $0 <asan|tsan> [duration_seconds] [http|https]" >&2
        exit 2
        ;;
esac
case "$scheme" in
    http|https) ;;
    *)
        echo "usage: $0 <asan|tsan> [duration_seconds] [http|https]" >&2
        exit 2
        ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
build="$root/build-loadtest-$sanitizer"
port="${PORT:-18080}"
logdir="$(mktemp -d)"
trap 'rm -rf "$logdir"; [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null || true' EXIT

# TSan needs mimalloc compiled WITH TSan instrumentation (otherwise its
# internal thread-heap/meta/block reuse shows up as false races in consumer
# frames). mimalloc only accepts MI_DEBUG_TSAN under clang, so the tsan build
# uses clang end to end plus the x64-linux-tsan triplet, which rebuilds the
# mimalloc port with MI_DEBUG_TSAN=ON.
extra_configure=()
if [ "$sanitizer" = "tsan" ]; then
    command -v clang++-19 >/dev/null 2>&1 || { echo "tsan mode needs clang-19" >&2; exit 1; }
    extra_configure=(
        -DCMAKE_C_COMPILER=clang-19
        -DCMAKE_CXX_COMPILER=clang++-19
        -DVCPKG_TARGET_TRIPLET=x64-linux-tsan
        -DVCPKG_OVERLAY_TRIPLETS="$root/scripts/vcpkg-triplets"
    )
fi

echo "== configuring $sanitizer build =="
cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DRUVIA_BUILD_TESTS=OFF \
    -DRUVIA_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_FLAGS="$flags -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags" \
    "${extra_configure[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

echo "== building bench_server =="
cmake --build "$build" --target ruvia_example_bench_server

server="$build/examples/ruvia_example_bench_server"
if [ "$sanitizer" = "asan" ]; then
    export ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0:log_path=$logdir/san"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$logdir/san"
else
    # mimalloc is TSan-instrumented in this build (see the x64-linux-tsan
    # triplet), so no suppressions: every report is a real race.
    export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:log_path=$logdir/san"
fi

# https drives the TLS handshake path -- the highest-churn TLS surface -- with a
# throwaway self-signed cert. bench_server switches to TLS when TLS_CERT/TLS_KEY
# are set; wrk and h2load talk to it over ALPN (h2 preferred).
tls_env=()
curl_insecure=()
if [ "$scheme" = "https" ]; then
    command -v openssl >/dev/null 2>&1 || { echo "https mode needs openssl" >&2; exit 1; }
    openssl req -x509 -newkey rsa:2048 -keyout "$logdir/key.pem" \
        -out "$logdir/cert.pem" -days 1 -nodes -subj "/CN=localhost" >/dev/null 2>&1
    tls_env=(TLS_CERT="$logdir/cert.pem" TLS_KEY="$logdir/key.pem")
    curl_insecure=(-k)
fi

echo "== starting $scheme server on :$port =="
env PORT="$port" "${tls_env[@]}" "$server" &
server_pid=$!
sleep 3
if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "server exited during startup" >&2
    exit 1
fi

base="$scheme://127.0.0.1:$port"
ran_any=0
if command -v wrk >/dev/null 2>&1; then
    ran_any=1
    echo "== wrk: keepalive GET / =="
    wrk -t4 -c128 -d"${duration}s" "$base/" || true
    echo "== wrk: connection churn (Connection: close) =="
    wrk -t4 -c128 -d"${duration}s" -H 'Connection: close' "$base/users/42" || true
else
    echo "wrk not found -- skipping HTTP/1.1 load" >&2
fi
if command -v h2load >/dev/null 2>&1; then
    ran_any=1
    echo "== h2load: HTTP/2 multiplexed =="
    h2load -n$((duration * 4000)) -c50 -m20 "$base/api/status" || true
else
    echo "h2load not found -- skipping HTTP/2 load" >&2
fi
if [ "$ran_any" -eq 0 ]; then
    echo "no load generator available (need wrk or h2load)" >&2
    exit 1
fi

# The server must still be serving, and no sanitizer report may have been logged.
if ! curl -fsS "${curl_insecure[@]}" -m 5 "$base/api/status" >/dev/null 2>&1; then
    echo "FAIL: server unresponsive after load (check $logdir)" >&2
    kill "$server_pid" 2>/dev/null || true
    exit 1
fi
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=""

if compgen -G "$logdir/san.*" >/dev/null; then
    echo "FAIL: sanitizer reported errors under load:" >&2
    cat "$logdir"/san.* >&2
    exit 1
fi
echo "PASS: $sanitizer clean under ${duration}s of concurrent load"
