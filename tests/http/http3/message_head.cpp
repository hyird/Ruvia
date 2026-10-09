#include <array>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3MessageHead.h"

#include "test_harness.h"

namespace {

std::variant<ruvia::Http3MessageHead, ruvia::Http3MessageHeadError> decode(
    std::initializer_list<ruvia::Http3FieldSectionFieldView> fields,
    ruvia::Http3MessageHeadKind kind, std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    ruvia::Http3MessageHeadLimits limits = {}) {
    const std::vector<ruvia::Http3FieldSectionFieldView> fieldVector(fields);
    const auto wire = ruvia::encodeHttp3FieldSection(fieldVector, std::pmr::get_default_resource());
    if ((wire.index() != 0)) {
        return ruvia::Http3MessageHeadError::kQpackDecompressionFailed;
    }
    return ruvia::decodeHttp3MessageHead(std::get<0>(wire), kind, resource, limits);
}

class FailingResource final : public std::pmr::memory_resource {
public:
    std::size_t live{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes >= 64) {
            throw std::bad_alloc();
        }
        auto* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http3_message_head_decodes_get_post_and_response) {
    const auto get = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/a?q=1"},
                                {":authority", "example.test"}, {"accept", "text/plain"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((get.index() == 0));
    if ((get.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(get).method, "GET");
        RUVIA_CHECK_EQ(std::get<0>(get).path, "/a?q=1");
        RUVIA_CHECK_EQ(std::get<0>(get).headers.size(), 1U);
        RUVIA_CHECK_EQ(std::get<0>(get).headers[0].name, "accept");
    }

    const auto post = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                 {":authority", "example.test"}, {"content-length", "3"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((post.index() == 0));
    if ((post.index() == 0)) {
        RUVIA_CHECK(std::get<0>(post).contentLength == std::uint64_t{3});
    }

    const auto response = decode({{":status", "204"}, {"content-length", "0"}},
        ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((response.index() == 0));
    if ((response.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(response).status, 204);
    }

    const auto wide = decode({{":status", "200"},
                                 {"content-length", " \t18446744073709551615, 018446744073709551615\t "},
                                 {"content-length", "18446744073709551615"}, {"x-ows", " value\t"}},
        ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((wide.index() == 0));
    if ((wide.index() == 0)) {
        RUVIA_CHECK(std::get<0>(wide).contentLength == UINT64_MAX);
        RUVIA_CHECK_EQ(std::get<0>(wide).headers.back().value, " value\t");
    }
}

RUVIA_TEST(http3_message_head_rejects_pseudo_header_order_duplicates_and_unknown_fields) {
    const auto latePseudo = decode({{":method", "GET"}, {":scheme", "https"}, {"x", "y"},
                                       {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((latePseudo.index() != 0) && std::get<1>(latePseudo) == ruvia::Http3MessageHeadError::kMessageError);
    const auto duplicate = decode({{":method", "GET"}, {":method", "POST"}, {":scheme", "https"},
                                      {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((duplicate.index() != 0) && std::get<1>(duplicate) == ruvia::Http3MessageHeadError::kMessageError);
    const auto unknown = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                    {":unknown", "x"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((unknown.index() != 0) && std::get<1>(unknown) == ruvia::Http3MessageHeadError::kMessageError);
    const auto uppercase = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                      {"X-Test", "value"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((uppercase.index() != 0) && std::get<1>(uppercase) == ruvia::Http3MessageHeadError::kMessageError);
    const auto invalidPath = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "relative"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((invalidPath.index() != 0) && std::get<1>(invalidPath) == ruvia::Http3MessageHeadError::kMessageError);
}

RUVIA_TEST(http3_message_head_field_names_accept_only_lowercase_token_bytes) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const std::size_t position : {0U, 4U, 31U, 63U}) {
        std::string name(64, 'x');
        for (unsigned byte = 0; byte < 256; ++byte) {
            name[position] = static_cast<char>(byte);
            const bool allowed = (byte >= 'a' && byte <= 'z') ||
                                 (byte >= '0' && byte <= '9') ||
                                 punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
            const auto response = decode({{":status", "200"}, {name, "value"}},
                ruvia::Http3MessageHeadKind::kResponse);
            RUVIA_CHECK_EQ((response.index() == 0), allowed);
            if (response.index() == 0) {
                RUVIA_CHECK_EQ(std::get<0>(response).headers.front().name, std::string_view(name));
            } else {
                RUVIA_CHECK(std::get<1>(response) == ruvia::Http3MessageHeadError::kMessageError);
            }
            const auto request = decode({{":method", "GET"}, {":scheme", "https"},
                                            {":authority", "example.test"}, {":path", "/"}, {name, "value"}},
                ruvia::Http3MessageHeadKind::kRequest);
            RUVIA_CHECK_EQ((request.index() == 0), allowed);
            if ((request.index() != 0)) {
                RUVIA_CHECK(std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
            }
        }
    }
}

RUVIA_TEST(http3_message_head_pseudo_names_are_case_sensitive) {
    for (const auto name : {":Method", ":Protocol", ":Scheme", ":Authority", ":Path"}) {
        const auto request = decode({{name, "GET"}, {":scheme", "https"}, {":path", "/"}},
            ruvia::Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    }
    const auto response = decode({{":Status", "200"}}, ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::Http3MessageHeadError::kMessageError);
}

RUVIA_TEST(http3_message_head_rejects_host_field_content_length_and_status_errors) {
    const auto hostMismatch = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                         {":authority", "one.test"}, {"host", "two.test"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((hostMismatch.index() != 0));
    const auto connectionField = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                            {"connection", "close"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((connectionField.index() != 0));
    const auto badTe = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"te", "gzip"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((badTe.index() != 0));
    const auto conflictingLength = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                              {"content-length", "2"}, {"content-length", "3"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((conflictingLength.index() != 0));
    for (const std::string_view value : {"", "1,", ",1", "1,,1", "1, 2", "+1", "-1", "18446744073709551616"}) {
        const auto invalid = decode({{":status", "200"}, {"content-length", value}},
            ruvia::Http3MessageHeadKind::kResponse);
        RUVIA_CHECK((invalid.index() != 0) && std::get<1>(invalid) == ruvia::Http3MessageHeadError::kMessageError);
    }
    const auto badStatus = decode({{":status", "099"}}, ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((badStatus.index() != 0));
}

RUVIA_TEST(http3_message_head_checks_origin_authority_connect_and_te_direction) {
    using ruvia::Http3MessageHeadKind;
    const auto options = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "*"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((options.index() == 0));
    const auto customScheme = decode({{":method", "GET"}, {":scheme", "custom"},
                                         {":authority", "example.test"}, {":path", "/"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((customScheme.index() == 0));
    const auto badPath = decode({{":method", "GET"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/bad\\path"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((badPath.index() != 0) && std::get<1>(badPath) == ruvia::Http3MessageHeadError::kMessageError);
    const auto badAuthority = decode({{":method", "GET"}, {":scheme", "https"},
                                         {":authority", "example.test:not-a-port"}, {":path", "/"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((badAuthority.index() != 0) && std::get<1>(badAuthority) == ruvia::Http3MessageHeadError::kMessageError);
    const auto tunnel = decode({{":method", "CONNECT"}, {":authority", "example.test:443"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((tunnel.index() == 0));
    if ((tunnel.index() == 0)) {
        RUVIA_CHECK(std::get<0>(tunnel).protocol.empty());
    }
    const auto missingPort = decode({{":method", "CONNECT"}, {":authority", "example.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((missingPort.index() != 0) && std::get<1>(missingPort) == ruvia::Http3MessageHeadError::kMessageError);
    const auto responseTe = decode({{":status", "200"}, {"te", "trailers"}},
        Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((responseTe.index() != 0) && std::get<1>(responseTe) == ruvia::Http3MessageHeadError::kMessageError);
}

RUVIA_TEST(http3_message_head_validates_extended_connect_pseudo_fields) {
    using ruvia::Http3MessageHeadKind;
    const auto websocket = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                      {":scheme", "https"}, {":authority", "example.test"},
                                      {":path", "/socket?room=one"}, {"x-end-to-end", "retained"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((websocket.index() == 0));
    if ((websocket.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(websocket).method, "CONNECT");
        RUVIA_CHECK_EQ(std::get<0>(websocket).protocol, "websocket");
        RUVIA_CHECK_EQ(std::get<0>(websocket).path, "/socket?room=one");
        RUVIA_CHECK_EQ(std::get<0>(websocket).headers.size(), 1U);
        RUVIA_CHECK_EQ(std::get<0>(websocket).headers[0].name, "x-end-to-end");
    }

    const auto extendedHostMismatch = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                                 {":scheme", "https"}, {":authority", "one.test"},
                                                 {":path", "/socket"}, {"host", "two.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((extendedHostMismatch.index() != 0));
    if ((extendedHostMismatch.index() != 0)) {
        RUVIA_CHECK(std::get<1>(extendedHostMismatch) == ruvia::Http3MessageHeadError::kMessageError);
    }

    const auto missingProtocol = decode({{":method", "CONNECT"}, {":scheme", "https"},
                                            {":authority", "example.test"}, {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((missingProtocol.index() != 0));
    const auto missingScheme = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                          {":authority", "example.test"}, {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((missingScheme.index() != 0));
    const auto missingAuthority = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                             {":scheme", "https"}, {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((missingAuthority.index() != 0));
    const auto missingPath = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                        {":scheme", "https"}, {":authority", "example.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((missingPath.index() != 0));

    const auto protocolBeforeMethod = decode({{":protocol", "websocket"}, {":method", "CONNECT"},
                                                 {":scheme", "https"}, {":authority", "example.test"},
                                                 {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    // RFC 9114 requires pseudo-fields to precede ordinary fields, but does not
    // impose an order among the pseudo-fields themselves.
    RUVIA_CHECK((protocolBeforeMethod.index() == 0));
    const auto protocolAfterRegular = decode({{":method", "CONNECT"}, {"x", "y"},
                                                 {":protocol", "websocket"}, {":scheme", "https"},
                                                 {":authority", "example.test"}, {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((protocolAfterRegular.index() != 0));
    const auto duplicateProtocol = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                              {":protocol", "websocket"}, {":scheme", "https"},
                                              {":authority", "example.test"}, {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((duplicateProtocol.index() != 0));
    const auto invalidProtocol = decode({{":method", "CONNECT"}, {":protocol", "web socket"},
                                            {":scheme", "https"}, {":authority", "example.test"},
                                            {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((invalidProtocol.index() != 0));
    const auto protocolOnGet = decode({{":method", "GET"}, {":protocol", "websocket"},
                                          {":scheme", "https"}, {":authority", "example.test"},
                                          {":path", "/socket"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((protocolOnGet.index() != 0));
    if ((protocolOnGet.index() != 0)) {
        RUVIA_CHECK(std::get<1>(protocolOnGet) == ruvia::Http3MessageHeadError::kMessageError);
    }
}

RUVIA_TEST(http3_message_head_validates_origin_and_rejects_every_duplicate_host) {
    using ruvia::Http3MessageHeadKind;
    const auto valid = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                  {":scheme", "https"}, {":authority", "example.test"},
                                  {":path", "/socket"}, {"origin", "https://example.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((valid.index() == 0));

    const auto opaque = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                   {":scheme", "https"}, {":authority", "example.test"},
                                   {":path", "/socket"}, {"origin", "null"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((opaque.index() == 0));

    const auto invalidOrigin = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                          {":scheme", "https"}, {":authority", "example.test"},
                                          {":path", "/socket"}, {"origin", "https://example.test/path"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((invalidOrigin.index() != 0));

    const auto duplicateHost = decode({{":method", "GET"}, {":scheme", "https"},
                                          {":authority", "example.test"}, {":path", "/"},
                                          {"host", "example.test"}, {"host", "example.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((duplicateHost.index() != 0));
}

RUVIA_TEST(http3_message_head_rejects_invalid_cors_preflight_fields) {
    using ruvia::Http3MessageHeadKind;
    const auto valid = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"origin", "https://app.example"},
                                  {"access-control-request-method", "POST"},
                                  {"access-control-request-headers", "x-trace, content-type"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((valid.index() == 0));

    for (const std::string_view invalid : {"x bad", "", "x-trace, bad name"}) {
        const auto request = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"},
                                        {"origin", "https://app.example"},
                                        {"access-control-request-method", "POST"},
                                        {"access-control-request-headers", invalid}},
            Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    }
    for (const std::string_view invalid : {"POST GET", ""}) {
        const auto request = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"},
                                        {"origin", "https://app.example"},
                                        {"access-control-request-method", invalid}},
            Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    }
}

RUVIA_TEST(http3_message_head_rejects_malformed_representation_fields) {
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"content-type", "application/json; charset=utf-8"},
                                  {"content-encoding", "gzip, br"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((valid.index() == 0));

    for (const auto field : {ruvia::Http3FieldSectionFieldView{"content-type", "text plain"},
             ruvia::Http3FieldSectionFieldView{"content-encoding", "gzip;q=1"}}) {
        const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"}, field},
            ruvia::Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    }
    const auto repeated = decode({{":method", "POST"}, {":scheme", "https"},
                                     {":authority", "example.test"}, {":path", "/"},
                                     {"content-type", "text/plain"},
                                     {"content-type", "application/json"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((repeated.index() != 0) && std::get<1>(repeated) == ruvia::Http3MessageHeadError::kMessageError);

    for (const auto field : {ruvia::Http3FieldSectionFieldView{"content-type", "text plain"},
             ruvia::Http3FieldSectionFieldView{"content-encoding", "gzip;q=1"}}) {
        const auto response = decode({{":status", "200"}, field},
            ruvia::Http3MessageHeadKind::kResponse);
        RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::Http3MessageHeadError::kMessageError);
    }
}

RUVIA_TEST(http3_message_head_rejects_forbidden_declared_trailer_names) {
    const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/"},
                                    {"trailer", "Content-Length"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    const auto response = decode({{":status", "200"}, {"trailer", "Content-Length"}},
        ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::Http3MessageHeadError::kMessageError);
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"trailer", "x-checksum"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((valid.index() == 0));
}

RUVIA_TEST(http3_message_head_rejects_malformed_expectation) {
    const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/"},
                                    {"expect", "foo?bar"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::Http3MessageHeadError::kMessageError);
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"expect", "100-continue"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((valid.index() == 0));
}

RUVIA_TEST(http3_message_head_applies_rfc_field_section_size_and_propagates_resource_exceptions) {
    const auto oversized = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest, std::pmr::get_default_resource(), {40, 16});
    RUVIA_CHECK((oversized.index() != 0) && std::get<1>(oversized) == ruvia::Http3MessageHeadError::kFieldSectionTooLarge);
    const auto exact = decode({{":status", "200"}}, ruvia::Http3MessageHeadKind::kResponse,
        std::pmr::get_default_resource(), {.maxFieldSectionSize = 42});
    RUVIA_CHECK((exact.index() == 0));
    for (const std::size_t limit : {std::size_t{0}, std::size_t{31}, std::size_t{41}}) {
        const auto rejected = decode({{":status", "200"}}, ruvia::Http3MessageHeadKind::kResponse,
            std::pmr::get_default_resource(), {.maxFieldSectionSize = limit});
        RUVIA_CHECK((rejected.index() != 0) && std::get<1>(rejected) == ruvia::Http3MessageHeadError::kFieldSectionTooLarge);
    }
    const auto oversizedWire = decode({{":method", "GET"}, {":scheme", "https"},
                                          {":authority", "example.test"}, {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest, std::pmr::get_default_resource(),
        {.maxEncodedBytes = 2});
    RUVIA_CHECK((oversizedWire.index() != 0) && std::get<1>(oversizedWire) ==
                                                    ruvia::Http3MessageHeadError::kFieldSectionTooLarge);

    FailingResource resource;
    const std::string longName(256, 'n');
    const std::string longValue(256, 'v');
    bool threw = false;
    try {
        (void)decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                         {longName, longValue}},
            ruvia::Http3MessageHeadKind::kRequest, &resource);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(resource.live, std::size_t{0});
}
