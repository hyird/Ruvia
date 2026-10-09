#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3ServerRequest.h"

#include "test_harness.h"

namespace {

class FailingResource final : public std::pmr::memory_resource {
public:
    bool reject{true};
    std::size_t minimumRejectedBytes{64};
    std::size_t live{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject && bytes >= minimumRejectedBytes) {
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

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t outstanding{};
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        outstanding += bytes;
        ++allocations;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        outstanding -= bytes;
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct RequestEvents final {
    std::pmr::memory_resource* requestResource;
    std::pmr::memory_resource* bodyPool;
    std::optional<ruvia::Http3ServerRequest> owner;
};

void receiveRequest(void* opaque, const ruvia::Http3ConnectionEvent& event) {
    auto& state = *static_cast<RequestEvents*>(opaque);
    switch (event.kind) {
        case ruvia::Http3ConnectionEventKind::kRequestHead:
            state.owner.emplace(*event.head, state.requestResource, state.bodyPool);
            break;
        case ruvia::Http3ConnectionEventKind::kBody:
            state.owner->appendBody(std::as_bytes(std::span(event.body.data(), event.body.size())));
            break;
        case ruvia::Http3ConnectionEventKind::kMessageEnd:
            state.owner->finishBody();
            break;
        default:
            break;
    }
}

ruvia::Http3MessageHead makeGet(std::pmr::memory_resource* resource) {
    auto encoded = ruvia::encodeHttp3FieldSection(
        std::array<ruvia::Http3FieldSectionFieldView, 7>{{{":method", "GET"}, {":scheme", "https"}, {":path", "/items?a=1"},
            {":authority", "example.test"}, {"x-first", "1"}, {"cookie", "a=1"},
            {"cookie", "b=2"}}},
        std::pmr::get_default_resource());
    auto decoded = ruvia::decodeHttp3MessageHead(std::get<0>(encoded), ruvia::Http3MessageHeadKind::kRequest, resource);
    return std::move(std::get<0>(decoded));
}

}  // namespace

RUVIA_TEST(http3_server_request_owns_connection_callback_head_until_message_end) {
    CountingResource protocolResource;
    CountingResource requestResource;
    CountingResource bodyPool;
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "POST"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/callback"},
        ruvia::Http3FieldSectionFieldView{"content-length", "2"},
    };
    const auto section = ruvia::encodeHttp3FieldSection(fields, &protocolResource);
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    std::array<char, 16> headPrefix{};
    const auto prefixSize = ruvia::encodeHttp3FrameHeader(headPrefix, 1, std::get<0>(section).size());
    RUVIA_CHECK((prefixSize.index() == 0));
    if ((prefixSize.index() != 0)) {
        return;
    }
    std::pmr::vector<char> headWire(&protocolResource);
    headWire.insert(headWire.end(), headPrefix.begin(), headPrefix.begin() + std::get<0>(prefixSize));
    headWire.insert(headWire.end(), std::get<0>(section).begin(), std::get<0>(section).end());

    RequestEvents captured{&requestResource, &bodyPool, std::nullopt};
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &protocolResource);
        const auto headResult = connection.feed(0, headWire, false, false, receiveRequest, &captured);
        RUVIA_CHECK(headResult.status == ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK(captured.owner.has_value());
        if (!captured.owner) {
            return;
        }
        RUVIA_CHECK_EQ(captured.owner->request().path(), "/callback");
        RUVIA_CHECK(!captured.owner->bodyComplete());
        RUVIA_CHECK(captured.owner->request().bodyBytes().empty());
        std::array<char, 16> dataPrefix{};
        const auto dataPrefixSize = ruvia::encodeHttp3FrameHeader(dataPrefix, 0, 2);
        RUVIA_CHECK((dataPrefixSize.index() == 0));
        if ((dataPrefixSize.index() != 0)) {
            return;
        }
        std::pmr::vector<char> dataWire(&protocolResource);
        dataWire.insert(dataWire.end(), dataPrefix.begin(), dataPrefix.begin() + std::get<0>(dataPrefixSize));
        dataWire.insert(dataWire.end(), {'o', 'k'});
        RUVIA_CHECK(connection.feed(0, dataWire, true, false, receiveRequest, &captured).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK(captured.owner->bodyComplete());
        RUVIA_CHECK_EQ(captured.owner->request().bodyBytes().size(), 2U);
    }
    RUVIA_CHECK_EQ(captured.owner->request().method(), "POST");
    RUVIA_CHECK_EQ(captured.owner->request().bodyBytes().front(), std::byte{'o'});
    captured.owner.reset();
    RUVIA_CHECK_EQ(requestResource.outstanding, 0U);
    RUVIA_CHECK_EQ(bodyPool.outstanding, 0U);
}

RUVIA_TEST(http3_server_request_adapts_pseudo_fields_cookies_and_host) {
    CountingResource resource;
    const auto baseline = resource.outstanding;
    {
        auto head = makeGet(&resource);
        const std::array body{std::byte{'o'}, std::byte{'k'}};
        ruvia::Http3ServerRequest owner(head, &resource, &resource);
        RUVIA_CHECK(owner.request().bodyBytes().empty());
        owner.appendBody(body);
        owner.finishBody();
        const auto& request = owner.request();
        RUVIA_CHECK(request.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
        RUVIA_CHECK(request.targetForm() == ruvia::HttpRequestTargetForm::kHttp3);
        RUVIA_CHECK_EQ(request.target(), "/items?a=1");
        RUVIA_CHECK_EQ(request.path(), "/items");
        RUVIA_CHECK_EQ(request.queryString(), "a=1");
        RUVIA_CHECK_EQ(owner.extendedConnectProtocol(), "");
        RUVIA_CHECK_EQ(request.scheme(), "https");
        RUVIA_CHECK_EQ(request.authority(), "example.test");
        RUVIA_CHECK_EQ(request.header("cookie").value(), "a=1; b=2");
        RUVIA_CHECK_EQ(request.header("host").value(), "example.test");
        RUVIA_CHECK_EQ(request.headers()[0].name(), "x-first");
        RUVIA_CHECK_EQ(request.headers()[1].name(), "cookie");
        RUVIA_CHECK_EQ(request.headers()[2].name(), "host");
        RUVIA_CHECK_EQ(request.bodyBytes().size(), 2U);
        RUVIA_CHECK(request.bodyBytes().data() != body.data());
        RUVIA_CHECK(resource.outstanding > baseline);
    }
    RUVIA_CHECK_EQ(resource.outstanding, baseline);
}

RUVIA_TEST(http3_server_request_adapts_standard_connect_authority_target) {
    CountingResource resource;
    auto encoded = ruvia::encodeHttp3FieldSection(
        std::array<ruvia::Http3FieldSectionFieldView, 2>{{{":method", "CONNECT"},
            {":authority", "example.test:443"}}},
        std::pmr::get_default_resource());
    auto decoded = ruvia::decodeHttp3MessageHead(std::get<0>(encoded), ruvia::Http3MessageHeadKind::kRequest, &resource);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() != 0)) {
        return;
    }
    ruvia::Http3ServerRequest owner(std::get<0>(decoded), &resource, &resource);
    owner.finishBody();
    RUVIA_CHECK_EQ(owner.request().target(), "example.test:443");
    RUVIA_CHECK_EQ(owner.request().path(), "");
    RUVIA_CHECK_EQ(owner.request().queryString(), "");
    RUVIA_CHECK(owner.request().knownMethod() == ruvia::HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(owner.extendedConnectProtocol(), "");
}

RUVIA_TEST(http3_server_request_preserves_extended_connect_wire_metadata) {
    CountingResource resource;
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket?channel=42"},
        ruvia::Http3FieldSectionFieldView{"host", "example.test"},
        ruvia::Http3FieldSectionFieldView{"x-preserved", "yes"},
        ruvia::Http3FieldSectionFieldView{"origin", "https://example.test"},
        ruvia::Http3FieldSectionFieldView{"authorization", "Bearer opaque"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-protocol", "chat, superchat"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-extensions", "permessage-deflate"},
        ruvia::Http3FieldSectionFieldView{"cookie", "a=1"},
        ruvia::Http3FieldSectionFieldView{"cookie", "b=2"},
    };
    const auto section = ruvia::encodeHttp3FieldSection(fields, std::pmr::get_default_resource());
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    const auto head = ruvia::decodeHttp3MessageHead(std::get<0>(section),
        ruvia::Http3MessageHeadKind::kRequest, &resource);
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() != 0)) {
        return;
    }

    ruvia::Http3ServerRequest owner(std::get<0>(head), &resource, &resource);
    owner.finishBody();
    const auto& request = owner.request();
    RUVIA_CHECK_EQ(request.method(), "CONNECT");
    RUVIA_CHECK(request.knownMethod() == ruvia::HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(owner.extendedConnectProtocol(), "websocket");
    RUVIA_CHECK_EQ(request.target(), "/socket?channel=42");
    RUVIA_CHECK_EQ(request.path(), "/socket");
    RUVIA_CHECK_EQ(request.queryString(), "channel=42");
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

    const std::array otherProtocolFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "other-protocol"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/tunnel"},
    };
    const auto otherSection = ruvia::encodeHttp3FieldSection(
        otherProtocolFields, std::pmr::get_default_resource());
    const auto otherHead = ruvia::decodeHttp3MessageHead(std::get<0>(otherSection),
        ruvia::Http3MessageHeadKind::kRequest, &resource);
    RUVIA_CHECK((otherHead.index() == 0));
    if ((otherHead.index() == 0)) {
        ruvia::Http3ServerRequest other(std::get<0>(otherHead), &resource, &resource);
        RUVIA_CHECK_EQ(other.extendedConnectProtocol(), "other-protocol");
        RUVIA_CHECK_EQ(other.request().method(), "CONNECT");
    }
}

RUVIA_TEST(http3_server_request_repeated_lifetimes_return_allocations) {
    CountingResource resource;
    for (int i = 0; i < 20; ++i) {
        auto head = makeGet(&resource);
        const auto baseline = resource.outstanding;
        {
            ruvia::Http3ServerRequest owner(head, &resource, &resource);
            owner.finishBody();
        }
        RUVIA_CHECK_EQ(resource.outstanding, baseline);
    }
}

RUVIA_TEST(http3_server_request_rejects_header_limit_and_cleans_partial_state) {
    CountingResource resource;
    const auto baseline = resource.outstanding;
    {
        ruvia::Http3MessageHead head(&resource);
        head.method = "GET";
        head.scheme = "https";
        head.authority = "example.test";
        head.path = "/";
        for (std::size_t i = 0; i < ruvia::kMaxHttpHeaderFields; ++i) {
            head.headers.emplace_back("x-test", "v", &resource);
        }
        bool threw = false;
        try {
            ruvia::Http3ServerRequest owner(head, &resource, &resource);
        } catch (const std::length_error&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    RUVIA_CHECK_EQ(resource.outstanding, baseline);
}

RUVIA_TEST(http3_server_request_allocation_exception_propagates) {
    auto head = makeGet(std::pmr::get_default_resource());
    FailingResource failing;
    bool threw = false;
    try {
        ruvia::Http3ServerRequest owner(head, &failing, std::pmr::get_default_resource());
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(failing.live, std::size_t{0});
}

RUVIA_TEST(http3_server_request_unstarted_owner_destruction_releases_resources) {
    CountingResource requestResource;
    CountingResource bodyPool;
    const auto requestBaseline = requestResource.outstanding;
    const auto poolBaseline = bodyPool.outstanding;
    {
        auto head = makeGet(std::pmr::get_default_resource());
        ruvia::Http3ServerRequest owner(head, &requestResource, &bodyPool);
        RUVIA_CHECK(!owner.bodyComplete());
        RUVIA_CHECK(owner.request().bodyBytes().empty());
        RUVIA_CHECK(requestResource.outstanding > requestBaseline);
    }
    RUVIA_CHECK_EQ(requestResource.outstanding, requestBaseline);
    RUVIA_CHECK_EQ(bodyPool.outstanding, poolBaseline);
}

RUVIA_TEST(http3_server_request_head_owner_and_incremental_body_lifetimes) {
    CountingResource requestResource;
    CountingResource pool;
    const auto poolBaseline = pool.outstanding;
    std::optional<ruvia::Http3ServerRequest> owner;
    {
        auto temporary = makeGet(std::pmr::get_default_resource());
        owner.emplace(temporary, &requestResource, &pool);
    }
    const auto& request = owner->request();
    RUVIA_CHECK_EQ(request.method(), "GET");
    RUVIA_CHECK_EQ(request.path(), "/items");
    RUVIA_CHECK_EQ(request.authority(), "example.test");
    RUVIA_CHECK_EQ(request.header("cookie").value(), "a=1; b=2");
    RUVIA_CHECK(request.bodyBytes().empty());
    RUVIA_CHECK(!owner->bodyComplete());

    std::vector<std::byte> chunk(4096, std::byte{'x'});
    for (int i = 0; i < 16; ++i) {
        owner->appendBody(chunk);
    }
    RUVIA_CHECK_EQ(owner->bodyBytes(), chunk.size() * 16);
    RUVIA_CHECK(request.bodyBytes().empty());
    RUVIA_CHECK(pool.outstanding > poolBaseline);
    owner->finishBody();
    RUVIA_CHECK(owner->bodyComplete());
    RUVIA_CHECK_EQ(request.bodyBytes().size(), chunk.size() * 16);
    RUVIA_CHECK(request.bodyBytes().front() == std::byte{'x'});
    bool rejected = false;
    try {
        owner->appendBody(chunk);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    owner.reset();
    RUVIA_CHECK_EQ(pool.outstanding, poolBaseline);
}

RUVIA_TEST(http3_server_request_abort_returns_pool_capacity_and_is_terminal) {
    CountingResource requestResource;
    CountingResource pool;
    auto head = makeGet(std::pmr::get_default_resource());
    ruvia::Http3ServerRequest owner(head, &requestResource, &pool);
    const auto baseline = pool.outstanding;
    std::vector<std::byte> chunk(8192, std::byte{'z'});
    owner.appendBody(chunk);
    RUVIA_CHECK(pool.outstanding > baseline);
    const auto deallocations = pool.deallocations;
    owner.abortBody();
    RUVIA_CHECK_EQ(owner.bodyBytes(), 0U);
    RUVIA_CHECK(!owner.bodyComplete());
    RUVIA_CHECK_EQ(pool.outstanding, baseline);
    RUVIA_CHECK(pool.deallocations > deallocations);
    bool rejected = false;
    try {
        owner.appendBody(chunk);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(http3_server_request_append_allocation_failure_preserves_state) {
    CountingResource requestResource;
    FailingResource bodyPool;
    bodyPool.reject = false;
    auto head = makeGet(std::pmr::get_default_resource());
    ruvia::Http3ServerRequest owner(head, &requestResource, &bodyPool);
    bodyPool.reject = true;
    const std::array<std::byte, 256> bytes{};
    bool threw = false;
    try {
        owner.appendBody(bytes);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(owner.bodyBytes(), 0U);
    RUVIA_CHECK(!owner.bodyComplete());
    RUVIA_CHECK(owner.request().bodyBytes().empty());
    owner.abortBody();
}

RUVIA_TEST(http3_server_request_copies_mixed_callback_head_resources) {
    CountingResource first;
    CountingResource requestResource;
    CountingResource bodyPool;
    ruvia::Http3MessageHead head(&first);
    head.method = "GET";
    head.scheme = "https";
    head.authority = "example.test";
    head.path = "/stable";
    head.headers.emplace_back("x-owned", std::string(200, 'v'), &bodyPool);
    {
        ruvia::Http3ServerRequest owner(head, &requestResource, &bodyPool);
        RUVIA_CHECK_EQ(owner.request().path(), "/stable");
        RUVIA_CHECK_EQ(owner.request().header("x-owned")->size(), 200U);
        RUVIA_CHECK(!owner.bodyComplete());
        owner.finishBody();
        RUVIA_CHECK(owner.bodyComplete());
    }
    RUVIA_CHECK_EQ(requestResource.outstanding, 0U);
}

RUVIA_TEST(http3_server_request_expectation_plan_tracks_remaining_content_and_repeated_fields) {
    std::pmr::unsynchronized_pool_resource resource;
    auto head = makeGet(&resource);
    head.method = "POST";
    head.contentLength = 3;
    head.headers.emplace_back("expect", "100-continue", &resource);
    ruvia::Http3ServerRequest request(head, &resource, &resource);
    const auto initial = request.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(initial.sendContinue() != nullptr);
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    request.appendBody(bytes);
    {
        const auto complete = request.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kReject);
        RUVIA_CHECK(complete.noAction() != nullptr);
    }
    request.finishBody();
    {
        const auto complete = request.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kReject);
        RUVIA_CHECK(complete.noAction() != nullptr);
    }
    head.headers.emplace_back("expect", "custom-expectation", &resource);
    ruvia::Http3ServerRequest unsupported(head, &resource, &resource);
    const auto rejected = unsupported.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(rejected.rejection() != nullptr);
    const auto ignored = unsupported.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kIgnore);
    RUVIA_CHECK(ignored.sendContinue() != nullptr);
    head.headers.pop_back();
    head.contentLength = 0;
    ruvia::Http3ServerRequest empty(head, &resource, &resource);
    const auto emptyPlan = empty.expectationPlan(ruvia::HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(emptyPlan.noAction() != nullptr);
}
