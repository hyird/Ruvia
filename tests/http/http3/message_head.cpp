#include <array>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3MessageHead.h"

#include "test_harness.h"

namespace {

std::expected<ruvia::Http3MessageHead, ruvia::Http3MessageHeadError> decode(
    std::initializer_list<ruvia::Http3FieldSectionFieldView> fields,
    ruvia::Http3MessageHeadKind kind, std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    ruvia::Http3MessageHeadLimits limits = {}) {
    const std::vector<ruvia::Http3FieldSectionFieldView> fieldVector(fields);
    const auto wire = ruvia::encodeHttp3FieldSection(fieldVector, std::pmr::get_default_resource());
    if (!wire) {
        return std::unexpected(ruvia::Http3MessageHeadError::kQpackDecompressionFailed);
    }
    return ruvia::decodeHttp3MessageHead(*wire, kind, resource, limits);
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
    RUVIA_CHECK(get.has_value());
    if (get) {
        RUVIA_CHECK_EQ(get->method, "GET");
        RUVIA_CHECK_EQ(get->path, "/a?q=1");
        RUVIA_CHECK_EQ(get->headers.size(), 1U);
        RUVIA_CHECK_EQ(get->headers[0].name, "accept");
    }

    const auto post = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                 {":authority", "example.test"}, {"content-length", "3"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(post.has_value());
    if (post) {
        RUVIA_CHECK(post->contentLength == std::uint64_t{3});
    }

    const auto response = decode({{":status", "204"}, {"content-length", "0"}},
        ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK(response.has_value());
    if (response) {
        RUVIA_CHECK_EQ(response->status, 204);
    }
}

RUVIA_TEST(http3_message_head_rejects_pseudo_header_order_duplicates_and_unknown_fields) {
    const auto latePseudo = decode({{":method", "GET"}, {":scheme", "https"}, {"x", "y"},
                                       {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!latePseudo && latePseudo.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto duplicate = decode({{":method", "GET"}, {":method", "POST"}, {":scheme", "https"},
                                      {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!duplicate && duplicate.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto unknown = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                    {":unknown", "x"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!unknown && unknown.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto uppercase = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                      {"X-Test", "value"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!uppercase && uppercase.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto invalidPath = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "relative"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!invalidPath && invalidPath.error() == ruvia::Http3MessageHeadError::kMessageError);
}

RUVIA_TEST(http3_message_head_rejects_host_field_content_length_and_status_errors) {
    const auto hostMismatch = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                         {":authority", "one.test"}, {"host", "two.test"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!hostMismatch);
    const auto connectionField = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                            {"connection", "close"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!connectionField);
    const auto badTe = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"te", "gzip"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!badTe);
    const auto conflictingLength = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                              {"content-length", "2"}, {"content-length", "3"}},
        ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!conflictingLength);
    const auto badStatus = decode({{":status", "099"}}, ruvia::Http3MessageHeadKind::kResponse);
    RUVIA_CHECK(!badStatus);
}

RUVIA_TEST(http3_message_head_checks_origin_authority_connect_and_te_direction) {
    using ruvia::Http3MessageHeadKind;
    const auto options = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "*"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(options.has_value());
    const auto customScheme = decode({{":method", "GET"}, {":scheme", "custom"},
                                         {":authority", "example.test"}, {":path", "/"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(customScheme.has_value());
    const auto badPath = decode({{":method", "GET"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/bad\\path"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!badPath && badPath.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto badAuthority = decode({{":method", "GET"}, {":scheme", "https"},
                                         {":authority", "example.test:not-a-port"}, {":path", "/"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!badAuthority && badAuthority.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto tunnel = decode({{":method", "CONNECT"}, {":authority", "example.test:443"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(tunnel.has_value());
    const auto missingPort = decode({{":method", "CONNECT"}, {":authority", "example.test"}},
        Http3MessageHeadKind::kRequest);
    RUVIA_CHECK(!missingPort && missingPort.error() == ruvia::Http3MessageHeadError::kMessageError);
    const auto responseTe = decode({{":status", "200"}, {"te", "trailers"}},
        Http3MessageHeadKind::kResponse);
    RUVIA_CHECK(!responseTe && responseTe.error() == ruvia::Http3MessageHeadError::kMessageError);
}

RUVIA_TEST(http3_message_head_applies_rfc_field_section_size_and_propagates_resource_exceptions) {
    const auto oversized = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest, std::pmr::get_default_resource(), {40, 16});
    RUVIA_CHECK(!oversized && oversized.error() == ruvia::Http3MessageHeadError::kFieldSectionTooLarge);
    const auto oversizedWire = decode({{":method", "GET"}, {":scheme", "https"},
                                          {":authority", "example.test"}, {":path", "/"}},
        ruvia::Http3MessageHeadKind::kRequest, std::pmr::get_default_resource(),
        {.maxEncodedBytes = 2});
    RUVIA_CHECK(!oversizedWire && oversizedWire.error() ==
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
