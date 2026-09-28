#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"

#include "test_harness.h"

namespace {

struct Captured final {
    std::vector<std::string> methods;
    std::vector<std::string> bodies;
    std::vector<std::string> trailers;
    std::unordered_map<std::uint64_t, std::size_t> bodyIndex;
    std::vector<std::uint64_t> ended;
    std::vector<std::uint64_t> reset;
    std::size_t responsePlanCount{0};
};

void ignoreEvent(void*, const ruvia::Http3ConnectionEvent&) {}

struct ClientCaptured final {
    std::unordered_map<std::uint64_t, std::uint16_t> statuses;
    std::unordered_map<std::uint64_t, std::string> bodies;
    std::unordered_map<std::uint64_t, std::vector<ruvia::Http3ConnectionEventKind>> kinds;
    std::unordered_map<std::uint64_t, std::vector<std::optional<ruvia::HttpResponseBodyPlan>>> responseBodyPlans;
    std::vector<std::uint64_t> ended;
    std::vector<std::uint64_t> reset;
};

void captureClient(void* opaque, const ruvia::Http3ConnectionEvent& event) {
    auto& result = *static_cast<ClientCaptured*>(opaque);
    result.kinds[event.streamId].push_back(event.kind);
    result.responseBodyPlans[event.streamId].emplace_back(event.responseBodyPlan);
    if (event.kind == ruvia::Http3ConnectionEventKind::kInformationalHead ||
        event.kind == ruvia::Http3ConnectionEventKind::kFinalHead) {
        result.statuses[event.streamId] = event.head->status;
    } else if (event.kind == ruvia::Http3ConnectionEventKind::kBody ||
               event.kind == ruvia::Http3ConnectionEventKind::kTunnelData) {
        result.bodies[event.streamId].append(event.body.data(), event.body.size());
    } else if (event.kind == ruvia::Http3ConnectionEventKind::kMessageEnd) {
        result.ended.push_back(event.streamId);
    } else if (event.kind == ruvia::Http3ConnectionEventKind::kReset) {
        result.reset.push_back(event.streamId);
    }
}

std::vector<char> responseMessage(std::pmr::memory_resource* resource, std::uint16_t status,
    std::optional<std::uint64_t> contentLength, std::string_view body, bool sendData) {
    const auto statusText = std::to_string(status);
    const auto lengthText = contentLength ? std::to_string(*contentLength) : std::string{};
    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{":status", statusText}, {"content-length", lengthText}}};
    const auto section = ruvia::encodeHttp3FieldSection(std::span(fields).first(contentLength ? 2 : 1), resource);
    std::vector<char> result(16);
    const auto head = ruvia::encodeHttp3FrameHeader(result, 1, section->size());
    result.resize(*head);
    result.insert(result.end(), section->begin(), section->end());
    if (sendData) {
        std::array<char, 16> frame{};
        const auto data = ruvia::encodeHttp3FrameHeader(frame, 0, body.size());
        result.insert(result.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*data));
        result.insert(result.end(), body.begin(), body.end());
    }
    return result;
}

std::vector<char> responseWire(std::pmr::memory_resource* resource, std::uint16_t status,
    std::string_view body) {
    return responseMessage(resource, status, body.size(), body, true);
}

bool samePlan(const ruvia::HttpResponseBodyPlan* left, const ruvia::HttpResponseBodyPlan* right) {
    return left != nullptr && right != nullptr && left->requestMethod() == right->requestMethod() &&
           left->responseStatus() == right->responseStatus() && left->contentSemantics() == right->contentSemantics() &&
           left->statusAllowsBody() == right->statusAllowsBody() && left->bodySuppressed() == right->bodySuppressed();
}

const ruvia::HttpResponseBodyPlan* responsePlan(const ClientCaptured& captured, std::uint64_t streamId,
    std::size_t index) {
    const auto found = captured.responseBodyPlans.find(streamId);
    return found != captured.responseBodyPlans.end() && index < found->second.size() && found->second[index]
               ? &*found->second[index]
               : nullptr;
}

void capture(void* opaque, const ruvia::Http3ConnectionEvent& event) {
    auto& result = *static_cast<Captured*>(opaque);
    if (event.responseBodyPlan) {
        ++result.responsePlanCount;
    }
    switch (event.kind) {
        case ruvia::Http3ConnectionEventKind::kRequestHead:
            result.methods.emplace_back(event.head->method);
            result.bodyIndex[event.streamId] = result.bodies.size();
            result.bodies.emplace_back();
            break;
        case ruvia::Http3ConnectionEventKind::kInformationalHead:
        case ruvia::Http3ConnectionEventKind::kFinalHead:
        case ruvia::Http3ConnectionEventKind::kTunnelData:
            break;
        case ruvia::Http3ConnectionEventKind::kBody:
            result.bodies[result.bodyIndex[event.streamId]].append(event.body.data(), event.body.size());
            break;
        case ruvia::Http3ConnectionEventKind::kMessageEnd:
            result.ended.push_back(event.streamId);
            break;
        case ruvia::Http3ConnectionEventKind::kReset:
            result.reset.push_back(event.streamId);
            break;
        case ruvia::Http3ConnectionEventKind::kTrailerField:
            result.trailers.emplace_back(event.trailer.name);
            break;
    }
}

std::vector<char> requestWire(std::pmr::memory_resource* resource, std::string_view method,
    std::string_view path, std::string_view body) {
    const std::array<ruvia::Http3FieldSectionFieldView, 4> fields{{{":method", method}, {":scheme", "https"}, {":authority", "example.test"}, {":path", path}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, resource);
    std::vector<char> result(128);
    auto header = ruvia::encodeHttp3FrameHeader(result, 1, section->size());
    result.resize(*header + section->size());
    std::copy(section->begin(), section->end(), result.begin() + static_cast<std::ptrdiff_t>(*header));
    std::array<char, 16> frameHeader{};
    const auto bodyHeader = ruvia::encodeHttp3FrameHeader(frameHeader, 0, body.size());
    result.insert(result.end(), frameHeader.begin(), frameHeader.begin() + static_cast<std::ptrdiff_t>(*bodyHeader));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

struct CountingResource final : std::pmr::memory_resource {
    std::size_t allocations{0};
    std::size_t deallocations{0};
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* ptr, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(ptr, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http3_connection_demultiplexes_fragmented_parallel_requests_and_preserves_callback_results) {
    CountingResource resource;
    Captured captured;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        const auto first = requestWire(&resource, "POST", "/one", "abc");
        const auto second = requestWire(&resource, "GET", "/two", "xy");
        for (std::size_t i = 0; i < first.size(); ++i) {
            const auto result = connection.feed(0, std::span(first).subspan(i, 1), i + 1 == first.size(), false,
                capture, &captured);
            RUVIA_CHECK(result.status == (i + 1 == first.size()
                                                 ? ruvia::Http3ConnectionStatus::kMessageEnd
                                                 : ruvia::Http3ConnectionStatus::kNeedMoreData));
        }
        RUVIA_CHECK(connection.feed(4, second, true, false, capture, &captured).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(captured.methods.size(), 2U);
        RUVIA_CHECK_EQ(captured.methods[0], "POST");
        RUVIA_CHECK_EQ(captured.methods[1], "GET");
        RUVIA_CHECK_EQ(captured.responsePlanCount, std::size_t{0});
        RUVIA_CHECK_EQ(captured.bodies[0], "abc");
        RUVIA_CHECK_EQ(captured.bodies[1], "xy");
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_handles_settings_stream_errors_and_request_reset_independently) {
    CountingResource resource;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        constexpr std::array<char, 3> settings{0x00, 0x04, 0x00};
        const auto accepted = connection.feed(2, settings, false, false, capture, nullptr);
        RUVIA_CHECK(accepted.status == ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK(connection.peerSettings().has_value());

        Captured captured;
        const auto partial = requestWire(&resource, "GET", "/cancel", "");
        RUVIA_CHECK(connection.feed(0, std::span(partial).first(1), false, false, capture, &captured).status ==
                    ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK(connection.feed(0, {}, false, true, capture, &captured).status ==
                    ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK(connection.feed(0, {}, false, true, capture, &captured).status ==
                    ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK_EQ(captured.reset.size(), 1U);
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);

        constexpr std::array<char, 3> badControl{0x00, 0x00, 0x00};
        const auto error = connection.feed(6, badControl, false, false, capture, &captured);
        RUVIA_CHECK(error.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(error.code == ruvia::Http3ConnectionErrorCode::kStreamCreationError);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_delivers_trailers_synchronously_and_finishes_the_message) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
    Captured captured;
    const auto initial = requestWire(&resource, "GET", "/trailers", "");
    RUVIA_CHECK(connection.feed(0, initial, false, false, capture, &captured).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    const std::array<ruvia::Http3FieldSectionFieldView, 1> fields{{{"x-end", "yes"}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
    std::array<char, 16> frame{};
    const auto header = ruvia::encodeHttp3FrameHeader(frame, 1, section->size());
    std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*header));
    wire.insert(wire.end(), section->begin(), section->end());
    RUVIA_CHECK(connection.feed(0, wire, true, false, capture, &captured).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(captured.trailers.size(), 1U);
    RUVIA_CHECK_EQ(captured.trailers[0], "x-end");
    RUVIA_CHECK_EQ(captured.ended.size(), 1U);
}

RUVIA_TEST(http3_connection_maps_qpack_critical_stream_errors_to_connection_scope) {
    std::pmr::monotonic_buffer_resource resource;
    Captured captured;
    {
        ruvia::Http3Connection encoder(ruvia::Http3PeerRole::kServer, &resource);
        constexpr std::array<char, 2> badEncoder{0x02, 0x00};
        const auto result = encoder.feed(2, badEncoder, false, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kQpackEncoderStreamError);
    }
    {
        ruvia::Http3Connection decoder(ruvia::Http3PeerRole::kServer, &resource);
        constexpr std::array<char, 2> badDecoder{0x03, 0x00};
        const auto result = decoder.feed(2, badDecoder, false, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kQpackDecoderStreamError);
    }
}

RUVIA_TEST(http3_connection_maps_qpack_decompression_and_reserved_frames_to_connection_errors) {
    std::pmr::monotonic_buffer_resource resource;
    Captured captured;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        constexpr std::array<char, 4> dynamicReference{0x01, 0x02, 0x01, 0x00};
        const auto result = connection.feed(0, dynamicReference, false, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kQpackDecompressionFailed);
    }
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        constexpr std::array<char, 2> reservedFrame{0x02, 0x00};
        const auto result = connection.feed(0, reservedFrame, false, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
    }
}

RUVIA_TEST(http3_connection_validates_all_trailers_before_delivering_any) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
    Captured captured;
    const auto initial = requestWire(&resource, "GET", "/bad-trailer", "");
    RUVIA_CHECK(connection.feed(0, initial, false, false, capture, &captured).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{"x-valid", "yes"}, {"connection", "close"}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
    std::array<char, 16> frame{};
    const auto header = ruvia::encodeHttp3FrameHeader(frame, 1, section->size());
    std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*header));
    wire.insert(wire.end(), section->begin(), section->end());
    const auto result = connection.feed(0, wire, true, false, capture, &captured);
    RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(captured.trailers.empty());
    RUVIA_CHECK(captured.ended.empty());
    RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
}

struct Reentry final {
    ruvia::Http3Connection* connection{nullptr};
    std::size_t count{0};
    std::size_t rejected{0};
};

void reenter(void* opaque, const ruvia::Http3ConnectionEvent&) {
    auto& state = *static_cast<Reentry*>(opaque);
    ++state.count;
    try {
        (void)state.connection->feed(0, {}, false, false, reenter, opaque);
    } catch (const std::logic_error&) {
        ++state.rejected;
    }
}

RUVIA_TEST(http3_connection_rejects_callback_reentry_for_all_request_events) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
    Reentry state{.connection = &connection};

    auto headAndBody = requestWire(&resource, "POST", "/reenter", "x");
    RUVIA_CHECK(connection.feed(0, headAndBody, true, false, reenter, &state).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(state.count, 3U);
    RUVIA_CHECK_EQ(state.rejected, 3U);

    state = {.connection = &connection};
    const auto initial = requestWire(&resource, "GET", "/trailer-reenter", "");
    RUVIA_CHECK(connection.feed(4, initial, false, false, reenter, &state).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    const std::array<ruvia::Http3FieldSectionFieldView, 1> fields{{{"x-end", "yes"}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
    std::array<char, 16> frame{};
    const auto header = ruvia::encodeHttp3FrameHeader(frame, 1, section->size());
    std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*header));
    wire.insert(wire.end(), section->begin(), section->end());
    RUVIA_CHECK(connection.feed(4, wire, true, false, reenter, &state).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(state.count, 3U);
    RUVIA_CHECK_EQ(state.rejected, 3U);

    state = {.connection = &connection};
    const auto partial = requestWire(&resource, "GET", "/reset-reenter", "");
    RUVIA_CHECK(connection.feed(8, std::span(partial).first(1), false, false, reenter, &state).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(connection.feed(8, {}, false, true, reenter, &state).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK_EQ(state.count, 1U);
    RUVIA_CHECK_EQ(state.rejected, 1U);
}

RUVIA_TEST(http3_connection_callback_exception_ends_unrecoverable_feed) {
    CountingResource resource;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        const auto wire = requestWire(&resource, "POST", "/exception", "ok");
        auto throwing = +[](void*, const ruvia::Http3ConnectionEvent& event) {
            if (event.kind == ruvia::Http3ConnectionEventKind::kRequestHead) {
                throw std::runtime_error("request head owner failed");
            }
        };
        bool failed = false;
        try {
            (void)connection.feed(0, wire, true, false, throwing, nullptr);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        const auto later = connection.feed(4, {}, false, false, ignoreEvent, nullptr);
        RUVIA_CHECK(later.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(later.code == ruvia::Http3ConnectionErrorCode::kInternalError);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_does_not_retain_closed_sparse_stream_ids) {
    CountingResource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
    const auto request = requestWire(&resource, "GET", "/sparse", "");
    const auto before = resource.allocations;
    for (std::uint64_t stream = 0; stream < 4000; stream += 4) {
        RUVIA_CHECK(connection.feed(stream, request, true, false, ignoreEvent, nullptr).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
    }
    const auto retained = resource.allocations - resource.deallocations;
    RUVIA_CHECK(retained <= 2U);
    RUVIA_CHECK(resource.allocations >= before);
}

RUVIA_TEST(http3_connection_accepts_control_settings_and_ignores_zero_length_data) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
    Captured captured;
    constexpr std::array<char, 4> settings{0x00, 0x04, 0x00, 0x00};
    const auto settingResult = connection.feed(2, settings, false, false, capture, &captured);
    RUVIA_CHECK(settingResult.scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(connection.peerSettings().has_value());

    const auto initial = requestWire(&resource, "GET", "/zero-data", "");
    RUVIA_CHECK(connection.feed(0, initial, false, false, capture, &captured).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    constexpr std::array<char, 2> zeroData{0x00, 0x00};
    RUVIA_CHECK(connection.feed(0, zeroData, true, false, capture, &captured).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(captured.bodies.size(), 1U);
    RUVIA_CHECK_EQ(captured.bodies[0], "");
    RUVIA_CHECK_EQ(captured.ended.size(), 1U);
}

RUVIA_TEST(http3_connection_rejects_content_length_mismatch_and_releases_request_state) {
    CountingResource resource;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        const std::array<ruvia::Http3FieldSectionFieldView, 5> fields{{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"content-length", "4"}}};
        const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
        std::array<char, 128> wire{};
        const auto header = ruvia::encodeHttp3FrameHeader(wire, 1, section->size());
        std::copy(section->begin(), section->end(), wire.begin() + static_cast<std::ptrdiff_t>(*header));
        Captured captured;
        const auto result = connection.feed(0,
            std::span<const char>(wire).first(*header + section->size()), true, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kMessageError);
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_prioritizes_connection_frame_error_over_same_input_stream_error) {
    CountingResource resource;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, &resource);
        const std::array<ruvia::Http3FieldSectionFieldView, 5> fields{{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
            {":path", "/"}, {"content-length", "0"}}};
        const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
        std::array<char, 16> prefix{};
        const auto prefixSize = ruvia::encodeHttp3FrameHeader(prefix, 1, section->size());
        std::pmr::vector<char> wire(&resource);
        wire.insert(wire.end(), prefix.begin(), prefix.begin() + *prefixSize);
        wire.insert(wire.end(), section->begin(), section->end());
        wire.insert(wire.end(), {0x0, 0x1, 'x', 0x2, 0x0});
        Captured captured;
        const auto result = connection.feed(0, wire, false, false, capture, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
        RUVIA_CHECK_EQ(captured.bodies.size(), 1U);
        RUVIA_CHECK(captured.bodies[0].empty());
        RUVIA_CHECK(captured.ended.empty());
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
        const auto subsequent = connection.feed(4, {}, false, false, capture, &captured);
        RUVIA_CHECK(subsequent.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(subsequent.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_client_connection_prioritizes_frame_error_over_same_input_response_error) {
    CountingResource resource;
    {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kClient, &resource);
        RUVIA_CHECK(connection.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{":status", "200"}, {"content-length", "0"}}};
        const auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
        std::array<char, 16> prefix{};
        const auto prefixSize = ruvia::encodeHttp3FrameHeader(prefix, 1, section->size());
        std::pmr::vector<char> wire(&resource);
        wire.insert(wire.end(), prefix.begin(), prefix.begin() + *prefixSize);
        wire.insert(wire.end(), section->begin(), section->end());
        wire.insert(wire.end(), {0x0, 0x1, 'x', 0x2, 0x0});
        ClientCaptured captured;
        const auto result = connection.feed(0, wire, false, false, captureClient, &captured);
        RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
        RUVIA_CHECK_EQ(captured.statuses.at(0), 200U);
        RUVIA_CHECK(captured.ended.empty());
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
        const auto subsequent = connection.feed(4, {}, false, false, captureClient, &captured);
        RUVIA_CHECK(subsequent.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(subsequent.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_rejects_unauthorized_client_push_and_ignores_unknown_uni_streams) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kClient, &resource);
    constexpr std::array<char, 1> pushType{0x01};
    const auto push = connection.feed(3, pushType, false, false, ignoreEvent, nullptr);
    RUVIA_CHECK(push.status == ruvia::Http3ConnectionStatus::kConnectionError);
    RUVIA_CHECK(push.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(push.code == ruvia::Http3ConnectionErrorCode::kIdError);
    ruvia::Http3Connection closedPush(ruvia::Http3PeerRole::kClient, &resource);
    const auto closed = closedPush.feed(3, pushType, true, false, ignoreEvent, nullptr);
    RUVIA_CHECK(closed.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(closed.code == ruvia::Http3ConnectionErrorCode::kIdError);

    ruvia::Http3Connection unknown(ruvia::Http3PeerRole::kClient, &resource);
    constexpr std::array<char, 2> unknownType{0x40, 0x21};
    RUVIA_CHECK(unknown.feed(3, std::span(unknownType).first(1), false, false, ignoreEvent, nullptr).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(unknown.feed(3, std::span(unknownType).subspan(1), false, false, ignoreEvent, nullptr).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(unknown.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(unknown.feed(0, {}, false, false, ignoreEvent, nullptr).status ==
                ruvia::Http3ConnectionStatus::kNeedMoreData);

    ruvia::Http3Connection illegal(ruvia::Http3PeerRole::kClient, &resource);
    const auto result = illegal.feed(1, {}, false, false, ignoreEvent, nullptr);
    RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(result.code == ruvia::Http3ConnectionErrorCode::kStreamCreationError);
}

RUVIA_TEST(http3_connection_routes_registered_client_responses_and_releases_terminal_state) {
    CountingResource resource;
    ClientCaptured captured;
    std::string savedBody;
    {
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
        RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(client.registerClientRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK_EQ(client.activeRequestCount(), 2U);
        const auto duplicate = client.registerClientRequest(0, ruvia::HttpKnownMethod::kPost);
        RUVIA_CHECK(duplicate.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(duplicate.code == ruvia::Http3ConnectionErrorCode::kStreamCreationError);
        const auto unregistered = client.feed(8, {}, false, false, captureClient, &captured);
        RUVIA_CHECK(unregistered.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(unregistered.code == ruvia::Http3ConnectionErrorCode::kStreamCreationError);

        const auto first = responseWire(&resource, 200, "alpha");
        const auto second = responseWire(&resource, 404, "beta");
        for (std::size_t i = 0; i < first.size(); ++i) {
            const auto result = client.feed(0, std::span(first).subspan(i, 1), i + 1 == first.size(),
                false, captureClient, &captured);
            RUVIA_CHECK(result.status == (i + 1 == first.size()
                                                 ? ruvia::Http3ConnectionStatus::kMessageEnd
                                                 : ruvia::Http3ConnectionStatus::kNeedMoreData));
        }
        savedBody = captured.bodies[0];
        RUVIA_CHECK(client.feed(4, second, true, false, captureClient, &captured).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(captured.statuses[0], 200U);
        RUVIA_CHECK_EQ(captured.statuses[4], 404U);
        RUVIA_CHECK_EQ(captured.bodies[0], "alpha");
        RUVIA_CHECK_EQ(captured.bodies[4], "beta");
        RUVIA_CHECK_EQ(savedBody, "alpha");
        const auto& firstKinds = captured.kinds.at(0);
        RUVIA_CHECK(firstKinds.size() >= 3);
        RUVIA_CHECK(firstKinds.front() == ruvia::Http3ConnectionEventKind::kFinalHead);
        RUVIA_CHECK(firstKinds.back() == ruvia::Http3ConnectionEventKind::kMessageEnd);
        RUVIA_CHECK(responsePlan(captured, 0, 0) != nullptr &&
                    responsePlan(captured, 0, 0)->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithContent);
        for (std::size_t i = 1; i + 1 < firstKinds.size(); ++i) {
            RUVIA_CHECK(firstKinds[i] == ruvia::Http3ConnectionEventKind::kBody);
            RUVIA_CHECK(!captured.responseBodyPlans.at(0)[i]);
        }
        RUVIA_CHECK(samePlan(responsePlan(captured, 0, 0), responsePlan(captured, 0, firstKinds.size() - 1)));
        RUVIA_CHECK(responsePlan(captured, 4, 0) != nullptr);
        RUVIA_CHECK(responsePlan(captured, 4, 0)->responseStatus().value() == 404);
        RUVIA_CHECK(samePlan(responsePlan(captured, 4, 0), responsePlan(captured, 4, 2)));
        RUVIA_CHECK_EQ(client.activeRequestCount(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_client_bridge_preserves_final_response_body_plans) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
    ClientCaptured captured;
    const std::array registrations{
        std::pair{0U, ruvia::HttpKnownMethod::kGet},
        std::pair{4U, ruvia::HttpKnownMethod::kHead},
        std::pair{8U, ruvia::HttpKnownMethod::kGet},
        std::pair{12U, ruvia::HttpKnownMethod::kGet},
        std::pair{16U, ruvia::HttpKnownMethod::kConnect},
    };
    for (const auto& [streamId, method] : registrations) {
        RUVIA_CHECK(client.registerClientRequest(streamId, method).scope == ruvia::Http3ConnectionErrorScope::kNone);
    }

    auto informational = responseMessage(&resource, 103, std::nullopt, {}, false);
    auto getWire = responseMessage(&resource, 200, 3, "abc", true);
    informational.insert(informational.end(), getWire.begin(), getWire.end());
    const auto headWire = responseMessage(&resource, 200, 123, {}, false);
    const auto noContentWire = responseMessage(&resource, 204, std::nullopt, {}, false);
    const auto notModifiedWire = responseMessage(&resource, 304, 123, {}, false);
    const auto connectWire = responseMessage(&resource, 200, std::nullopt, "tunnel", true);
    const std::array<std::pair<std::uint64_t, std::vector<char>>, 5> responses{{{0, std::move(informational)}, {4, headWire}, {8, noContentWire}, {12, notModifiedWire}, {16, connectWire}}};
    for (const auto& [streamId, wire] : responses) {
        RUVIA_CHECK(client.feed(streamId, wire, true, false, captureClient, &captured).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
    }

    RUVIA_CHECK((captured.kinds.at(0) == std::vector<ruvia::Http3ConnectionEventKind>{
                                             ruvia::Http3ConnectionEventKind::kInformationalHead, ruvia::Http3ConnectionEventKind::kFinalHead,
                                             ruvia::Http3ConnectionEventKind::kBody, ruvia::Http3ConnectionEventKind::kMessageEnd}));
    RUVIA_CHECK(!captured.responseBodyPlans.at(0)[0]);
    RUVIA_CHECK(responsePlan(captured, 0, 1) != nullptr &&
                responsePlan(captured, 0, 1)->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(!captured.responseBodyPlans.at(0)[2]);
    RUVIA_CHECK(samePlan(responsePlan(captured, 0, 1), responsePlan(captured, 0, 3)));

    for (const auto streamId : {4U, 8U, 12U}) {
        RUVIA_CHECK((captured.kinds.at(streamId) == std::vector<ruvia::Http3ConnectionEventKind>{
                                                        ruvia::Http3ConnectionEventKind::kFinalHead, ruvia::Http3ConnectionEventKind::kMessageEnd}));
        const auto* plan = responsePlan(captured, streamId, 0);
        RUVIA_CHECK(plan != nullptr && plan->contentSemantics() == ruvia::HttpResponseContentSemantics::kWithoutContent);
        RUVIA_CHECK(plan != nullptr && plan->bodySuppressed());
        RUVIA_CHECK(samePlan(plan, responsePlan(captured, streamId, 1)));
    }
    RUVIA_CHECK((captured.kinds.at(16) == std::vector<ruvia::Http3ConnectionEventKind>{
                                              ruvia::Http3ConnectionEventKind::kFinalHead, ruvia::Http3ConnectionEventKind::kTunnelData,
                                              ruvia::Http3ConnectionEventKind::kMessageEnd}));
    const auto* connectPlan = responsePlan(captured, 16, 0);
    RUVIA_CHECK(connectPlan != nullptr && connectPlan->contentSemantics() == ruvia::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(connectPlan != nullptr && connectPlan->bodySuppressed());
    RUVIA_CHECK(!captured.responseBodyPlans.at(16)[1]);
    RUVIA_CHECK(samePlan(connectPlan, responsePlan(captured, 16, 2)));
    RUVIA_CHECK_EQ(captured.bodies[0], "abc");
    RUVIA_CHECK_EQ(captured.bodies[16], "tunnel");
    RUVIA_CHECK_EQ(client.activeRequestCount(), 0U);
}

RUVIA_TEST(http3_connection_client_reset_and_stream_errors_are_isolated) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
    ClientCaptured captured;
    RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(client.registerClientRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto reset = client.feed(0, {}, false, true, captureClient, &captured);
    RUVIA_CHECK(reset.status == ruvia::Http3ConnectionStatus::kReset);
    RUVIA_CHECK_EQ(captured.reset.size(), 1U);
    RUVIA_CHECK_EQ(client.activeRequestCount(), 1U);
    const std::array<ruvia::Http3FieldSectionFieldView, 2> badFields{{{":status", "200"}, {"content-length", "2"}}};
    const auto badSection = ruvia::encodeHttp3FieldSection(badFields, &resource);
    std::array<char, 16> badFrameHeader{};
    const auto badHeaderLength = ruvia::encodeHttp3FrameHeader(badFrameHeader, 1, badSection->size());
    std::vector<char> badWire(badFrameHeader.begin(), badFrameHeader.begin() + static_cast<std::ptrdiff_t>(*badHeaderLength));
    badWire.insert(badWire.end(), badSection->begin(), badSection->end());
    std::array<char, 16> dataHeader{};
    const auto dataHeaderLength = ruvia::encodeHttp3FrameHeader(dataHeader, 0, 1);
    badWire.insert(badWire.end(), dataHeader.begin(), dataHeader.begin() + static_cast<std::ptrdiff_t>(*dataHeaderLength));
    badWire.push_back('x');
    const auto badStream = client.feed(4, badWire, true, false, captureClient, &captured);
    RUVIA_CHECK(badStream.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(badStream.code == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK_EQ(client.activeRequestCount(), 0U);
    RUVIA_CHECK(client.registerClientRequest(8, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto good = responseWire(&resource, 200, "");
    RUVIA_CHECK(client.feed(8, good, true, false, captureClient, &captured).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
}

RUVIA_TEST(http3_connection_client_qpack_errors_latch_connection_and_validate_ids_and_budget) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource, {.maxActiveStreams = 1});
    auto invalidRegistration = client.registerClientRequest(1, ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(invalidRegistration.code == ruvia::Http3ConnectionErrorCode::kStreamCreationError);
    RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(client.registerClientRequest(4, ruvia::HttpKnownMethod::kGet).code ==
                ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK(client.feed(1, {}, false, false, ignoreEvent, nullptr).code ==
                ruvia::Http3ConnectionErrorCode::kStreamCreationError);

    ruvia::Http3Connection broken(ruvia::Http3PeerRole::kClient, &resource);
    RUVIA_CHECK(broken.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(broken.registerClientRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    constexpr std::array<char, 4> dynamicReference{0x01, 0x02, 0x01, 0x00};
    const auto error = broken.feed(0, dynamicReference, false, false, ignoreEvent, nullptr);
    RUVIA_CHECK(error.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(error.code == ruvia::Http3ConnectionErrorCode::kQpackDecompressionFailed);
    RUVIA_CHECK(broken.feed(0, {}, false, false, ignoreEvent, nullptr).code == error.code);
    RUVIA_CHECK_EQ(broken.activeRequestCount(), 0U);
    RUVIA_CHECK(broken.registerClientRequest(8, ruvia::HttpKnownMethod::kGet).code == error.code);
}

RUVIA_TEST(http3_connection_retirement_returns_all_protocol_storage_without_events) {
    CountingResource resource;
    for (const auto role : {ruvia::Http3PeerRole::kServer, ruvia::Http3PeerRole::kClient}) {
        ruvia::Http3Connection connection(role, &resource);
        unsigned events = 0;
        const auto callback = [](void* opaque, const ruvia::Http3ConnectionEvent&) {
            ++*static_cast<unsigned*>(opaque);
        };
        const std::array<char, 3> settings{0, 4, 0};
        const auto controlId = role == ruvia::Http3PeerRole::kServer ? 2U : 3U;
        RUVIA_CHECK(connection.feed(controlId, settings, false, false, callback, &events).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(connection.peerSettings().has_value());
        if (role == ruvia::Http3PeerRole::kClient) {
            (void)connection.registerClientRequest(0, ruvia::HttpKnownMethod::kGet);
            (void)connection.registerClientRequest(4, ruvia::HttpKnownMethod::kGet);
        }
        const auto wire = role == ruvia::Http3PeerRole::kServer
                              ? requestWire(&resource, "POST", "/items", "payload")
                              : responseWire(&resource, 200, "payload");
        RUVIA_CHECK(connection.feed(0, std::span(wire).first(3), false, false, callback, &events).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(connection.feed(4, wire, true, false, callback, &events).status == ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 1U);
        const auto previousEvents = events;
        RUVIA_CHECK(connection.retire());
        RUVIA_CHECK_EQ(events, previousEvents);
        RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
        RUVIA_CHECK(!connection.retire() && !connection.peerSettings() && !connection.peerGoawayId());
        RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
        RUVIA_CHECK(connection.feed(8, wire, true, false, callback, &events).scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(connection.registerClientRequest(8, ruvia::HttpKnownMethod::kGet).scope != ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK_EQ(events, previousEvents);
        auto moved = std::move(connection);
        RUVIA_CHECK(!moved.retire() && !connection.retire());
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_locally_retires_server_parsers_without_peer_reset_events) {
    CountingResource resource;
    {
        ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &resource, {.maxActiveStreams = 1});
        Captured events;
        const auto wire = requestWire(&resource, "POST", "/items", "payload");
        std::size_t baseline = 0;
        for (std::uint64_t step = 0; step < 32; ++step) {
            const auto id = step * 4;
            const auto input = (step & 1U) == 0 ? std::span(wire).first(3) : std::span(wire);
            RUVIA_CHECK(server.feed(id, input, false, false, capture, &events).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK_EQ(server.activeRequestCount(), 1U);
            RUVIA_CHECK(!server.retireClientRequest(id));
            const auto before = resource.deallocations;
            RUVIA_CHECK(server.retireServerRequest(id));
            RUVIA_CHECK(!server.retireServerRequest(id));
            RUVIA_CHECK(resource.deallocations > before);
            RUVIA_CHECK_EQ(server.activeRequestCount(), 0U);
            if (step == 0) {
                baseline = resource.allocations - resource.deallocations;
            } else {
                RUVIA_CHECK_EQ(resource.allocations - resource.deallocations, baseline);
            }
        }
        RUVIA_CHECK(events.reset.empty() && events.ended.empty());
        RUVIA_CHECK(!server.retireServerRequest(2));
        RUVIA_CHECK(server.feed(128, wire, true, false, capture, &events).status == ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK(events.ended.size() == 1 && events.bodies.back() == "payload");
        RUVIA_CHECK(!server.retireServerRequest(128));
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
        (void)client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet);
        RUVIA_CHECK(!client.retireServerRequest(0));
        RUVIA_CHECK_EQ(client.activeRequestCount(), 1U);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_cannot_retire_server_parser_from_feed_callback) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &resource);
    struct Attempt {
        ruvia::Http3Connection* connection;
        bool called{};
        bool removed{};
    } attempt{&server};
    const auto wire = requestWire(&resource, "POST", "/items", "payload");
    const auto result = server.feed(0, wire, true, false, [](void* opaque, const ruvia::Http3ConnectionEvent& event) {
            auto& state = *static_cast<Attempt*>(opaque);
            if (event.kind == ruvia::Http3ConnectionEventKind::kRequestHead) {
                state.called = true;
                state.removed = state.connection->retireServerRequest(event.streamId) || state.connection->retire();
            } }, &attempt);
    RUVIA_CHECK(attempt.called && !attempt.removed);
    RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
}

RUVIA_TEST(http3_connection_retires_only_live_client_response_parser_state) {
    CountingResource resource;
    {
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
        ClientCaptured events;
        RUVIA_CHECK(!client.retireClientRequest(0));
        RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(client.registerClientRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto partial = responseWire(&resource, 200, "partial");
        RUVIA_CHECK(client.feed(0, std::span(partial).first(3), false, false, captureClient, &events).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK_EQ(client.activeRequestCount(), 2U);
        RUVIA_CHECK(!client.retireClientRequest(2));
        RUVIA_CHECK(client.retireClientRequest(0));
        RUVIA_CHECK(!client.retireClientRequest(0));
        RUVIA_CHECK_EQ(client.activeRequestCount(), 1U);
        const auto completed = responseWire(&resource, 200, "ok");
        RUVIA_CHECK(client.feed(4, completed, true, false, captureClient, &events).status ==
                    ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK(events.bodies[4] == "ok");
        RUVIA_CHECK_EQ(client.activeRequestCount(), 0U);
        RUVIA_CHECK(!client.retireClientRequest(4));
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http3_connection_cannot_retire_client_parser_from_its_feed_callback) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
    struct Attempt final {
        ruvia::Http3Connection* connection{};
        bool called{};
        bool removed{};
    } attempt{&client};
    RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto wire = responseWire(&resource, 200, "ok");
    const auto parsed = client.feed(0, wire, true, false, [](void* opaque, const ruvia::Http3ConnectionEvent& event) {
        auto& state = *static_cast<Attempt*>(opaque);
        if (event.kind == ruvia::Http3ConnectionEventKind::kFinalHead) {
            state.called = true;
            state.removed = state.connection->retireClientRequest(event.streamId) || state.connection->retire();
        } }, &attempt);
    RUVIA_CHECK(attempt.called && !attempt.removed);
    RUVIA_CHECK(parsed.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(client.activeRequestCount(), 0U);
}

RUVIA_TEST(http3_connection_unprocessed_evidence_excludes_observed_responses_and_unknown_requests) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
    const auto rejected = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestRejected);
    for (const auto id : {0U, 4U, 8U, 12U}) {
        RUVIA_CHECK(client.registerClientRequest(id, ruvia::HttpKnownMethod::kPost).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(!client.peerReportsUnprocessed(id));
        RUVIA_CHECK(client.peerReportsUnprocessed(id, rejected));
        RUVIA_CHECK(!client.peerReportsUnprocessed(id, 0));
    }
    RUVIA_CHECK(!client.peerReportsUnprocessed(16, rejected));
    const auto finalHead = responseWire(&resource, 200, "");
    RUVIA_CHECK(client.feed(8, finalHead, false, false, ignoreEvent, nullptr).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const std::array informationalFields{ruvia::Http3FieldSectionFieldView{":status", "103"}};
    const auto section = ruvia::encodeHttp3FieldSection(informationalFields, &resource);
    std::vector<char> informational(16);
    const auto prefix = ruvia::encodeHttp3FrameHeader(informational, 1, section->size());
    informational.resize(*prefix);
    informational.insert(informational.end(), section->begin(), section->end());
    RUVIA_CHECK(client.feed(12, informational, false, false, ignoreEvent, nullptr).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(!client.peerReportsUnprocessed(8, rejected));
    RUVIA_CHECK(!client.peerReportsUnprocessed(12, rejected));
    constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
    RUVIA_CHECK(client.feed(3, control, false, false, ignoreEvent, nullptr).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(!client.peerReportsUnprocessed(0));
    RUVIA_CHECK(client.peerReportsUnprocessed(4));
    RUVIA_CHECK(!client.peerReportsUnprocessed(8));
    RUVIA_CHECK(!client.peerReportsUnprocessed(12));
    RUVIA_CHECK(client.retireClientRequest(4));
    RUVIA_CHECK(!client.peerReportsUnprocessed(4, rejected));
    RUVIA_CHECK(client.feed(3, {}, true, false, ignoreEvent, nullptr).scope ==
                ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(!client.peerReportsUnprocessed(0, rejected));
}

RUVIA_TEST(http3_connection_client_does_not_admit_requests_past_peer_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
    constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
    RUVIA_CHECK(client.feed(3, control, false, false, ignoreEvent, nullptr).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(client.peerGoawayId() == 4);
    RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto rejected = client.registerClientRequest(4, ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(rejected.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(rejected.code == ruvia::Http3ConnectionErrorCode::kRequestRejected);
    RUVIA_CHECK_EQ(client.activeRequestCount(), 1U);
}

RUVIA_TEST(http3_connection_moved_from_metadata_is_empty) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3Connection connection(ruvia::Http3PeerRole::kClient, &resource);
    auto moved = std::move(connection);
    RUVIA_CHECK(!connection.peerSettings().has_value());
    RUVIA_CHECK(!connection.peerGoawayId().has_value());
    RUVIA_CHECK_EQ(connection.activeRequestCount(), 0U);
    RUVIA_CHECK(!moved.peerSettings().has_value());
}

RUVIA_TEST(http3_connection_exposes_peer_goaway_after_settings_for_both_roles) {
    std::pmr::monotonic_buffer_resource resource;
    {
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &resource);
        RUVIA_CHECK(!client.peerGoawayId().has_value());
        constexpr std::array<char, 9> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04, 0x07, 0x01, 0x00};
        for (std::size_t i = 0; i < control.size(); ++i) {
            const auto result = client.feed(3, std::span(control).subspan(i, 1), false, false, ignoreEvent, nullptr);
            RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(client.peerGoawayId() == (i < 5 ? std::nullopt : i < 8 ? std::optional<std::uint64_t>{4}
                                                                               : std::optional<std::uint64_t>{0}));
        }
        constexpr std::array<char, 3> increasing{0x07, 0x01, 0x04};
        const auto invalid = client.feed(3, increasing, false, false, ignoreEvent, nullptr);
        RUVIA_CHECK(invalid.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(invalid.code == ruvia::Http3ConnectionErrorCode::kIdError);
        RUVIA_CHECK(client.peerGoawayId() == 0);
    }
    {
        ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &resource);
        RUVIA_CHECK(!server.peerGoawayId().has_value());
        constexpr std::array<char, 9> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x05, 0x07, 0x01, 0x03};
        for (std::size_t i = 0; i < control.size(); ++i) {
            const auto result = server.feed(2, std::span(control).subspan(i, 1), false, false, ignoreEvent, nullptr);
            RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(server.peerGoawayId() == (i < 5 ? std::nullopt : i < 8 ? std::optional<std::uint64_t>{5}
                                                                               : std::optional<std::uint64_t>{3}));
        }
        constexpr std::array<char, 3> increasing{0x07, 0x01, 0x04};
        const auto invalid = server.feed(2, increasing, false, false, ignoreEvent, nullptr);
        RUVIA_CHECK(invalid.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(invalid.code == ruvia::Http3ConnectionErrorCode::kIdError);
        RUVIA_CHECK(server.peerGoawayId() == 3);
    }
}
