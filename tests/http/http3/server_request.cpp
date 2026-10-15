#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_server_request.h"

#include "test_harness.h"

namespace {

class failing_resource final : public std::pmr::memory_resource {
public:
    bool reject_{true};
    std::size_t minimum_rejected_bytes_{64};
    std::size_t live_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_ && bytes_value >= minimum_rejected_bytes_) {
            throw std::bad_alloc();
        }
        auto* value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        --live_;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t outstanding_{};
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        outstanding_ += bytes_value;
        ++allocations_;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        outstanding_ -= bytes_value;
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct request_events final {
    std::pmr::memory_resource* body_pool_;
    std::optional<ruvia::http3_server_request> owner_;
};

void receive_request(void* opaque, const ruvia::http3_connection_event& event) {
    auto& state_value = *static_cast<request_events*>(opaque);
    switch (event.kind_) {
        case ruvia::http3_connection_event_kind::request_head:
            state_value.owner_.emplace(std::move(*event.head_), state_value.body_pool_);
            break;
        case ruvia::http3_connection_event_kind::body:
            state_value.owner_->append_body(std::as_bytes(std::span(event.body_.data(), event.body_.size())));
            break;
        case ruvia::http3_connection_event_kind::message_end:
            state_value.owner_->finish_body();
            break;
        default:
            break;
    }
}

ruvia::http3_message_head make_get(std::pmr::memory_resource* resource) {
    auto encoded = ruvia::encode_http3_field_section(
        std::array<ruvia::http3_field_section_field_view, 7>{{{":method", "GET"}, {":scheme", "https"}, {":path", "/items?a=1"},
            {":authority", "example.test"}, {"x-first", "1"}, {"cookie", "a=1"},
            {"cookie", "b=2"}}},
        std::pmr::get_default_resource());
    auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded), ruvia::http3_message_head_kind::request, resource);
    return std::move(std::get<0>(decoded));
}

}  // namespace

RUVIA_TEST(http3_server_request_owns_connection_callback_head_until_message_end) {
    counting_resource protocol_resource;
    counting_resource request_resource;
    counting_resource body_pool;
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "POST"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/callback"},
        ruvia::http3_field_section_field_view{"content-length", "2"},
    };
    const auto section = ruvia::encode_http3_field_section(fields_value, &protocol_resource);
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    std::array<char, 16> head_prefix{};
    const auto prefix_size = ruvia::encode_http3_frame_header(head_prefix, 1, std::get<0>(section).size());
    RUVIA_CHECK((prefix_size.index() == 0));
    if ((prefix_size.index() != 0)) {
        return;
    }
    std::pmr::vector<char> head_wire(&protocol_resource);
    head_wire.insert(head_wire.end(), head_prefix.begin(), head_prefix.begin() + std::get<0>(prefix_size));
    head_wire.insert(head_wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());

    request_events captured_value{&body_pool, std::nullopt};
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &protocol_resource);
        const auto head_result = connection.feed(0, head_wire, false, false, receive_request, &captured_value, &request_resource);
        RUVIA_CHECK(head_result.status_ == ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK(captured_value.owner_.has_value());
        if (!captured_value.owner_) {
            return;
        }
        RUVIA_CHECK_EQ(captured_value.owner_->request().path(), "/callback");
        RUVIA_CHECK(!captured_value.owner_->body_complete());
        RUVIA_CHECK(captured_value.owner_->request().body_bytes().empty());
        std::array<char, 16> data_prefix{};
        const auto data_prefix_size = ruvia::encode_http3_frame_header(data_prefix, 0, 2);
        RUVIA_CHECK((data_prefix_size.index() == 0));
        if ((data_prefix_size.index() != 0)) {
            return;
        }
        std::pmr::vector<char> data_wire(&protocol_resource);
        data_wire.insert(data_wire.end(), data_prefix.begin(), data_prefix.begin() + std::get<0>(data_prefix_size));
        data_wire.insert(data_wire.end(), {'o', 'k'});
        RUVIA_CHECK(connection.feed(0, data_wire, true, false, receive_request, &captured_value).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK(captured_value.owner_->body_complete());
        RUVIA_CHECK_EQ(captured_value.owner_->request().body_bytes().size(), 2U);
    }
    RUVIA_CHECK_EQ(captured_value.owner_->request().method(), "POST");
    RUVIA_CHECK_EQ(captured_value.owner_->request().body_bytes().front(), std::byte{'o'});
    captured_value.owner_.reset();
    RUVIA_CHECK_EQ(request_resource.outstanding_, 0U);
    RUVIA_CHECK_EQ(body_pool.outstanding_, 0U);
}

RUVIA_TEST(http3_server_request_adapts_pseudo_fields_cookies_and_host) {
    counting_resource resource;
    const auto baseline = resource.outstanding_;
    {
        auto head = make_get(&resource);
        const std::array body{std::byte{'o'}, std::byte{'k'}};
        ruvia::http3_server_request owner_value(std::move(head), &resource);
        RUVIA_CHECK(owner_value.request().body_bytes().empty());
        owner_value.append_body(body);
        owner_value.finish_body();
        const auto& request = owner_value.request();
        RUVIA_CHECK(request.protocol_version() == ruvia::http_protocol_version::http3);
        RUVIA_CHECK(request.target_form() == ruvia::http_request_target_form::http3);
        RUVIA_CHECK_EQ(request.target(), "/items?a=1");
        RUVIA_CHECK_EQ(request.path(), "/items");
        RUVIA_CHECK_EQ(request.query_string(), "a=1");
        RUVIA_CHECK_EQ(owner_value.extended_connect_protocol(), "");
        RUVIA_CHECK_EQ(request.scheme(), "https");
        RUVIA_CHECK_EQ(request.authority(), "example.test");
        RUVIA_CHECK_EQ(request.header("cookie").value(), "a=1; b=2");
        RUVIA_CHECK_EQ(request.header("host").value(), "example.test");
        RUVIA_CHECK_EQ(request.headers()[0].name(), "x-first");
        RUVIA_CHECK_EQ(request.headers()[1].name(), "cookie");
        RUVIA_CHECK_EQ(request.headers()[2].name(), "host");
        RUVIA_CHECK_EQ(request.body_bytes().size(), 2U);
        RUVIA_CHECK(request.body_bytes().data() != body.data());
        RUVIA_CHECK(resource.outstanding_ > baseline);
    }
    RUVIA_CHECK_EQ(resource.outstanding_, baseline);
}

RUVIA_TEST(http3_server_request_adapts_standard_connect_authority_target) {
    counting_resource resource;
    auto encoded = ruvia::encode_http3_field_section(
        std::array<ruvia::http3_field_section_field_view, 2>{{{":method", "CONNECT"},
            {":authority", "example.test:443"}}},
        std::pmr::get_default_resource());
    auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded), ruvia::http3_message_head_kind::request, &resource);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() != 0)) {
        return;
    }
    ruvia::http3_server_request owner_value(std::move(std::get<0>(decoded)), &resource);
    owner_value.finish_body();
    RUVIA_CHECK_EQ(owner_value.request().target(), "example.test:443");
    RUVIA_CHECK_EQ(owner_value.request().path(), "");
    RUVIA_CHECK_EQ(owner_value.request().query_string(), "");
    RUVIA_CHECK(owner_value.request().known_method() == ruvia::http_known_method::connect);
    RUVIA_CHECK_EQ(owner_value.extended_connect_protocol(), "");
}

RUVIA_TEST(http3_server_request_preserves_extended_connect_wire_metadata) {
    counting_resource resource;
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket?channel=42"},
        ruvia::http3_field_section_field_view{"host", "example.test"},
        ruvia::http3_field_section_field_view{"x-preserved", "yes"},
        ruvia::http3_field_section_field_view{"origin", "https://example.test"},
        ruvia::http3_field_section_field_view{"authorization", "Bearer opaque"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
        ruvia::http3_field_section_field_view{"sec-websocket-protocol", "chat, superchat"},
        ruvia::http3_field_section_field_view{"sec-websocket-extensions", "permessage-deflate"},
        ruvia::http3_field_section_field_view{"cookie", "a=1"},
        ruvia::http3_field_section_field_view{"cookie", "b=2"},
    };
    const auto section = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    auto head = ruvia::decode_http3_message_head(std::get<0>(section),
        ruvia::http3_message_head_kind::request, &resource);
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() != 0)) {
        return;
    }

    ruvia::http3_server_request owner_value(std::move(std::get<0>(head)), &resource);
    owner_value.finish_body();
    const auto& request = owner_value.request();
    RUVIA_CHECK_EQ(request.method(), "CONNECT");
    RUVIA_CHECK(request.known_method() == ruvia::http_known_method::connect);
    RUVIA_CHECK_EQ(owner_value.extended_connect_protocol(), "websocket");
    RUVIA_CHECK_EQ(request.target(), "/socket?channel=42");
    RUVIA_CHECK_EQ(request.path(), "/socket");
    RUVIA_CHECK_EQ(request.query_string(), "channel=42");
    RUVIA_CHECK_EQ(request.headers().size(), 8U);
    RUVIA_CHECK_EQ(request.header("host").value(), "example.test");
    RUVIA_CHECK_EQ(request.header("x-preserved").value(), "yes");
    RUVIA_CHECK_EQ(request.header("origin").value(), "https://example.test");
    RUVIA_CHECK_EQ(request.header("authorization").value(), "Bearer opaque");
    RUVIA_CHECK_EQ(request.header("sec-websocket-version").value(), "13");
    RUVIA_CHECK_EQ(request.header("sec-websocket-protocol").value(), "chat, superchat");
    RUVIA_CHECK_EQ(
        request.header("sec-websocket-extensions").value(), "permessage-deflate");
    RUVIA_CHECK_EQ(request.header("cookie").value(), "a=1; b=2");

    const std::array other_protocol_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "other-protocol"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/tunnel"},
    };
    const auto other_section = ruvia::encode_http3_field_section(
        other_protocol_fields, std::pmr::get_default_resource());
    auto other_head = ruvia::decode_http3_message_head(std::get<0>(other_section),
        ruvia::http3_message_head_kind::request, &resource);
    RUVIA_CHECK((other_head.index() == 0));
    if ((other_head.index() == 0)) {
        ruvia::http3_server_request other(std::move(std::get<0>(other_head)), &resource);
        RUVIA_CHECK_EQ(other.extended_connect_protocol(), "other-protocol");
        RUVIA_CHECK_EQ(other.request().method(), "CONNECT");
    }
}

RUVIA_TEST(http3_server_request_repeated_lifetimes_return_allocations) {
    counting_resource resource;
    for (int i = 0; i < 20; ++i) {
        const auto baseline = resource.outstanding_;
        {
            auto head = make_get(&resource);
            ruvia::http3_server_request owner_value(std::move(head), &resource);
            owner_value.finish_body();
        }
        RUVIA_CHECK_EQ(resource.outstanding_, baseline);
    }
}

RUVIA_TEST(http3_server_request_enforces_method_requirements_before_buffering_content) {
    for (const std::string_view method : {"TRACE", "OPTIONS", "HEAD"}) {
        for (const bool content_type : {false, true}) {
            std::pmr::unsynchronized_pool_resource resource;
            std::vector<ruvia::http3_field_section_field_view> fields_value{
                {":method", method}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}};
            if (content_type) {
                fields_value.push_back({"content-type", "text/plain"});
            }
            const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
            RUVIA_CHECK((encoded.index() == 0));
            if (encoded.index() != 0) {
                continue;
            }
            auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded),
                ruvia::http3_message_head_kind::request, &resource);
            RUVIA_CHECK((decoded.index() == 0));
            if (decoded.index() != 0) {
                continue;
            }
            ruvia::http3_server_request owner_value(std::move(std::get<0>(decoded)), &resource);
            owner_value.append_body({});
            bool rejected = false;
            try {
                owner_value.append_body(std::as_bytes(std::span<const char>("x", 1)));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            const bool allowed = method == "HEAD" || (method == "OPTIONS" && content_type);
            RUVIA_CHECK(rejected != allowed);
            RUVIA_CHECK_EQ(owner_value.body_bytes(), allowed ? std::size_t{1} : std::size_t{0});
            if (allowed) {
                owner_value.finish_body();
                RUVIA_CHECK_EQ(owner_value.request().body_bytes().size(), std::size_t{1});
            } else {
                owner_value.abort_body();
            }
        }
    }
}

RUVIA_TEST(http3_server_request_preserves_length_contract_after_rejected_operations) {
    const auto make_head = [] {
        const std::array fields_value{
            ruvia::http3_field_section_field_view{":method", "POST"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/"},
            ruvia::http3_field_section_field_view{"content-length", "2"}};
        const auto encoded = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
        auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded), ruvia::http3_message_head_kind::request);
        return std::move(std::get<0>(decoded));
    };
    ruvia::http3_server_request owner_value(make_head(), std::pmr::get_default_resource());
    bool oversized = false;
    try {
        owner_value.append_body(std::as_bytes(std::span<const char>("abc", 3)));
    } catch (const std::invalid_argument&) {
        oversized = true;
    }
    RUVIA_CHECK(oversized);
    RUVIA_CHECK_EQ(owner_value.body_bytes(), std::size_t{0});
    owner_value.append_body(std::as_bytes(std::span<const char>("a", 1)));
    bool incomplete = false;
    try {
        owner_value.finish_body();
    } catch (const std::logic_error&) {
        incomplete = true;
    }
    RUVIA_CHECK(incomplete);
    RUVIA_CHECK(!owner_value.body_complete());
    RUVIA_CHECK_EQ(owner_value.body_bytes(), std::size_t{1});
    owner_value.append_body(std::as_bytes(std::span<const char>("b", 1)));
    owner_value.finish_body();
    RUVIA_CHECK(owner_value.body_complete());
    RUVIA_CHECK_EQ(owner_value.request().body_bytes().size(), std::size_t{2});
}

RUVIA_TEST(http3_server_request_extended_connect_has_no_http_body_length) {
    for (const std::string_view length : {"0", "9"}) {
        const std::array fields_value{
            ruvia::http3_field_section_field_view{":method", "CONNECT"},
            ruvia::http3_field_section_field_view{":protocol", "websocket"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/socket"},
            ruvia::http3_field_section_field_view{"content-length", length}};
        const auto encoded = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
        RUVIA_CHECK((encoded.index() == 0));
        if (encoded.index() != 0) {
            continue;
        }
        auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded), ruvia::http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if (decoded.index() != 0) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(decoded).content_length_ == (length == "0" ? 0U : 9U));
        ruvia::http3_server_request owner_value(std::move(std::get<0>(decoded)), std::pmr::get_default_resource());
        bool rejected = false;
        try {
            owner_value.append_body(std::as_bytes(std::span<const char>("x", 1)));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        owner_value.finish_body();
        RUVIA_CHECK(owner_value.body_complete());
        RUVIA_CHECK(owner_value.request().body_bytes().empty());
    }
}

RUVIA_TEST(http3_server_request_body_allocation_failure_can_retry_without_recounting) {
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "POST"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/"},
        ruvia::http3_field_section_field_view{"content-length", "128"}};
    const auto encoded = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
    auto decoded = ruvia::decode_http3_message_head(std::get<0>(encoded), ruvia::http3_message_head_kind::request);
    failing_resource body_pool;
    {
        ruvia::http3_server_request owner_value(std::move(std::get<0>(decoded)), &body_pool);
        std::array<char, 128> payload{};
        payload.fill('x');
        bool threw = false;
        try {
            owner_value.append_body(std::as_bytes(std::span(payload)));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(owner_value.body_bytes(), std::size_t{0});
        RUVIA_CHECK(!owner_value.body_complete());
        body_pool.reject_ = false;
        owner_value.append_body(std::as_bytes(std::span(payload)));
        owner_value.finish_body();
        RUVIA_CHECK(owner_value.body_complete());
        RUVIA_CHECK_EQ(owner_value.request().body_bytes().size(), payload.size());
    }
    RUVIA_CHECK_EQ(body_pool.live_, std::size_t{0});
}

RUVIA_TEST(http3_server_request_rejects_header_limit_and_cleans_partial_state) {
    counting_resource resource;
    const auto baseline = resource.outstanding_;
    {
        ruvia::http3_message_head head(&resource);
        head.method_ = "GET";
        head.scheme_ = "https";
        head.authority_ = "example.test";
        head.path_ = "/";
        for (std::size_t i = 0; i < ruvia::max_http_header_fields; ++i) {
            head.headers_.push_back(ruvia::http_header::copy_of("x-test", "v", &resource));
        }
        bool threw = false;
        try {
            ruvia::http3_server_request owner_value(std::move(head), &resource);
        } catch (const std::length_error&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    RUVIA_CHECK_EQ(resource.outstanding_, baseline);
}

RUVIA_TEST(http3_server_request_allocation_exception_propagates) {
    failing_resource failing;
    bool threw = false;
    {
        failing.reject_ = false;
        auto head = make_get(&failing);
        failing.reject_ = true;
        try {
            ruvia::http3_server_request owner_value(std::move(head), std::pmr::get_default_resource());
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(failing.live_, std::size_t{0});
}

RUVIA_TEST(http3_server_request_unstarted_owner_destruction_releases_resources) {
    counting_resource request_resource;
    counting_resource body_pool;
    const auto request_baseline = request_resource.outstanding_;
    const auto pool_baseline = body_pool.outstanding_;
    {
        auto head = make_get(&request_resource);
        ruvia::http3_server_request owner_value(std::move(head), &body_pool);
        RUVIA_CHECK(!owner_value.body_complete());
        RUVIA_CHECK(owner_value.request().body_bytes().empty());
        RUVIA_CHECK(request_resource.outstanding_ > request_baseline);
    }
    RUVIA_CHECK_EQ(request_resource.outstanding_, request_baseline);
    RUVIA_CHECK_EQ(body_pool.outstanding_, pool_baseline);
}

RUVIA_TEST(http3_server_request_head_owner_and_incremental_body_lifetimes) {
    counting_resource request_resource;
    counting_resource pool;
    const auto pool_baseline = pool.outstanding_;
    std::optional<ruvia::http3_server_request> owner;
    {
        auto temporary = make_get(&request_resource);
        owner.emplace(std::move(temporary), &pool);
    }
    const auto& request = owner->request();
    RUVIA_CHECK_EQ(request.method(), "GET");
    RUVIA_CHECK_EQ(request.path(), "/items");
    RUVIA_CHECK_EQ(request.authority(), "example.test");
    RUVIA_CHECK_EQ(request.header("cookie").value(), "a=1; b=2");
    RUVIA_CHECK(request.body_bytes().empty());
    RUVIA_CHECK(!owner->body_complete());

    std::vector<std::byte> chunk(4096, std::byte{'x'});
    for (int i = 0; i < 16; ++i) {
        owner->append_body(chunk);
    }
    RUVIA_CHECK_EQ(owner->body_bytes(), chunk.size() * 16);
    RUVIA_CHECK(request.body_bytes().empty());
    RUVIA_CHECK(pool.outstanding_ > pool_baseline);
    owner->finish_body();
    RUVIA_CHECK(owner->body_complete());
    RUVIA_CHECK_EQ(request.body_bytes().size(), chunk.size() * 16);
    RUVIA_CHECK(request.body_bytes().front() == std::byte{'x'});
    bool rejected = false;
    try {
        owner->append_body(chunk);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    owner.reset();
    RUVIA_CHECK_EQ(pool.outstanding_, pool_baseline);
}

RUVIA_TEST(http3_server_request_abort_returns_pool_capacity_and_is_terminal) {
    counting_resource request_resource;
    counting_resource pool;
    auto head = make_get(&request_resource);
    ruvia::http3_server_request owner_value(std::move(head), &pool);
    const auto baseline = pool.outstanding_;
    std::vector<std::byte> chunk(8192, std::byte{'z'});
    owner_value.append_body(chunk);
    RUVIA_CHECK(pool.outstanding_ > baseline);
    const auto deallocations = pool.deallocations_;
    owner_value.abort_body();
    RUVIA_CHECK_EQ(owner_value.body_bytes(), 0U);
    RUVIA_CHECK(!owner_value.body_complete());
    RUVIA_CHECK_EQ(pool.outstanding_, baseline);
    RUVIA_CHECK(pool.deallocations_ > deallocations);
    bool rejected = false;
    try {
        owner_value.append_body(chunk);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(http3_server_request_append_allocation_failure_preserves_state) {
    counting_resource request_resource;
    failing_resource body_pool;
    body_pool.reject_ = false;
    auto head = make_get(&request_resource);
    ruvia::http3_server_request owner_value(std::move(head), &body_pool);
    body_pool.reject_ = true;
    const std::array<std::byte, 256> bytes_value{};
    bool threw = false;
    try {
        owner_value.append_body(bytes_value);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(owner_value.body_bytes(), 0U);
    RUVIA_CHECK(!owner_value.body_complete());
    RUVIA_CHECK(owner_value.request().body_bytes().empty());
    owner_value.abort_body();
}

RUVIA_TEST(http3_server_request_adopts_explicitly_copied_head) {
    counting_resource first;
    counting_resource request_resource;
    counting_resource body_pool;
    std::optional<ruvia::http3_server_request> owner;
    {
        ruvia::http3_message_head head(&first);
        head.method_ = "GET";
        head.scheme_ = "https";
        head.authority_ = "example.test";
        head.path_ = "/stable";
        head.headers_.push_back(ruvia::http_header::copy_of("x-owned", std::string(200, 'v'), &body_pool));
        owner.emplace(head.copy(&request_resource), &body_pool);
    }
    RUVIA_CHECK_EQ(first.outstanding_, 0U);
    RUVIA_CHECK_EQ(body_pool.outstanding_, 0U);
    RUVIA_CHECK_EQ(owner->request().path(), "/stable");
    RUVIA_CHECK_EQ(owner->request().header("x-owned")->size(), 200U);
    RUVIA_CHECK(!owner->body_complete());
    owner->finish_body();
    RUVIA_CHECK(owner->body_complete());
    owner.reset();
    RUVIA_CHECK_EQ(request_resource.outstanding_, 0U);
}

RUVIA_TEST(http3_server_request_expectation_plan_tracks_remaining_content_and_repeated_fields) {
    std::pmr::unsynchronized_pool_resource resource;
    const auto make_head = [&resource](std::string_view content_length, bool unsupported) {
        const std::array fields{
            ruvia::http3_field_section_field_view{":method", "POST"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/items"},
            ruvia::http3_field_section_field_view{"content-length", content_length},
            ruvia::http3_field_section_field_view{"expect", "100-continue"},
            ruvia::http3_field_section_field_view{"expect", "custom-expectation"},
        };
        const auto section = ruvia::encode_http3_field_section(std::span(fields).first(unsupported ? fields.size() : fields.size() - 1), &resource);
        auto head = ruvia::decode_http3_message_head(std::get<0>(section), ruvia::http3_message_head_kind::request, &resource);
        return std::move(std::get<0>(head));
    };
    ruvia::http3_server_request request(make_head("3", false), &resource);
    const auto initial_value = request.expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(initial_value.send_continue() != nullptr);
    const std::array bytes_value{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    request.append_body(bytes_value);
    {
        const auto complete_value = request.expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(complete_value.no_action() != nullptr);
    }
    request.finish_body();
    {
        const auto complete_value = request.expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(complete_value.no_action() != nullptr);
    }
    ruvia::http3_server_request unsupported(make_head("3", true), &resource);
    const auto rejected = unsupported.expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(rejected.rejection() != nullptr);
    const auto ignored = unsupported.expectation_plan(ruvia::http_unsupported_expectation_policy::ignore);
    RUVIA_CHECK(ignored.send_continue() != nullptr);
    ruvia::http3_server_request empty(make_head("0", false), &resource);
    const auto empty_plan = empty.expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(empty_plan.no_action() != nullptr);
}
