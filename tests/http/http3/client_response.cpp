#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3VarInt.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {
struct Events final {
    std::vector<ruvia::Http3ClientResponseEventKind> kinds;
    std::string body;
    std::vector<std::string> trailers;
    std::vector<std::string> trailer_values;
    std::vector<bool> never_indexed;
    std::vector<std::optional<ruvia::HttpResponseBodyPlan>> responseBodyPlans;
    std::vector<std::optional<std::uint64_t>> contentLengths;
    std::vector<std::optional<ruvia::HttpClientRequestContentSignal>> requestContentSignals;
};
struct CountingResource final : std::pmr::memory_resource {
    std::size_t outstanding{};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++outstanding;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
struct Reenter final {
    ruvia::Http3ClientResponse* response;
};
void reenter(void* opaque, const ruvia::Http3ClientResponseEvent&) {
    auto& state = *static_cast<Reenter*>(opaque);
    static_cast<void>(state.response->feed({}, false, false, reenter, opaque));
}
void collect(void* p, const ruvia::Http3ClientResponseEvent& event) {
    auto& out = *static_cast<Events*>(p);
    out.kinds.push_back(event.kind);
    out.requestContentSignals.push_back(event.requestContentSignal);
    if (!event.body.empty()) {
        out.body.append(event.body.data(), event.body.size());
    }
    out.responseBodyPlans.emplace_back(event.responseBodyPlan);
    out.contentLengths.emplace_back(event.head == nullptr ? std::nullopt : event.head->contentLength);
    if (event.kind == ruvia::Http3ClientResponseEventKind::kTrailerField) {
        out.trailers.emplace_back(event.trailer.name);
        out.trailer_values.emplace_back(event.trailer.value);
        out.never_indexed.push_back(event.trailer.neverIndexed);
    }
}
std::vector<char> frame(std::uint64_t type, std::span<const char> payload) {
    std::vector<char> result(16 + payload.size());
    auto typeSize = ruvia::encodeHttp3VarInt(result, type);
    auto lengthSize = ruvia::encodeHttp3VarInt(std::span<char>(result).subspan(std::get<0>(typeSize)), payload.size());
    result.resize(std::get<0>(typeSize) + std::get<0>(lengthSize));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
std::vector<char> fieldSection(std::span<const ruvia::Http3FieldSectionFieldView> fields) {
    std::pmr::monotonic_buffer_resource resource;
    auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource);
    return {std::get<0>(encoded).begin(), std::get<0>(encoded).end()};
}
std::vector<char> responseHead(std::uint16_t status, std::optional<std::uint64_t> contentLength = {}) {
    const auto statusText = std::to_string(status);
    const auto lengthText = contentLength ? std::to_string(*contentLength) : std::string{};
    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{":status", statusText},
        {"content-length", lengthText}}};
    const auto section = fieldSection(std::span(fields).first(contentLength ? 2 : 1));
    return frame(1, section);
}
std::vector<char> responseData(std::string_view body) {
    return frame(0, body);
}
const ruvia::HttpResponseBodyPlan* responsePlan(const Events& events, std::size_t index) {
    return index < events.responseBodyPlans.size() && events.responseBodyPlans[index]
               ? &*events.responseBodyPlans[index]
               : nullptr;
}
bool samePlan(const ruvia::HttpResponseBodyPlan* left, const ruvia::HttpResponseBodyPlan* right) {
    return left != nullptr && right != nullptr && left->requestMethod() == right->requestMethod() &&
           left->responseStatus() == right->responseStatus() && left->contentSemantics() == right->contentSemantics() &&
           left->statusAllowsBody() == right->statusAllowsBody() && left->bodySuppressed() == right->bodySuppressed();
}
}  // namespace

RUVIA_TEST(http3_client_response_handles_final_body_and_fragmentation) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    Events events;
    constexpr std::array<char, 10> wire{1, 3, 0, 0, static_cast<char>(0xd9),
        0, 3, 'a', 'b', 'c'};
    auto result = response.feed(std::span<const char>(wire).first(2), false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kNeedMoreData);
    result = response.feed(std::span<const char>(wire).subspan(2, 6), false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kNeedMoreData);
    result = response.feed(std::span<const char>(wire).subspan(8), true, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.body, std::string("abc"));
    RUVIA_CHECK(events.kinds.size() >= 3);
    RUVIA_CHECK(events.kinds.front() == ruvia::Http3ClientResponseEventKind::kFinalHead);
    RUVIA_CHECK(events.kinds.back() == ruvia::Http3ClientResponseEventKind::kMessageEnd);
    for (std::size_t i = 1; i + 1 < events.kinds.size(); ++i) {
        RUVIA_CHECK(events.kinds[i] == ruvia::Http3ClientResponseEventKind::kBody);
        RUVIA_CHECK(responsePlan(events, i) == nullptr);
    }
    const auto* finalPlan = responsePlan(events, 0);
    const auto* endPlan = responsePlan(events, events.kinds.size() - 1);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->responseStatus() == ruvia::http_status::kOk);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(finalPlan != nullptr && !finalPlan->bodySuppressed());
    RUVIA_CHECK(samePlan(finalPlan, endPlan));
}

RUVIA_TEST(http3_client_response_rejects_data_before_headers_and_bad_stream_ids) {
    std::pmr::monotonic_buffer_resource memory;
    bool rejected = false;
    try {
        ruvia::Http3ClientResponse invalid(1, ruvia::HttpKnownMethod::kGet, &memory);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        ruvia::Http3ClientResponse invalid(ruvia::kHttp3VarIntMax + 1, ruvia::HttpKnownMethod::kGet, &memory);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    Events events;
    constexpr std::array<char, 2> data{0, 0};
    const auto result = response.feed(data, false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kConnectionError);
    RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
}

RUVIA_TEST(http3_client_response_rejects_unadvertised_push_promise_with_id_error) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &resource);
    Events events;
    constexpr std::array<ruvia::Http3FieldSectionFieldView, 1> status{{{":status", "200"}}};
    const auto headers = frame(1, fieldSection(status));
    RUVIA_CHECK(response.feed(headers, false, false, collect, &events).status ==
                ruvia::Http3ClientResponseStatus::kNeedMoreData);
    constexpr std::array<char, 1> pushId{0};
    const auto promise = frame(5, pushId);
    const auto result = response.feed(promise, false, false, collect, &events);
    RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kIdError);
    RUVIA_CHECK(events.kinds.size() == 1);
}

RUVIA_TEST(http3_client_response_head_rejects_payload_but_accepts_empty_fin) {
    std::pmr::monotonic_buffer_resource memory;
    Events events;
    constexpr std::array<char, 5> headers{1, 3, 0, 0, static_cast<char>(0xd9)};
    {
        ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kHead, &memory);
        auto result = response.feed(headers, false, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kNeedMoreData);
        result = response.feed({}, true, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    }
    {
        ruvia::Http3ClientResponse response(4, ruvia::HttpKnownMethod::kHead, &memory);
        constexpr std::array<char, 8> payload{1, 3, 0, 0, static_cast<char>(0xd9), 0, 1, 'x'};
        const auto result = response.feed(payload, true, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kStreamError);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kMessageError);
    }
}

RUVIA_TEST(http3_client_response_validates_all_trailers_before_callback) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{"x-one", "1", false}, {"x-two", "2", false}}};
    auto encoded = fieldSection(fields);
    auto headFrame = frame(1, head);
    auto trailerFrame = frame(1, encoded);
    headFrame.insert(headFrame.end(), trailerFrame.begin(), trailerFrame.end());
    Events events;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    auto result = response.feed(headFrame, false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kNeedMoreData);
    RUVIA_CHECK_EQ(events.trailers.size(), std::size_t{2});
    RUVIA_CHECK_EQ(events.trailers[0], std::string("x-one"));
    RUVIA_CHECK_EQ(events.trailers[1], std::string("x-two"));

    ruvia::Http3ClientResponseLimits limits{};
    limits.maxFields = 1;
    ruvia::Http3ClientResponse limited(8, ruvia::HttpKnownMethod::kGet, &memory, limits);
    Events limitedEvents;
    result = limited.feed(headFrame, false, false, collect, &limitedEvents);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kConnectionError);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK(limitedEvents.trailers.empty());

    constexpr std::array<std::string_view, 6> invalid_names{
        "host", "X-Test", "x bad", ":status", std::string_view("x\0bad", 5), "\x80-name"};
    for (const auto name : invalid_names) {
        const std::array<ruvia::Http3FieldSectionFieldView, 2> invalid_fields{{{"x-good", "ok", false}, {name, "bad", false}}};
        const auto invalid_encoded = fieldSection(invalid_fields);
        auto invalid_frame = frame(1, head);
        const auto invalid_trailer_frame = frame(1, invalid_encoded);
        invalid_frame.insert(invalid_frame.end(), invalid_trailer_frame.begin(), invalid_trailer_frame.end());
        Events rejected_events;
        ruvia::Http3ClientResponse rejected(4, ruvia::HttpKnownMethod::kGet, &memory);
        result = rejected.feed(invalid_frame, false, false, collect, &rejected_events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kStreamError);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kMessageError);
        RUVIA_CHECK(rejected_events.trailers.empty());
    }
}

RUVIA_TEST(http3_client_response_trailer_storage_is_atomic_and_reclaimed_on_allocation_failure) {
    const std::string name = "x-" + std::string(80, 'n');
    const std::string value(100, 'v');
    const std::array fields{ruvia::Http3FieldSectionFieldView{name, value, true},
        ruvia::Http3FieldSectionFieldView{"x-final", "done", false}};
    const auto trailer_wire = frame(1, fieldSection(fields));
    const auto head_wire = responseHead(200);
    bool succeeded = false;
    std::size_t failures = 0;
    for (std::size_t allowance = 0; allowance != 64 && !succeeded; ++allowance) {
        failing_memory_resource memory;
        {
            ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
            Events events;
            RUVIA_CHECK(response.feed(head_wire, false, false, collect, &events).status ==
                        ruvia::Http3ClientResponseStatus::kNeedMoreData);
            memory.fail_after(allowance);
            try {
                const auto result = response.feed(trailer_wire, true, false, collect, &events);
                RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
                succeeded = true;
                RUVIA_CHECK_EQ(events.trailers.size(), std::size_t{2});
                if (events.trailers.size() == 2) {
                    RUVIA_CHECK_EQ(events.trailers.front(), name);
                    RUVIA_CHECK_EQ(events.trailer_values.front(), value);
                    RUVIA_CHECK(events.never_indexed.front());
                    RUVIA_CHECK(!events.never_indexed.back());
                }
            } catch (const std::bad_alloc&) {
                ++failures;
                RUVIA_CHECK(events.trailers.empty());
            }
            memory.allow_allocations();
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    RUVIA_CHECK(failures >= 2);
}

RUVIA_TEST(http3_client_response_rejects_forbidden_response_trailer_fields) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    for (const auto field : {ruvia::Http3FieldSectionFieldView{"set-cookie", "sid=secret"},
             ruvia::Http3FieldSectionFieldView{"content-encoding", "gzip"}}) {
        const std::array fields{field};
        const auto trailer = fieldSection(fields);
        auto wire = frame(1, head);
        const auto trailingFrame = frame(1, trailer);
        wire.insert(wire.end(), trailingFrame.begin(), trailingFrame.end());
        Events events;
        ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
        const auto result = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kStreamError);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kMessageError);
        RUVIA_CHECK(events.trailers.empty());
    }
    const std::array validFields{ruvia::Http3FieldSectionFieldView{"etag", "\"tag\""}};
    const auto trailer = fieldSection(validFields);
    auto wire = frame(1, head);
    const auto trailingFrame = frame(1, trailer);
    wire.insert(wire.end(), trailingFrame.begin(), trailingFrame.end());
    Events events;
    ruvia::Http3ClientResponse valid(0, ruvia::HttpKnownMethod::kGet, &memory);
    const auto result = valid.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.trailers.size(), std::size_t{1});
}

RUVIA_TEST(http3_client_response_preserves_final_plan_after_informational_response) {
    std::pmr::monotonic_buffer_resource memory;
    constexpr std::array<char, 3> informationalSection{0, 0, static_cast<char>(0xd8)};
    auto wire = frame(1, informationalSection);
    auto finalHead = responseHead(200, 3);
    auto data = responseData("abc");
    wire.insert(wire.end(), finalHead.begin(), finalHead.end());
    wire.insert(wire.end(), data.begin(), data.end());

    Events events;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    const auto result = response.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.body, std::string("abc"));
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{4});
    RUVIA_CHECK(events.kinds[0] == ruvia::Http3ClientResponseEventKind::kInformationalHead);
    RUVIA_CHECK(events.kinds[1] == ruvia::Http3ClientResponseEventKind::kFinalHead);
    RUVIA_CHECK(events.kinds[2] == ruvia::Http3ClientResponseEventKind::kBody);
    RUVIA_CHECK(events.kinds[3] == ruvia::Http3ClientResponseEventKind::kMessageEnd);
    RUVIA_CHECK(responsePlan(events, 0) == nullptr);
    RUVIA_CHECK(responsePlan(events, 2) == nullptr);
    const auto* finalPlan = responsePlan(events, 1);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->responseStatus() == ruvia::http_status::kOk);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(samePlan(finalPlan, responsePlan(events, 3)));
}

RUVIA_TEST(http3_client_response_preserves_body_suppression_and_head_representation_length) {
    std::pmr::monotonic_buffer_resource memory;
    struct Case final {
        std::uint64_t streamId;
        ruvia::HttpKnownMethod method;
        std::uint16_t status;
        std::optional<std::uint64_t> contentLength;
    };
    constexpr std::array cases{
        Case{0, ruvia::HttpKnownMethod::kHead, 200, 123},
        Case{4, ruvia::HttpKnownMethod::kGet, 204, std::nullopt},
        Case{8, ruvia::HttpKnownMethod::kGet, 304, 123},
    };
    for (const auto& test : cases) {
        auto wire = responseHead(test.status, test.contentLength);
        ruvia::Http3ClientResponse response(test.streamId, test.method, &memory);
        Events events;
        const auto result = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK(events.body.empty());
        RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{2});
        RUVIA_CHECK(events.kinds[0] == ruvia::Http3ClientResponseEventKind::kFinalHead);
        RUVIA_CHECK(events.kinds[1] == ruvia::Http3ClientResponseEventKind::kMessageEnd);
        RUVIA_CHECK(events.contentLengths[0] == test.contentLength);
        const auto* finalPlan = responsePlan(events, 0);
        RUVIA_CHECK(finalPlan != nullptr && finalPlan->requestMethod() == test.method);
        RUVIA_CHECK(finalPlan != nullptr && finalPlan->responseStatus().value() == test.status);
        RUVIA_CHECK(finalPlan != nullptr && finalPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithoutContent);
        RUVIA_CHECK(finalPlan != nullptr && finalPlan->bodySuppressed());
        RUVIA_CHECK(samePlan(finalPlan, responsePlan(events, 1)));
    }
}

RUVIA_TEST(http3_client_response_accepts_get_with_legal_empty_body_and_fin) {
    std::pmr::monotonic_buffer_resource memory;
    auto wire = responseHead(200, 0);
    const auto data = responseData({});
    wire.insert(wire.end(), data.begin(), data.end());
    ruvia::Http3ClientResponse response(12, ruvia::HttpKnownMethod::kGet, &memory);
    Events events;
    const auto result = response.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK(events.body.empty());
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{2});
    RUVIA_CHECK(events.kinds[0] == ruvia::Http3ClientResponseEventKind::kFinalHead);
    RUVIA_CHECK(events.kinds[1] == ruvia::Http3ClientResponseEventKind::kMessageEnd);
    const auto* finalPlan = responsePlan(events, 0);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(finalPlan != nullptr && !finalPlan->bodySuppressed());
    RUVIA_CHECK(samePlan(finalPlan, responsePlan(events, 1)));
}

RUVIA_TEST(http3_client_response_rejects_101_and_qpack_trailer_atomically) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<ruvia::Http3FieldSectionFieldView, 1> switching{{{":status", "101", false}}};
    auto section = fieldSection(switching);
    auto wire = frame(1, section);
    Events events;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    auto result = response.feed(wire, false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kStreamError);

    constexpr std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    auto combined = frame(1, head);
    constexpr std::array<char, 3> brokenQpack{0, 0, static_cast<char>(0xff)};
    auto badTrailer = frame(1, brokenQpack);
    combined.insert(combined.end(), badTrailer.begin(), badTrailer.end());
    ruvia::Http3ClientResponse withBadTrailer(4, ruvia::HttpKnownMethod::kGet, &memory);
    Events trailerEvents;
    result = withBadTrailer.feed(combined, false, false, collect, &trailerEvents);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kConnectionError);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kQpackDecompressionFailed);
    RUVIA_CHECK(trailerEvents.trailers.empty());
}

RUVIA_TEST(http3_client_response_connect_success_delivers_tunnel_data) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kConnect, &memory);
    Events events;
    // Feed successful CONNECT headers and tunnel bytes over separate frames.
    const auto headers = responseHead(200);
    auto result = response.feed(headers, false, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kNeedMoreData);
    constexpr std::array<char, 5> data{0, 3, 'a', 'b', 'c'};
    result = response.feed(data, true, false, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.body, std::string("abc"));
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{3});
    RUVIA_CHECK(events.kinds[0] == ruvia::Http3ClientResponseEventKind::kFinalHead);
    RUVIA_CHECK(events.kinds[1] == ruvia::Http3ClientResponseEventKind::kTunnelData);
    RUVIA_CHECK(events.kinds[2] == ruvia::Http3ClientResponseEventKind::kMessageEnd);
    const auto* finalPlan = responsePlan(events, 0);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(finalPlan != nullptr && finalPlan->bodySuppressed());
    RUVIA_CHECK(responsePlan(events, 1) == nullptr);
    RUVIA_CHECK(samePlan(finalPlan, responsePlan(events, 2)));
}

RUVIA_TEST(http3_client_response_callback_reentrancy_is_terminal) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kGet, &memory);
    Reenter state{&response};
    constexpr std::array<char, 5> headers{1, 3, 0, 0, static_cast<char>(0xd9)};
    bool threw = false;
    try {
        static_cast<void>(response.feed(headers, false, false, reenter, &state));
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    const auto result = response.feed({}, false, false, collect, nullptr);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kStreamError);
}

RUVIA_TEST(http3_client_response_pmr_lifetime_covers_terminal_and_unstarted_paths) {
    CountingResource memory;
    constexpr std::array<char, 10> wire{1, 3, 0, 0, static_cast<char>(0xd9), 0, 3, 'a', 'b', 'c'};
    std::string retained;
    for (std::uint64_t id = 0; id < 16; id += 4) {
        ruvia::Http3ClientResponse response(id, ruvia::HttpKnownMethod::kGet, &memory);
        Events events;
        const auto result = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        retained = events.body;
    }
    {
        ruvia::Http3ClientResponse reset(0, ruvia::HttpKnownMethod::kGet, &memory);
        Events events;
        static_cast<void>(reset.feed({}, false, true, collect, &events));
    }
    {
        ruvia::Http3ClientResponse failed(0, ruvia::HttpKnownMethod::kGet, &memory);
        constexpr std::array<char, 2> dataFirst{0, 0};
        Events events;
        static_cast<void>(failed.feed(dataFirst, false, false, collect, &events));
    }
    {
        ruvia::Http3ClientResponse neverStarted(0, ruvia::HttpKnownMethod::kGet, &memory);
    }
    RUVIA_CHECK_EQ(retained, std::string("abc"));
    RUVIA_CHECK_EQ(memory.outstanding, std::size_t{0});
}

RUVIA_TEST(http3_client_response_reset_is_terminal) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kHead, &memory);
    Events events;
    const auto result = response.feed({}, false, true, collect, &events);
    RUVIA_CHECK(result.status == ruvia::Http3ClientResponseStatus::kReset);
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{1});
}

RUVIA_TEST(http3ClientResponseSignalsContinueOnlyFor100AndStopsContentAtFinalHead) {
    std::pmr::monotonic_buffer_resource resource;
    Events events;
    ruvia::Http3ClientResponse response(0, ruvia::HttpKnownMethod::kPost, &resource);
    const std::array<ruvia::Http3FieldSectionFieldView, 1> early{{{":status", "103"}}};
    const std::array<ruvia::Http3FieldSectionFieldView, 1> continued{{{":status", "100"}}};
    const std::array<ruvia::Http3FieldSectionFieldView, 2> final{{{":status", "204"}, {"x-final", "retained"}}};
    auto makeHead = [&](auto fields) {
        const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource);
        std::string wire;
        std::array<char, 16> prefix{};
        auto type = ruvia::encodeHttp3VarInt(prefix, 1);
        wire.append(prefix.data(), std::get<0>(type));
        auto length = ruvia::encodeHttp3VarInt(prefix, std::get<0>(encoded).size());
        wire.append(prefix.data(), std::get<0>(length));
        wire.append(std::get<0>(encoded).data(), std::get<0>(encoded).size());
        return wire;
    };
    RUVIA_CHECK(response.feed(makeHead(early), false, false, collect, &events).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(response.feed(makeHead(continued), false, false, collect, &events).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(response.feed(makeHead(final), true, false, collect, &events).status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.requestContentSignals.size(), std::size_t{4});
    if (events.requestContentSignals.size() == 4) {
        RUVIA_CHECK(!events.requestContentSignals[0]);
        RUVIA_CHECK(events.requestContentSignals[1] == ruvia::HttpClientRequestContentSignal::kContinue);
        RUVIA_CHECK(events.requestContentSignals[2] == ruvia::HttpClientRequestContentSignal::kExchangeComplete);
        RUVIA_CHECK(!events.requestContentSignals[3]);
    }
}
