#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "failing_memory_resource.h"
#include "output_drain.h"
#include "test_harness.h"

namespace {

void append_frame(std::pmr::string& wire, ruvia::http2_frame_type type,
    std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(
            header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

std::uint32_t read_u32(std::string_view bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes.substr(0, 4)) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

std::size_t advertised_receive_window(std::string_view output) {
    output.remove_prefix(ruvia::http2_client_preface.size());
    std::uint32_t connection_window = 65535;
    std::uint32_t stream_window = 65535;
    while (!output.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        if (!header || output.size() < ruvia::http2_frame_header_bytes + header->length_) {
            throw std::logic_error("invalid initial HTTP/2 output");
        }
        const auto payload = output.substr(ruvia::http2_frame_header_bytes, header->length_);
        if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings)) {
            for (std::size_t index = 0; index + 6 <= payload.size(); index += 6) {
                if (payload[index] == 0 && payload[index + 1] == 4) {
                    stream_window = read_u32(payload.substr(index + 2));
                }
            }
        } else if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update) && header->stream_id_ == 0) {
            connection_window += read_u32(payload) & 0x7fffffffU;
        }
        output.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return std::min(connection_window, stream_window);
}

void append_body(std::pmr::string& wire, std::size_t bytes) {
    const std::string chunk(16384, 'x');
    while (bytes != 0) {
        const auto count = std::min(bytes, chunk.size());
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1,
            std::string_view(chunk).substr(0, count));
        bytes -= count;
    }
}

}  // namespace

RUVIA_TEST(http2_public_receive_credit_retry_preserves_exact_window) {
    std::size_t allocation_failures = 0;
    for (std::size_t prefix_frames = 0; prefix_frames != 6; ++prefix_frames) {
        for (std::size_t failure_point = 0; failure_point != 2; ++failure_point) {
            failing_memory_resource resource;
            {
                auto client = ruvia::http2_connection::client({.resource_ = &resource});
                const auto receive_window = advertised_receive_window(ruvia::testing::drain_output(client));
                RUVIA_CHECK(receive_window != 0);
                const auto submitted = client.submit_request_head(
                    ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
                RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
                (void)ruvia::testing::drain_output(client);

                std::pmr::string wire;
                append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
                std::pmr::string block;
                ruvia::hpack_encoder::encode_header(block, ":status", "200");
                append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
                append_body(wire, receive_window);
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

                std::optional<ruvia::http2_received_data_credit> credit;
                while (auto event = client.next_event()) {
                    if (auto* body = event->message_body_chunk()) {
                        if (credit) {
                            credit->merge(body->take_credit());
                        } else {
                            credit.emplace(body->take_credit());
                        }
                    }
                }
                RUVIA_CHECK(credit && credit->valid());
                if (!credit) {
                    return;
                }

                wire.clear();
                for (std::size_t index = 0; index < prefix_frames; ++index) {
                    append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, "12345678");
                }
                if (!wire.empty()) {
                    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
                }
                // Output queued before the acknowledge (the SETTINGS and PING
                // acknowledgements) must survive an allocation failure untouched,
                // and a retry must append only the WINDOW_UPDATE frames.
                std::pmr::string prefix;
                append_frame(prefix, ruvia::http2_frame_type::settings, 0x1, 0, {});
                for (std::size_t index = 0; index < prefix_frames; ++index) {
                    append_frame(prefix, ruvia::http2_frame_type::ping, 0x1, 0, "12345678");
                }
                resource.fail_after(failure_point);
                try {
                    RUVIA_CHECK(client.acknowledge(std::move(*credit)) ==
                                ruvia::http2_received_data_acknowledge_status::acknowledged);
                } catch (const std::bad_alloc&) {
                    ++allocation_failures;
                    resource.allow_allocations();
                    RUVIA_CHECK(credit->valid());
                    RUVIA_CHECK(ruvia::testing::drain_output(client) == std::string_view(prefix));
                    prefix.clear();
                    RUVIA_CHECK(client.acknowledge(std::move(*credit)) ==
                                ruvia::http2_received_data_acknowledge_status::acknowledged);
                }
                resource.allow_allocations();
                RUVIA_CHECK(!credit->valid());

                const auto output = ruvia::testing::drain_output(client);
                RUVIA_CHECK(output.starts_with(prefix));
                if (!output.starts_with(prefix)) {
                    return;
                }
                std::string_view updates(output);
                updates.remove_prefix(prefix.size());
                std::size_t connection_updates = 0;
                std::size_t stream_updates = 0;
                while (!updates.empty()) {
                    const auto header = ruvia::parse_http2_frame_header(
                        std::span(updates.data(), updates.size()));
                    RUVIA_CHECK(header.has_value());
                    if (!header) {
                        return;
                    }
                    RUVIA_CHECK(header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update));
                    RUVIA_CHECK_EQ(header->length_, 4u);
                    RUVIA_CHECK_EQ(read_u32(updates.substr(ruvia::http2_frame_header_bytes)), receive_window);
                    connection_updates += header->stream_id_ == 0;
                    stream_updates += header->stream_id_ == 1;
                    updates.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
                }
                RUVIA_CHECK_EQ(connection_updates, std::size_t{1});
                RUVIA_CHECK_EQ(stream_updates, std::size_t{1});
                RUVIA_CHECK(!client.wants_write());

                // One successful acknowledgement restores exactly one window,
                // including after an allocation failure and retry.
                wire.clear();
                append_body(wire, receive_window + 1);
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
                RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::flow_control_error);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
    }
    RUVIA_CHECK(allocation_failures != 0);
}
