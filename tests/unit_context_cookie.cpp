#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;

asio::awaitable<void> cloneHeaderValue(ruvia::Context& context, std::string& output) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    const auto value = clone.header("X-Trace");
    output.assign(value.data(), value.size());
}

asio::awaitable<void> cloneParseProtoBody(ruvia::Context& context, bool& safeOk, bool& protoDropped) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    const auto form = clone.parseBody({.dot = true});
    const auto safe = form.get("safe").value();
    safeOk = safe.has_value() && *safe == std::string_view("ok");
    protoDropped = !static_cast<bool>(form.get("__proto__"));
}

asio::awaitable<void> cloneParseArrayForm(
    ruvia::Context& context,
    std::size_t& tagsSize,
    bool& tagsArray,
    std::size_t& xSize,
    std::string& xValue) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    const auto form = clone.parseBody({});
    const auto tags = form.get("tags[]");
    tagsSize = tags.size();
    tagsArray = tags.array();
    const auto x = form.get("x");
    xSize = x.size();
    if (const auto xv = x.value(); xv.has_value()) {
        xValue.assign(xv->data(), xv->size());
    }
}

asio::awaitable<void> cloneParseMultipart(
    ruvia::Context& context,
    std::string& nameValue,
    std::string& fileName,
    std::string& fileType,
    std::string& fileData) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    const auto form = clone.parseBody({});
    if (const auto nv = form.get("name").value(); nv.has_value()) {
        nameValue.assign(nv->data(), nv->size());
    }
    const auto file = form.get("file");
    if (const auto* f = file.field(); f != nullptr) {
        fileName.assign(f->filename().data(), f->filename().size());
    }
    if (const auto b = file.blob(); b.has_value()) {
        fileType.assign(b->type().data(), b->type().size());
        fileData.assign(b->text().data(), b->text().size());
    }
}

asio::awaitable<void> cloneParseBodyDiscard(ruvia::Context& context) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    (void)clone.parseBody({});
}

asio::awaitable<void> cloneParseScalarPair(
    ruvia::Context& context,
    std::string& aValue,
    bool& aPresent,
    std::string& bValue,
    bool& bPresent) {
    auto clone = co_await ruvia::detail::taskAsAwaitable(ruvia::cloneRawRequest(context.req()));
    const auto form = clone.parseBody({});
    if (const auto v = form.get("a").value(); v.has_value()) {
        aValue.assign(v->data(), v->size());
        aPresent = true;
    }
    if (const auto v = form.get("b").value(); v.has_value()) {
        bValue.assign(v->data(), v->size());
        bPresent = true;
    }
}

}  // namespace

RUVIA_TEST(context_request_cookie_single_lookup_does_not_materialize_cookie_list) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Cookie", "a=1; b=2; a=3"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto cookie = context.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("3"));
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));
}

RUVIA_TEST(context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "b=2"}, slot));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto cookie = context.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("1"));
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));
}

RUVIA_TEST(context_request_cookie_fields_include_repeated_cookie_headers) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "b=2; a=3"}, slot));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto& cookies = ruvia::detail::requestCookieFields(context.req());
    RUVIA_CHECK_EQ(cookies.size(), std::size_t{3});
    RUVIA_CHECK_EQ(cookies[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(cookies[0].value(), std::string_view("1"));
    RUVIA_CHECK_EQ(cookies[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(cookies[1].value(), std::string_view("2"));
    RUVIA_CHECK_EQ(cookies[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(cookies[2].value(), std::string_view("3"));
    const auto latest = cookies.get("a");
    RUVIA_CHECK(latest.has_value());
    RUVIA_CHECK_EQ(*latest, std::string_view("3"));
}

RUVIA_TEST(context_request_query_single_lookup_decodes_without_materializing_query_tables) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=one+two&b=2&a=3");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto query = context.req().query("a");
    RUVIA_CHECK(query.has_value());
    RUVIA_CHECK_EQ(*query, std::string_view("3"));
    RUVIA_CHECK(!ContextAccess::requestQueryMaterialized(context));
}

RUVIA_TEST(context_request_query_list_uses_last_duplicate_like_single_lookup) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=1&b=2&a=3");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // Single-value lookup resolves a duplicate name to its LAST value.
    RUVIA_CHECK_EQ(*context.req().query("a"), std::string_view("3"));

    // The flattened query field list (used by controller field binding) must
    // agree. It previously kept the first occurrence ("1"), so a caller binding
    // fields from the list and one calling query("a") saw different values for
    // ?a=1&a=3 -- the inconsistency this pins.
    const auto& list = ruvia::detail::requestQueryFields(context.req());
    RUVIA_CHECK_EQ(list.size(), std::size_t{2});  // deduped to unique names a, b
    const auto viaList = list.get("a");
    RUVIA_CHECK(viaList.has_value());
    RUVIA_CHECK_EQ(*viaList, std::string_view("3"));
    RUVIA_CHECK_EQ(*list.get("b"), std::string_view("2"));

    // queries() still exposes every value in order (getAll semantics).
    const auto all = context.req().queries("a");
    RUVIA_CHECK(all.has_value());
    RUVIA_CHECK_EQ(all->size(), std::size_t{2});
    RUVIA_CHECK_EQ((*all)[0], std::string_view("1"));
    RUVIA_CHECK_EQ((*all)[1], std::string_view("3"));
}

RUVIA_TEST(context_request_param_single_lookup_decodes_without_materializing_param_table) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);

    const std::string_view names[] = {"unused", "id"};
    const std::string_view values[] = {"skip", "one%20two"};
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(
        requestMemory,
        request,
        "/items/:id",
        names,
        values,
        std::size(names),
        ruvia::HttpMethod::kGet,
        0,
        0);

    const auto param = context.req().param("id");
    RUVIA_CHECK(param.has_value());
    RUVIA_CHECK_EQ(*param, std::string_view("one two"));
    RUVIA_CHECK(!ContextAccess::routeParamsMaterialized(context));
}

RUVIA_TEST(raw_request_clone_header_lookup_uses_last_match) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "first"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x-trace", "second"}));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context io;
    std::string header;
    asio::co_spawn(io, cloneHeaderValue(context, header), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(header, std::string("second"));
}

RUVIA_TEST(context_clone_parse_body_drops_prototype_pollution_keys) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "__proto__.evil=1&safe=ok");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // With dot-path parsing on, a field whose name traverses "__proto__." is
    // dropped (prototype-pollution defense for the nested-object binding) while a
    // benign sibling survives.
    asio::io_context io;
    bool safeOk = false;
    bool protoDropped = false;
    asio::co_spawn(io, cloneParseProtoBody(context, safeOk, protoDropped), asio::detached);
    io.run();

    RUVIA_CHECK(safeOk);
    RUVIA_CHECK(protoDropped);
}

RUVIA_TEST(context_parse_body_groups_arrays_and_compacts_repeated_scalars) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "tags[]=a&tags[]=b&x=1&x=2");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // With the default (non-.all) options, a "[]" field keeps every value (an
    // array) while a repeated scalar field is compacted to its last value.
    asio::io_context io;
    std::size_t tagsSize = 0;
    bool tagsArray = false;
    std::size_t xSize = 0;
    std::string xValue;
    asio::co_spawn(io, cloneParseArrayForm(context, tagsSize, tagsArray, xSize, xValue), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(tagsSize, std::size_t{2});   // both array elements kept
    RUVIA_CHECK(tagsArray);                      // flagged as an array
    RUVIA_CHECK_EQ(xSize, std::size_t{1});       // repeated scalar compacted to one
    RUVIA_CHECK_EQ(xValue, std::string("2"));    // last value wins
}

RUVIA_TEST(context_parse_body_multipart_yields_text_field_and_file_blob) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Type", "multipart/form-data; boundary=BOUNDARY"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(
        request,
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"f.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello\r\n"
        "--BOUNDARY--\r\n");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // A multipart body parses into a text field plus a file part whose filename,
    // content type, and bytes are all preserved through the RequestBlob.
    asio::io_context io;
    std::string nameValue, fileName, fileType, fileData;
    asio::co_spawn(io, cloneParseMultipart(context, nameValue, fileName, fileType, fileData), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(nameValue, std::string("value"));
    RUVIA_CHECK_EQ(fileName, std::string("f.txt"));
    RUVIA_CHECK_EQ(fileType, std::string("text/plain"));
    RUVIA_CHECK_EQ(fileData, std::string("hello"));
}

RUVIA_TEST(context_parse_body_rejects_malformed_urlencoded) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "a=%zz");  // invalid percent-encoding

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // A malformed body must surface as an exception (the handler maps it to a 400)
    // rather than a silently-empty form or a crash.
    asio::io_context io;
    auto future = asio::co_spawn(io, cloneParseBodyDiscard(context), asio::use_future);
    io.run();
    bool threw = false;
    try {
        future.get();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_parse_body_skips_empty_urlencoded_segments) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    // Leading/trailing/consecutive '&' are empty segments the parser skips, yielding
    // no field. Because the field-vector reservation is sized from the delimiter
    // count, an all-'&' body would otherwise over-reserve massively; the reservation
    // is now bounded, and this pins that empty segments still parse to nothing while
    // the real fields are unaffected.
    HttpRequestAccess::setBody(request, "&&a=1&&&b=2&&");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    std::string aValue;
    std::string bValue;
    bool aPresent = false;
    bool bPresent = false;
    asio::io_context io;
    auto future = asio::co_spawn(
        io, cloneParseScalarPair(context, aValue, aPresent, bValue, bPresent), asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(aPresent);
    RUVIA_CHECK_EQ(aValue, std::string("1"));
    RUVIA_CHECK(bPresent);
    RUVIA_CHECK_EQ(bValue, std::string("2"));
}

RUVIA_TEST(context_generate_cookie_serializes_all_attributes) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // The Set-Cookie serialization is a two-pass design: prepareSetCookie computes
    // an exact byte count, then writeSetCookie fills precisely that many bytes.
    // A maximal cookie exercises every branch of BOTH passes at once, so any
    // divergence between the size computed and the bytes written (an over- or
    // under-allocation, or a dropped attribute in one pass only) surfaces as a
    // wrong output string. Case A: __Host- prefix with all string/flag attributes
    // and a Max-Age (no Domain/Expires -- __Host- forbids Domain).
    ruvia::CookieOptions host;
    host.prefix = ruvia::CookiePrefix::kHost;
    host.secure = true;
    host.path = "/";
    host.httpOnly = true;
    host.sameSite = "Strict";
    host.maxAge = 3600;
    host.priority = "High";
    host.partitioned = true;
    const auto hostCookie = context.generateCookie("id", "abc", host);
    RUVIA_CHECK_EQ(
        std::string_view(hostCookie.data(), hostCookie.size()),
        std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                         "SameSite=Strict; Priority=High; Partitioned"));

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    ruvia::CookieOptions secure;
    secure.prefix = ruvia::CookiePrefix::kSecure;
    secure.secure = true;
    secure.path = "/app";
    secure.domain = "example.com";
    secure.sameSite = "None";
    secure.expires = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890));
    const auto secureCookie = context.generateCookie("sess", "xyz", secure);
    RUVIA_CHECK_EQ(
        std::string_view(secureCookie.data(), secureCookie.size()),
        std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                         "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
}
