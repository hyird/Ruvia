#include "test_harness.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "http/HttpResponseHeaderState.h"
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
