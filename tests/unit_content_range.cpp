#include "test_harness.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <concepts>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/StaticFileMetadata.h"
#include "ruvia/web/detail/StaticFilesInternal.h"
#include "ruvia/web/detail/server/HttpFileOpen.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::detail::setResponseAllowHeader;
using ruvia::detail::setResponseContentRange;
using ruvia::detail::setResponseContentRangeUnsatisfied;

constexpr std::uint32_t methodBit(HttpKnownMethod method) {
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
    RUVIA_CHECK_EQ(whole.header("Content-Range").value_or(""), std::string_view("bytes 0-99/1000"));

    auto mid = makeResponse();
    setResponseContentRange(mid, 500, 200, 1000);
    RUVIA_CHECK_EQ(mid.header("Content-Range").value_or(""), std::string_view("bytes 500-699/1000"));

    // A single-byte range.
    auto one = makeResponse();
    setResponseContentRange(one, 0, 1, 1);
    RUVIA_CHECK_EQ(one.header("Content-Range").value_or(""), std::string_view("bytes 0-0/1"));

    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    auto boundary = makeResponse();
    setResponseContentRange(boundary, maximum - 1, 1, maximum);
    RUVIA_CHECK_EQ(
        boundary.header("Content-Range").value_or(""),
        std::string_view(
            "bytes 18446744073709551614-18446744073709551614/"
            "18446744073709551615"));
}

RUVIA_TEST(content_range_rejects_out_of_bounds_and_overflow) {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();

    for (const auto values : {
             std::array<std::uint64_t, 3>{maximum, 2, maximum},
             std::array<std::uint64_t, 3>{10, 1, 10},
             std::array<std::uint64_t, 3>{11, 1, 10}}) {
        auto response = makeResponse();
        bool rejected = false;
        try {
            setResponseContentRange(
                response, values[0], values[1], values[2]);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(!response.header("Content-Range").has_value());
    }
}

RUVIA_TEST(static_file_response_owns_path_after_handler_local_root_is_destroyed) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_local_root";
    fs::create_directories(dir);
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << "owned-static-path";
    }

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    auto response = [&] {
        ruvia::StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        ruvia::StaticRoot handlerLocalRoot(dir, std::move(options));
        return context.staticFile(handlerLocalRoot, "payload.txt", "text/plain");
    }();

    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        auto input = ruvia::detail::openResponseFileInput(*file);
        std::string body(std::string("owned-static-path").size(), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(
            input.gcount(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(body, std::string("owned-static-path"));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_replacement_cannot_reuse_indexed_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() /
        "ruvia_static_replacement_identity";
    const auto servedPath = dir / "payload.txt";
    const auto replacementPath = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(servedPath, std::ios::binary);
        output << "old-representation";
    }

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot root(dir, std::move(options));

    // Use the same byte length so a size-only guard would accept and transmit
    // the replacement under the old ETag/Last-Modified framing.
    {
        std::ofstream output(replacementPath, std::ios::binary);
        output << "new-representation";
    }
    static_assert(
        std::string_view("old-representation").size() ==
        std::string_view("new-representation").size());
#if defined(_WIN32)
    fs::remove(servedPath);
#endif
    fs::rename(replacementPath, servedPath);

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);
    auto response = context.staticFile(root, "payload.txt", "text/plain");
    const std::string oldEtag(response.header("ETag").value_or(""));
    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requiresValidation());
        auto input = ruvia::detail::openResponseFileInput(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
        char byte = '\0';
        input.read(&byte, 1);
        RUVIA_CHECK_EQ(input.gcount(), std::streamsize{0});
    }

    ruvia::StaticRootOptions refreshedOptions;
    refreshedOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot refreshedRoot(dir, std::move(refreshedOptions));
    auto refreshed = context.staticFile(
        refreshedRoot, "payload.txt", "text/plain");
    RUVIA_CHECK(!oldEtag.empty());
    RUVIA_CHECK(refreshed.header("ETag").value_or("") != oldEtag);

    fs::remove_all(dir);
}

RUVIA_TEST(context_file_replacement_cannot_reuse_response_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() /
        "ruvia_context_file_replacement_identity";
    const auto servedPath = dir / "payload.txt";
    const auto replacementPath = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(servedPath, std::ios::binary);
        output << "old-context-body";
    }

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);
    auto response = context.file(servedPath, "text/plain");

    {
        std::ofstream output(replacementPath, std::ios::binary);
        output << "new-context-body";
    }
#if defined(_WIN32)
    fs::remove(servedPath);
#endif
    fs::rename(replacementPath, servedPath);

    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requiresValidation());
        auto input = ruvia::detail::openResponseFileInput(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(content_range_formats_unsatisfied) {
    // 416 Range Not Satisfiable advertises the total with an unknown range.
    auto response = makeResponse();
    setResponseContentRangeUnsatisfied(response, 1000);
    RUVIA_CHECK_EQ(response.header("Content-Range").value_or(""), std::string_view("bytes */1000"));
}

RUVIA_TEST(allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = makeResponse();
    setResponseAllowHeader(many, methodBit(HttpKnownMethod::kGet) | methodBit(HttpKnownMethod::kPost) |
                                     methodBit(HttpKnownMethod::kHead));
    RUVIA_CHECK_EQ(many.header("Allow").value_or(""), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = makeResponse();
    setResponseAllowHeader(one, methodBit(HttpKnownMethod::kDelete));
    RUVIA_CHECK_EQ(one.header("Allow").value_or(""), std::string_view("DELETE"));
}

RUVIA_TEST(static_file_type_policy_has_closed_exact_alternatives) {
    static_assert(!std::default_initializable<ruvia::StaticFileTypePolicy>);
    static_assert(!std::is_aggregate_v<ruvia::StaticFileTypePolicy>);

    bool emptyOnlyThrew = false;
    try {
        (void)ruvia::StaticFileTypePolicy::only({});
    } catch (const std::invalid_argument&) {
        emptyOnlyThrew = true;
    }
    RUVIA_CHECK(emptyOnlyThrew);

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_file_type_policy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "html";
    std::ofstream(dir / "asset.custom") << "custom";

    ruvia::StaticRoot defaultRoot(dir);
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(defaultRoot, "index.html").has_value());
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::find(defaultRoot, "asset.custom").has_value());

    ruvia::StaticRootOptions allOptions;
    allOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot allRoot(dir, std::move(allOptions));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(allRoot, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(allRoot, "asset.custom").has_value());

    ruvia::StaticRootOptions onlyOptions;
    onlyOptions.fileTypes = ruvia::StaticFileTypePolicy::only({".CUSTOM"});
    ruvia::StaticRoot onlyRoot(dir, std::move(onlyOptions));
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::find(onlyRoot, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(onlyRoot, "asset.custom").has_value());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_extension_preserves_unicode_without_ascii_aliasing) {
    std::pmr::monotonic_buffer_resource resource;
    const auto extension = ruvia::detail::lowerStaticFileExtension(
        std::filesystem::path(u8"asset.\u0168TML"), &resource);

    RUVIA_CHECK_EQ(
        std::string_view(extension),
        std::string_view(reinterpret_cast<const char*>(u8".\u0168tml")));
    RUVIA_CHECK(extension != ".html");
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
    {
        std::ofstream out(dir / "empty.txt", std::ios::binary | std::ios::trunc);
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serveFile = [&root](
        std::string_view path,
        std::string_view range) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!range.empty()) {
            HttpRequestAccess::addHeader(
                request,
                HttpHeaderView{"Range", range},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, path, "text/plain");
        // Copy out before the request arena unwinds.
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("Content-Range").value_or("")));
    };
    const auto serve = [&serveFile](std::string_view range) {
        return serveFile("data.txt", range);
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

    // Range units are case-insensitive (RFC 9110 §14.1); this is still a 206.
    const auto caseInsensitiveUnit = serve("Bytes=5-9");
    RUVIA_CHECK_EQ(caseInsensitiveUnit.first, std::uint16_t{206});
    RUVIA_CHECK_EQ(caseInsensitiveUnit.second, std::string("bytes 5-9/100"));

    // This server uses RFC 9110 §14.2's permitted ignore policy for a selected
    // representation with no content, avoiding an invalid zero-length 206 range.
    const auto empty = serveFile("empty.txt", "bytes=0-0");
    RUVIA_CHECK_EQ(empty.first, std::uint16_t{200});
    RUVIA_CHECK(empty.second.empty());

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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view path) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    // No sidecar and no Accept-Encoding -> the identity file is served, but it must
    // STILL declare Vary: Accept-Encoding: the same URL would serve a compressed
    // variant to a capable client, so a shared cache keyed only on the URL must not
    // reuse this identity body for everyone (RFC 9110 12.5.5 / RFC 9111 4.1). The
    // identity body carries no Content-Encoding.
    const auto served = context.staticFile(root, "app.js", "text/javascript");
    RUVIA_CHECK_EQ(served.status(), std::uint16_t{200});
    RUVIA_CHECK(served.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos);
    RUVIA_CHECK(!served.header("Content-Encoding").has_value());

    // Context::file serves a single path with no encoding negotiation, so it must
    // NOT declare Vary: Accept-Encoding (which would needlessly fragment caches).
    const auto direct = context.file(filePath);
    RUVIA_CHECK_EQ(direct.status(), std::uint16_t{200});
    RUVIA_CHECK(!direct.header("Vary").has_value());
    RUVIA_CHECK_EQ(
        direct.header("Content-Type").value_or(""),
        std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK(ruvia::detail::responseBody(direct).ownedFile() != nullptr);

    fs::remove_all(dir);
}

RUVIA_TEST(sse_stream_head_defaults_cache_control_but_honors_a_caller_value) {
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::prepareResponseStreamHead;
    using ruvia::detail::ResponseTrailerIntent;
    using ruvia::detail::ResponseStreamFraming;
    using ruvia::detail::ResponseStreamKind;

    const auto head = [](bool presetNoCache) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        if (presetNoCache) {
            ContextAccess::setResponseHeader(context, "Cache-Control", "no-cache");
        }
        auto response = ContextAccess::streamingHead(context);
        auto streamHead = prepareResponseStreamHead(
            std::move(response),
            ResponseStreamKind::kSse,
            ruvia::detail::httpResponseStreamCommitPlan(
                ResponseStreamFraming::kHttp1Chunked,
                HttpKnownMethod::kGet,
                200,
                ResponseTrailerIntent::kNone));
        return std::string(streamHead.response().header("Cache-Control").value_or(""));
    };

    // With no caller value, an SSE stream defaults to no-store so the event stream
    // is never cached.
    RUVIA_CHECK_EQ(head(false), std::string("no-store"));
    // A handler that set its own Cache-Control -- e.g. the recommended SSE
    // "no-cache" -- must have it preserved, not clobbered with no-store.
    RUVIA_CHECK_EQ(head(true), std::string("no-cache"));
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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    // Serve with a Range plus an optional If-Range; returns (status, Last-Modified).
    const auto serve = [&root](std::optional<std::string_view> ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(
            request, HttpHeaderView{"Range", "bytes=0-4"},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (ifRange.has_value()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{"If-Range", *ifRange},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto ctx = ContextAccess::make(memory, request);
        const auto response = ctx.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("Last-Modified").value_or("")));
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve(std::nullopt);
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

    // A present empty If-Range is not an entity-tag or HTTP-date. Its condition
    // is therefore false, so it must suppress the Range rather than being
    // confused with an absent field and producing a partial response.
    RUVIA_CHECK_EQ(serve(std::string_view{}).first, std::uint16_t{200});

    // If-Range date NEWER than Last-Modified: the file's mtime is older, so it is a
    // DIFFERENT representation than the client holds. RFC 9110 §13.1.5 requires an
    // exact match, so the range MUST be refused and the full 200 served. (The old
    // "<=" comparison wrongly returned 206 here -- the corruption path.)
    RUVIA_CHECK_EQ(serve(fmt(modified + 86400)).first, std::uint16_t{200});

    // If-Range date OLDER than Last-Modified: representation has since changed -> 200.
    RUVIA_CHECK_EQ(serve(fmt(modified - 86400)).first, std::uint16_t{200});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_clamps_future_last_modified_and_rejects_it_for_if_range) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_future_mtime_dir";
    fs::create_directories(dir);
    const auto path = dir / "data.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "future";
    }
    std::error_code ec;
    fs::last_write_time(
        path,
        fs::file_time_type::clock::now() + std::chrono::hours(24),
        ec);
    RUVIA_CHECK(!ec);

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::optional<std::string_view> ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(
            request, HttpHeaderView{"Range", "bytes=0-1"},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (ifRange.has_value()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{"If-Range", *ifRange},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::uint16_t, std::string>(
            response.status(),
            std::string(response.header("Last-Modified").value_or("")));
    };

    const auto before = std::time(nullptr);
    const auto base = serve(std::nullopt);
    const auto after = std::time(nullptr);
    RUVIA_CHECK_EQ(base.first, std::uint16_t{206});
    const auto lastModified = ruvia::detail::httpParseHttpDate(base.second);
    RUVIA_CHECK(lastModified.has_value());
    RUVIA_CHECK(lastModified.value_or(after + 1) >= before);
    RUVIA_CHECK(lastModified.value_or(after + 1) <= after);

    // The clamped wire date is the response time, not the file's actual
    // validator. It is therefore weak and cannot authorize stitching a partial
    // response into the client's stored representation.
    RUVIA_CHECK_EQ(serve(base.second).first, std::uint16_t{200});

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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    options.enableRanges = true;
    options.enableValidators = false;  // no ETag / Last-Modified on responses
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
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
        HttpRequestAccess::setMethod(request, "GET");
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
            etag.assign(response.header("ETag").value_or(""));
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

    // Presence is distinct from a non-empty field value. The empty #entity-tag
    // list matches no current representation, so an empty If-Match fails rather
    // than being treated as if the field were absent.
    RUVIA_CHECK_EQ(
        serve({{RequestKnownHeader::kIfMatch, "If-Match", ""}}).first,
        std::uint16_t{412});

    constexpr std::string_view kFutureDate =
        "Fri, 31 Dec 9999 23:59:59 GMT";
    RUVIA_CHECK_EQ(
        serve({{RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", kFutureDate}}).first,
        std::uint16_t{304});
    // Even an empty If-None-Match is present and therefore takes precedence over
    // If-Modified-Since. Its empty list does not match, so the response is 200.
    RUVIA_CHECK_EQ(
        serve({{RequestKnownHeader::kIfNoneMatch, "If-None-Match", ""},
               {RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", kFutureDate}}).first,
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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](
        ruvia::detail::RequestKnownHeader slot, std::string_view headerName, std::string_view headerValue) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!headerName.empty()) {
            HttpRequestAccess::addHeader(
                request, HttpHeaderView{headerName, headerValue}, HttpRequestAccess::knownHeaderSlot(slot));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<std::uint16_t, std::string>(
            response.status(), std::string(response.header("ETag").value_or("")));
    };

    // An unconditional GET yields 200 and a strong ETag validator.
    const auto plain = serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "", "");
    RUVIA_CHECK_EQ(plain.first, std::uint16_t{200});
    RUVIA_CHECK(!plain.second.empty());
    const std::string etag = plain.second;

    // If-None-Match with the current ETag -> 304; a stale one falls through to 200.
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", etag).first, std::uint16_t{304});
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", "\"stale\"").first, std::uint16_t{200});

    // A comma inside an opaque tag is data, not a list separator. This malformed
    // value closes that tag immediately before the current ETag and must not let
    // the apparent suffix satisfy the condition.
    const std::string malformedList = std::string("\"stale, ") + etag;
    RUVIA_CHECK_EQ(
        serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", malformedList).first,
        std::uint16_t{200});

    // If-Match against a non-matching ETag is a 412 precondition failure (thrown).
    bool precondition = false;
    try {
        (void)serve(ruvia::detail::RequestKnownHeader::kIfMatch, "If-Match", "\"stale\"");
    } catch (const ruvia::HttpError& error) {
        precondition = error.info().status() == 412;
    }
    RUVIA_CHECK(precondition);

    precondition = false;
    try {
        (void)serve(ruvia::detail::RequestKnownHeader::kIfMatch, "If-Match", malformedList);
    } catch (const ruvia::HttpError& error) {
        precondition = error.info().status() == 412;
    }
    RUVIA_CHECK(precondition);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_selects_precompressed_representation_atomically) {
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
    {
        std::ofstream out(dir / "data.txt.br", std::ios::binary | std::ios::trunc);
        const std::string content(30, 'b');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "data.txt.zst", std::ios::binary | std::ios::trunc);
        const std::string content(40, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    struct ServedRepresentation final {
        std::string contentEncoding;
        std::string vary;
        std::uint64_t size{0};
    };
    const auto serve = [&root](std::string_view acceptEncoding) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!acceptEncoding.empty()) {
            HttpRequestAccess::addHeader(
                request,
                HttpHeaderView{"Accept-Encoding", acceptEncoding},
                HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        const auto file = ruvia::detail::responseBody(response).file();
        return ServedRepresentation{
            .contentEncoding = std::string(
                response.header("Content-Encoding").value_or("")),
            .vary = std::string(response.header("Vary").value_or("")),
            .size = file.has_value() ? file->length() : 0};
    };

    // Accept-Encoding: gzip with a .gz sidecar present serves the gzip variant,
    // marked Content-Encoding: gzip and Vary: Accept-Encoding so a cache keys on it.
    const auto gz = serve("gzip");
    RUVIA_CHECK_EQ(gz.contentEncoding, std::string("gzip"));
    RUVIA_CHECK_EQ(gz.size, std::uint64_t{20});
    RUVIA_CHECK(gz.vary.find("Accept-Encoding") != std::string::npos);

    const auto br = serve("br");
    RUVIA_CHECK_EQ(br.contentEncoding, std::string("br"));
    RUVIA_CHECK_EQ(br.size, std::uint64_t{30});

    const auto zstd = serve("zstd");
    RUVIA_CHECK_EQ(zstd.contentEncoding, std::string("zstd"));
    RUVIA_CHECK_EQ(zstd.size, std::uint64_t{40});

    // Equal quality uses the one canonical server preference order.
    const auto tied = serve("gzip, zstd, br");
    RUVIA_CHECK_EQ(tied.contentEncoding, std::string("br"));
    RUVIA_CHECK_EQ(tied.size, std::uint64_t{30});

    const auto prefersGzip = serve("gzip;q=1, br;q=0.5, zstd;q=0.25");
    RUVIA_CHECK_EQ(prefersGzip.contentEncoding, std::string("gzip"));
    RUVIA_CHECK_EQ(prefersGzip.size, std::uint64_t{20});

    // identity is implicitly q=1. A lower-quality gzip preference must leave
    // the original representation selected even when a sidecar exists.
    const auto prefersIdentity = serve("gzip;q=0.5");
    RUVIA_CHECK(prefersIdentity.contentEncoding.empty());
    RUVIA_CHECK_EQ(prefersIdentity.size, std::uint64_t{100});

    // Without Accept-Encoding the plain file is served, with no Content-Encoding.
    const auto plain = serve("");
    RUVIA_CHECK(plain.contentEncoding.empty());
    RUVIA_CHECK_EQ(plain.size, std::uint64_t{100});

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_empty_custom_mime_type) {
    namespace fs = std::filesystem;
    using ruvia::StaticMimeType;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;

    const auto dir =
        fs::temp_directory_path() / "ruvia_static_empty_mime_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(
            dir / "data.custom",
            std::ios::binary | std::ios::trunc);
        out << "content";
    }

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticMimeType mime;
    mime.extension = ".custom";
    options.mimeTypes.push_back(std::move(mime));

    bool rejected = false;
    try {
        StaticRoot root(dir, std::move(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
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
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifModifiedSince) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
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
        HttpRequestAccess::setMethod(request, "GET");
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
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), std::uint16_t{403});
    }

    // With an index file configured (and present), the directory root serves it.
    {
        std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
        out << "<h1>i</h1>";
        out.close();
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        options.indexFile = "index.html";
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), std::uint16_t{200});
    }

    fs::remove_all(dir);
}
