# Static files and compression

[Documentation index](../README.md#contents)

## Static Files and Compression

```cpp
ruvia::app()
    .listen({.address = "0.0.0.0", .http = 8080})
    .compression({})
    .documentRoot({
        .root = "public",
        .runtime = {.refreshInterval = std::chrono::milliseconds(500)},
        .precompressGzip = true,
    })
    .run();
```

The document root refreshes automatically (default: once per second) and requires
the blocking pool. A failed refresh keeps the last complete index; monitor
`App::httpStats().documentRootRefreshFailures`. Dotfiles are hidden by default;
use `StaticRootOptions::dotfiles = StaticDotfilePolicy::kServe` to publish them.

## Serving files

- `c.file()` serves a selected file without compression negotiation.
- `c.staticFile()` and the document-root fallback can select compressed variants.
- Fresh `.br`, `.gz`, and `.zst` sidecars take precedence over variants generated
  by `precompressBrotli`, `precompressGzip`, and `precompressZstd` during refresh.
- Conditional GET/HEAD and byte ranges are supported, including up to 16 ranges.

## Compression policy

Compression is off by default. `compression({})` enables negotiated gzip,
Brotli, and zstd; `compression(nullptr)` disables it.

| Buffered body size | Default behavior |
| --- | --- |
| Below `minBytes` (1 KiB) | Uncompressed. |
| Through `syncBytes` (64 KiB) | Compress on the worker. |
| Through `maxBytes` (64 MiB) | Compress on the blocking pool. |
| Larger | Uncompressed. |

A full blocking pool falls back to uncompressed output. Explicitly disabling
the pool makes eligible buffered compression synchronous. When no acceptable
representation is available and the client forbids identity, the response is
`406 Not Acceptable`.

Streams and SSE compress incrementally. Static files use prebuilt variants
only. Negotiated responses include `Vary: Accept-Encoding`; known codings in an
application-provided `Content-Encoding` must also be acceptable to the client.

See [files_static.cpp](../examples/web/files_static.cpp) and
[streaming.cpp](../examples/web/streaming.cpp) for complete examples.
