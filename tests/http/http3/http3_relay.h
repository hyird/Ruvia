#pragma once

// In-memory HTTP/3 peer pair for codec tests. QUIC is replaced by an ordered,
// lossless byte relay: every stream byte one codec emits is delivered to its
// peer in order and, unless the test withholds it, acknowledged immediately.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"

namespace ruvia::testing {

// Stream data delivered to the peer whose QUIC acknowledgement has not been
// reported back to the sending codec yet.
struct http3_withheld_acknowledgement final {
    std::int64_t stream_id_{-1};
    std::size_t bytes_{0};
};

[[nodiscard]] inline std::span<const std::byte> http3_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

// Delivers everything `source` can currently send to `destination` and hands
// each decoded event to `consume` as a mutable lvalue, so a consumer may move
// it out. Input bytes are scrubbed after each delivery: a consumer must not
// retain borrowed DATA views. Returns false on any codec failure or when the
// source never runs dry within the iteration bound.
template <typename event_consumer_type>
[[nodiscard]] bool relay_http3(http3_connection& source, http3_connection& destination,
    event_consumer_type&& consume, std::uint64_t& timestamp,
    std::vector<http3_withheld_acknowledgement>* withheld = nullptr) {
    constexpr int max_outputs = 4096;
    http3_stream_output output;
    std::vector<std::byte> input;
    for (int iteration = 0; iteration < max_outputs; ++iteration) {
        const auto produced = source.peek_output(64 * 1024, output);
        if (produced.status_ == http3_status::would_block) {
            return true;
        }
        const auto output_size = output.size();
        if (produced.status_ != http3_status::accepted ||
            source.commit_output(output.stream_id_, output_size) != http3_status::accepted) {
            return false;
        }
        input.clear();
        input.reserve(output_size);
        for (std::size_t index = 0; index < output.buffer_count_; ++index) {
            input.insert(input.end(), output.buffers_[index].begin(), output.buffers_[index].end());
        }
        const auto received = destination.read_stream(output.stream_id_, input, output.fin_, timestamp++);
        if (received.status_ != http3_status::accepted) {
            return false;
        }
        while (auto event = destination.next_event()) {
            consume(*event);
        }
        std::fill(input.begin(), input.end(), std::byte{0});
        if (output_size == 0) {
            continue;
        }
        if (withheld != nullptr) {
            withheld->push_back({output.stream_id_, output_size});
        } else if (source.acknowledge_output(output.stream_id_, output_size) != http3_status::accepted) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] inline bool acknowledge_http3(
    http3_connection& source, const std::vector<http3_withheld_acknowledgement>& withheld) {
    for (const auto& acknowledgement : withheld) {
        if (source.acknowledge_output(acknowledgement.stream_id_, acknowledgement.bytes_) !=
            http3_status::accepted) {
            return false;
        }
    }
    return true;
}

struct http3_field_record final {
    std::string name_;
    std::string value_;
};

struct http3_headers_record final {
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const noexcept {
        for (const auto& field : fields_) {
            if (field.name_ == name) {
                return field.value_;
            }
        }
        return std::nullopt;
    }

    std::int64_t stream_id_{-1};
    std::vector<http3_field_record> fields_;
    bool stream_ended_{false};
};

// Owned copy of every event a codec published, in arrival order. sequence_
// records one letter per event kind (H headers, D data, E stream end,
// C stream close, G goaway, T transport action) with consecutive DATA events
// collapsed, so tests can assert frame order independent of chunking.
class http3_event_log final {
public:
    void operator()(const http3_event& event) {
        if (const auto* headers = std::get_if<http3_headers_event>(&event)) {
            http3_headers_record record{headers->stream_id_, {}, headers->stream_ended_};
            for (const auto& field : headers->fields_) {
                record.fields_.push_back({std::string(field.name()), std::string(field.value())});
            }
            headers_.push_back(std::move(record));
            sequence_.push_back('H');
        } else if (const auto* data = std::get_if<http3_data_event>(&event)) {
            data_.emplace_back(data->stream_id_, std::string(data->bytes_));
            if (sequence_.empty() || sequence_.back() != 'D') {
                sequence_.push_back('D');
            }
        } else if (const auto* end = std::get_if<http3_stream_end_event>(&event)) {
            ends_.push_back(end->stream_id_);
            sequence_.push_back('E');
        } else if (const auto* close = std::get_if<http3_stream_close_event>(&event)) {
            closes_.push_back(*close);
            sequence_.push_back('C');
        } else if (const auto* goaway = std::get_if<http3_goaway_event>(&event)) {
            goaways_.push_back(goaway->id_);
            sequence_.push_back('G');
        } else if (const auto* action = std::get_if<http3_transport_action_event>(&event)) {
            transport_actions_.push_back(*action);
            sequence_.push_back('T');
        } else if (const auto* origins = std::get_if<http_origin_advertisement>(&event)) {
            std::vector<std::string> list;
            list.reserve(origins->origins_.size());
            for (const auto& origin : origins->origins_) {
                list.emplace_back(origin);
            }
            origins_.push_back(std::move(list));
            sequence_.push_back('O');
        }
    }

    [[nodiscard]] std::string data(std::int64_t stream_id) const {
        std::string bytes;
        for (const auto& [id, chunk] : data_) {
            if (id == stream_id) {
                bytes += chunk;
            }
        }
        return bytes;
    }

    [[nodiscard]] bool ended(std::int64_t stream_id) const noexcept {
        for (const auto id : ends_) {
            if (id == stream_id) {
                return true;
            }
        }
        return false;
    }

    void clear() {
        *this = http3_event_log{};
    }

    std::vector<http3_headers_record> headers_;
    std::vector<std::pair<std::int64_t, std::string>> data_;
    std::vector<std::int64_t> ends_;
    std::vector<http3_stream_close_event> closes_;
    std::vector<std::int64_t> goaways_;
    std::vector<http3_transport_action_event> transport_actions_;
    std::vector<std::vector<std::string>> origins_;
    std::string sequence_;
};

// A client and a server codec with their control and QPACK streams bound to
// the identifiers QUIC would open first for each role (RFC 9000 §2.1).
class http3_peers final {
public:
    explicit http3_peers(http3_connection_options server_options = {},
        http3_connection_options client_options = {})
        : client_(http3_connection::client(client_options)),
          server_(http3_connection::server(server_options)) {}

    [[nodiscard]] bool bind() {
        return client_.bind_control_streams(2, 6, 10) == http3_status::accepted &&
               server_.bind_control_streams(3, 7, 11) == http3_status::accepted;
    }

    template <typename event_consumer_type>
    [[nodiscard]] bool to_server(event_consumer_type&& consume) {
        return relay_http3(client_, server_, std::forward<event_consumer_type>(consume), timestamp_);
    }

    template <typename event_consumer_type>
    [[nodiscard]] bool to_client(event_consumer_type&& consume,
        std::vector<http3_withheld_acknowledgement>* withheld = nullptr) {
        return relay_http3(server_, client_, std::forward<event_consumer_type>(consume), timestamp_, withheld);
    }

    // Submits a request head on `stream_id` and delivers it to the server.
    [[nodiscard]] bool send_request(std::int64_t stream_id, std::span<const http3_field_view> fields,
        bool end_stream, http3_event_log& server_log) {
        return client_.submit_request(stream_id, fields, end_stream) == http3_status::accepted &&
               to_server(server_log);
    }

    http3_connection client_;
    http3_connection server_;
    std::uint64_t timestamp_{1};
};

}  // namespace ruvia::testing
