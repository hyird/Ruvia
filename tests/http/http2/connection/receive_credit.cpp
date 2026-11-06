#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

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

// Acknowledging a credit for the whole advertised receive window restores
// exactly one window on the connection and on the stream.
RUVIA_TEST(http2_public_receive_credit_restores_exact_window) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto receive_window = advertised_receive_window(ruvia::testing::drain_output(client));
    RUVIA_CHECK(receive_window != 0);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
    (void)ruvia::testing::drain_output(client);

    std::pmr::string wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    // The client's advertised stream window applies once the peer ACKs it.
    append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
    std::pmr::string block(&resource);
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
    // Received bytes are credited only on acknowledgement.
    std::pmr::string prefix(&resource);
    append_frame(prefix, ruvia::http2_frame_type::settings, 0x1, 0, {});
    RUVIA_CHECK(ruvia::testing::drain_output(client) == std::string_view(prefix));
    RUVIA_CHECK(client.acknowledge(std::move(*credit)) == ruvia::http2_received_data_acknowledge_status::acknowledged);
    RUVIA_CHECK(!credit->valid());

    const auto output = ruvia::testing::drain_output(client);
    std::string_view updates(output);
    std::size_t connection_updates = 0;
    std::size_t stream_updates = 0;
    while (!updates.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(updates.data(), updates.size()));
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

    wire.clear();
    append_body(wire, receive_window + 1);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::flow_control_error);
}
