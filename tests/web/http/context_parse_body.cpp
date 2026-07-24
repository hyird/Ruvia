#include "context_request_fixture.h"

// Parsing a request body into form data: urlencoded and multipart, dotted names, arrays and the
// limits on both.

RUVIA_TEST(context_parse_body_drops_prototype_pollution_keys) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "__proto__.evil=1&safe=ok");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // With dot-path parsing on, a field whose name traverses "__proto__." is
    // dropped (prototype-pollution defense for the nested-object binding) while a
    // benign sibling survives.
    asio::io_context& io = ruvia::test::newTestIoContext();
    bool safeOk = false;
    bool protoDropped = false;
    asio::co_spawn(io, parseProtoBody(context, safeOk, protoDropped), asio::detached);
    io.run();

    RUVIA_CHECK(safeOk);
    RUVIA_CHECK(protoDropped);
}

RUVIA_TEST(context_parse_body_groups_arrays_and_compacts_repeated_scalars) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "tags[]=a&tags[]=b&x=1&x=2");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // With the default (non-.all) options, a "[]" field keeps every value (an
    // array) while a repeated scalar field is compacted to its last value.
    asio::io_context& io = ruvia::test::newTestIoContext();
    std::size_t tagsSize = 0;
    bool tagsArray = false;
    std::size_t xSize = 0;
    std::string xValue;
    asio::co_spawn(io, parseArrayForm(context, tagsSize, tagsArray, xSize, xValue), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(tagsSize, std::size_t{2});  // both array elements kept
    RUVIA_CHECK(tagsArray);                    // flagged as an array
    RUVIA_CHECK_EQ(xSize, std::size_t{1});     // repeated scalar compacted to one
    RUVIA_CHECK_EQ(xValue, std::string("2"));  // last value wins
}

RUVIA_TEST(context_parse_body_defaults_absent_part_content_type) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "multipart/form-data; boundary=BOUNDARY"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    // The "upload" part carries no Content-Type header.
    HttpRequestAccess::setBody(request,
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"note.txt\"\r\n"
        "\r\n"
        "hello\r\n"
        "--BOUNDARY--\r\n");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context& io = ruvia::test::newTestIoContext();
    std::string contentType;
    asio::co_spawn(io, parsePartContentType(context, contentType), asio::detached);
    io.run();

    // RFC 7578 4.4: an absent part Content-Type defaults to text/plain.
    RUVIA_CHECK_EQ(contentType, std::string("text/plain"));
}

RUVIA_TEST(context_parse_body_keeps_every_repeated_file_part) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "multipart/form-data; boundary=BOUNDARY"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    // A standard <input type=file name="photos" multiple> emits several parts
    // under one non-"[]" name; the default last-value policy must not collapse
    // them and silently drop uploads.
    HttpRequestAccess::setBody(request,
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"photos\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "AAA\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"photos\"; filename=\"b.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "BBB\r\n"
        "--BOUNDARY--\r\n");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context& io = ruvia::test::newTestIoContext();
    std::size_t count = 0;
    std::size_t fileCount = 0;
    bool sawA = false;
    bool sawB = false;
    asio::co_spawn(io, parseRepeatedFiles(context, count, fileCount, sawA, sawB), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(count, std::size_t{2});      // both file parts retained
    RUVIA_CHECK_EQ(fileCount, std::size_t{2});  // both flagged as files
    RUVIA_CHECK(sawA && sawB);                  // neither upload dropped
}

RUVIA_TEST(context_parse_body_rejects_a_flood_of_fields) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "a=1&b=2&c=3&d=4&e=5");  // five fields

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // A body carrying more fields than maxFields is rejected with 413 before the
    // field vector can grow without bound.
    asio::io_context& io = ruvia::test::newTestIoContext();
    bool rejected = false;
    int status = 0;
    asio::co_spawn(io, parseWithFieldCap(context, 3, rejected, status), asio::detached);
    io.run();

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(status, 413);
}

RUVIA_TEST(context_parse_body_all_retains_duplicates_and_selects_last_value) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "x=first&x=last");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    std::size_t valueCount = 0;
    std::string selectedValue;
    asio::io_context& io = ruvia::test::newTestIoContext();
    asio::co_spawn(io, parseAllRepeatedScalar(context, valueCount, selectedValue), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(valueCount, std::size_t{2});
    RUVIA_CHECK_EQ(selectedValue, std::string("last"));
}

RUVIA_TEST(context_parse_body_multipart_yields_text_field_and_file_blob) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "multipart/form-data; boundary=BOUNDARY"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request,
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
    asio::io_context& io = ruvia::test::newTestIoContext();
    std::string nameValue, fileName, fileType, fileData;
    asio::co_spawn(io, parseMultipart(context, nameValue, fileName, fileType, fileData), asio::detached);
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
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request, "a=%zz");  // invalid percent-encoding

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // A malformed body must surface as an exception (the handler maps it to a 400)
    // rather than a silently-empty form or a crash.
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto future = asio::co_spawn(io, parseBodyDiscard(context), asio::use_future);
    io.run();
    bool threw = false;
    try {
        future.get();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_parse_body_maps_multipart_failure_to_http_protocol_error) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "multipart/form-data; boundary=BOUNDARY"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
    HttpRequestAccess::setBody(request,
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n\r\n"
        "truncated");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto future = asio::co_spawn(io, parseBodyDiscard(context), asio::use_future);
    io.run();
    bool mapped = false;
    try {
        future.get();
    } catch (const ruvia::HttpProtocolError& error) {
        mapped = error.status() == ruvia::http_status::kBadRequest && error.what() == std::string_view("incomplete multipart body");
    }
    RUVIA_CHECK(mapped);
}

RUVIA_TEST(context_parse_body_skips_empty_urlencoded_segments) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Type", "application/x-www-form-urlencoded"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentType));
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
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto future = asio::co_spawn(io, parseScalarPair(context, aValue, aPresent, bValue, bPresent), asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(aPresent);
    RUVIA_CHECK_EQ(aValue, std::string("1"));
    RUVIA_CHECK(bPresent);
    RUVIA_CHECK_EQ(bValue, std::string("2"));
}
