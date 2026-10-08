# HTTP protocol library

[Documentation index](../README.md#contents)

## HTTP Protocol Library

Use `ruvia::http` when supplying your own transport. It parses and produces
protocol data; your driver provides I/O, clocks, cancellation, and QUIC crypto
callbacks. For a ready-to-run server or client, use `ruvia::web` instead.

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS http)
target_link_libraries(protocol_tool PRIVATE ruvia::http)
```

### Choose an entry point

| Protocol | Client | Server |
| --- | --- | --- |
| HTTP/1 | `Http1ClientRequestWriter`, `Http1ClientResponseParser`, `Http1ClientResponseBodyDecoder` | `Http1ServerRequestParser`, response head/stream plans |
| HTTP/2 | `Http2Connection::client()` | `Http2Connection::server()` |
| HTTP/3 | `Http3Connection` with client role | `Http3Connection` with server role |
| WebSocket | `Http1WebSocketClientHandshake` or `WebSocketClientNegotiation`, then `WebSocketConnection` | Handshake helpers, then `WebSocketConnection` |

Headers live under `ruvia/http/`. Start with
[Http2Connection.h](../ruvia-http/include/ruvia/http/Http2Connection.h),
[Http3Connection.h](../ruvia-http/include/ruvia/http/Http3Connection.h), or
[WebSocketConnection.h](../ruvia-http/include/ruvia/http/WebSocketConnection.h).

### Driving a connection

1. Configure the role, limits, and memory resource before processing input.
2. Feed received bytes and retain any unconsumed suffix.
3. Consume returned events and output plans. Keep borrowed buffers alive until
   the corresponding operation accepts or releases them.
4. Submit output to your transport and acknowledge actual accepted output;
   preserve ordering and honor backpressure.
5. Complete cancellation and pending transport work before releasing the owner.

HTTP/1 connections are reusable only after complete response framing/body
consumption. HTTP/2 DATA events carry flow-control credits: release them after
consuming their bytes. Retained events, credits, and PMR results require their
memory resource to remain alive.

For HTTP/3 `kQpackBlocked`, retain the unconsumed input and FIN, feed encoder-stream
bytes, then retry. Drain encoder/decoder output on their critical streams and
keep those streams open. Use `http3_buffered_response_cursor` and
`http3_critical_stream_output` to drive buffered responses and control output.

### Optional features

| Feature | API / configuration |
| --- | --- |
| QUIC v1/v2 | `quic_connection_config::version`, `preferred_version`. |
| Server push | HTTP/2 `enablePush`; HTTP/3 `prepareMaxPushId()`. |
| Priorities | `HttpPriority` and connection priority updates. |
| Extended CONNECT | Handshake helpers; requires the peer's enabling SETTINGS. |
| CONNECT-UDP / capsules | `HttpConnectUdp.h`, `HttpDatagram.h`. |
| Connection advertisements | `HttpConnectionAdvertisement.h`; authenticate advertised origins before use. |
| Dynamic QPACK | `Http3QpackConnection.h`, connection QPACK limits. |

Native QUIC datagrams require HTTP and QUIC negotiation; capsules provide a
reliable alternative. Protocol support does not automatically enable a Web
application feature.

### WebSocket

Complete and validate the handshake before exchanging frames. Set the connection
role and negotiated compression. Client-role connections require a
cryptographically random mask-key generator whose context outlives the connection.

`WebSocketClientNegotiation::validate_configuration(config_view)` checks a
borrowed configuration synchronously; constructing a negotiation owns its fields.
HTTP/1 handshake helpers additionally take a caller-generated random nonce.

The default message limit is 16 MiB, including decompressed content.
`max_buffered_input_bytes` separately bounds queued input. A `backpressured`
result consumes no input: drain events before retrying, and split oversized input
chunks. Preserve handshake owners while publishing their borrowed response parts.

### Message utilities

| Need | Header |
| --- | --- |
| Content-coding parsing and buffered codecs | `HttpContentCoding.h`, `HttpContentCodec.h` |
| Incremental compression | `HttpContentEncoder.h` |
| Transfer decoding / chunk framing | `HttpTransferCodingDecoder.h`, `Http1ChunkedBodyDecoder.h` |
| Multipart input | `MultipartParser.h` |
| SSE formatting | `Sse.h` |
| URL encoding | `UrlEncoding.h` |
| Streaming response preparation | `HttpResponseStream.h` |

Use bounded decode limits. `http_content_encoder::finish()` completes a stream;
encoder failure is terminal. Consume decoding output before reusing its scratch
buffer. URL `kForm` mode converts `+` to a space; `kPercent` leaves it literal.

`HttpRequest` strings and `*View` request types borrow their input. `HttpResponse`
headers set with `header_stable_view()` also borrow their strings. Keep those
sources unchanged until use completes. File responses describe payload ranges;
your transport performs the file I/O.
