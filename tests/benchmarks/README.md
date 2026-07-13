# Protocol Hot-Path Benchmarks

These benchmarks measure the reusable HTTP/1 server request-head parser,
HTTP/2 frame-header encode/decode, the HTTP/2 `feed()` PING fast path, and a
complete HTTP/2 request HEADERS decode/event/stream-cleanup cycle. They are
timing records, not CTest correctness gates: compare JSON only for the same
runner, compiler, architecture, configuration, and flags.

Configure an optimized HTTP-only build and run the aggregate target:

```powershell
cmake -S . -B build/bench -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DRUVIA_BUILD_CORE=OFF -DRUVIA_BUILD_WEB=OFF `
  -DRUVIA_BUILD_BENCHMARKS=ON
cmake --build build/bench --target ruvia_benchmarks
& ./build/bench/tests/benchmarks/ruvia_benchmark_http_hot_paths `
  --output build/bench/http-hot-paths.json
```

Use `--warmup-ms` and `--duration-ms` to control each benchmark interval. CI
uses a short run and uploads the JSON record; longer local runs reduce noise.
