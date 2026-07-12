# Ruvia

Ruvia is a small C++23 HTTP/Web framework with explicit library boundaries. The repository is a monorepo, but the reusable core and HTTP layers are first-class CMake targets that can be consumed without the full web framework.

## Targets

| Directory | Target | Public alias | Purpose |
| --- | --- | --- | --- |
| `ruvia-core/` | `ruvia-core` | `ruvia::core` | Runtime foundation: coroutine task type, Asio integration glue, PMR memory resources, mimalloc integration, and small runtime helpers. |
| `ruvia-http/` | `ruvia-http` | `ruvia::http` | Pure sans-I/O protocol library (zero core/asio/socket runtime): HTTP message types, header token/value helpers, HTTP/1 parser + connection core, HTTP/2 connection core (server + client roles), WebSocket protocol core, HPACK, request/response body framing and HTTP/2 stream state, multipart/SSE/content-encoding protocol helpers, cookie/cache/range/conditional/negotiation helpers, and the outbound client's protocol core. |
| `ruvia-web/` | `ruvia-web` | `ruvia::web` | The full server-side web framework: App, Context, Controller, Router, middleware, model/validation, server I/O over the HTTP cores (Asio, TLS/ALPN, timeouts, streaming, WebSocket routes, and file read buffers), plus application policies such as session, CSRF/JWT, CORS, security headers, rate limits, static roots with MIME/validator metadata, AutoHTTPS redirect, DB, and Redis. |

Dependency direction:

```text
ruvia-web  -> ruvia-core + ruvia-http
```

## Layer Boundary

The boundary is decided by who owns the behavior, not by whether a file touches HTTP header names.

- `ruvia-core` owns the runtime foundation: coroutine task machinery, Asio awaiter/driver glue, PMR resources, worker memory, connection scanning, and small runtime helpers.
- `ruvia-http` owns HTTP itself and must not depend on `ruvia-core`: wire bytes, message shape, header syntax helpers, parser/framing rules, connection persistence, `Expect: 100-continue`, upgrade handshakes, HTTP/2 frames/settings/flow control, WebSocket frames, multipart/SSE/content-encoding protocol logic, and allocation-free `HttpProtocolError` signals.
- `ruvia-web` owns the application framework built on HTTP: route dispatch, middleware, controllers, `Context`, model/validation JSON serialization, `HttpError`/JSON application error responses, sessions, CSRF, JWT integration, CORS policy, security-header policy, rate limits, static-file indexing, AutoHTTPS redirect, DB/Redis integrations, and the socket/TLS/Asio runtime drivers that drive `ruvia-http`.

If code decides how bytes are parsed, framed, serialized, kept alive, upgraded, or rejected by the HTTP/WebSocket/HTTP2 protocols, it belongs in `ruvia-http`. If code decides what the application product does with those protocol facts, it belongs in `ruvia-web`.

Protocol primitives report status plus a static diagnostic through `HttpProtocolError`.
`ruvia-web` translates that signal into its `HttpErrorInfo`/JSON envelope. Router and
custom error handlers never set HTTP/1 connection persistence; the server runtime applies
`Connection: close` only after it knows the request-body and keep-alive state.
The HTTP/1 parser first returns one immutable `Http1ServerConnectionPlan`, which binds the exact
`HttpProtocolVersion` (`kHttp10` or `kHttp11`) directly to `Http1ConnectionDisposition`.
Version-specific `http1PlanHttp10RequestConnection()` and
`http1PlanHttp11RequestConnection()` factories cannot accidentally admit HTTP/2; the named
`http11Close()` fallback is reserved for errors produced before a valid request version exists.
Buffered, body-reader, streaming, and WebSocket-failure paths carry or tighten the parsed plan.
Typed `Http1RequestBodyConsumption`, request-limit close policy, and an application
`Connection: close` may only call `requireClose()`, which preserves the exact version; the former
lossy `Http1ResponseConnectionSignal`, `responseSignal()`, and generic version factory do not
exist.
Repeated `Connection` field lines are evaluated as one list: any `close` token tightens the
plan and the finalizer collapses contradictory fields to one `Connection: close`; an HTTP/1.0
reuse verdict adds `keep-alive` even when another connection option was already present.
The response finalizer consumes that exact version, and every `Http1ResponseHeadPlan` owns it
through status-line serialization: an HTTP/1.0 request produces an `HTTP/1.0` response line while
HTTP/1.1 remains `HTTP/1.1`, following
[RFC 9110 Section 2.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.5).
For streaming responses, `ruvia-http` first returns an `Http1ResponseStreamPlan` that binds
the request connection plan, version/body state, candidate chunked versus close-delimited framing,
and a typed external close policy. At head commit, `PreparedHttp1ResponseStream` also folds in
the response method/status and `Connection` options, canonicalizes the Connection field, and returns
the authoritative connection disposition. It also carries one exclusive `Http1ResponseHeadPlan`:
`Http1BufferedResponseHead`, `Http1ChunkedResponseStreamHead`, or
`Http1CloseDelimitedResponseStreamHead`. `appendResponseHead()` accepts only that plan; the former
policy plus `suppressAutoContentLength` boolean entry no longer exists. Buffered output is passed as
one `Http1BufferedResponsePlan`, inseparably pairing `HttpBufferedResponseWritePlan` with the head
plan; `Http1BufferedResponseHead` owns the same canonical representation length used to decide body
I/O. `HttpResponseBodyPlan` also owns the exact numeric response status from which its write policy
and content semantics were derived, and `HttpBufferedResponseWritePlan` exposes that same status.
The only buffered-plan factory accepts the request method plus the response; the former loose
`bodyPlan + response` overload is gone. The body plan also retains the exact `HttpKnownMethod`
provenance used to derive its semantics. HTTP/1 rejects a response whose mutable status or
representation length no longer matches the prepared plan, while the HTTP/2 head planner reports
typed status/representation mismatches before HPACK or stream mutation. Web's
`prepareBufferedHttpResponse()` creates one plan only after materialization, compression, and CORS;
HTTP/1 and HTTP/2 consume that same snapshot.
`Http2Connection::submitResponseHead()` therefore requires the prepared plan and returns
`kResponsePlanMismatch` if its method, status, or representation length disagrees with the live
stream/response—the former two-argument entry and hidden core re-planning are gone. Web therefore
cannot reconstruct status, version, or length from loose inputs; the former standalone
`http1BufferedResponseHeadPlan()` factory does not exist. The writer is the sole owner
of canonical `Transfer-Encoding: chunked`, replacing any application framing declaration. A
body-open close-delimited response filters both application `Transfer-Encoding` and
`Content-Length`; a body-suppressed HEAD/304 response may retain representation-length metadata but
still never sends Transfer-Encoding to an HTTP/1.0 peer. These rules follow
[RFC 9112 Section 6.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1) and the ordered
message-length rules in
[Section 6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3). Thus an HTTP/1.0 body-allowed stream remains
close-delimited, while a HEAD/204/205/304 response on an opted-in persistent connection is
self-delimited and can be reused. The committed sink returns the complete connection plan, so
its socket disposition cannot drift away from the version and Connection field already emitted.
Streaming termination is also one HTTP-owned contract. `ResponseStreamCommitPlan` binds the exact
numeric response status and selected framing to the method-derived body plan, post-head phase
(`body-open`, `trailers-only`, or `message-ended`), and available trailer framing. The prepared
head rejects a plan for a different status, and the Web sink retains that one committed plan
instead of decomposing it into parallel flags. Application code submits an optional complete
trailer section only through `ResponseStreamWriter::end(std::span<const HttpHeaderView>)`; there is
no incremental `addTrailer()` side channel. HTTP/1 accepts that section only for a body-allowed chunked response,
so HTTP/1.0 close-delimited responses and HEAD/1xx/204/304 reject it instead of dropping it.
HTTP/2 can keep a content-forbidden response open in the explicit
`Http2LocalResponseTrailersOnly` alternative of `Http2LocalSendState` and terminate it with
trailing HEADERS; it never
becomes DATA-open or falls back to empty DATA.
`Http2Connection::finishResponse(streamId, trailers)` explicitly receives the complete terminal
section (possibly empty; trailers-only requires one) and performs validation, detached HPACK
encoding, ordering behind blocked DATA, and `END_STREAM` as one atomic transaction. Its typed
`Http2FinishSubmitStatus` distinguishes closed/wrong-phase, `kInvalidTrailerSection`, and
incomplete-length outcomes without mutating output or stream state. The former
`submitResponseTrailerSection()`/`Http2ResponseTrailerSubmitStatus` staging API and the per-stream
trailer block are gone; a blocked terminal section moves directly into core-owned pending DATA.
Before an initial head is committed, the Web sink uses HTTP's shared
`responseTrailerSectionValid()` preflight so invalid application metadata remains a pre-commit
failure; the core repeats the defensive check at its final mutation boundary without Web
reimplementing any field rule.
Router and runtime completion are discriminated too: handled routes own no dummy `HttpResponse`,
buffered fallbacks own the response, committed/failed streams own the status from the commit plan,
and a peer abort before commit owns no fictitious status. Consequently HTTP/1 close-delimited
streams are logged before the socket closes, and HTTP/1 or HTTP/2 custom streaming statuses cannot
be reconstructed as the former default 200. Automatic stream termination runs inside the route
coroutine while its bound `Context` is still alive; the outer transport driver only consumes the
committed plan and never dereferences a retained Context pointer after route completion.
Buffered HTTP/1 writes now have the same explicit commit boundary.
`Http1BufferedResponseWriteResult` is exactly one of completed, failed-before-commit, or
failed-after-commit. The writer uses the composed operation's cumulative byte count: only a prefix
that contains the complete serialized response head commits an HTTP status. Completed and
post-commit failure carry the status from the exact `Http1BufferedResponsePlan`; a partial head
carries only its transport error and cannot produce a response-completion access-log record. The
former `Task<void>` plus `std::error_code&` side channel and the later
`HttpResponse::status()` reconstruction are gone.
Buffered HTTP/2 completion follows the same rule. Every valid buffered branch—including early
417/429 application responses—uses one preparation/submission/logging path. Its
`Http2BufferedResponseDispatchResult` distinguishes completed, peer-aborted-before-commit,
peer-aborted-after-commit, failed-before-commit, and failed-after-commit alternatives. Only
post-commit alternatives carry the status from the submitted `HttpBufferedResponseWritePlan`;
an invalid final head or peer reset before HEADERS commit has no HTTP status and cannot trigger a
response-completion access-log record.
Request methods have two deliberately separate representations. RFC 9110 defines the wire method
as an extensible, case-sensitive token, so `HttpRequest::method()`, `ContextRequest::method()`,
`RawRequestClone::method()`, and `AccessLogRecord::method()` preserve that exact token for both
HTTP/1 and HTTP/2. `HttpKnownMethod` and the corresponding `knownMethod()` accessors are only the
framework's fixed routing/body-policy classification; `classifyHttpMethod()` does not validate
syntax, while `isValidHttpMethodToken()` does not require a known classification. Consequently, a
valid extension such as `PROPFIND` (and a differently cased token such as `get`) is neither an
HTTP/1 parse failure nor an HTTP/2 stream protocol error. If no fixed framework semantic exists,
`ruvia-web` renders 501 through the normal error handler; an actually malformed token remains a
protocol failure. RFC 8441 Extended CONNECT likewise remains `CONNECT` in the request, clone, and
access log. Only WebSocket route lookup uses `Http2RequestBuilder::routeMethod()` to select an
existing GET route; it never rewrites the wire method stored in the request.
The public `Http1RequestParser` is explicitly an HTTP/1 whole-message, zero-copy scanner. Its
`Http1RequestParseResult` is discriminated: `Http1RequestNeedMore` alone carries an optional exact
`requiredTotalBytes()` for a partial Content-Length message, `Http1ParsedRequest` alone carries the
borrowed request, its immutable body plan, the exact framed `wireBody()`, and the first message's
consumed length, while `Http1RequestParseFailure` alone carries a real protocol error rather than a
`kNone` sentinel. A known required total is likewise nonzero. A chunked success
retains the complete chunk-size/data/delimiter/trailer section for the shared decoder instead of
returning an empty body, and no request can be read from need-more or failure states. The reusable
`Http1ServerRequestParseState` also names the runtime boundary explicitly:
`kRequestHeadReady` is the only phase from which Web may dispatch a route, whereas
`kRequestMessageReady` means the complete framed request was scanned. `kNeedRequestHead` and
`kNeedRequestBody` remain distinct, `messageBytes` is valid only for message-ready, and optional
`requiredTotalBytes` belongs only to a fixed-length body that needs more input. The old generic
`HttpParseStatus` tuple is not part of the API, so head completion cannot masquerade as message
completion and buffer growth cannot interpret a completed-message boundary as future capacity.
The parser also emits one immutable `Http1RequestBodyPlan` for every request. It is the only
contract passed to buffered readers, streaming readers, and WebSocket gates. Its framing is a
variant of `Http1RequestWithoutBody`, `Http1KnownLengthRequestBody`, or
`Http1ChunkedRequestBody`; absent framing remains distinct from an explicit `Content-Length: 0`.
Only the known-length alternative exposes `contentLength()`, and only the chunked alternative
exposes the preceding transfer-coding order. Expectations are common plan metadata and derive one
typed action from whether the active alternative actually requires consumption. Construction is
private to `Http1ServerRequestParser`, so neither Web nor an installed consumer can inject an
unchecked coding count or synthesize a lookalike plan. Web readers dispatch on the active
alternative instead of reading fake zero/empty payload from an unrelated mode. This directly
models the ordered request framing rules in
[RFC 9112 Section 6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3).

Ruvia accepts final `chunked` and one supported `gzip` or
`deflate` coding before it (for example `gzip, chunked`), de-chunks before incrementally
decoding that coding, maps an unsupported coding to 501, and rejects Transfer-Encoding on
HTTP/1.0.
Inbound Expect semantics are shared across protocol versions by the fixed-size
`HttpRequestExpectations` state. It incrementally parses the RFC 9110 `#expectation` list across
repeated field lines, ignores recipient-side empty members, recognizes case-insensitive
`100-continue`, and preserves any extension member as one `kUnsupported` fact. A syntactically
valid extension is no longer an `HttpParseError::kExpectationFailed` or an HTTP/2 stream error:
the pure parser reports `HttpServerExpectationAction`, while `ruvia-web` deliberately chooses a
417 through its normal error handler. HTTP/1.0 suppresses the continue action. When request
content will follow, the HTTP/1 body reader drives `Http1InterimResponseWriter`; the HTTP/2
session immediately drives `Http2Connection::submitInterimResponseHead()` before waiting for
DATA, so a conforming client cannot deadlock while withholding content.
Protocol version is likewise one typed message-control contract. `HttpProtocolVersion`
contains `kHttp10`, `kHttp11`, and `kHttp2`; both inbound `HttpRequest` and owning
`HttpClientResponse` expose it through `protocolVersion()`. The HTTP/1 parsers validate the
case-sensitive start-line token and convert it once, while the HTTP/2 builder records `kHttp2`
from the connection protocol instead of inventing a borrowed `"HTTP/2"` wire value. Connection,
stream-framing, WebSocket, and final-response control consume the same enum. The former
`HttpRequest::httpVersion()` string view and private `HttpResponseProtocolVersion` duplicate are
removed, so no layer can compare an arbitrary or dangling version spelling.

Access logging consumes that same message value. `AccessLogRecord` borrows the one immutable
`HttpRequest` for the duration of the callback and derives `method()`, `knownMethod()`, `path()`, and
`protocolVersion()` from it. `recordHttpAccess()` therefore accepts no transport/version flag: an
HTTP/1.0 request remains distinguishable from HTTP/1.1, HTTP/2 remains `kHttp2`, and log metadata
cannot disagree with the request that was actually parsed. The former `http2()` boolean and copied
request-field tuple are removed without adding allocation or type-erasure.
For streaming responses, its status is taken from the same `ResponseStreamCommitPlan` that emitted
the final head; a pre-commit peer abort has no final response and therefore does not invoke this
response-completion hook with an invented status.
Buffered HTTP/1 records likewise consume `Http1BufferedResponseWriteResult`: a complete head or a
later body failure has the plan's committed status, while a transport failure during a partial head
has none.

Buffered response storage is exclusive too. `HttpResponseBody` contains exactly one
`HttpEmptyResponseBody`, `HttpBorrowedResponseBytes`, `HttpStaticResponseBytes`,
`HttpOwnedResponseBytes`, `HttpOwnedResponseFile`, or `HttpBorrowedResponseFile`. Owned byte and
path alternatives use the response's PMR resource; borrowed alternatives retain only their view.
The sole read boundary is `responseBody(response)`: its `bytes()`, `file()`, and `size()` are derived
from the active alternative, and the non-default `ResponseFileBody` view can only be produced by a
file alternative. A zero-length file therefore remains distinct from an empty body. The former
`BodyKind` plus simultaneous string/view/optional-file storage and the
`responseHasFileBody()` + `responseFileBody()` and `responseBodyBytes()`/`responseBodySize()` side
channels are removed. This reduces resident response state without adding allocation, virtual
dispatch, or request-time type erasure.

Outbound HTTP/1 request emission is now the matching public sans-I/O boundary.
`HttpClientRequestContent::none()` is distinct from `bytes("")`: only the latter represents an
explicit empty content message and causes `Content-Length: 0`. The value is a discriminated union of
`HttpClientRequestWithoutContent` and `HttpClientRequestBytes`; only the bytes alternative exposes
`value()`, so an absent request cannot be mistaken for borrowed empty bytes. This follows the
ordered request-framing rules in
[RFC 9112 Section 6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), where an absent
Transfer-Encoding and Content-Length ends a request at the head while a present Content-Length
defines its body length. `Http1ClientRequestWriter` validates
the exact, case-sensitive method, direct-origin request target, every field, header count, and the
method-specific TRACE/OPTIONS constraints before touching a caller-provided head buffer. It emits
Host first from the typed `HttpOrigin` and owns the sole generated Content-Length, optional
`Connection: close`, and `Expect: 100-continue`. `Http1ClientRequestWirePolicy` selects no
expectation or the 100-continue handshake; a caller-supplied Host, Content-Length,
Transfer-Encoding, Trailer, or Expect field is rejected instead of reconciling parallel sources.
Caller-supplied connection fields are one typed contract as well: sender-side `Connection` and
`Upgrade` lists reject empty or malformed members, and an `Upgrade` or `TE` field is rejected
unless the logical repeated `Connection` list contains the corresponding option.
`prepareConnect()` is a dedicated entry: it derives an authority-form target with an explicit
nonzero port while generating the corresponding Host authority, so a regular origin-form request
cannot accidentally masquerade as CONNECT.

Preparation returns buffer-too-small, `PreparedHttp1ClientRequest`, or typed failure without
allocation or partial output. The prepared alternative exposes the head plus one immutable
`Http1ClientRequestContentPlan`. Its exclusive alternatives are
`Http1ClientRequestWithoutContent`, `Http1ClientImmediateRequestContent` (including explicit empty
content), and `Http1ClientContinueGatedRequestContent`; only the latter two expose `bytes()`.
A continue-gated plan requires a separate head write; the external runtime may
release its borrowed content after 100 Continue or its own finite wait policy, without rescanning
Expect. `Http1ClientResponseParser` can only be constructed from that Prepared value, so method,
Upgrade offer, Expect gate, and effective close signal cannot be replaced with caller-reconstructed
lookalike facts. It derives the Expect exchange state from the continue-gated alternative; the
private response context carries no duplicate `expectsContinue` flag.

Outbound HTTP/1 response-head parsing is a public sans-I/O boundary. One stateful
`Http1ClientResponseParser` is bound to one Prepared request and remains in use across every
informational response until the final response, tunnel, or upgrade. An external runtime gives it
the growing buffer including the terminal CRLF CRLF and receives one
discriminated `Http1ClientResponseParseResult`: `Http1ClientResponseNeedMore`, an owning parsed
head, or `Http1ClientResponseParseFailure`. A parsed head owns its `HttpClientResponse` header
storage through PMR and reports the exact head boundary through `consumedBytes()`, leaving all
following bytes for the body, tunnel, upgraded protocol, or next informational/final head driver.
Protocol failures are typed and allocation-free; the owning response is allocated only after the
whole head and its framing plan validate, so there is no partially mutated response out-parameter
and no exception for malformed wire input (resource exhaustion while materializing success can
still throw).

That success alternative carries one immutable, discriminated `Http1ClientResponsePlan`, not a
mode/length/codings/connection tuple. Its mutually exclusive alternatives are
`Http1ClientInformationalResponse`, `Http1ClientResponseWithoutContent`,
`Http1ClientKnownLengthResponse`, `Http1ClientChunkedResponse`,
`Http1ClientCloseDelimitedResponse`, `Http1ClientConnectTunnel`, and
`Http1ClientProtocolUpgrade`. Only the known-length alternative exposes `contentLength()`; only
chunked and close-delimited alternatives expose transfer-decoding order; and only self-delimited
final responses expose `Http1ClientResponsePersistence`. Close-delimited always consumes through
EOF and closes, while informational, tunnel, and upgrade alternatives cannot be mistaken for an
HTTP response body or a poolable final response. The parser constructs this alternative directly,
without a mutable intermediate framing tuple.

This mapping follows the ordered response-length rules in
[RFC 9112 Section 6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), including the
method/status no-content precedence, immediate successful-CONNECT tunnel transition, final
chunked framing, and close delimiting. Connection reuse additionally follows
[RFC 9112 Section 9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3): a reusable client
must consume the complete self-delimited response. For a continue-gated request the plan also
emits one `Http1ClientRequestContentSignal`: none for unrelated informational responses, continue
for 100, and exchange-complete when a final response or 101 means pending content must no longer
be started. Consequently, a body-allowed response without Content-Length or final chunked is
always close-delimited; HTTP/1.0 defaults to close unless its response opts into `keep-alive`; and
a 2xx CONNECT switches to an
explicit tunnel alternative instead of being mistaken for a response body. A valid
`101 Switching Protocols` likewise produces a protocol-upgrade alternative only when the actual
request and response both carry `Connection: Upgrade`, the selected `Upgrade` protocol was offered
by that request, and the 101
does not carry Content-Length or Transfer-Encoding. Protocol names compare case-insensitively,
while protocol versions remain exact tokens. When Upgrade and 100-continue are both present, the
stateful parser rejects a 101 that was not preceded by 100 Continue. A content-bearing request also
requires the external runtime to acknowledge its completed write through
`completeRequestContent()` before 101 can be accepted; the returned
`Http1ClientRequestContentCompletionStatus` keeps duplicate and terminal notifications explicit.
This prevents switching protocols in the middle of the request message, as required by RFC 9110. A
parser becomes terminal after a final response or protocol failure, preventing a second response
from being interpreted as part of the same exchange. Redirect planning
uses the same exact method-token comparison. In accordance with RFC 9110, 301/302 may rewrite only
POST to GET, 303 selects GET while preserving HEAD, and 307/308 retain the method; the typed plan
separately tells the I/O owner whether representation bytes and content-specific fields must be
dropped. Although RFC 9110 forbids a server from generating content in a 205 response, RFC 9112
does not place 205 in the HEAD/1xx/204/304 header-terminated framing class: the client plan still
consumes its declared zero-length/chunked framing, and an unframed 205 is close-delimited, before
the external runtime can safely reuse the connection.
Request and response parsers share `HttpContentLengthState`: a comma-combined field such as
`Content-Length: 5, 5` is accepted only after every decimal member is parsed and found equal,
while any differing or malformed member rejects the whole message. They also share
`HttpTransferEncodingState`, which preserves list order across repeated field lines, requires
chunked to be final when present, and rejects parameters on gzip/deflate/chunked coding names;
chunk extensions remain a separate grammar on body chunk-size lines.
HTTP/1 chunked framing has one protocol-owned implementation and two typed consumption views.
The incremental `Http1ChunkedBodyDecoder` returns a zero-allocation `std::variant` containing
exactly one of `Http1ChunkDecodeNeedMore`, `Http1ChunkDecodeBodyChunk`, or
`Http1ChunkDecodeComplete`. Every alternative owns the input prefix consumed by that call, but
only `Http1ChunkDecodeBodyChunk` exposes borrowed representation bytes. The Web runtime may refill,
deliver, or finish only by driving those accessors; it does not inspect size lines, delimiters, or
trailers itself. The whole-message scanner similarly returns `HttpChunkScanNeedMore`,
`HttpChunkScanComplete`, or `HttpChunkScanFailure`: only complete owns the final consumed message
boundary, and only failure owns the typed chunk error.

Completion follows
[RFC 9112 Section 7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1): a zero-size last
chunk, the optional trailer section, and its terminating empty line are all required before either
decoder reports complete. An input that ends before that boundary remains need-more rather than
becoming an implicitly complete body, matching the incomplete-message rules in
[RFC 9112 Section 8](https://www.rfc-editor.org/rfc/rfc9112.html#section-8). The former generic
framer/event tuple is intentionally absent, so need-more and failure cannot accidentally expose a
plausible whole-message consumption boundary.
All HTTP/1 connection-control consumers also share `HttpConnectionOptions` and
`HttpUpgradeProtocols`. Repeated field lines extend one allocation-free state; sender paths reject
empty members, while recipient paths ignore empty members within the existing bounded header
budget as required by RFC 9110. The server request parser, client writer, client response parser,
and server response finalizer therefore cannot disagree about close, keep-alive, Upgrade, or TE.
The response finalizer supplies a missing `Connection: Upgrade` option and, when the authoritative
plan closes the connection, emits one `Connection: close, Upgrade` field so a retained Upgrade
advertisement never loses its required hop-by-hop marker.
HTTP/1 WebSocket validation derives Connection, every Upgrade offer, and duplicate handshake
fields from one parsed `HttpRequest`; it no longer accepts a separate flags object that could
describe different wire bytes or rely on the single-value known-header cache.
The server handshake is also negotiated exactly once. The immutable HTTP-owned
`WebSocketServerNegotiation` binds the selected subprotocol to one exclusive
`WebSocketDeflateNegotiation` alternative: disabled, accepted, or accepted while echoing
`server_max_window_bits=15`. HTTP/1 serializes its 101 response from that value. RFC 8441 passes the
same value into `Http2Connection::submitWebSocketHandshake()`, whose submitted alternative alone
exposes the negotiation committed to the 200 response; the subsequent `WsConnection` is configured
from that committed alternative. There is no H1 compression `bool&`, H2 `subprotocol + extensions`
tuple, or second request scan that can make response metadata disagree with RSV1 handling. This
follows [RFC 6455 Section 4.2.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-4.2.2),
[RFC 7692 Section 7.1](https://www.rfc-editor.org/rfc/rfc7692.html#section-7.1), and
[RFC 8441 Section 5](https://www.rfc-editor.org/rfc/rfc8441.html#section-5).
WebSocket close handling follows the same single-owner rule. `ruvia-http`'s `WsConnection`
exposes one `poll()` input driver. It returns `std::optional<WsEvent>`: `std::nullopt` means that a
complete event is not yet available because the parser needs more transport bytes, while every
materialized event is a zero-allocation
`std::variant` containing exactly one of `WsMessageEvent`, `WsPingEvent`, `WsPongEvent`,
`WsCloseEvent`, `WsProtocolErrorEvent`, or `WsTransportEndEvent`. There is no `kNone` event and no
writable `kind + opcode + payload + closeCode` tuple. Borrowed message/control payloads and the
Close reason remain valid until the next `poll()` call.

The inbound pipeline beneath `poll()` is discriminated end to end. Per
[RFC 6455 Section 5.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.2),
`webSocketTryReadFrame()` returns a `WebSocketFrameReadResult` containing exactly need-input, one
borrowed frame, or one failure carrying `WebSocketProtocolFailure`; it has no byte-count/EOF side
channel and never throws for peer bytes. Per
[RFC 6455 Section 5.4](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.4),
`WebSocketInboundAssembler::accept()` then returns a `WebSocketInboundResult` containing exactly
continue-reading, one control frame, one message with its content encoding, or one failure—there is
no action enum coupled to an output message. Malformed framing/fragmentation, invalid UTF-8, and an
oversized message carry Close codes 1002, 1007, and 1009 respectively as defined by
[RFC 6455 Section 7.4.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.4.1).
`WsConnection` is the sole layer that turns those typed failures into a Close frame and a
`WsProtocolErrorEvent`.

The typed fields follow the wire semantics rather than exposing an ambiguous raw union. Per
[RFC 6455 Section 5.5.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.5.1),
`WsCloseEvent` exposes the parsed status code and UTF-8 reason; an absent status is reported locally
as 1005 by [RFC 6455 Section 7.1.5](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.1.5),
while [Section 7.4.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.4.1) forbids that reserved
code from appearing on wire. Ping and Pong payloads have their own event types, and the core
automatically echoes Ping data in the Pong required by
[RFC 6455 Section 5.5.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.5.2).

The immutable `WsOutputPlan` still solely binds opaque frame bytes to a typed
keep-open/end-transport disposition. `WsTransportEndEvent` only stops the input pump; runtimes must
drive the plan rather than infer a transport action from that observation. A locally initiated
Close is therefore flushed without ending the transport; application messages are no longer
delivered, but the core keeps parsing until the peer Close completes the RFC 6455 handshake. Only
that completion (or a protocol failure/transport EOF) makes the output plan terminal. The RFC 8441
adapter maps the terminal disposition to HTTP/2 `END_STREAM`; it never infers END_STREAM merely
because a Close frame was sent. Heartbeat intervals, Pong timeout, and the close-handshake timeout
are runtime policy and live only in `ruvia-web` as `WebSocketLifecycleOptions`. The default
close-handshake timeout is five seconds, zero disables it, and negative route values are rejected
at registration. A timeout aborts only that WebSocket transport—`RST_STREAM(CANCEL)` for an HTTP/2
tunnel—rather than closing the multiplexed connection and its unrelated streams.

Buffered and streaming body decisions are also HTTP-owned. One allocation-free
`HttpResponseContentSemantics` classifies a method/status pair as exactly one of
`HttpInformationalResponseContent`, `HttpProtocolSwitchResponseContent`,
`HttpConnectTunnelResponseContent`, `HttpResponseWithoutContent`, or `HttpResponseWithContent`. The HTTP/1 client
parser, HTTP/2 client role, and `HttpResponseBodyPlan` all consume those same exclusive alternatives;
the body plan additionally binds the sender-only write policy (including 205's required zero-length
generation rule), while `HttpBufferedResponseWritePlan` adds the representation length and final
send-body verdict. `ruvia-web` must not recompute them with loose `skipBody` flags. In particular, a HEAD
response keeps the GET representation metadata (including negotiated content coding and
content length) but never emits payload bytes or HTTP/2 DATA frames. A 205 Reset Content is
also suppressed by that shared status plan: HTTP/1 canonicalizes it to `Content-Length: 0`
without transfer coding, while HTTP/2 ends it on the response HEADERS; caller-provided body
bytes or contradictory framing fields are never sent.
Status/control semantics are committed through one non-default-constructible
`HttpFinalResponseControlPlanResult`, not inferred from the body policy. The result contains either
one `HttpFinalResponseControlPlan` or one `HttpFinalResponseControlPlanFailure`; failures alone expose
`HttpFinalResponseControlPlanError`, so there is no `status()/accepted()` plus plausible default
Upgrade payload. A successful plan then contains exactly one `Http1FinalResponseControl` or
`Http2FinalResponseControl`. Only the HTTP/1 alternative exposes the already parsed repeated
`HttpConnectionOptions` and `HttpUpgradeProtocols`; the finalizer consumes those values instead of
rescanning response fields. The HTTP/2 alternative is available only after all connection-specific
response fields have been rejected. Buffered, streaming, and successful CONNECT final-head paths
must obtain that alternative before HPACK or stream mutation, and the encoder requires it rather
than silently dropping fields. This follows
[RFC 9113 Section 8.2.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2.2): an endpoint must
not generate `Connection`, `Proxy-Connection`, `Keep-Alive`, `Transfer-Encoding`, or `Upgrade`, and
the request-only `TE: trailers` exception does not apply to responses.

Outbound HTTP status codes are limited to the RFC 9110 range `100..599`;
`HttpResponse`, `Context`, and generic buffered/streaming handlers represent final responses only
(`200..599`). Non-101 1xx progress heads use the immutable, bodyless, borrowed
`HttpInterimResponseHead`. HTTP/1.1 encodes it only through the allocation-free,
transactional `Http1InterimResponseWriter::prepare()` entry; HTTP/2 submits it only through
`Http2Connection::submitInterimResponseHead()`. Its field storage must remain stable through that
synchronous prepare/submit call; initializer-list and rvalue-container construction are deleted, and
vectors are not accepted as implicit borrowed storage.
The response message model is protocol-version neutral: `HttpResponse` and `Context::ResponseInit`
carry only the numeric status code and expose no `statusText` or custom reason-phrase setter.
HTTP/1 final and interim writers derive conventional presentation text at serialization time through
`httpReasonPhrase()`; an unregistered status such as 299 gets an empty phrase, while the status line
still retains the required SP before CRLF under
[RFC 9112 §4](https://www.rfc-editor.org/rfc/rfc9112.html#section-4). HTTP/2 serializes only the
`:status` pseudo-header required by
[RFC 9113 §8.3.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.3.2) and never consumes a
reason phrase. `HttpErrorInfo::statusText` remains a `ruvia-web` JSON/error-label concern; it cannot
alter either protocol's wire response.
Both writers share validation before touching the output buffer, HPACK block, or stream: malformed
fields, Content-Length, Transfer-Encoding, Trailer, and repeated singleton fields are rejected.
The HTTP/1 writer additionally enforces the 64-field/64-KiB head limits, validates Connection/Upgrade,
reports the exact required buffer size transactionally, and carries `Connection: close` forward as
`requiresFinalConnectionClose()` because a final response is still required. It encodes exactly the
fields in the typed head—no hidden field injection. The Web runtime's automatic 100
Continue path drives this writer and treats socket write failure as transport failure; it contains no
independent HTTP status-line bytes. HTTP/2 additionally rejects all connection-specific fields
transactionally; an application-originated final response is never treated like an intermediary
translation whose hop-by-hop fields may be removed.
Across the HTTP-owned HTTP/1 and HTTP/2 final/interim encoders, a caller-supplied `Server` field is
preserved, but an absent field stays absent; the dedicated WebSocket handshake paths likewise never
invent a `Server` product identity. Product-banner policy belongs to the application/Web layer. This
follows [RFC 9110 §10.2.4](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.2.4), which makes
`Server` optional and cautions against exposing needlessly detailed implementation information. This
does not remove required protocol metadata: final HTTP/1 and HTTP/2 response paths still synthesize
`Date` when absent, satisfying the clocked-origin requirement for 2xx/3xx/4xx responses in
[RFC 9110 §6.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.6.1); non-switching 1xx
writers remain exact and do not add it.
Status 101 remains exclusive to a dedicated driver because it transfers connection ownership. A 426
response over HTTP/1 must contain at least one syntactically valid `Upgrade` protocol before any
connection/header mutation, after which the finalizer supplies the paired `Connection: Upgrade`.
HTTP/2 cannot represent 426 because RFC 9110 requires that Upgrade field while RFC 9113 forbids it;
the core therefore returns `kInvalidMessage` without HPACK or stream mutation, and the Web driver
resets an otherwise-open stream instead of leaving the peer waiting indefinitely. Actual HTTP/1
WebSocket 101 and HTTP/2 Extended CONNECT transitions remain owned by their dedicated drivers.
The HTTP/2 core records local frame permission as one `Http2LocalSendState`, exactly one of
`Http2LocalHeadPending`, `Http2LocalRequestContentOpen`, `Http2LocalResponseContentOpen`,
`Http2LocalResponseTrailersOnly`, `Http2LocalConnectPending`, `Http2LocalTunnelOpen`,
`Http2LocalEndStreamQueued`, `Http2LocalEndStreamCommitted`, or `Http2StreamAborted`. Interim heads
leave the head-pending alternative unchanged; one request/final-response/WebSocket head selects the
applicable content, trailer, CONNECT, or terminal alternative. Request DATA, response DATA, and
tunnel DATA therefore cannot be confused, while a trailers-only response can never become
DATA-open. Queued END_STREAM means that the core owns a terminal signal still waiting behind
flow-controlled DATA or trailers; committed END_STREAM means that the terminal HEADERS or DATA is
already materialized in the outbound buffer. Only `Http2StreamAborted` owns an immutable,
non-`kNone` `Http2StreamCloseSource`. It covers local/peer RST_STREAM and a request excluded by the
peer GOAWAY last-stream-id; the latter is not a reset, so the lifecycle exposes `isAborted()` and one
atomic `abort(source)` transition rather than `isReset()`/`markReset()`/`removeReset()` vocabulary.
Aborting also selects the remote-aborted alternative and clears ready-queue ownership, whereas normal
END_STREAM remains a committed half-close. `Http2StreamLifecycle` and `Http2StreamState` expose one const
`localSend()` view instead of a phase/kind/boolean product or forwarding accessors. Mutation is
private below `Http2StreamState`: `Http2LocalSendState` only accepts its lifecycle friend, the
lifecycle only accepts its stream-state friend, and the connection mutates through that single typed
entry point so tunnel/content checks cannot be bypassed. These transitions
model the half-closed(local) boundary in
[RFC 9113 Section 5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1), DATA permissions and
terminal END_STREAM in [Section 6.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.1), and
HEADERS-carried END_STREAM in
[Section 6.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.2).
RST_STREAM whole-stream termination follows
[Section 6.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4); GOAWAY exclusion of requests
above its last-stream-id follows
[Section 6.8](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8).

Remote frame permission is likewise one `Http2RemoteReceiveState`, exactly one of
`Http2RemoteHeadPending`, `Http2RemoteHeadEndStreamPending`, `Http2RemoteContentOpen`,
`Http2RemoteConnectPending`, `Http2RemoteConnectPendingEndStream`,
`Http2RemoteConnectRejectedAwaitingEndStream`, `Http2RemoteTunnelOpen`,
`Http2RemoteEndStream`, or `Http2RemoteAborted`. Initial/final HEADERS therefore select content,
CONNECT-decision, tunnel, or peer-half-close semantics atomically; interim responses remain
head-pending. The server can distinguish a CONNECT whose HTTP content is necessarily absent from a
peer send half that is still open. Rejecting such a CONNECT accepts exactly the peer's empty
DATA frames and uses `END_STREAM` to terminate; the same empty terminal DATA can half-close a
pending CONNECT before the decision. Accepting an open CONNECT enters `Http2RemoteTunnelOpen`, so deferred reads
continue replenishing both connection and stream receive windows until an actual peer
`END_STREAM`. `Http2StreamLifecycle` and `Http2StreamState` expose one const `remoteReceive()` view;
the former `headersDecoded()`, `bodyEnded()`, and `peerEndStream()` booleans and their independent
mutators do not exist. Friend-only mutation follows the same lifecycle-to-stream ownership chain as
local send state. This models peer half-close in
[RFC 9113 Section 5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1), message termination in
[Section 8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1), CONNECT tunnel DATA and empty
terminal DATA in [Section 8.5](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.5), and orderly
Extended CONNECT closure in
[RFC 8441 Section 5](https://www.rfc-editor.org/rfc/rfc8441.html#section-5).
`submitData()` also distinguishes `kQueued` (the core copied and owns the
unsent suffix) from `kBackpressured` (the core accepted nothing, so the caller must retry after
the prior submission drains). `Http2LocalContentState` binds a final response's declared
`Content-Length` to the DATA bytes accepted by the core. Its state is exactly one of
`Http2LocalContentUnset`, `Http2LocalContentForbidden`, `Http2LocalContentUnbounded`, or
`Http2LocalContentKnownLength`; only the last alternative exposes `declaredLength()`. An unset
message rejects DATA with `kNotStarted`, while forbidden content rejects every DATA submission.
Accepted and committed byte counters remain common accounting, available through the stream's one
const `localContent()` view instead of duplicated mode/length/counter forwarding accessors.
Before HPACK or local stream mutation, `Http2ResponseHeadPlan` also makes response-head
Content-Length ownership exactly one of `Http2CanonicalResponseContentLength`,
`Http2ExplicitResponseContentLength`, `Http2AbsentResponseContentLength`, or
`Http2ForbiddenResponseContentLength`. Buffered, streaming, and successful CONNECT heads use
`http2BufferedResponseHeadPlan()`, `http2StreamingResponseHeadPlan()`, and
`http2ConnectResponseHeadPlan()` respectively. `Http2ResponseHeadPlanResult` keeps an invalid
explicit length in its failure alternative; a valid explicit value is parsed once, canonicalized
on the wire, and reused to initialize `Http2LocalContentKnownLength`. The HPACK encoder accepts only
that plan—the former `autoContentLength + emitAutoContentLength` scalar entry does not exist.
Overrun and premature END_STREAM are rejected before output/window mutation,
`finishResponse(streamId, trailers)` refuses an incomplete exact body, and invalid preserved lengths reject the head
before HPACK output. This follows HTTP/2's HEADERS-then-DATA message order and terminal END_STREAM in
[RFC 9113 Section 8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1), plus its exact
Content-Length/DATA requirement in
[Section 8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1).

Peer-sent message content is accounted independently by `Http2RemoteContentState`. It is exactly one
of `Http2RemoteContentAllowedWithoutLength`, `Http2RemoteContentAllowedKnownLength`,
`Http2RemoteContentMetadataOnlyWithoutLength`, or
`Http2RemoteContentMetadataOnlyKnownLength`. Only the two known-length alternatives expose
`declaredLength()`; only the allowed alternatives own received bytes. `Http2StreamState` exposes only
one const `remoteContent()` view. After a client decodes the final
HEAD/204/304 head, the shared `HttpResponseContentSemantics` atomically selects metadata-only while
preserving any representation Content-Length. Non-empty DATA then returns
`Http2RemoteContentAccountingResult::kContentForbidden` and the connection emits
`RST_STREAM(PROTOCOL_ERROR)` as required for a malformed response; empty DATA may still carry the
terminal END_STREAM without producing a content event. This follows the no-content rules in
[RFC 9110 Section 6.4.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.4.1) and malformed-message
handling in [RFC 9113 Section 8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1).
All allowed DATA uses one atomic `account()` transition: `kCounterOverflow`,
`kDeclaredLengthExceeded`, and `kContentForbidden` leave `receivedBytes()` unchanged, while only
`kAccepted` advances it. A late length declaration or metadata-only transition fails without replacing
the active alternative. Flow control still counts the complete DATA frame payload, including padding,
before message semantics are applied as required by
[RFC 9113 Section 6.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.1), and every END_STREAM path
consults the active state's `terminalLengthValid()` result.

Inbound DATA delivery now has one explicit protocol/runtime ownership boundary. Every non-empty
`Http2MessageBodyChunkEvent` or `Http2TunnelDataEvent` retains the complete flow-controlled frame
debt (including Pad Length and padding) inside `Http2Connection`; the external owner calls
`releaseReceivedData(streamId)` only after all currently delivered event bytes for that stream have
been copied or consumed. Empty and metadata-only DATA have no application bytes to retain, while
stream removal returns unreleased debt at connection scope. There is no
`deferStreamWindowRelease()` opt-in, `releaseStreamWindow()` alias, or `Http2ConnectionLimits`
route-policy object. This follows
[RFC 9113 Section 5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.2), where receive credit
describes capacity the receiver is prepared to accept, and
[Section 6.9.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.1), where WINDOW_UPDATE is
sent as consumed data frees that capacity.

Buffered/streamed request storage is therefore Web runtime state, not HTTP/2 protocol state.
The connection coroutine also requires a complete `Http2SansIoSessionContext` value. It binds the
server options, `ConnectionScanner::Entry`, graceful-shutdown atomic, and explicit
`ContextServices` integrations—including the already-typed `ConnInfo`—before the coroutine starts.
The former default-constructed
`Http2SansIoSessionEnv` pointer bag is removed: an installed caller cannot silently fall back to
default options, an unlinked local scanner, or a session that ignores shutdown. Bare defaults used
by socket tests now live only in a test fixture.
`ruvia-web` owns a PMR-stable `Http2SansIoStreamRuntimeTable`; each entry owns one
`Http2RequestBodyRuntime`, its `Http2SansIoBodyQueue`, and the optional
`Http2SansIoStreamSignal` that is also the stream's dispatch lease. The table increments
`dispatchedCount()` synchronously before `co_spawn`; runtime-level admission is table-only, so no
caller can create a signal while bypassing the aggregate lease, and admission is rejected until
`Http2SansIoStreamRuntime::selectRoute()` has stored the stream's `RouteResolution` and selected its
optional `RequestBodyMode` in one operation. `Http2RequestBodyRuntime` has no independently callable
mode selector and no fake buffered default plus selection flag. Writer exit consumes that dispatch
count; idle classification consumes the same table's `size()`, so a buffered body that has not reached
dispatch is still treated as active payload work. Teardown wakeup and removal traverse that one
runtime lifetime. There is no parallel
default-heap `streamSignals` vector or per-signal `unique_ptr` allocation. Body readers,
WebSocket transports, and response-stream sinks require a signal reference rather than accepting a
nullable parallel state. The ordered message-head event selects the route's `RequestBodyMode`
before later body events are stored—even when HEADERS and DATA arrived in
one `feed()` span; storing before that one-time selection returns `kModeNotSelected`. Buffered
events are copied and acknowledged after the complete event batch;
streaming request and CONNECT tunnel events remain window-deferred until the handler drains its Web
queue. A stream signal sets its timer deadline once: registering a concurrent waiter never changes
the expiry or self-cancels an earlier waiter, while one wake cancels all registered waits so each
consumer can re-check its own readiness. Total and streaming-backlog product limits are applied
there. Because an owner-side reset
does not echo a `kStreamClosed` event back to that owner, the session immediately removes an
undispatched Web runtime on its reset path; a dispatched handler retains its runtime until handler
cleanup. `Http2RequestBuilder` receives the resulting body view explicitly. Buffered-response
compression uses a handler-local Web scratch buffer. Consequently, `Http2StreamState` contains no
route mode, coroutine waiter, application
body queue/buffer, or response-compression storage, and the former `Http2BodyState.h`,
`Http2BodyQueue.h`, `Http2StreamBodyQueue.h`, and `Http2StreamBodyPolicy.h` headers are absent from
the independently usable HTTP package.

Client-role regular request heads use `Http2RequestContent` as the single framing contract.
`none()`, `knownLength(n)`, and
`streaming()` create `Http2RequestWithoutContent`, `Http2KnownLengthRequestContent`, and
`Http2StreamingRequestContent` alternatives; only the known-length alternative exposes
`length()`. They deterministically select canonical Content-Length, HEADERS END_STREAM, and the
same stream-owned content state without a mode-wide fake zero. A raw `content-length` field is
rejected rather than becoming a second source of truth. This models
[RFC 9113 Section 8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1), where HEADERS/DATA
and the final END_STREAM delimit one message, and
[Section 8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1), where a present
Content-Length must equal the DATA payload total. Each request-head API performs validation, odd stream-ID allocation,
`SETTINGS_MAX_CONCURRENT_STREAMS` admission, and HEADERS emission as one transaction, returning
one discriminated `Http2RequestHeadSubmitResult`. Its `Http2SubmittedRequestHead` alternative alone
exposes the allocated nonzero stream ID; `Http2RequestHeadSubmitFailure` alone exposes a typed
`Http2RequestHeadSubmitError`. There is no accepted/status field paired with a zero stream-ID
sentinel. Failure consumes no ID, HPACK bytes, or peer concurrency slot. This matches
[RFC 9113 Section 5.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.1), where zero is
reserved for connection control and client-created streams use increasing odd identifiers, while
[Section 5.1.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.2) makes concurrent-stream
admission peer-controlled and
[Section 6.5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2) defines that setting as a
directional creation limit. A submitted stream holds that slot until RST_STREAM or both END_STREAM
halves complete. There is no public API for preallocating an RFC-idle stream, and request heads are
rejected until `beginConnection()` has queued the client preface. The explicitly named
`submitRegularRequestHead()` accepts every valid non-CONNECT method token—including extension
methods—and rejects malformed tokens, CONNECT, and caller-supplied pseudo-headers. Standard CONNECT
goes through `submitConnectRequestHead()` with authority-form validation; Extended CONNECT goes
through `submitExtendedConnectRequestHead()`
only after the peer advertises `SETTINGS_ENABLE_CONNECT_PROTOCOL=1`. Both remain DATA-closed
until a final 2xx response; inbound Extended CONNECT is likewise rejected unless the core has
advertised its own capability through `beginConnection()`. A server accepts a pending tunnel through
`submitConnectResponseHead()` (the WebSocket specialization uses
`submitWebSocketHandshake()`). Successful tunnel DATA is surfaced as `kTunnelData`, peer
`END_STREAM` as `kTunnelEnd`, and each direction remains independently half-open. The core
rejects request content, trailers, DATA before acceptance, DATA after the peer FIN, and every
non-management frame on a connected stream. The content state's `accepted` counter moves
once for the whole `kAccepted`/`kQueued` input, while `committed` moves only as DATA payload is
materialized; WINDOW_UPDATE drain never double-counts ownership. HEAD/204/205/304 metadata is
body-forbidden rather than an exact DATA contract, and tunnel bytes remain unbounded. A queued
terminal marker is tracked separately from a serialized
`END_STREAM`; reset and rejection transitions discard deferred DATA/trailers before the owner is
woken. This keeps head ordering, retry ownership, and stream termination inside the sans-I/O
protocol state machine instead of relying on each runtime driver to reproduce them.

Final response submission is discriminated in the same direction. The buffered
`submitResponseHead(streamId, response, writePlan)` consumes the caller's final prepared
representation snapshot and rejects a stale plan separately from a malformed response.
`submitResponseHead()` returns
`Http2BufferedResponseHeadSubmitResult`, and `submitStreamingResponseHead()` returns
`Http2StreamingResponseHeadSubmitResult`. Only the `Http2SubmittedResponseHead<Plan>` alternative
exposes its immutable buffered write plan or streaming commit plan; only
`Http2ResponseHeadSubmitFailure` exposes `Http2ResponseHeadSubmitError`. The result itself has no
status, accepted flag, or plan accessor, so a closed stream, invalid phase, or malformed response
cannot expose plausible DATA/END_STREAM instructions. Failure is transactional: it emits no
HEADERS/HPACK bytes and does not advance the stream send phase, while success atomically commits the
initial response HEADERS and its exact subsequent-content plan. This mirrors
[RFC 9113 Section 8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1), where a response is
sent on its request stream as a header section followed by content/trailers, and
[Section 8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1), which defines malformed
responses. The submitted alternative also owns whether the initial HEADERS carries END_STREAM, as
specified by [RFC 9113 Section 6.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.2).

HTTP/2 startup has one role-aware, idempotent entry: `beginConnection()`. A client emits the
connection magic before SETTINGS; a server requires that magic and emits its SETTINGS without it.
In accordance with RFC 9113 Sections 3.1 and 3.3, TLS selects `h2` through ALPN and cleartext
HTTP/2 requires prior knowledge plus the client connection preface. The obsolete HTTP/1.1
Upgrade/`HTTP2-Settings` handshake and synthetic stream-1 seed do not exist in either the core or
the Web driver.
One typed `PrefacePhase` represents not-started, client-magic, peer-SETTINGS, and ready states;
split booleans cannot create impossible combinations. Calling `feed()` before startup returns
`kConnectionNotStarted` with zero bytes consumed and no state mutation, so the driver retains and
retries the same input after `beginConnection()`. After the role-specific magic boundary, both
roles require the peer's first frame to be a non-ACK SETTINGS frame; an ACK cannot complete the
peer preface, and `receivedPeerSettings()` becomes true only in the ready phase.
Input and event delivery share one ownership contract. `nextEvent()` returns
`std::optional<Http2Event>`; `std::nullopt` is the only drained-queue signal, while every
materialized event is a zero-allocation `std::variant` with exactly one typed payload. There is no
sentinel event and no writable `kind + streamId + bytes + error` tuple from which callers can form
invalid combinations. `Http2MessageBodyChunkEvent` and `Http2TunnelDataEvent` carry zero-copy views
that remain valid until the next input-consuming `feed()`. If any event is still queued, `feed()`
returns `kEventsPending` with zero bytes consumed and preserves both the event and its body view;
the owner drains the queue and retries the identical input span. A `kNeedMore` result has the
opposite ownership: all supplied bytes were consumed into the core, but a partial preface/frame
remains buffered, and events produced by earlier complete frames still need draining. The in-tree
Web driver uses one feed-and-drain path for initial and ordinary socket bytes. It also performs
`Http2StreamClosedEvent` cleanup directly from that event's stream ID, because the protocol core
may already have erased an unpinned reset stream.

As required by [RFC 9113 Section 6.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4),
`Http2StreamClosedEvent` retains the close source and exact `RST_STREAM` error code; local stream
failures report the same code emitted on the wire. The owner never reconstructs this reason from
stream state after closure.
Inbound GOAWAY is also a protocol-core lifecycle transition, not a hint for each driver to
reinterpret. `peerGoaway()` returns the latest typed last-stream-id/error pair and immediately
blocks new client requests. For every locally initiated stream above that boundary whose response
has not started, the core discards deferred DATA/trailers and flow-control debt, releases its peer
concurrency slot and stream-table storage, then emits an `Http2RequestUnprocessedEvent` so the owner
can safely retry it on another connection. In accordance with
[RFC 9113 Section 6.8](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8), the
`Http2GoawayEvent` owns the connection-level last-stream-id and error; the per-request safe-retry
event carries only its stream ID instead of duplicating GOAWAY metadata. Repeated GOAWAY frames may
retain or lower the boundary but never increase it; a boundary that excludes an already decoded
response head is contradictory and closes the connection with `PROTOCOL_ERROR`. A valid peer GOAWAY
enters the same idempotent
`beginDrain()` path to emit the directional reciprocal GOAWAY, but it is not a fatal local error:
frames already present in the same input batch and established streams continue to completion.
`connectionError()` is the separate typed optional populated only when this endpoint detects a
fatal frame-layer error; the Web driver stops reading on that value, not on graceful drain. The
generic core sans-I/O pump takes an explicit, inlined stop predicate instead of imposing a lossy
one-boolean lifecycle contract on every protocol. Arbitrary GOAWAY error emission remains internal
to protocol-error handling.
`Http2LocalSettings` is the only source for local receive capabilities: the exact SETTINGS bytes,
accepted frame size, stream and connection receive windows, and bounded stream/ready-queue
capacity all derive from it. The protocol connection constructor accepts no route/body-limit
configuration; application limits belong to the runtime that consumes DATA events. The connection
send window starts at the RFC default and changes only in response to peer SETTINGS or
WINDOW_UPDATE; no runtime configuration knob may impersonate peer flow-control state.
Every inbound DATA frame not being rejected immediately as a connection error is debited from the
connection receive window by its entire payload length, including padding fields, before stream
lookup or any stream-level semantic discard.
Consequently, DATA for a closed/reset/GOAWAY-discarded stream can still cause the required
connection `FLOW_CONTROL_ERROR` when shared credit is exhausted. A successful debit for discarded
DATA is returned exactly once through a stream-0 WINDOW_UPDATE; only a live stream proceeds to a
separate stream-window debit, and deferred delivery retains both debts until release or close.
`Http2PeerSettings` is constructed with the local `Http2Role`, so directional requirements are
not optional caller checks: a client rejects a server's `SETTINGS_ENABLE_PUSH=1`, while a server
can accept either legal value from a client. Applying one entry returns a discriminated
`Http2PeerSettingApplyResult`: ordinary valid settings and unknown identifiers expose only
`Http2PeerSettingApplied`; `SETTINGS_INITIAL_WINDOW_SIZE` exposes only
`Http2PeerInitialWindowChange::delta()`; invalid values expose only
`Http2PeerSettingFailure::error()`. There is no status/changed/delta tuple, and
`Http2Connection` is the sole consumer that maps a failure to GOAWAY or propagates a signed delta
to every active stream. This keeps unknown-setting handling and value validation aligned with
[RFC 9113 Section 6.5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2), and keeps the
all-stream window adjustment required by
[Section 6.9.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.2) inseparable from the only
alternative that owns the delta. The irreversible `SETTINGS_ENABLE_CONNECT_PROTOCOL` transition
continues to follow [RFC 8441 Section 3](https://www.rfc-editor.org/rfc/rfc8441.html#section-3).

Inbound field blocks have the same single-owner rule: even when a stream was locally reset,
refused during drain, or rejected for a stream error, the core reassembles every required
CONTINUATION and HPACK-decodes the complete block into detached scratch before discarding its
HTTP fields. If an owner closes a live stream mid-block, the partial compressed bytes move to
that scratch; a locally sent RST remains the last local stream frame. Failure to complete the
mandatory decompression closes the connection with `COMPRESSION_ERROR`. RFC 9113-deprecated
priority dependency and weight values are shape-checked and otherwise ignored, so they never
mutate or reset a stream.

The root `CMakeLists.txt` only coordinates global options, dependency discovery, installation, package export, tests, and examples. Each library owns its own `CMakeLists.txt`, `include/`, and `src/` directory. There is no root-level source `include/`, `src/`, or `fuzz/` tree.

## Requirements

- CMake 3.24 or newer.
- C++23 compiler.
- vcpkg.
- Component manifest dependencies: `core` uses `asio`/`mimalloc`, `http` uses
  `zlib`/`brotli`/`zstd`, and `web` adds `openssl`.
- Optional vcpkg features: `mariadb`, `redis`, `jwt`.

On Windows, the root CMake file defaults `VCPKG_TARGET_TRIPLET` to `x64-windows-static` when it is not already set.

## Build

Default build:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Debug
```

Build only one standalone component (vcpkg installs only that component's
dependency feature set):

```powershell
# core only
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_CORE=ON `
  -DRUVIA_BUILD_HTTP=OFF `
  -DRUVIA_BUILD_WEB=OFF

# http only
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_CORE=OFF `
  -DRUVIA_BUILD_HTTP=ON `
  -DRUVIA_BUILD_WEB=OFF
```

Build tests and examples:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Enable optional web integrations:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_ENABLE_MARIADB=ON `
  -DRUVIA_ENABLE_REDIS=ON `
  -DRUVIA_ENABLE_JWT=ON
```

## Install And Consume

Install:

```powershell
cmake --install build --config Debug --prefix build/install
```

Each library has its own installed CMake export. A component-scoped package install
therefore installs the selected library component plus the shared `Development`
metadata; Web additionally needs both lower components:

```powershell
# Standalone core package
cmake --install build --config Debug --prefix build/install-core --component core
cmake --install build --config Debug --prefix build/install-core --component Development

# Complete Web package closure
cmake --install build --config Debug --prefix build/install-web --component core
cmake --install build --config Debug --prefix build/install-web --component http
cmake --install build --config Debug --prefix build/install-web --component web
cmake --install build --config Debug --prefix build/install-web --component Development
```

Consume the full web framework:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(my_app PRIVATE ruvia::web)
```

Consume narrower libraries:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS core)
target_link_libraries(tool PRIVATE ruvia::core)

find_package(ruvia CONFIG REQUIRED COMPONENTS http)
target_link_libraries(protocol_tool PRIVATE ruvia::http)
```

`find_package()` imports only the requested target closure: `core` and `http` remain
independent, while `web` imports `core + http + web`. It also derives
`ruvia_AVAILABLE_COMPONENTS` from the exports actually present in the install prefix,
so a partial install never advertises or imports a missing library. Missing
`OPTIONAL_COMPONENTS` remain optional; a missing required component rejects the
package. When no component is requested, all usable installed components are loaded.
New projects should still request the component they use explicitly.

## Minimal Web App

```cpp
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class HelloController final : public ruvia::Controller<HelloController> {
public:
    RUVIA_ROUTES_BEGIN
        RUVIA_GET("/hello", hello)
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello");
    }
};

int main() {
    ruvia::app()
        .setHttpListenPort(8080)
        .run();
}
```

Handlers are async-only and use `ruvia::Task<T>`. HTTP request data is read through
`c.req()`, and response helpers live on `Context`. Adapter-owned connection metadata is
kept out of the request model and queried with the Hono-like `ruvia::getConnInfo(c)`:

```cpp
const auto info = ruvia::getConnInfo(c);
const auto peerAddress = info.remote().address();
if (const auto* tls = info.tls()) {
    const auto clientSubject = tls->clientCertificateSubject();
}
```

`ConnInfo` contains exactly one `PlainConnectionTransport` or
`TlsConnectionTransport`. Only the TLS alternative exposes the verified mutual-TLS
peer subject DN, which is empty when TLS was used without a client certificate. The
former independent `secure()` boolean and top-level `clientCertificateSubject()`
cannot represent that relationship and are removed together with the internal
`withTransport(..., bool secure)` refinement.

The server creates this typed connection value once: plain TCP is classified at the
accepted-socket entry, while TLS is classified only after a successful handshake.
The same borrowed, allocation-free value then flows through HTTP/1 or HTTP/2
`ContextServices`, into `Context`, and out through `getConnInfo()`. HTTP/2 no longer
re-derives security from its stream template for every dispatched request, and a
plain connection cannot carry client-certificate identity. Because both address and
certificate are borrowed from connection-owned storage, rvalue owning-string
transport refinements are deleted rather than creating dangling metadata. The
pointer-returning alternative accessors are lvalue-only, so callers must retain the
small `ConnInfo` value exactly as shown above instead of taking a pointer from a
temporary.

## Core API Shape

Ruvia's public web API is macro-based and startup-built:

- `RUVIA_CONTROLLER_GROUP(...)`
- `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END`
- `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE`
- `RUVIA_GET_STREAM`, `RUVIA_GET_SSE`
- `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS`
- `RUVIA_MODEL`, `RUVIA_FIELD`, `RUVIA_FIELD_NAME`
- `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_RULE`

The request hot path uses prebuilt route tables and middleware chains. Public APIs expose Ruvia types, not Web runtime objects.

The internal registration-to-dispatch chain is also discriminated. A move-only `RouteEndpoint`
contains exactly one `BufferedRouteEndpoint`, `ResponseStreamRouteEndpoint`, or
`WebSocketRouteEndpoint`, binding the legal handler shape to only its relevant request-body,
stream-kind, or WebSocket metadata. The former `ResponseBodyMode` plus simultaneous buffered and
stream handler fields are removed. Lookup returns a self-contained `RouteResolution` containing
exactly one `ResolvedRoute`, `RouteMethodNotAllowed`, or `RouteNotFound`; only `ResolvedRoute` owns
the route reference and its copied `RouteMatch`, and only `RouteMethodNotAllowed` exposes the Allow
mask. `resolve()` no longer accepts caller-owned match scratch, so HTTP/1 and HTTP/2 transport
drivers consume the same result rather than maintaining a parallel match buffer or reading payload
through `found()`/top-level accessors. Those transports now call the one concrete, startup-frozen
`RouteTable` directly. The former `RequestDispatcher` virtual interface is removed: it had only one
implementation but imposed request-time virtual lookup and dispatch on every HTTP/2 stream.

Per-dispatch `Context` capabilities follow the same exclusive-state rule. `ContextRequestBodySource`
contains exactly one `ContextBufferedRequestBodySource`, `ContextLazyRequestBodySource`, or
`ContextStreamingRequestBodySource`; `ContextResponseOutput` contains exactly one
`ContextBufferedResponseOutput`, `ContextResponseStreamOutput`, or `ContextWebSocketOutput`. The
non-buffered alternatives borrow a non-null runtime facade, and `ContextServices` copies the two
small discriminated values into `Context`. The former four nullable `bodyReader`/`bodyLoader`/
`responseStream`/`webSocket` slots and the `withBodyReader`/`withBodyLoader` refinements are removed,
so an output cannot be both a response stream and a WebSocket and a body cannot be both lazy and
streaming. This adds no allocation, lock, virtual dispatch, or request-time type erasure.

## Runtime Model

- Per-worker standalone Asio `io_context`.
- One acceptor/server/thread per worker.
- Connections stay owned by their worker.
- Request parsing is zero-copy by default.
- Header limit is 64KB; ordinary body limit is 16MB.
- Explicit stream routes handle large request bodies.
- Responses use fixed header buffers and scatter-gather writes.
- File responses avoid full-file buffering and use zero-copy paths where available.
- The HTTP/1 request path is allocation- and lock-free; HTTP/2 multiplexing adds one recycled
  coroutine frame per concurrently dispatched stream. Route lookup and handler dispatch call the
  concrete `RouteTable` without a per-request virtual interface.
- The optional rate limiter is the one shared-atomic structure on the request path; it is
  off by default, so per-request atomic cost is opt-in. Its monotonic clock is selected at
  compile time, which keeps fixed-window boundary tests deterministic without adding an
  indirect call or type erasure to the production hot path.
- Two PMR pooling tiers back memory: a process-level mimalloc resource and a per-request
  monotonic arena. Per-worker isolation comes from an object pool plus mimalloc's
  thread-local heaps rather than a distinct PMR tier.

Exhausted per-worker DB and Redis pools share `ruvia-core`'s allocation-free intrusive
`PoolWaiterQueue`. `PoolWaiter` is itself the coroutine awaiter, and the queue commits one
non-default-constructible `PoolWaiterResult` before resuming it. The result contains exactly
`PoolWaiterAcquired`, `PoolWaiterTimedOut`, or `PoolWaiterClosed`; only acquisition exposes a slot
index. Pool shutdown therefore produces an explicit closed alternative instead of encoding closure
as `ready + !timedOut + pool-size sentinel`, and the two integrations no longer duplicate a local
awaiter or reconstruct completion from three parallel scalars. Callers observe completion only
through the awaiter protocol (`co_await`/`await_resume()`), with no parallel result accessor.
`closeAll()` commits closure to the entire queued snapshot before resuming any coroutine, so a
re-entrant continuation cannot turn another closing waiter into a successful acquisition.

## HTTP Library

`ruvia::http` is intended to be useful without the web framework or the Ruvia runtime foundation, in the nghttp2 class: a pure, core-free, asio-free, sans-I/O protocol library. It owns HTTP wire/message/framing/connection semantics and reusable helpers -- the h1 parser and connection semantics, the HTTP/2 connection state machine (`Http2Connection`, one implementation driven in both server and client role), the WebSocket protocol core (`WebSocketProtocol.h`), HPACK, response-head serialization helpers, cookie/cache/range/conditional request/content negotiation helpers, multipart/form/url encoding (`MultipartParser.h`), SSE formatting (`Sse.h`), content decoding, and opaque protocol body handles. You feed it bytes and drive its events from any runtime.

Single byte-range handling has one protocol entry, `resolveHttpByteRange()`, and one discriminated
`HttpByteRangeResolution`. `HttpByteRangeIgnored` owns no slicing data,
`HttpByteRangeUnsatisfiable` owns no fake default range, and only `HttpResolvedByteRange` exposes a
nonempty `offset()`/`length()` pair.
The resolver handles the case-insensitive range unit, clamps syntactically valid decimal numerals
larger than `uint64_t` according to their semantics instead of treating conversion overflow as
malformed input, and uses RFC 9110's permitted ignore policy for an empty selected representation
and unsupported multipart range sets. `ruvia::web` performs no preliminary comma scan: it maps only
these alternatives to the full 200 response, 416 plus unsatisfied Content-Range, or a single 206
file slice. This follows the extensible, case-insensitive range-unit requirement in
[RFC 9110 Section 14.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.1), the numeric and
byte-range rules in [Section 14.1.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.1.2), and
the processing choices in
[Section 14.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.2).

Multipart parsing uses one RFC contract in both buffered and streaming paths.
`MultipartBoundary` validates and owns the 1–70 byte boundary once; Content-Type extraction additionally
enforces MIME token/quoted-string syntax. Every staged decision is discriminated rather than represented
as a writable status plus unrelated fields: `HttpMultipartBoundaryParseResult` owns either the boundary or
a typed failure, `HttpMultipartPartHeaderParseResult` owns either parsed header views or a typed failure,
and `HttpMultipartDelimiterResult` distinguishes no-match, need-input, regular part delimiter, and closing
delimiter. Only a delimiter candidate exposes its offset, and only a complete regular/closing delimiter
exposes its line length.

The public `MultipartParser` accepts bytes through `feed()`, receives explicit end-of-input through
`finishInput()`, and returns a `MultipartPollResult` containing exactly one of `MultipartPollNeedInput`,
`MultipartStreamPart`, `MultipartPollDone`, or `MultipartPollFailure`; only the part alternative exposes
borrowed metadata/body and its typed `MultipartChunkPhase`, while only failure owns a
`MultipartParseError`. Malformed or incomplete wire input is therefore reported without a wire-format
exception; the Web facade maps that typed failure to the allocation-free `HttpProtocolError` signal. There
is no `PollStatus + optional part` pair and no throwing part accessor.
This preserves the boundary grammar required by
[RFC 7578 Section 4.1](https://www.rfc-editor.org/rfc/rfc7578.html#section-4.1) and prevents a closing
delimiter split at an I/O boundary from being committed before explicit EOF. In accordance with
[RFC 2046 Section 5.1.1](https://www.rfc-editor.org/rfc/rfc2046.html#section-5.1.1), the `ruvia::web`
adapter ignores the epilogue semantically but still drains those HTTP body bytes before allowing connection
reuse; it only drives typed parser results and never rescans delimiter bytes.

It does not own `App`, `Context`, `Controller`, `Router`, middleware, model validation, DB, Redis, JWT, CORS policy, security-header middleware, static-file product policy, or any socket/TLS I/O. Its `HttpRequest` represents the HTTP message only and therefore never stores peer addresses, TLS state, or client-certificate identity. Reading or writing HTTP headers is not by itself a reason to live in `ruvia::http`: protocol decisions such as framing, keep-alive, upgrade handshakes, and response-head serialization belong here; product decisions such as CORS, sessions, CSRF, rate limits, redirects, and static-root indexing live in `ruvia::web`.

The outbound HTTP client surface is intentionally limited to the low-level, sans-I/O protocol API in `ruvia::http`. `HttpScheme` and the immutable borrowed `HttpOrigin::http()`/`https()` factories bind scheme, host, and normalized numeric port without a transport-flavored TLS boolean or allocation; the borrowed host storage must outlive the value and remain unchanged, and rvalue string factories are deleted. Construction validates one non-empty RFC 3986 `uri-host`, including bracketed IPv6/IPvFuture literals, so authority formatting and redirects no longer repeat origin validation. Host, absolute-form, and redirect parsing share `HttpAuthorityView`, which preserves absent, explicitly empty, and numeric ports; HTTP comparison maps absent/empty ports to the scheme default, folds host case, normalizes percent-encoding hex case, and decodes only percent-encoded unreserved octets. Encoded reserved characters remain distinct from their raw spelling. A numeric port zero remains a distinct syntactic origin for the external transport to accept or reject, while CONNECT still requires a non-empty, nonzero tunnel port. `HttpClientRequest` is a borrowed HTTP message view (method, request target, headers, and typed content). Its HTTP/1 wire representation is prepared transactionally by `Http1ClientRequestWriter`; method and header storage borrowed by the resulting Prepared-bound parser must remain alive and unchanged until the corresponding final response head or protocol-switch decision has been parsed. `HttpClientResponse`/`HttpClientResponseHeader` own parser output through PMR. Runtime promises such as redirect counts, Expect wait duration, stream decode switches, TLS files, pools, and timeouts are deliberately absent because this target has no I/O runtime to honor them. Redirect helpers return a typed request/content plan, distinguish absent, empty, and repeated response fields, and resolve relative Location URI references against the current request target before enforcing same-origin scheme/host/port. A malformed or userinfo-bearing Location authority is an invalid Location; only a syntactically valid unequal authority is classified as a different origin.

Redirect protocol helpers are a public sans-I/O surface in `HttpClientRedirect.h`, not an installed
`detail` header. `lookupUniqueHttpClientResponseHeader()` returns one
`HttpClientResponseHeaderLookupResult`: `HttpClientResponseHeaderAbsent`,
`HttpClientResponseHeaderFound`, or `HttpClientResponseHeaderRepeated`. Only `Found` exposes the
borrowed value, so a present empty Location remains distinct from absent or repeated Location.
`resolveHttpClientSameOriginRedirectTarget()` takes a PMR resource and returns one move-only
`HttpClientRedirectTargetResult`: either an owning `HttpClientRedirectTarget` or an
`HttpClientRedirectTargetFailure` carrying `HttpClientRedirectTargetError`; it has no mutable output
parameter and failure cannot leave stale target bytes behind. This matches Location's single
URI-reference grammar and duplicate-field warning in
[RFC 9110 Section 10.2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.2.2), while relative
path/query inheritance and dot-segment removal continue to follow
[RFC 3986 Section 5.2](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.2) and preserve the
undefined-versus-empty component distinction described by
[RFC 3986 Section 5.3](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.3).

`Http2Connection::submitRegularRequestHead()` accepts the three-alternative
`Http2RequestContent` plan instead of an independent Content-Length/END_STREAM pair and deliberately
rejects CONNECT. Only `Http2KnownLengthRequestContent` owns a length; absent and streaming content
cannot expose a synthetic zero. It and the dedicated
Standard/Extended CONNECT entries return `Http2RequestHeadSubmitResult`, atomically allocate their
own stream, and enforce the peer's concurrent-stream limit inside the protocol core; owners never
preallocate idle streams or duplicate that gate. Callers branch on `submitted()` or `failure()`:
only `Http2SubmittedRequestHead` has `streamId()`, and only
`Http2RequestHeadSubmitFailure` has `error()`. Extended CONNECT is additionally gated by
`peerExtendedConnectEnabled()`, and neither CONNECT form permits tunnel DATA before a successful
response. Per-stream CONNECT progress is one allocation-free `Http2TunnelState`, exactly one of
`Http2NotConnect`, `Http2ConnectPending`, `Http2TunnelOpen`, or `Http2ConnectRejected`. Only the
pending alternative exposes `Http2ConnectForm`; after acceptance the retained `:protocol` is the
authoritative Extended CONNECT signal, while rejection resumes ordinary HTTP response semantics.
There is no independent kind/phase product and `Http2StreamState` exposes only one const `tunnel()`
view instead of forwarding phase/form booleans. `beginStandardConnect()` or
`beginExtendedConnect()` can transition only from not-connect, and only pending can be accepted or
rejected. This models the 2xx-to-tunnel boundary and connected-stream frame restrictions in
[RFC 9113 Section 8.5](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.5), plus Extended CONNECT's
`:protocol` contract in
[RFC 8441 Section 4](https://www.rfc-editor.org/rfc/rfc8441.html#section-4).
`beginConnection()` idempotently emits the role-correct preface and advertises the local
`Http2LocalSettings`; `feed()` is a zero-consumption retry boundary until startup, and the peer
preface completes only on an initial non-ACK SETTINGS frame. Inbound GOAWAY closes every unprocessed
higher-numbered request inside the core and reports it through `Http2RequestUnprocessedEvent`;
external runtimes decide whether and when to replay that typed safe-retry event, but do not
reconstruct stream ownership from the raw frame. Inbound Extended CONNECT is enabled only after the
local advertisement and peer preface are established. The library contains no client transport.
`ruvia::web` does not provide a socket/TLS client runtime, `fetch`, or reverse-proxy integration;
applications that need outbound HTTP drive the protocol API from their own I/O runtime.

Inbound byte ownership is one direct `Http2FeedResult` enum, never a
`Http2FeedStatus + consumed` tuple. `kConnectionNotStarted` retains the exact span for retry after
`beginConnection()`, and `kEventsPending` retains it until `nextEvent()` has drained the prior
zero-copy events. `kAccepted` and `kNeedInput` both mean that the core accepted the whole span; the
latter only reports a buffered partial connection preface or frame. `kProtocolFailure` is terminal,
so the current span is dropped and never retried. These boundaries follow the connection preface in
[RFC 9113 Section 3.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4) and the fixed nine-octet
frame header plus variable payload in
[RFC 9113 Section 4.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-4.1). The Web driver drains
events and retries the same span only for `kEventsPending`; it neither computes a consumed offset nor
re-queries `connectionError()` to reinterpret the authoritative feed result.

## Build Options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build the standalone `ruvia-core` runtime target. |
| `RUVIA_BUILD_HTTP` | `ON` | Build the standalone, core-free `ruvia-http` protocol target. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia-web`; requires both core and HTTP targets. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit and smoke tests. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Build MariaDB-compatible DB integration into `ruvia-web`. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Build Redis integration into `ruvia-web`. |
| `RUVIA_ENABLE_JWT` | `OFF` | Build JWT helpers into `ruvia-web`. |

The outbound HTTP client protocol core is a `ruvia-http` capability with no separate build switch. Applications supply the transport implementation for outbound requests.

CMake derives vcpkg manifest features from these switches before dependency
installation. Optional MariaDB, Redis, and JWT switches require `RUVIA_BUILD_WEB=ON`.

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- ruvia-core/
|   |-- CMakeLists.txt
|   |-- include/ruvia/core/         # core-only public/install namespace
|   `-- src/                        # flat runtime implementations
|-- ruvia-http/
|   |-- CMakeLists.txt
|   |-- include/ruvia/http/         # HTTP-only public/install namespace
|   `-- src/{body,client,http2,parser,server,websocket}/
|-- ruvia-web/
|   |-- CMakeLists.txt
|   |-- include/ruvia/web/          # Web-only public/install namespace
|   `-- src/{app,auth,db,http,redis,router,server}/
|-- examples/
|-- tests/
`-- vcpkg.json
```

Each target compiles files only from its own source directory and installs headers only
under its matching `ruvia/core`, `ruvia/http`, or `ruvia/web` namespace. Targets share
contracts only through the dependency target's installed include interface; physical
cross-target source/private-header paths and mixed install roots are rejected at configure
time and by the boundary checker.

The only local build directory is `build/`. If CMake cache or generated files become suspicious, delete `build/` and configure again. Vcpkg installation trees, CodeGraph indexes, and local agent directories are ignored.

## Verification

Common development verification:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build/install
```

Quick cleanup checks:

```powershell
git diff --check
rg -n '<stale split terms>' README.md AGENTS.md CMakeLists.txt ruvia-core ruvia-http ruvia-web tests examples
```
