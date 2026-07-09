#include "test_harness.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "http/ContextInternal.h"
#include "FileResponseHelpers.h"
#include "HttpRequestInternal.h"
#include "HttpResponseHeaderState.h"
#include "net/server/HttpResponseStreamHead.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/StaticFiles.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::HttpMethod;
using ruvia::HttpResponse;
using ruvia::detail::setResponseAllowHeader;
using ruvia::detail::setResponseContentRange;
using ruvia::detail::setResponseContentRangeUnsatisfied;

constexpr std::uint32_t methodBit(HttpMethod method) {
    return std::uint32_t{1} << static_cast<std::uint32_t>(method);
}

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(content_range_formats_satisfied_range) {
    // RFC 7233: bytes <first>-<last>/<total>, where last = offset + length - 1.
    auto whole = makeResponse();
    setResponseContentRange(whole, 0, 100, 1000);
    RUVIA_CHECK_EQ(whole.header("Content-Range"), std::string_view("bytes 0-99/1000"));

    auto mid = makeResponse();
    setResponseContentRange(mid, 500, 200, 1000);
    RUVIA_CHECK_EQ(mid.header("Content-Range"), std::string_view("bytes 500-699/1000"));

    // A single-byte range.
    auto one = makeResponse();
    setResponseContentRange(one, 0, 1, 1);
    RUVIA_CHECK_EQ(one.header("Content-Range"), std::string_view("bytes 0-0/1"));
}

RUVIA_TEST(content_range_formats_unsatisfied) {
    // 416 Range Not Satisfiable advertises the total with an unknown range.
    auto response = makeResponse();
    setResponseContentRangeUnsatisfied(response, 1000);
    RUVIA_CHECK_EQ(response.header("Content-Range"), std::string_view("bytes */1000"));
}

RUVIA_TEST(allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = makeResponse();
    setResponseAllowHeader(many, methodBit(HttpMethod::kGet) | methodBit(HttpMethod::kPost) |
                                     methodBit(HttpMethod::kHead));
    RUVIA_CHECK_EQ(many.header("Allow"), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = makeResponse();
    setResponseAllowHeader(one, methodBit(HttpMethod::kDelete));
    RUVIA_CHECK_EQ(one.header("Allow"), std::string_view("DELETE"));
}

RUVIA_TEST(static_file_range_serving_status_and_content_range) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_range_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view range) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        if (!range.empty()) {
            HttpRequestAccess::addHeader(
                request,
                HttpHeaderView{"Range", range},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        // Copy out before the request arena unwinds.
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("Content-Range")));
    };

    // A valid single range -> 206 with the byte range echoed.
    const auto ok = serve("bytes=0-4");
    RUVIA_CHECK_EQ(ok.first, std::uint16_t{206});
    RUVIA_CHECK_EQ(ok.second, std::string("bytes 0-4/100"));

    // Multiple ranges are not supported, so the whole file is served (RFC 7233).
    const auto multi = serve("bytes=0-9,20-29");
    RUVIA_CHECK_EQ(multi.first, std::uint16_t{200});

    // A wholly unsatisfiable range -> 416 with "bytes */size".
    const auto bad = serve("bytes=1000-2000");
    RUVIA_CHECK_EQ(bad.first, std::uint16_t{416});
    RUVIA_CHECK_EQ(bad.second, std::string("bytes */100"));

    // An unknown range unit MUST be ignored (RFC 9110 §14.2) -> full 200, not 416.
    const auto unknownUnit = serve("items=0-9");
    RUVIA_CHECK_EQ(unknownUnit.first, std::uint16_t{200});

    // A syntactically malformed byte range is likewise ignored -> full 200.
    const auto malformed = serve("bytes=abc");
    RUVIA_CHECK_EQ(malformed.first, std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_resolves_percent_encoded_name_and_stays_traversal_safe) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_pct_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "my report.txt", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view path) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        std::uint16_t status = 0;
        try {
            status = context.staticFile(root, path, "text/plain").status();
        } catch (const ruvia::HttpError& error) {
            status = error.info().status();
        }
        return status;
    };

    // "%20" resolves to the space in the real on-disk name (RFC 3986 percent
    // equivalence). Before decoding this 404'd: the raw bytes "my%20report.txt"
    // were compared against the decoded index key "my report.txt".
    RUVIA_CHECK_EQ(serve("my%20report.txt"), std::uint16_t{200});

    // Decoding must not open a traversal hole: "%2e%2e%2f" -> "../" is still
    // clamped at the root (403), and encoded separators plus dot-segments cannot
    // ascend past it either.
    RUVIA_CHECK_EQ(serve("%2e%2e%2fetc%2fpasswd"), std::uint16_t{403});
    RUVIA_CHECK_EQ(serve("sub%2f%2e%2e%2f%2e%2e%2fetc"), std::uint16_t{403});

    // A decoded NUL ("%00") cannot occur in a filename and is rejected outright.
    RUVIA_CHECK_EQ(serve("my%00report.txt"), std::uint16_t{403});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_declares_vary_accept_encoding_but_context_file_does_not) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_vary_dir";
    fs::create_directories(dir);
    const auto filePath = dir / "app.js";
    {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        const std::string content(50, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    // No sidecar and no Accept-Encoding -> the identity file is served, but it must
    // STILL declare Vary: Accept-Encoding: the same URL would serve a compressed
    // variant to a capable client, so a shared cache keyed only on the URL must not
    // reuse this identity body for everyone (RFC 9110 12.5.5 / RFC 9111 4.1). The
    // identity body carries no Content-Encoding.
    const auto served = context.staticFile(root, "app.js", "text/javascript");
    RUVIA_CHECK_EQ(served.status(), std::uint16_t{200});
    RUVIA_CHECK(served.header("Vary").find("Accept-Encoding") != std::string_view::npos);
    RUVIA_CHECK(served.header("Content-Encoding").empty());

    // Context::file serves a single path with no encoding negotiation, so it must
    // NOT declare Vary: Accept-Encoding (which would needlessly fragment caches).
    const auto direct = context.file(filePath, "text/javascript");
    RUVIA_CHECK_EQ(direct.status(), std::uint16_t{200});
    RUVIA_CHECK(direct.header("Vary").empty());

    fs::remove_all(dir);
}

RUVIA_TEST(sse_stream_head_defaults_cache_control_but_honors_a_caller_value) {
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::prepareResponseStreamHead;
    using ruvia::detail::ResponseStreamFraming;
    using ruvia::detail::ResponseStreamKind;

    const auto head = [](bool presetNoCache) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        if (presetNoCache) {
            ContextAccess::setResponseHeader(context, "Cache-Control", "no-cache");
        }
        auto streamHead = prepareResponseStreamHead(
            ContextAccess::streamingHead(context), ResponseStreamKind::kSse,
            ResponseStreamFraming::kHttp1Chunked);
        return std::string(streamHead.response().header("Cache-Control"));
    };

    // With no caller value, an SSE stream defaults to no-store so the event stream
    // is never cached.
    RUVIA_CHECK_EQ(head(false), std::string("no-store"));
    // A handler that set its own Cache-Control -- e.g. the recommended SSE
    // "no-cache" -- must have it preserved, not clobbered with no-store.
    RUVIA_CHECK_EQ(head(true), std::string("no-cache"));
}

RUVIA_TEST(http1_stream_head_framing_follows_request_version) {
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::prepareResponseStreamHead;
    using ruvia::detail::ResponseStreamFraming;
    using ruvia::detail::ResponseStreamKind;

    const auto head = [](ResponseStreamFraming framing, bool connectionWillClose) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        auto streamHead = prepareResponseStreamHead(
            ContextAccess::streamingHead(context), ResponseStreamKind::kGeneric, framing,
            connectionWillClose);
        return std::pair<std::string, std::string>(
            std::string(streamHead.response().header("Transfer-Encoding")),
            std::string(streamHead.response().header("Connection")));
    };

    // HTTP/1.1 kept-alive stream: chunked framing, no Connection: close (persistent
    // by default).
    const auto chunkedKeepAlive = head(ResponseStreamFraming::kHttp1Chunked, false);
    RUVIA_CHECK_EQ(chunkedKeepAlive.first, std::string("chunked"));
    RUVIA_CHECK(chunkedKeepAlive.second.empty());

    // HTTP/1.1 stream that will close (e.g. the per-connection request limit is
    // reached): still chunked, but the head must announce Connection: close so the
    // client does not reuse the socket the session is about to shut -- matching the
    // buffered path. The head is committed before that verdict is finalized, so it
    // is passed in.
    const auto chunkedClosing = head(ResponseStreamFraming::kHttp1Chunked, true);
    RUVIA_CHECK_EQ(chunkedClosing.first, std::string("chunked"));
    RUVIA_CHECK_EQ(chunkedClosing.second, std::string("close"));

    // HTTP/1.0 stream: RFC 9112 6.1 forbids Transfer-Encoding to a non-HTTP/1.1
    // client, so the head carries no chunked framing; the body is delimited by the
    // connection close, always announced with Connection: close.
    const auto closeDelimited = head(ResponseStreamFraming::kHttp1CloseDelimited, false);
    RUVIA_CHECK(closeDelimited.first.empty());
    RUVIA_CHECK_EQ(closeDelimited.second, std::string("close"));
}

RUVIA_TEST(static_file_if_range_date_requires_exact_match) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_if_range_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    // Serve with a Range plus an optional If-Range; returns (status, Last-Modified).
    const auto serve = [&root](std::string_view ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(
            request, HttpHeaderView{"Range", "bytes=0-4"},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (!ifRange.empty()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{"If-Range", ifRange},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto ctx = ContextAccess::make(memory, request);
        const auto response = ctx.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("Last-Modified")));
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve("");
    RUVIA_CHECK_EQ(base.first, std::uint16_t{206});
    RUVIA_CHECK(!base.second.empty());
    const auto lastModified = ruvia::detail::httpParseHttpDate(base.second);
    RUVIA_CHECK(lastModified.has_value());
    const std::time_t modified = lastModified.value_or(0);

    const auto fmt = [](std::time_t t) {
        const auto out = ruvia::detail::httpFormatDate(std::pmr::get_default_resource(), t);
        return std::string(out.data(), out.size());
    };

    // Exact match -> the representation is unchanged, so the range is honored (206).
    RUVIA_CHECK_EQ(serve(fmt(modified)).first, std::uint16_t{206});

    // If-Range date NEWER than Last-Modified: the file's mtime is older, so it is a
    // DIFFERENT representation than the client holds. RFC 9110 §13.1.5 requires an
    // exact match, so the range MUST be refused and the full 200 served. (The old
    // "<=" comparison wrongly returned 206 here -- the corruption path.)
    RUVIA_CHECK_EQ(serve(fmt(modified + 86400)).first, std::uint16_t{200});

    // If-Range date OLDER than Last-Modified: representation has since changed -> 200.
    RUVIA_CHECK_EQ(serve(fmt(modified - 86400)).first, std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_ignores_range_with_if_range_when_validators_disabled) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ifrange_novalidator_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    options.enableRanges = true;
    options.enableValidators = false;  // no ETag / Last-Modified on responses
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(
            request, HttpHeaderView{"Range", "bytes=0-4"},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (!ifRange.empty()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{"If-Range", ifRange},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto ctx = ContextAccess::make(memory, request);
        return ctx.staticFile(root, "data.txt", "text/plain").status();
    };

    // A plain range with no If-Range is still honored without validators -> 206.
    RUVIA_CHECK_EQ(serve(""), std::uint16_t{206});
    // A range WITH If-Range but no server validator cannot be confirmed, so the
    // Range MUST be ignored and the full representation served (RFC 9110 13.1.5) --
    // not a 206 stitched from bytes the client cannot verify it still holds.
    // (Gating the If-Range check on enableValidators skipped it and returned 206.)
    RUVIA_CHECK_EQ(serve("\"stale-etag\""), std::uint16_t{200});
    RUVIA_CHECK_EQ(serve("Wed, 21 Oct 2015 07:28:00 GMT"), std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_match_takes_precedence_over_if_unmodified_since) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_precedence_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    struct Header {
        RequestKnownHeader slot;
        std::string_view name;
        std::string_view value;
    };
    const auto serve = [&root](std::initializer_list<Header> headers) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        for (const auto& header : headers) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{header.name, header.value},
                HttpRequestAccess::knownHeaderSlot(header.slot));
        }
        auto context = ContextAccess::make(memory, request);
        std::uint16_t status = 0;
        std::string etag;
        try {
            const auto response = context.staticFile(root, "data.txt", "text/plain");
            status = response.status();
            etag.assign(response.header("ETag"));
        } catch (const ruvia::HttpError& error) {
            status = error.info().status();
        }
        return std::pair<std::uint16_t, std::string>(status, std::move(etag));
    };

    // Discover the current strong ETag with a bare request.
    const auto base = serve({});
    RUVIA_CHECK_EQ(base.first, std::uint16_t{200});
    const std::string etag = base.second;
    RUVIA_CHECK(!etag.empty());
    // A date well before the file's mtime -> If-Unmodified-Since fails on its own.
    constexpr std::string_view kOldDate = "Thu, 01 Jan 1970 00:00:00 GMT";

    // If-Unmodified-Since alone (stale date) is a 412 precondition failure.
    RUVIA_CHECK_EQ(
        serve({{RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", kOldDate}}).first,
        std::uint16_t{412});

    // With a matching If-Match present, RFC 9110 §13.2.2 requires If-Unmodified-Since
    // to be ignored -- the strong validator matched, so serve 200 rather than 412.
    RUVIA_CHECK_EQ(
        serve({{RequestKnownHeader::kIfMatch, "If-Match", etag},
               {RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", kOldDate}}).first,
        std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_conditional_request_serving) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_conditional_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](
        ruvia::detail::RequestKnownHeader slot, std::string_view headerName, std::string_view headerValue) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        if (!headerName.empty()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{headerName, headerValue}, HttpRequestAccess::knownHeaderSlot(slot));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("ETag")));
    };

    // An unconditional GET yields 200 and a strong ETag validator.
    const auto plain = serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "", "");
    RUVIA_CHECK_EQ(plain.first, std::uint16_t{200});
    RUVIA_CHECK(!plain.second.empty());
    const std::string etag = plain.second;

    // If-None-Match with the current ETag -> 304; a stale one falls through to 200.
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", etag).first, std::uint16_t{304});
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", "\"stale\"").first, std::uint16_t{200});

    // If-Match against a non-matching ETag is a 412 precondition failure (thrown).
    bool precondition = false;
    try {
        (void)serve(ruvia::detail::RequestKnownHeader::kIfMatch, "If-Match", "\"stale\"");
    } catch (const ruvia::HttpError& error) {
        precondition = error.info().status() == 412;
    }
    RUVIA_CHECK(precondition);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_serves_precompressed_gzip_variant) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_variant_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "data.txt.gz", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'g');  // sidecar bytes; served verbatim
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view acceptEncoding) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        if (!acceptEncoding.empty()) {
            HttpRequestAccess::addHeader(
                request,
                HttpHeaderView{"Accept-Encoding", acceptEncoding},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::string, std::string>(
            std::string(response.header("Content-Encoding")),
            std::string(response.header("Vary")));
    };

    // Accept-Encoding: gzip with a .gz sidecar present serves the gzip variant,
    // marked Content-Encoding: gzip and Vary: Accept-Encoding so a cache keys on it.
    const auto gz = serve("gzip");
    RUVIA_CHECK_EQ(gz.first, std::string("gzip"));
    RUVIA_CHECK(gz.second.find("Accept-Encoding") != std::string::npos);

    // Without Accept-Encoding the plain file is served, with no Content-Encoding.
    const auto plain = serve("");
    RUVIA_CHECK(plain.first.empty());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_modified_since_serving) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ims_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(50, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.allowAll = true;
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifModifiedSince) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(
            request,
            HttpHeaderView{"If-Modified-Since", ifModifiedSince},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfModifiedSince));
        auto context = ContextAccess::make(memory, request);
        return context.staticFile(root, "data.txt", "text/plain").status();
    };

    // The file was just written, so an If-Modified-Since far in the future means
    // "not modified since then" -> 304; one far in the past means it HAS changed
    // -> 200.
    RUVIA_CHECK_EQ(serve("Fri, 01 Jan 2100 00:00:00 GMT"), std::uint16_t{304});
    RUVIA_CHECK_EQ(serve("Sat, 01 Jan 2000 00:00:00 GMT"), std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_directory_root_index_and_403) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_dir_index";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "other.txt", std::ios::binary | std::ios::trunc);
        out << "x";
    }

    const auto serveRoot = [](StaticRoot& root) -> std::uint16_t {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, ruvia::HttpMethod::kGet);
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        try {
            return context.staticFile(root, "", "text/html").status();
        } catch (const ruvia::HttpError& error) {
            return error.info().status();
        }
    };

    // A directory root with no configured index is forbidden (never a listing).
    {
        StaticRootOptions options;
        options.allowAll = true;
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), std::uint16_t{403});
    }

    // With an index file configured (and present), the directory root serves it.
    {
        std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
        out << "<h1>i</h1>";
        out.close();
        StaticRootOptions options;
        options.allowAll = true;
        options.indexFile = "index.html";
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), std::uint16_t{200});
    }

    fs::remove_all(dir);
}
