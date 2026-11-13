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
#include <vector>

#include "ruvia/http/http3_connection.h"
namespace ruvia::test {

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
        if (produced.status_ == http3_transport_status::would_block) {
            return true;
        }
        const auto output_size = output.size();
        if (produced.status_ != http3_transport_status::accepted ||
            source.commit_output(output.stream_id_, output_size) != http3_transport_status::accepted) {
            return false;
        }
        input.clear();
        input.reserve(output_size);
        for (std::size_t index = 0; index < output.buffer_count_; ++index) {
            input.insert(input.end(), output.buffers_[index].begin(), output.buffers_[index].end());
        }
        const auto received = destination.read_stream(output.stream_id_, input, output.fin_, timestamp++);
        if (received.status_ != http3_transport_status::accepted) {
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
        } else if (source.acknowledge_output(output.stream_id_, output_size) != http3_transport_status::accepted) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] inline bool acknowledge_http3(
    http3_connection& source, const std::vector<http3_withheld_acknowledgement>& withheld) {
    for (const auto& acknowledgement : withheld) {
        if (source.acknowledge_output(acknowledgement.stream_id_, acknowledgement.bytes_) !=
            http3_transport_status::accepted) {
            return false;
        }
    }
    return true;
}

struct http3_field_record final {
    std::string name_;
    std::string value_;
};

[[nodiscard]] inline std::optional<std::string_view> find_http3_field(
    std::span<const http3_field_record> fields, std::string_view name) noexcept {
    for (const auto& field : fields) {
        if (field.name_ == name) {
            return field.value_;
        }
    }
    return std::nullopt;
}

// A received head. A server's request head lists the pseudo-header fields
// the validated head reports, then the fields of the request it builds (so
// Host follows from :authority and Cookie crumbs are joined). A client's
// response head carries its status and no :status field.
struct http3_headers_record final {
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const noexcept {
        return find_http3_field(fields_, name);
    }

    std::int64_t stream_id_{-1};
    std::vector<http3_field_record> fields_;
    bool stream_ended_{false};
    std::optional<http_received_status> status_;
    bool early_data_{false};
};

// A message end and the trailer section that preceded it (empty for none).
struct http3_message_end_record final {
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const noexcept {
        return find_http3_field(trailers_, name);
    }

    std::int64_t stream_id_{-1};
    std::vector<http3_field_record> trailers_;
};

// Owned copy of every event a codec published, in arrival order. sequence_
// records one letter per event kind (H head, D data, E message end, C stream
// close, G goaway, T transport action, U request unprocessed, O origin
// advertisement, X datagram) with consecutive DATA events collapsed, so
// tests can assert frame order independent of chunking.
class http3_event_log final {
public:
    void operator()(const http3_event& event) {
        if (const auto* request = event.request_head()) {
            record_request_head(request->head());
        } else if (const auto* interim = event.informational_head()) {
            record_response_head(interim->stream_id(), interim->head(), false);
        } else if (const auto* head = event.response_head()) {
            record_response_head(head->stream_id(), head->head(), head->stream_ended());
            content_lengths_.emplace_back(head->stream_id(), head->head().content_length());
        } else if (const auto* data = event.data()) {
            data_.emplace_back(data->stream_id(), std::string(data->bytes()));
            if (sequence_.empty() || sequence_.back() != 'D') {
                sequence_.push_back('D');
            }
        } else if (const auto* end = event.message_end()) {
            http3_message_end_record record{end->stream_id(), {}};
            for (const auto& field : end->trailers()) {
                record.trailers_.push_back({std::string(field.name()), std::string(field.value())});
            }
            ends_.push_back(std::move(record));
            sequence_.push_back('E');
        } else if (const auto* close = event.stream_close()) {
            closes_.push_back(*close);
            sequence_.push_back('C');
        } else if (const auto* goaway = event.goaway()) {
            goaways_.push_back(goaway->id());
            sequence_.push_back('G');
        } else if (const auto* action = event.transport_action()) {
            transport_actions_.push_back(*action);
            sequence_.push_back('T');
        } else if (const auto* unprocessed = event.request_unprocessed()) {
            unprocessed_.push_back(unprocessed->stream_id());
            sequence_.push_back('U');
        } else if (const auto* datagram = event.datagram()) {
            datagrams_.emplace_back(datagram->stream_id(), std::string(datagram->payload()));
            sequence_.push_back('X');
        } else if (const auto* origins = event.origin_advertisement()) {
            std::vector<std::string> list;
            list.reserve(origins->origins_.size());
            for (const auto& origin : origins->origins_) {
                list.emplace_back(origin);
            }
            origins_.push_back(std::move(list));
            sequence_.push_back('O');
        }
    }

    void record_request_head(const http3_request_head& head) {
        http3_headers_record record{head.stream_id(), {}, head.stream_ended(), std::nullopt, head.early_data()};
        const auto pseudo = [&](std::string_view name, std::string_view value) {
            if (!value.empty()) {
                record.fields_.push_back({std::string(name), std::string(value)});
            }
        };
        pseudo(":method", head.method());
        pseudo(":scheme", head.scheme());
        pseudo(":authority", head.authority());
        pseudo(":path", head.path());
        pseudo(":protocol", head.protocol());
        const auto request = head.build_request();
        for (const auto& field : request.headers()) {
            record.fields_.push_back({std::string(field.name()), std::string(field.value())});
        }
        headers_.push_back(std::move(record));
        sequence_.push_back('H');
    }

    void record_response_head(std::int64_t stream_id, const http_client_response_head& head, bool stream_ended) {
        http3_headers_record record{stream_id, {}, stream_ended, head.received_status(), false};
        for (const auto& field : head.headers()) {
            record.fields_.push_back({std::string(field.name()), std::string(field.value())});
        }
        headers_.push_back(std::move(record));
        sequence_.push_back('H');
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

    [[nodiscard]] const http3_message_end_record* end(std::int64_t stream_id) const noexcept {
        for (const auto& record : ends_) {
            if (record.stream_id_ == stream_id) {
                return &record;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool ended(std::int64_t stream_id) const noexcept {
        return end(stream_id) != nullptr;
    }

    // Any event that names `stream_id`, other than transport actions.
    [[nodiscard]] bool names_message(std::int64_t stream_id) const noexcept {
        return std::ranges::any_of(headers_, [&](const auto& record) { return record.stream_id_ == stream_id; }) ||
               std::ranges::any_of(data_, [&](const auto& chunk) { return chunk.first == stream_id; }) ||
               ended(stream_id);
    }

    void clear() {
        *this = http3_event_log{};
    }

    std::vector<http3_headers_record> headers_;
    std::vector<std::pair<std::int64_t, std::string>> data_;
    std::vector<std::pair<std::int64_t, std::optional<std::uint64_t>>> content_lengths_;
    std::vector<http3_message_end_record> ends_;
    std::vector<http3_stream_close_event> closes_;
    std::vector<std::optional<std::int64_t>> goaways_;
    std::vector<http3_transport_action_event> transport_actions_;
    std::vector<std::int64_t> unprocessed_;
    std::vector<std::pair<std::int64_t, std::string>> datagrams_;
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
        return client_.bind_control_streams(2, 6, 10) == http3_transport_status::accepted &&
               server_.bind_control_streams(3, 7, 11) == http3_transport_status::accepted;
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
    [[nodiscard]] bool send_request(std::int64_t stream_id, http3_regular_request_head_view head,
        bool end_stream, http3_event_log& server_log) {
        if (!end_stream && head.content_.without_content() != nullptr) {
            head.content_ = http_request_content_plan::streaming();
        }
        return client_.submit_request_head(stream_id, head) == http3_submit_status::accepted &&
               to_server(server_log);
    }

    http3_connection client_;
    http3_connection server_;
    std::uint64_t timestamp_{1};
};

}  // namespace ruvia::test
