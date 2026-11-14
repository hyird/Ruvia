// In-memory HTTP/3 peer pair for codec tests. QUIC is replaced by an ordered,
// lossless byte relay: every stream byte one codec emits is delivered to its
// peer in order and, unless the test withholds it, acknowledged immediately.

#include "http3_connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "content_decoding_fixture.h"
#include "http1_server_response.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "websocket_handshake.h"

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
[[nodiscard]] bool relay_http3(http3_connection& source, http3_connection& destination, event_consumer_type&& consume, std::uint64_t& timestamp, std::vector<http3_withheld_acknowledgement>* withheld = nullptr) {
    constexpr int max_outputs = 4096;
    http3_stream_output output;
    std::vector<std::byte> input;
    for (int iteration = 0; iteration < max_outputs; ++iteration) {
        const auto produced = source.peek_output(64 * 1024, output);
        if (produced.status_ == http3_transport_status::would_block) {
            return true;
        }
        const auto output_size = output.size();
        if (produced.status_ != http3_transport_status::accepted || source.commit_output(output.stream_id_, output_size) != http3_transport_status::accepted) {
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

[[nodiscard]] inline bool acknowledge_http3(http3_connection& source, const std::vector<http3_withheld_acknowledgement>& withheld) {
    for (const auto& acknowledgement : withheld) {
        if (source.acknowledge_output(acknowledgement.stream_id_, acknowledgement.bytes_) != http3_transport_status::accepted) {
            return false;
        }
    }
    return true;
}

struct http3_field_record final {
    std::string name_;
    std::string value_;
};

[[nodiscard]] inline std::optional<std::string_view> find_http3_field(std::span<const http3_field_record> fields, std::string_view name) noexcept {
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
        return std::ranges::any_of(headers_,
                   [&](const auto& record) {
            return record.stream_id_ == stream_id;
        }) ||
            std::ranges::any_of(data_,
                [&](const auto& chunk) {
            return chunk.first == stream_id;
        }) ||
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
    explicit http3_peers(http3_connection_options server_options = {}, http3_connection_options client_options = {}) :
        client_(http3_connection::client(client_options)),
        server_(http3_connection::server(server_options)) {}

    [[nodiscard]] bool bind() {
        return client_.bind_control_streams(2, 6, 10) == http3_transport_status::accepted && server_.bind_control_streams(3, 7, 11) == http3_transport_status::accepted;
    }

    template <typename event_consumer_type>
    [[nodiscard]] bool to_server(event_consumer_type&& consume) {
        return relay_http3(client_, server_, std::forward<event_consumer_type>(consume), timestamp_);
    }

    template <typename event_consumer_type>
    [[nodiscard]] bool to_client(event_consumer_type&& consume, std::vector<http3_withheld_acknowledgement>* withheld = nullptr) {
        return relay_http3(server_, client_, std::forward<event_consumer_type>(consume), timestamp_, withheld);
    }

    // Submits a request head on `stream_id` and delivers it to the server.
    [[nodiscard]] bool send_request(std::int64_t stream_id, http3_regular_request_head_view head, bool end_stream, http3_event_log& server_log) {
        if (!end_stream && head.content_.without_content() != nullptr) {
            head.content_ = http_request_content_plan::streaming();
        }
        return client_.submit_request_head(stream_id, head) == http3_submit_status::accepted && to_server(server_log);
    }

    http3_connection client_;
    http3_connection server_;
    std::uint64_t timestamp_{1};
};

// Raw HTTP/3 request and response stream bytes for codec tests that must
// reach the receiving codec with field sections its peer API never sends.

// RFC 9204 §4.1.1 prefixed integer.
inline void append_qpack_integer(std::string& bytes, std::size_t value, unsigned prefix_bits, unsigned tag) {
    const auto mask = (1u << prefix_bits) - 1;
    if (value < mask) {
        bytes.push_back(static_cast<char>(tag | static_cast<unsigned>(value)));
        return;
    }
    bytes.push_back(static_cast<char>(tag | mask));
    value -= mask;
    while (value >= 128) {
        bytes.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    bytes.push_back(static_cast<char>(value));
}

// RFC 9000 §16 variable-length integer.
inline void append_quic_varint(std::string& bytes, std::uint64_t value) {
    if (value < 64) {
        bytes.push_back(static_cast<char>(value));
    } else if (value < 16384) {
        bytes.push_back(static_cast<char>(0x40 | (value >> 8)));
        bytes.push_back(static_cast<char>(value & 255));
    } else if (value < (std::uint64_t{1} << 30)) {
        bytes.push_back(static_cast<char>(0x80 | (value >> 24)));
        bytes.push_back(static_cast<char>((value >> 16) & 255));
        bytes.push_back(static_cast<char>((value >> 8) & 255));
        bytes.push_back(static_cast<char>(value & 255));
    } else {
        bytes.push_back(static_cast<char>(0xc0 | (value >> 56)));
        for (int shift = 48; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }
}

inline void append_http3_wire_frame(std::string& bytes, std::uint64_t type, std::string_view payload) {
    append_quic_varint(bytes, type);
    append_quic_varint(bytes, payload.size());
    bytes.append(payload);
}

// One field line exactly as the wire carries it.
struct http3_wire_field final {
    std::string_view name_;
    std::string_view value_;
};

// RFC 9204 §4.5.6: literal names and values, without dynamic table
// references or Huffman coding, exercise the real decoder with arbitrary
// HTTP semantics.
inline void append_http3_wire_headers(std::string& bytes, std::span<const http3_wire_field> fields) {
    std::string section(2, '\0');  // Required Insert Count = 0; Delta Base = 0.
    for (const auto& field : fields) {
        append_qpack_integer(section, field.name_.size(), 3, 0x20);
        section.append(field.name_);
        append_qpack_integer(section, field.value_.size(), 7, 0);
        section.append(field.value_);
    }
    append_http3_wire_frame(bytes, 1, section);
}

inline void append_http3_wire_headers(std::string& bytes, std::initializer_list<http3_wire_field> fields) {
    append_http3_wire_headers(bytes, std::span(fields.begin(), fields.size()));
}

// Feeds raw stream bytes to `receiver` and logs the events they produced.
[[nodiscard]] inline http3_transport_result receive_http3_wire(http3_connection& receiver, std::int64_t stream_id, std::string_view bytes, bool fin, std::uint64_t& timestamp, http3_event_log& log) {
    const auto result = receiver.read_stream(stream_id, http3_bytes(bytes), fin, timestamp++);
    while (auto event = receiver.next_event()) {
        log(*event);
    }
    return result;
}

[[nodiscard]] inline bool has_local_message_error(const http3_event_log& log, std::int64_t stream_id) {
    bool stop_sending = false;
    bool reset_stream = false;
    for (const auto& action : log.transport_actions_) {
        if (action.stream_id() == stream_id && action.error() == http3_error_code::message_error && action.origin() == http3_transport_action_origin::local) {
            stop_sending = stop_sending || action.action() == http3_transport_action::stop_sending;
            reset_stream = reset_stream || action.action() == http3_transport_action::reset_stream;
        }
    }
    return stop_sending && reset_stream;
}

// A malformed message is answered with H3_MESSAGE_ERROR (RFC 9114 §4.1.2).
// nghttp3 read errors are connection-fatal: its documented API forbids
// further calls except destruction. Codec validation resets only the stream,
// with local STOP_SENDING and RESET_STREAM.
[[nodiscard]] inline bool rejected_http3_message(const http3_transport_result& result, const http3_connection& connection, const http3_event_log& log, std::int64_t stream_id) {
    return (result.status_ == http3_transport_status::protocol_error && result.error_code_ == http3_error_code::message_error && connection.connection_error() == http3_error_code::message_error) || (result.status_ == http3_transport_status::accepted && has_local_message_error(log, stream_id));
}

}  // namespace ruvia::test

namespace {

template <typename bytes_type>
constexpr bool can_read_http3_stream = requires(ruvia::http3_connection& connection, bytes_type&& bytes) { connection.read_stream(0, std::forward<bytes_type>(bytes), false, 0); };

template <typename connection_type>
constexpr bool can_read_http3_stream_braced_bytes = requires(connection_type& connection) { connection.read_stream(0, {std::byte{1}, std::byte{2}}, false, 0); };

// DATA events come only from the codec, and their credit is linear.
static_assert(!std::is_constructible_v<ruvia::http3_data_event, std::int64_t, std::string_view, bool>);
static_assert(!std::is_copy_constructible_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_copy_assignable_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_move_assignable_v<ruvia::http3_received_data_credit>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_default_constructible_v<ruvia::http3_received_data_credit>);

static_assert(can_read_http3_stream<std::vector<std::byte>&>);
static_assert(can_read_http3_stream<const std::vector<std::byte>&>);
static_assert(can_read_http3_stream<std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<const std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<std::byte (&)[3]>);
static_assert(can_read_http3_stream<const std::byte (&)[3]>);
static_assert(can_read_http3_stream<std::span<std::byte>>);
static_assert(can_read_http3_stream<std::span<const std::byte>>);
static_assert(can_read_http3_stream<std::span<std::byte, 3>>);
static_assert(can_read_http3_stream<std::span<const std::byte, 3>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<const ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>&>);
static_assert(!can_read_http3_stream<std::vector<std::byte>>);
static_assert(!can_read_http3_stream<const std::vector<std::byte>>);
static_assert(!can_read_http3_stream<std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<const std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream<const std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream_braced_bytes<ruvia::http3_connection>);

constexpr ruvia::http3_regular_request_head_view get_request_head{.authority_ = "example.test", .target_ = "/resource"};

[[nodiscard]] auto submit_no_content_response(ruvia::http3_connection& connection, std::int64_t stream_id) {
    ruvia::http_response response;
    response.status(ruvia::http_status::no_content);
    response.header("Server", "ruvia");
    return connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
}

// RFC 9114 §8.1 reserves 0x1f * N + 0x21 for greasing; a receiver keeps the
// value and treats it as H3_NO_ERROR.
constexpr auto reserved_error = static_cast<ruvia::http3_error_code>(0x1f * 7 + 0x21);

}  // namespace

TEST(Ruvia, http3_transport_feedback_blocks_and_resumes_request_output) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log received;
    ASSERT_TRUE(peers.to_server(received));
    ASSERT_TRUE(peers.to_client(received));
    ASSERT_EQ(peers.client_.submit_request_head(0, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.block_stream(0), ruvia::http3_transport_status::accepted);
    ASSERT_TRUE(peers.to_server(received));
    EXPECT_TRUE(received.headers_.empty());
    ASSERT_EQ(peers.client_.unblock_stream(0), ruvia::http3_transport_status::accepted);
    ASSERT_TRUE(peers.to_server(received));
    ASSERT_EQ(received.headers_.size(), std::size_t{1});
    EXPECT_EQ(received.headers_[0].stream_id_, 0);
    EXPECT_TRUE(received.ended(0));
}

TEST(Ruvia, http3_transport_feedback_preserves_direction_and_peer_error) {
    for (const bool receive_reset : {false, true}) {
        SCOPED_TRACE(receive_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log received;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, received));
        auto& connection = peers.client_;
        const auto status = receive_reset ? connection.shutdown_stream_read(0, ruvia::http3_error_code::request_cancelled) : connection.shutdown_stream_write(0, ruvia::http3_error_code::request_cancelled);
        ASSERT_EQ(status, ruvia::http3_transport_status::accepted);
        auto event = connection.next_event();
        ASSERT_TRUE(event.has_value());
        const auto* action = event->transport_action();
        ASSERT_NE(action, nullptr);
        EXPECT_EQ(action->origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(action->stream_id(), 0);
        EXPECT_EQ(action->error(), ruvia::http3_error_code::request_cancelled);
        EXPECT_EQ(action->action(), receive_reset ? ruvia::http3_transport_action::reset_stream : ruvia::http3_transport_action::stop_sending);
        if (receive_reset) {
            EXPECT_EQ(connection.finish_stream(0), ruvia::http3_finish_status::accepted);
        } else {
            EXPECT_EQ(connection.finish_stream(0), ruvia::http3_finish_status::closed);
            ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submit_no_content_response(peers.server_, 0)));
            ruvia::test::http3_event_log response;
            ASSERT_TRUE(peers.to_client(response));
            EXPECT_TRUE(response.ended(0));
        }
        ASSERT_EQ(connection.shutdown_stream_read(0), ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(connection.next_event().has_value());
        EXPECT_EQ(connection.close_stream(0, std::nullopt, std::nullopt), ruvia::http3_transport_status::accepted);
    }
}

TEST(Ruvia, http3_nghttp3_client_server_exchange_headers_and_stream_end) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    if (request.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(request.headers_[0].stream_id_, std::int64_t{0});
    // The four pseudo-header fields, then the request's Host from :authority.
    EXPECT_EQ(request.headers_[0].fields_.size(), std::size_t{5});
    EXPECT_TRUE(request.headers_[0].stream_ended_);
    EXPECT_TRUE(request.headers_[0].field(":path") == std::string_view("/resource"));
    EXPECT_TRUE(request.headers_[0].field("host") == std::string_view("example.test"));
    EXPECT_TRUE(request.ended(0));
    // A message without a trailer section ends with an empty one.
    EXPECT_TRUE(request.end(0) != nullptr && request.end(0)->trailers_.empty());
    EXPECT_EQ(request.sequence_, std::string("HE"));

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_EQ(response.headers_.size(), std::size_t{1});
    if (response.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(response.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_TRUE(response.headers_[0].field("server") == "ruvia");
    EXPECT_TRUE(response.headers_[0].stream_ended_);
    EXPECT_TRUE(response.headers_[0].status_ && response.headers_[0].status_->value() == 204);
    EXPECT_TRUE(response.ended(0));
    EXPECT_EQ(response.sequence_, std::string("HE"));
}

struct http3_credit_sink final {
    static void record(void* context, std::int64_t stream_id, std::size_t bytes) noexcept {
        static_cast<http3_credit_sink*>(context)->returned_.emplace_back(stream_id, bytes);
    }
    [[nodiscard]] std::size_t total(std::int64_t stream_id) const noexcept {
        std::size_t bytes = 0;
        for (const auto& [id, count] : returned_) {
            bytes += id == stream_id ? count : 0;
        }
        return bytes;
    }
    std::vector<std::pair<std::int64_t, std::size_t>> returned_;
};

[[nodiscard]] bool submit_post(ruvia::test::http3_peers& peers, std::int64_t stream_id, std::string_view content) {
    return peers.client_.submit_request_head(stream_id, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::known_length(content.size())}) == ruvia::http3_submit_status::accepted && peers.client_.submit_data(stream_id, ruvia::test::http3_bytes(content)).consumed_bytes_ == content.size() && peers.client_.finish_stream(stream_id) == ruvia::http3_finish_status::accepted;
}

TEST(Ruvia, http3_received_data_credit_returns_once_when_released) {
    http3_credit_sink sink;
    ruvia::test::http3_peers peers({.own_data_events_ = true, .received_data_credit_sink_ = &http3_credit_sink::record, .received_data_credit_context_ = &sink});
    ASSERT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}) == ruvia::http3_submit_status::accepted);
    std::vector<ruvia::http3_received_data_credit> credits;
    for (const std::string_view chunk : {"abc", "de"}) {
        ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes(chunk)).consumed_bytes_, chunk.size());
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                credits.push_back(data->take_credit());
                // A taken credit leaves none behind in the event.
                EXPECT_FALSE(data->take_credit().valid());
            }
        }));
    }
    ASSERT_EQ(credits.size(), std::size_t{2});
    EXPECT_TRUE(sink.returned_.empty());
    credits[0].merge(std::move(credits[1]));
    EXPECT_FALSE(credits[1].valid());
    EXPECT_TRUE(sink.returned_.empty());
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        credits[0].merge(std::move(credits[1]));
    }));
    auto moved = std::move(credits[0]);
    EXPECT_FALSE(credits[0].valid());
    credits.clear();
    EXPECT_TRUE(sink.returned_.empty());
    {
        const auto released = std::move(moved);
    }
    ASSERT_EQ(sink.returned_.size(), std::size_t{1});
    EXPECT_EQ(sink.returned_[0], (std::pair<std::int64_t, std::size_t>{0, 5}));
}

TEST(Ruvia, http3_received_data_credits_of_different_streams_do_not_merge) {
    http3_credit_sink sink;
    std::optional<ruvia::http3_received_data_credit> first;
    std::optional<ruvia::http3_received_data_credit> second;
    {
        ruvia::test::http3_peers peers({.received_data_credit_sink_ = &http3_credit_sink::record, .received_data_credit_context_ = &sink});
        ASSERT_TRUE(peers.bind());
        ASSERT_TRUE(submit_post(peers, 0, "ab"));
        ASSERT_TRUE(submit_post(peers, 4, "cde"));
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                (data->stream_id() == 0 ? first : second).emplace(data->take_credit());
            }
        }));
        ASSERT_TRUE(first.has_value() && second.has_value());
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            first->merge(std::move(*second));
        }));
        EXPECT_TRUE(first->valid() && second->valid());
        first.reset();
        EXPECT_EQ(sink.total(0), std::size_t{2});
        EXPECT_EQ(sink.total(4), std::size_t{0});
    }
    // A credit that outlives its codec returns nothing.
    second.reset();
    EXPECT_EQ(sink.total(4), std::size_t{0});
}

TEST(Ruvia, http3_data_event_move_assignment_returns_the_target_credit) {
    http3_credit_sink sink;
    ruvia::test::http3_peers peers({.own_data_events_ = true, .received_data_credit_sink_ = &http3_credit_sink::record, .received_data_credit_context_ = &sink});
    ASSERT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}) == ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_data_event> retained;
    for (const std::string_view chunk : {"abc", "de"}) {
        ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes(chunk)).consumed_bytes_, chunk.size());
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                if (retained) {
                    *retained = std::move(*data);
                } else {
                    retained.emplace(std::move(*data));
                }
            }
        }));
    }
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->bytes(), "de");
    EXPECT_EQ(sink.total(0), std::size_t{3});
    retained.reset();
    EXPECT_EQ(sink.total(0), std::size_t{5});
}

TEST(Ruvia, http3_nghttp3_streaming_body_is_owned_until_acknowledged) {
    ruvia::test::http3_peers peers({.own_data_events_ = true});
    EXPECT_TRUE(peers.bind());

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_ = ruvia::http_request_content_plan::known_length(3)}) == ruvia::http3_submit_status::accepted);
    const auto submitted = peers.client_.submit_data(0, ruvia::test::http3_bytes("abc"));
    EXPECT_TRUE(submitted.status_ == ruvia::http3_data_submit_status::complete);
    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{3});
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);

    // The relay scrubs every delivered input buffer, so owned DATA is the
    // only way the payload can still read "abc" after delivery.
    std::optional<ruvia::http3_data_event> retained;
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        request(event);
        if (auto* data = event.data()) {
            retained.emplace(std::move(*data));
        }
    }));

    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    EXPECT_TRUE(retained.has_value() && retained->stream_id() == 0 && retained->bytes() == "abc");
    EXPECT_EQ(request.data(0), std::string("abc"));
    EXPECT_TRUE(request.ended(0));
    EXPECT_EQ(request.sequence_, std::string("HDE"));
}

// A request stream that QUIC reports closed is published once, with the
// application error code only when the closure carried one.
TEST(Ruvia, http3_close_stream_publishes_stream_close_event) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0) && request.ended(4));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_TRUE(response.ended(0));

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_transport_status::accepted);
    ruvia::test::http3_event_log closed;
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id(), std::int64_t{0});
        EXPECT_FALSE(closed.closes_[0].error().has_value());
    }

    // An unknown code keeps its exact value (RFC 9114 §9).
    EXPECT_TRUE(peers.server_.close_stream(4, reserved_error, std::nullopt) == ruvia::http3_transport_status::accepted);
    closed.clear();
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id(), std::int64_t{4});
        EXPECT_EQ(closed.closes_[0].error(), std::optional(reserved_error));
        EXPECT_EQ(static_cast<std::uint64_t>(*closed.closes_[0].error()), std::uint64_t{0x1f * 7 + 0x21});
    }
    EXPECT_EQ(closed.sequence_, std::string("C"));

    // A closed stream accepts no further response.
    EXPECT_TRUE(std::holds_alternative<ruvia::http3_response_head_error>(submit_no_content_response(peers.server_, 0)));
}

// RFC 9114 §5.2 graceful shutdown: the notice still admits in-flight
// requests, the final GOAWAY names the first unprocessed request stream, and
// the server is drained only once every admitted request stream has closed.
TEST(Ruvia, http3_server_drain_sends_notice_then_final_goaway) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(peers.server_.begin_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(!peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    // The shutdown notice bounds no stream (RFC 9114 §5.2).
    EXPECT_EQ(client.goaways_.size(), std::size_t{1});
    EXPECT_TRUE(!client.goaways_.empty() && !client.goaways_[0].has_value());
    EXPECT_TRUE(client.unprocessed_.empty());

    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.goaways_.size(), std::size_t{2});
    EXPECT_TRUE(client.goaways_.size() == 2 && client.goaways_[1] == std::optional<std::int64_t>(4));
    EXPECT_TRUE(client.unprocessed_.empty());
    // Stream 0 was admitted before the final GOAWAY and remains open.
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submit_no_content_response(peers.server_, 0)));
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_transport_status::accepted);
    while (auto event = peers.server_.next_event()) {
        (void)event;
    }
    EXPECT_TRUE(peers.server_.drained());
    // A repeated final drain is idempotent.
    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
}

TEST(Ruvia, http3_peer_termination_keeps_unknown_error_codes) {
    for (const bool receive_reset : {false, true}) {
        SCOPED_TRACE(receive_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, server));
        // The server sees the client's RESET_STREAM or STOP_SENDING.
        const auto status = receive_reset ? peers.server_.shutdown_stream_read(0, reserved_error) : peers.server_.shutdown_stream_write(0, reserved_error);
        ASSERT_EQ(status, ruvia::http3_transport_status::accepted);
        ruvia::test::http3_event_log log;
        while (auto event = peers.server_.next_event()) {
            log(*event);
        }
        ASSERT_EQ(log.transport_actions_.size(), std::size_t{1});
        const auto& action = log.transport_actions_[0];
        EXPECT_EQ(action.stream_id(), 0);
        EXPECT_EQ(action.origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(action.action(), receive_reset ? ruvia::http3_transport_action::reset_stream : ruvia::http3_transport_action::stop_sending);
        EXPECT_EQ(static_cast<std::uint64_t>(action.error()), std::uint64_t{0x1f * 7 + 0x21});
        EXPECT_EQ(peers.server_.close_stream(0, reserved_error, reserved_error), ruvia::http3_transport_status::accepted);
        log.clear();
        while (auto event = peers.server_.next_event()) {
            log(*event);
        }
        ASSERT_EQ(log.closes_.size(), std::size_t{1});
        EXPECT_EQ(log.closes_[0].error(), std::optional(reserved_error));
    }
}

[[nodiscard]] bool has_local_cancel(const ruvia::test::http3_event_log& log, std::int64_t stream_id, ruvia::http3_transport_action action) {
    for (const auto& event : log.transport_actions_) {
        if (event.stream_id() == stream_id && event.action() == action && event.origin() == ruvia::http3_transport_action_origin::local && event.error() == ruvia::http3_error_code::request_cancelled) {
            return true;
        }
    }
    return false;
}

// RFC 9114 §5.2: request streams at or above the final GOAWAY identifier were
// not processed and may be retried; the client cancels what is still open and
// no later event names them.
TEST(Ruvia, http3_client_reports_requests_beyond_goaway_as_unprocessed) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    // Streams 4 and 8 never reach the server before its final GOAWAY.
    ASSERT_EQ(peers.client_.submit_request_head(4, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.submit_request_head(8, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.server_.begin_drain(), ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    // The shutdown notice names no stream, so every request may still finish.
    EXPECT_EQ(client.sequence_, std::string("G"));

    ASSERT_EQ(peers.server_.finish_drain(), ruvia::http3_submit_status::accepted);
    client.clear();
    ASSERT_TRUE(peers.to_client(client));
    ASSERT_EQ(client.goaways_.size(), std::size_t{1});
    EXPECT_EQ(client.goaways_[0], std::optional<std::int64_t>(4));
    EXPECT_EQ(client.unprocessed_, (std::vector<std::int64_t>{4, 8}));
    EXPECT_EQ(client.sequence_, std::string("GUTTUTT"));
    for (const std::int64_t stream_id : {4, 8}) {
        SCOPED_TRACE(stream_id);
        EXPECT_TRUE(has_local_cancel(client, stream_id, ruvia::http3_transport_action::stop_sending));
        EXPECT_TRUE(has_local_cancel(client, stream_id, ruvia::http3_transport_action::reset_stream));
    }

    // Stream 0 was admitted and still completes.
    ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submit_no_content_response(peers.server_, 0)));
    client.clear();
    ASSERT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(client.unprocessed_.empty());

    // QUIC retires the cancelled streams without another event naming them.
    for (const std::int64_t stream_id : {4, 8}) {
        EXPECT_EQ(peers.client_.close_stream(stream_id, std::nullopt, ruvia::http3_error_code::request_cancelled), ruvia::http3_transport_status::accepted);
    }
    EXPECT_EQ(peers.client_.close_stream(0, std::nullopt, std::nullopt), ruvia::http3_transport_status::accepted);
    client.clear();
    while (auto event = peers.client_.next_event()) {
        client(*event);
    }
    ASSERT_EQ(client.closes_.size(), std::size_t{1});
    EXPECT_EQ(client.closes_[0].stream_id(), 0);
}

// RFC 9114 §4.1.1: H3_REQUEST_REJECTED before the final response head means
// the server did not process the request; after it, the termination is an
// ordinary peer notification.
TEST(Ruvia, http3_client_reports_rejected_requests_as_unprocessed_only_before_the_final_head) {
    for (const bool peer_reset : {true, false}) {
        SCOPED_TRACE(peer_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, server));
        // RESET_STREAM ends the client's receive half, STOP_SENDING its send half.
        const auto terminate = [&](std::int64_t stream_id) {
            return peer_reset ? peers.client_.shutdown_stream_read(stream_id, ruvia::http3_error_code::request_rejected) : peers.client_.shutdown_stream_write(stream_id, ruvia::http3_error_code::request_rejected);
        };
        ASSERT_EQ(terminate(0), ruvia::http3_transport_status::accepted);
        ruvia::test::http3_event_log client;
        while (auto event = peers.client_.next_event()) {
            client(*event);
        }
        EXPECT_EQ(client.unprocessed_, (std::vector<std::int64_t>{0}));
        EXPECT_EQ(client.sequence_, std::string("UT"));
        // The client cancels the half the server's frame left open.
        EXPECT_TRUE(has_local_cancel(client, 0, peer_reset ? ruvia::http3_transport_action::reset_stream : ruvia::http3_transport_action::stop_sending));
        EXPECT_EQ(peers.client_.close_stream(0, ruvia::http3_error_code::request_rejected, ruvia::http3_error_code::request_cancelled), ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(peers.client_.next_event().has_value());

        ASSERT_TRUE(peers.send_request(4, get_request_head, true, server));
        ruvia::http_response head;
        ASSERT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(peers.server_.submit_streaming_response_head(4, std::move(head))));
        client.clear();
        ASSERT_TRUE(peers.to_client(client));
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        ASSERT_EQ(terminate(4), ruvia::http3_transport_status::accepted);
        client.clear();
        while (auto event = peers.client_.next_event()) {
            client(*event);
        }
        EXPECT_TRUE(client.unprocessed_.empty());
        ASSERT_EQ(client.transport_actions_.size(), std::size_t{1});
        EXPECT_EQ(client.transport_actions_[0].stream_id(), 4);
        EXPECT_EQ(client.transport_actions_[0].origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(client.transport_actions_[0].error(), ruvia::http3_error_code::request_rejected);
    }
}

struct http3_observation final {
    static void frame(void* context, const ruvia::http3_frame_metadata& metadata) noexcept {
        static_cast<http3_observation*>(context)->frames_.push_back(metadata);
    }
    static void field(void* context, const ruvia::http3_field_metadata& metadata) noexcept {
        auto& self = *static_cast<http3_observation*>(context);
        self.fields_.push_back({metadata.stream_id_, metadata.section_, metadata.field_index_, metadata.phase_, std::string(metadata.name_), std::string(metadata.value_)});
    }

    struct field_record final {
        std::int64_t stream_id_;
        ruvia::http3_field_section_kind section_;
        std::size_t field_index_;
        ruvia::http3_field_observation_phase phase_;
        std::string name_;
        std::string value_;
    };

    std::vector<ruvia::http3_frame_metadata> frames_;
    std::vector<field_record> fields_;
};

// The observers see the received frame headers and QPACK field sections in
// wire order, before HTTP validation.
TEST(Ruvia, http3_frame_and_field_observers_report_received_wire_order) {
    http3_observation observation;
    ruvia::test::http3_peers peers({.frame_observer_ = &http3_observation::frame, .field_observer_ = &http3_observation::field, .observer_context_ = &observation});
    ASSERT_TRUE(peers.bind());
    ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).status_, ruvia::http3_data_submit_status::complete);
    constexpr std::array trailers{ruvia::http_header_view{"x-checksum", "c2f1"}};
    ASSERT_EQ(peers.client_.finish_request(0, trailers), ruvia::http3_finish_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_TRUE(server.ended(0));

    ASSERT_FALSE(observation.frames_.empty());
    for (std::size_t index = 0; index < observation.frames_.size(); ++index) {
        EXPECT_EQ(observation.frames_[index].observation_sequence_, index);
    }
    // The client control stream opens with SETTINGS (RFC 9114 §6.2.1).
    const auto control = std::ranges::find_if(observation.frames_, [](const auto& frame) {
        return frame.stream_id_ == 2;
    });
    ASSERT_NE(control, observation.frames_.end());
    EXPECT_EQ(control->type_, ruvia::http3_frame_type::settings);
    std::vector<ruvia::http3_frame_metadata> request;
    std::ranges::copy_if(observation.frames_, std::back_inserter(request), [](const auto& frame) {
        return frame.stream_id_ == 0;
    });
    ASSERT_EQ(request.size(), std::size_t{3});
    EXPECT_EQ(request[0].type_, ruvia::http3_frame_type::headers);
    EXPECT_EQ(request[1].type_, ruvia::http3_frame_type::data);
    EXPECT_EQ(request[1].payload_length_, std::uint64_t{3});
    EXPECT_EQ(request[2].type_, ruvia::http3_frame_type::headers);
    EXPECT_LT(request[0].observation_sequence_, request[1].observation_sequence_);
    EXPECT_LT(request[1].observation_sequence_, request[2].observation_sequence_);
    // QPACK encoder and decoder streams carry no frames.
    EXPECT_TRUE(std::ranges::none_of(observation.frames_, [](const auto& frame) {
        return frame.stream_id_ == 6 || frame.stream_id_ == 10;
    }));

    using phase = ruvia::http3_field_observation_phase;
    using section = ruvia::http3_field_section_kind;
    ASSERT_FALSE(observation.fields_.empty());
    const auto& begin = observation.fields_.front();
    EXPECT_EQ(begin.stream_id_, 0);
    EXPECT_EQ(begin.section_, section::header);
    EXPECT_EQ(begin.phase_, phase::begin);
    EXPECT_TRUE(begin.name_.empty() && begin.value_.empty());
    const auto header_end = std::ranges::find_if(observation.fields_, [](const auto& record) {
        return record.phase_ == phase::end;
    });
    ASSERT_NE(header_end, observation.fields_.end());
    EXPECT_EQ(header_end->section_, section::header);
    std::size_t header_fields = 0;
    bool saw_method = false;
    for (auto record = observation.fields_.begin() + 1; record != header_end; ++record) {
        EXPECT_EQ(record->phase_, phase::field);
        EXPECT_EQ(record->section_, section::header);
        EXPECT_EQ(record->field_index_, header_fields++);
        saw_method = saw_method || (record->name_ == ":method" && record->value_ == "POST");
    }
    EXPECT_TRUE(saw_method);
    EXPECT_EQ(header_end->field_index_, header_fields);
    ASSERT_EQ(observation.fields_.end() - header_end, 4);
    const auto trailer = header_end + 1;
    EXPECT_EQ(trailer[0].section_, section::trailer);
    EXPECT_EQ(trailer[0].phase_, phase::begin);
    EXPECT_EQ(trailer[1].section_, section::trailer);
    EXPECT_EQ(trailer[1].phase_, phase::field);
    EXPECT_EQ(trailer[1].name_, "x-checksum");
    EXPECT_EQ(trailer[1].value_, "c2f1");
    EXPECT_EQ(trailer[2].section_, section::trailer);
    EXPECT_EQ(trailer[2].phase_, phase::end);
    EXPECT_EQ(trailer[2].field_index_, std::size_t{1});
}

TEST(Ruvia, http3_settings_and_datagrams_are_negotiated_and_stream_scoped) {
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true, .enable_h3_datagram_ = true}, {.enable_h3_datagram_ = true});
    EXPECT_TRUE(peers.bind());
    EXPECT_TRUE(!peers.client_.peer_settings().has_value());
    ruvia::test::http3_event_log log;
    EXPECT_TRUE(peers.to_client(log));
    EXPECT_TRUE(peers.to_server(log));
    const auto settings = peers.client_.peer_settings();
    EXPECT_TRUE(settings && settings->enable_connect_protocol_ && settings->h3_datagram_);
    EXPECT_TRUE(!peers.client_.datagram_prefix(0));
    EXPECT_TRUE(peers.send_request(0, get_request_head, false, log));
    const auto prefix = peers.client_.datagram_prefix(0);
    EXPECT_TRUE(prefix && prefix->size_ == 1 && prefix->bytes_[0] == std::byte{0});
    const std::string wire("\0\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::accepted);
    // Public input calls must drain the prior owned event before appending more.
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::events_pending);
    auto event = peers.server_.next_event();
    const auto* datagram = event ? event->datagram() : nullptr;
    EXPECT_TRUE(datagram && datagram->stream_id() == 0 && datagram->payload() == std::string_view("\0echo", 5));
    EXPECT_TRUE(!peers.server_.next_event());
    // An unknown request stream is dropped, not retained for future streams.
    const std::string unknown("\1\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(unknown)).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
    EXPECT_TRUE(peers.server_.close_stream(0, ruvia::http3_error_code::request_cancelled, ruvia::http3_error_code::request_cancelled) == ruvia::http3_transport_status::accepted);
    while (peers.server_.next_event()) {
    }
    EXPECT_TRUE(!peers.server_.datagram_prefix(0));
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
}

TEST(Ruvia, http3_datagram_truncated_prefix_and_disabled_setting_are_connection_errors) {
    auto enabled = ruvia::http3_connection::server({.enable_h3_datagram_ = true});
    const auto truncated = enabled.read_datagram(ruvia::test::http3_bytes("\x40"));
    EXPECT_TRUE(truncated.status_ == ruvia::http3_transport_status::protocol_error);
    EXPECT_EQ(truncated.error_code_, std::optional(ruvia::http3_error_code::datagram_error));
    auto disabled = ruvia::http3_connection::server();
    const std::string wire("\0hello", 6);
    EXPECT_TRUE(disabled.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::protocol_error);
}

TEST(Ruvia, http3_origin_frame_advertises_initial_and_dynamic_origins) {
    constexpr std::array<std::string_view, 2> initial_origins{
        "https://example.test",
        "https://api.example.test:8443",
    };
    ruvia::test::http3_peers peers({.origins_ = initial_origins});
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log client_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{1});
    if (client_log.origins_.size() == 1) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{2});
        if (client_log.origins_[0].size() == 2) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://example.test"));
            EXPECT_EQ(client_log.origins_[0][1], std::string("https://api.example.test:8443"));
        }
    }

    constexpr std::array<std::string_view, 1> dynamic_origins{"https://cdn.example.test"};
    EXPECT_TRUE(peers.client_.submit_origin_advertisement(dynamic_origins) == ruvia::http3_submit_status::invalid_state);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(dynamic_origins) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(std::span<const std::string_view>{}) == ruvia::http3_submit_status::accepted);
    constexpr std::array<std::string_view, 1> invalid_origin{"not-an-origin"};
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(invalid_origin) == ruvia::http3_submit_status::invalid_message);

    client_log.clear();
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{2});
    if (client_log.origins_.size() == 2) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{1});
        if (!client_log.origins_[0].empty()) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://cdn.example.test"));
        }
        EXPECT_TRUE(client_log.origins_[1].empty());
    }
}

TEST(Ruvia, http3_priority_headers_and_priority_update_frames_reprioritize_streams) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array priority_fields{ruvia::http_header_view{"priority", "u=1, i"}};
    const ruvia::http3_regular_request_head_view prioritized_request_head{.authority_ = "example.test", .target_ = "/resource", .headers_ = priority_fields};
    EXPECT_TRUE(peers.send_request(0, prioritized_request_head, true, server_log));
    auto initial_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(initial_priority.has_value());
    if (initial_priority) {
        EXPECT_EQ(initial_priority->urgency_.value(), std::uint8_t{1});
        EXPECT_TRUE(initial_priority->incremental_);
    }

    EXPECT_TRUE(peers.client_.submit_priority_update(0, {.urgency_ = ruvia::http_priority_urgency(5), .incremental_ = false}) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    auto updated_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(updated_priority.has_value());
    if (updated_priority) {
        EXPECT_EQ(updated_priority->urgency_.value(), std::uint8_t{5});
        EXPECT_TRUE(!updated_priority->incremental_);
    }

    ruvia::http_response response;
    response.header("Priority", "u=2, i");
    response.body("prioritized");
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
    auto overridden = peers.server_.stream_priority(0);
    EXPECT_TRUE(overridden.has_value());
    if (overridden) {
        EXPECT_EQ(overridden->urgency_.value(), std::uint8_t{2});
        EXPECT_TRUE(overridden->incremental_);
    }
    EXPECT_EQ(peers.server_.submit_data(0, ruvia::test::http3_bytes(response.body())).consumed_bytes_, response.body().size());
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_client(client_log));
    ASSERT_EQ(client_log.headers_.size(), std::size_t{1});
    EXPECT_TRUE(client_log.headers_[0].field("priority") == "u=2, i");
}

TEST(Ruvia, http3_request_enforces_declared_content_length) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming(5)}) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).status_ == ruvia::http3_data_submit_status::complete);
    // Content short of or beyond the declared length is a value failure
    // that leaves the stream open.
    EXPECT_EQ(peers.client_.finish_stream(0), ruvia::http3_finish_status::invalid_message);
    const auto overrun = peers.client_.submit_data(0, ruvia::test::http3_bytes("def"));
    EXPECT_EQ(overrun.status_, ruvia::http3_data_submit_status::invalid_message);
    EXPECT_EQ(overrun.consumed_bytes_, std::size_t{0});
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("de")).status_ == ruvia::http3_data_submit_status::complete);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    EXPECT_EQ(server_log.data(0), "abcde");
    EXPECT_TRUE(server_log.ended(0));
    // Finished content is a caller ordering error, not a value.
    EXPECT_THROW((void)peers.client_.finish_stream(0), std::logic_error);
    EXPECT_THROW((void)peers.client_.submit_data(0, ruvia::test::http3_bytes("f")), std::logic_error);
}

TEST(Ruvia, http3_concurrent_streams_interleave_partial_header_blocks) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    EXPECT_TRUE(peers.client_.submit_request_head(0, get_request_head) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.client_.submit_request_head(4, get_request_head) == ruvia::http3_submit_status::accepted);

    std::vector<std::byte> stream0;
    std::vector<std::byte> stream4;
    ruvia::http3_stream_output out;
    while (peers.client_.peek_output(64 * 1024, out).status_ == ruvia::http3_transport_status::accepted) {
        const auto size = out.size();
        if (out.stream_id_ == 0 || out.stream_id_ == 4) {
            auto& dst = out.stream_id_ == 0 ? stream0 : stream4;
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                dst.insert(dst.end(), out.buffers_[idx].begin(), out.buffers_[idx].end());
            }
        } else {
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                EXPECT_TRUE(peers.server_.read_stream(out.stream_id_, out.buffers_[idx], out.fin_, 1).status_ == ruvia::http3_transport_status::accepted);
            }
        }
        EXPECT_TRUE(peers.client_.commit_output(out.stream_id_, size) == ruvia::http3_transport_status::accepted);
        if (size != 0) {
            EXPECT_TRUE(peers.client_.acknowledge_output(out.stream_id_, size) == ruvia::http3_transport_status::accepted);
        }
    }
    EXPECT_TRUE(stream0.size() > std::size_t{4});
    EXPECT_TRUE(stream4.size() > std::size_t{4});

    // Feed the first half of stream 0's HEADERS frame, then all of stream 4's
    // HEADERS frame, then the rest of stream 0's HEADERS frame.
    const auto split = stream0.size() / 2;
    EXPECT_TRUE(peers.server_.read_stream(0, std::span(stream0.data(), split), false, 1).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(4, stream4, true, 2).status_ == ruvia::http3_transport_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(0, std::span(stream0.data() + split, stream0.size() - split), true, 3).status_ == ruvia::http3_transport_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(!peers.server_.header_block_in_progress());
    EXPECT_EQ(server_log.headers_.size(), std::size_t{2});
}

namespace {

static_assert(std::is_nothrow_move_constructible_v<ruvia::http3_request_head>);
static_assert(!std::is_move_assignable_v<ruvia::http3_request_head>);
// Only a server connection creates validated heads.
static_assert(!std::is_constructible_v<ruvia::http3_request_head, std::int64_t, std::vector<ruvia::http_header>, bool, bool>);

using field_list = std::initializer_list<ruvia::test::http3_wire_field>;

struct received_request final {
    ruvia::http3_transport_result result_;
    ruvia::test::http3_event_log log_;
    std::optional<ruvia::http3_request_head> head_;
};

// Sends `fields` verbatim as the HEADERS frame of request stream
// `stream_id`, which ends with it when `stream_ended`, and keeps what the
// server published. The wire reaches field sections a client never sends.
[[nodiscard]] received_request receive_request(ruvia::test::http3_peers& peers, std::span<const ruvia::test::http3_wire_field> fields, bool stream_ended, std::int64_t stream_id = 0) {
    std::string wire;
    ruvia::test::append_http3_wire_headers(wire, fields);
    received_request received;
    received.result_ = peers.server_.read_stream(stream_id, ruvia::test::http3_bytes(wire), stream_ended, peers.timestamp_++);
    while (auto event = peers.server_.next_event()) {
        received.log_(*event);
        if (auto* head = event->request_head()) {
            received.head_.emplace(std::move(*head).take_head());
        }
    }
    return received;
}

// A malformed request is a stream error of type H3_MESSAGE_ERROR (RFC 9114
// §4.1.2) that no message event reveals.
[[nodiscard]] bool rejected(const received_request& received, const ruvia::http3_connection& server, std::int64_t stream_id = 0) {
    return !received.head_.has_value() && !received.log_.names_message(stream_id) && ruvia::test::rejected_http3_message(received.result_, server, received.log_, stream_id);
}

[[nodiscard]] bool parses(field_list fields, bool stream_ended = true) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), stream_ended);
    if (received.head_.has_value()) {
        EXPECT_TRUE(received.log_.transport_actions_.empty());
        return true;
    }
    EXPECT_TRUE(rejected(received, peers.server_));
    return false;
}

[[nodiscard]] std::optional<ruvia::http_server_expectation_plan> expectation_plan(field_list fields, bool stream_ended, ruvia::http_unsupported_expectation_policy policy) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), stream_ended);
    if (!received.head_) {
        return std::nullopt;
    }
    return received.head_->expectation_plan(policy);
}

}  // namespace

// A GET decoded by nghttp3 maps to an http_request: the query is split from
// :path, Host comes from :authority, and split Cookie crumbs are rejoined
// with "; " (RFC 9114 §4.2.1).
TEST(Ruvia, http3_request_head_parses_get_received_over_nghttp3) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    constexpr std::array request_fields{
        ruvia::http_header_view{"cookie", "session=abc"},
        ruvia::http_header_view{"accept", "text/html"},
        ruvia::http_header_view{"cookie", "theme=dark"},
    };
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.authority_ = "example.test", .target_ = "/search?q=h3&page=2", .headers_ = request_fields}) == ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            EXPECT_EQ(received->stream_id(), std::int64_t{0});
            head.emplace(std::move(*received).take_head());
        }
    }));
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->stream_id(), std::int64_t{0});
    EXPECT_TRUE(head->method() == "GET");
    EXPECT_TRUE(head->scheme() == "https");
    EXPECT_TRUE(head->authority() == "example.test");
    EXPECT_TRUE(head->path() == "/search?q=h3&page=2");
    EXPECT_TRUE(!head->connect());
    EXPECT_TRUE(head->stream_ended());
    EXPECT_FALSE(head->early_data());
    EXPECT_TRUE(head->content() == ruvia::http_request_content_indication::no_content);

    const auto request = head->build_request();
    EXPECT_TRUE(request.method() == "GET");
    EXPECT_TRUE(request.known_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(request.protocol_version() == ruvia::http_protocol_version::http3);
    EXPECT_TRUE(request.target_form() == ruvia::http_request_target_form::http3);
    EXPECT_TRUE(request.target() == "/search?q=h3&page=2");
    EXPECT_TRUE(request.path() == "/search");
    EXPECT_TRUE(request.query_string() == "q=h3&page=2");
    EXPECT_TRUE(request.last_raw_query_value("page") == std::string_view("2"));
    EXPECT_TRUE(request.scheme() == "https");
    EXPECT_TRUE(request.authority() == "example.test");
    EXPECT_TRUE(request.header("host") == std::string_view("example.test"));
    EXPECT_TRUE(request.header("accept") == std::string_view("text/html"));
    EXPECT_TRUE(request.header("cookie") == std::string_view("session=abc; theme=dark"));
    EXPECT_TRUE(request.cookie("session") == std::string_view("abc"));
    EXPECT_TRUE(request.cookie("theme") == std::string_view("dark"));
    EXPECT_TRUE(request.body().empty());
}

TEST(Ruvia, http3_request_head_keeps_cookie_crumbs_after_moving) {
    const std::string long_cookie = "session=" + std::string(128, 'x');
    const std::string long_joined = long_cookie + "; b=2";
    struct cookie_case final {
        std::array<std::string_view, 3> crumbs_;
        std::string_view expected_;
    };
    const std::array cases{
        cookie_case{{"", "", ""}, ""},
        cookie_case{{"", long_cookie, ""}, long_cookie},
        cookie_case{{long_cookie, "", "b=2"}, long_joined},
        cookie_case{{"a=1", "", ""}, "a=1"},
        cookie_case{{"", "", "a=1"}, "a=1"},
        cookie_case{{"", "a=1", "b=2"}, "a=1; b=2"},
        cookie_case{{"a=1", "", "a=2"}, "a=1; a=2"},
        cookie_case{{"a=1", "b=2", ""}, "a=1; b=2"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.expected_);
        {
            std::optional<ruvia::http3_request_head> head;
            {
                ruvia::test::http3_peers peers;
                ASSERT_TRUE(peers.bind());
                const std::array fields{
                    ruvia::test::http3_wire_field{":method", "GET"},
                    ruvia::test::http3_wire_field{":scheme", "https"},
                    ruvia::test::http3_wire_field{":authority", "example.test"},
                    ruvia::test::http3_wire_field{":path", "/"},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[0]},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[1]},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[2]},
                };
                auto received = receive_request(peers, fields, true);
                ASSERT_TRUE(received.head_.has_value());
                head.emplace(std::move(*received.head_));
            }
            // The head outlives the connection that produced it.
            const auto request = head->build_request();
            if (test_case.expected_.empty()) {
                EXPECT_TRUE(!request.header("cookie").has_value());
            } else {
                EXPECT_TRUE(request.header("cookie") == test_case.expected_);
            }
        }
    }
}

TEST(Ruvia, http3_request_head_bounds_single_and_joined_cookie_values) {
    for (const bool split : {false, true}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
            SCOPED_TRACE(split);
            SCOPED_TRACE(excess);
            // The field section limit stays above the per-field bound.
            ruvia::test::http3_peers peers({.max_field_section_size_ = 4 * ruvia::max_http_header_bytes});
            ASSERT_TRUE(peers.bind());
            const std::string first = "a=" + std::string(ruvia::max_http_header_bytes - 2 - (split ? 5 : 0) + excess, 'x');
            std::vector<ruvia::test::http3_wire_field> fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"cookie", first}};
            if (split) {
                fields.push_back({"cookie", "b=2"});
            }
            const auto received = receive_request(peers, fields, true);
            if (excess != 0 && !split) {
                // nghttp3's QPACK decoder refuses a single value above 64 KiB
                // as a connection error before the codec sees the field.
                EXPECT_EQ(received.result_.status_, ruvia::http3_transport_status::protocol_error);
                EXPECT_FALSE(received.log_.names_message(0));
            } else if (excess != 0) {
                EXPECT_TRUE(rejected(received, peers.server_));
                EXPECT_TRUE(ruvia::test::has_local_message_error(received.log_, 0));
            } else {
                ASSERT_TRUE(received.head_.has_value());
                const auto request = received.head_->build_request();
                EXPECT_TRUE(request.header("cookie") == first + (split ? "; b=2" : ""));
            }
        }
    }
}

// RFC 9114 §4.4: CONNECT carries only :method and :authority, and its target
// is the authority form.
TEST(Ruvia, http3_request_head_parses_connect_authority_target) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "proxy.test:443"}), ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            head.emplace(std::move(*received).take_head());
        }
    }));
    ASSERT_TRUE(head.has_value());
    EXPECT_TRUE(head->connect());
    EXPECT_FALSE(head->stream_ended());
    EXPECT_TRUE(head->authority() == "proxy.test:443");
    EXPECT_TRUE(head->scheme().empty());
    EXPECT_TRUE(head->path().empty());

    const auto request = head->build_request("tunnel");
    EXPECT_TRUE(request.known_method() == ruvia::http_known_method::connect);
    EXPECT_TRUE(request.protocol_version() == ruvia::http_protocol_version::http3);
    EXPECT_TRUE(request.target() == "proxy.test:443");
    EXPECT_TRUE(request.authority() == "proxy.test:443");
    EXPECT_TRUE(request.query_string().empty());
    EXPECT_TRUE(request.body() == "tunnel");

    // A CONNECT that names a scheme or path, or lacks its authority, is malformed.
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "proxy.test:443"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "proxy.test:443"}, {":path", "/"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "proxy.test"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", ":443"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "user@proxy.test:443"}}, false));
}

TEST(Ruvia, http3_request_head_rejects_authority_and_host_disagreement) {
    EXPECT_TRUE(parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"host", "example.test"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"host", "other.test"}}));
}

// The asterisk form names the server itself and exists only for OPTIONS.
TEST(Ruvia, http3_request_head_rejects_asterisk_path_outside_options) {
    EXPECT_TRUE(parses({{":method", "OPTIONS"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
}

// The connection answers a request its own validation rejects (nghttp3
// accepts both heads below) with H3_MESSAGE_ERROR on that stream alone (RFC 9114 §4.1.2): local
// STOP_SENDING and RESET_STREAM, no message event, and the connection keeps
// serving other streams.
TEST(Ruvia, http3_server_resets_only_the_stream_of_a_semantically_malformed_request) {
    const std::array<std::vector<ruvia::test::http3_wire_field>, 2> malformed{{
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"host", "other.test"}},
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/%zz"}},
    }};
    for (const auto& fields : malformed) {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        const auto received = receive_request(peers, fields, false);
        EXPECT_EQ(received.result_.status_, ruvia::http3_transport_status::accepted);
        EXPECT_TRUE(ruvia::test::has_local_message_error(received.log_, 0));
        EXPECT_FALSE(received.head_.has_value());
        EXPECT_EQ(received.log_.sequence_, std::string("TT"));
        EXPECT_FALSE(peers.server_.connection_error().has_value());
        // Content and the end of the rejected stream stay unpublished.
        std::string tail;
        ruvia::test::append_http3_wire_frame(tail, 0, "late");
        ruvia::test::http3_event_log late;
        EXPECT_EQ(ruvia::test::receive_http3_wire(peers.server_, 0, tail, true, peers.timestamp_, late).status_, ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(late.names_message(0));
        const std::array valid{
            ruvia::test::http3_wire_field{":method", "GET"},
            ruvia::test::http3_wire_field{":scheme", "https"},
            ruvia::test::http3_wire_field{":authority", "example.test"},
            ruvia::test::http3_wire_field{":path", "/"},
        };
        const auto next = receive_request(peers, valid, true, 4);
        ASSERT_TRUE(next.head_.has_value());
        EXPECT_EQ(next.head_->stream_id(), 4);
        EXPECT_TRUE(next.log_.ended(4));
    }
}

// RFC 9114 §4.3.1: an https request needs :authority or Host.
TEST(Ruvia, http3_request_head_requires_authority_or_host_for_https) {
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}}));

    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const std::array fields{
        ruvia::test::http3_wire_field{":method", "GET"},
        ruvia::test::http3_wire_field{":scheme", "https"},
        ruvia::test::http3_wire_field{":path", "/"},
        ruvia::test::http3_wire_field{"host", "example.test"},
    };
    const auto received = receive_request(peers, fields, true);
    ASSERT_TRUE(received.head_.has_value());
    const auto request = received.head_->build_request();
    EXPECT_TRUE(request.authority() == "example.test");
    EXPECT_TRUE(request.header("host") == std::string_view("example.test"));
}

// A HEADERS frame that ended the stream carries no content, so a nonzero
// Content-Length contradicts the framing.
TEST(Ruvia, http3_request_head_rejects_content_length_on_ended_stream) {
    const field_list fields{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/upload"}, {"content-length", "5"}};
    EXPECT_TRUE(!parses(fields, true));
    EXPECT_TRUE(parses(fields, false));
    EXPECT_TRUE(parses({{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/upload"}, {"content-length", "0"}}, true));

    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), false);
    ASSERT_TRUE(received.head_.has_value());
    EXPECT_TRUE(received.head_->declared_content_length() == std::optional<std::uint64_t>(5));
    EXPECT_TRUE(received.head_->content() == ruvia::http_request_content_indication::will_follow);
}

TEST(Ruvia, http3_request_head_rejects_invalid_fields) {
    const auto with_field = [](std::string_view name, std::string_view value) {
        return parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {name, value}});
    };
    EXPECT_TRUE(with_field("x-trace", "valid value"));
    // nghttp3 removes a regular field whose value is invalid before the codec
    // sees it, so the request arrives without it.
    for (const std::string_view invalid : {std::string_view("bad\x01value"), std::string_view("split\r\nvalue"), std::string_view("nul\0byte", 8)}) {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        const std::array fields{
            ruvia::test::http3_wire_field{":method", "GET"},
            ruvia::test::http3_wire_field{":scheme", "https"},
            ruvia::test::http3_wire_field{":authority", "example.test"},
            ruvia::test::http3_wire_field{":path", "/"},
            ruvia::test::http3_wire_field{"x-trace", invalid},
        };
        const auto received = receive_request(peers, fields, true);
        ASSERT_TRUE(received.head_.has_value());
        EXPECT_FALSE(received.log_.headers_[0].field("x-trace").has_value());
    }
    // RFC 9114 §4.2: uppercase names and connection-specific fields are malformed.
    EXPECT_TRUE(!with_field("X-Trace", "value"));
    EXPECT_TRUE(!with_field("connection", "keep-alive"));
    EXPECT_TRUE(!with_field("transfer-encoding", "chunked"));
    // Pseudo-header fields after a regular field, or unknown ones, are malformed.
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {"accept", "*/*"}, {":authority", "example.test"}, {":path", "/"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {":status", "200"}}));
}

// RFC 9110 §10.1.1: 100-continue is answered only when content follows; an
// unknown expectation is refused under the reject policy.
TEST(Ruvia, http3_request_head_plans_expectations) {
    const field_list continue_with_content{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/object"}, {"content-length", "10"}, {"expect", "100-continue"}};
    EXPECT_TRUE(expectation_plan(continue_with_content, false, ruvia::http_unsupported_expectation_policy::reject) == ruvia::http_server_expectation_plan::send_continue);

    const field_list continue_without_content{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/object"}, {"expect", "100-continue"}};
    EXPECT_TRUE(expectation_plan(continue_without_content, true, ruvia::http_unsupported_expectation_policy::reject) == ruvia::http_server_expectation_plan::no_action);

    const field_list unknown{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/object"}, {"content-length", "10"}, {"expect", "x-unknown-expectation"}};
    EXPECT_TRUE(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::reject) == ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_TRUE(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::ignore) == ruvia::http_server_expectation_plan::no_action);
}

// RFC 9110 §6.5.1: framing and routing fields are discarded from a trailer
// section; RFC 9114 §4.3: a pseudo-header field there is malformed.
TEST(Ruvia, http3_request_trailers_drop_forbidden_names_and_reject_pseudo_fields) {
    std::vector<ruvia::http_header> fields;
    fields.emplace_back("content-length", "5");
    fields.emplace_back("x-checksum", "sha-256=abc");
    fields.emplace_back("host", "example.test");
    fields.emplace_back("server-timing", "db;dur=5");

    ruvia::http_request_trailers trailers;
    EXPECT_TRUE(!trailers.append_received(fields).has_value());
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(trailers.field("x-checksum") == std::string_view("sha-256=abc"));
    EXPECT_TRUE(trailers.field("server-timing") == std::string_view("db;dur=5"));
    EXPECT_TRUE(!trailers.field("content-length").has_value());
    EXPECT_TRUE(!trailers.field("host").has_value());

    std::vector<ruvia::http_header> pseudo;
    pseudo.emplace_back("x-extra", "1");
    pseudo.emplace_back(":path", "/");
    EXPECT_TRUE(trailers.append_received(pseudo) == std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(!trailers.field("x-extra").has_value());

    std::vector<ruvia::http_header> invalid_value;
    invalid_value.emplace_back("x-extra", "bad\r\nvalue");
    EXPECT_TRUE(trailers.append_received(invalid_value) == std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
}

TEST(Ruvia, http3_request_head_enforces_materialized_field_count_limit) {
    // Host synthesis and a nonempty Cookie each consume one materialized field.
    for (const bool with_cookie : {false, true}) {
        const auto regular_limit = ruvia::max_http_header_fields - 1 - static_cast<std::size_t>(with_cookie);
        for (const std::size_t regular_count : {regular_limit, regular_limit + 1}) {
            SCOPED_TRACE(regular_count);
            ruvia::test::http3_peers peers;
            ASSERT_TRUE(peers.bind());
            std::vector<ruvia::test::http3_wire_field> fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}};
            fields.resize(fields.size() + regular_count, {"x-field", "v"});
            if (with_cookie) {
                fields.push_back({"cookie", "a=1"});
            }
            const auto received = receive_request(peers, fields, true);
            if (regular_count == regular_limit) {
                ASSERT_TRUE(received.head_.has_value());
                const auto request = received.head_->build_request();
                EXPECT_EQ(request.headers().size(), ruvia::max_http_header_fields);
            } else {
                EXPECT_TRUE(rejected(received, peers.server_));
            }
        }
    }
}

TEST(Ruvia, http3_regular_request_submission_rejects_invalid_heads_without_consuming_stream_ids) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const ruvia::http3_regular_request_head_view valid{.authority_ = "example.test", .target_ = "/search?q=h3"};
    auto invalid = valid;
    invalid.method_ = "GET\r\n";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.method_ = "CONNECT";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.scheme_ = "1https";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.authority_ = "user@example.test";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.target_ = "https://other.test/resource";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.method_ = "TRACE";
    invalid.content_ = ruvia::http_request_content_plan::known_length(1);
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array forbidden_fields{ruvia::http_header_view{"Connection", "close"}};
    invalid = valid;
    invalid.headers_ = forbidden_fields;
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array pseudo_fields{ruvia::http_header_view{":path", "/injected"}};
    invalid.headers_ = pseudo_fields;
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    EXPECT_TRUE(server.headers_.empty());
    ASSERT_TRUE(peers.client_.submit_request_head(0, valid) == ruvia::http3_submit_status::accepted);
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":method") == "GET");
    EXPECT_TRUE(server.headers_[0].field(":authority") == "example.test");
    EXPECT_TRUE(server.headers_[0].field(":path") == "/search?q=h3");
    EXPECT_TRUE(server.ended(0));
}

TEST(Ruvia, http3_all_request_head_forms_obey_local_and_negotiated_section_budgets) {
    for (const bool local_limit : {false, true}) {
        SCOPED_TRACE(local_limit);
        ruvia::http3_connection_options server_options;
        server_options.enable_connect_protocol_ = true;
        ruvia::http3_connection_options client_options;
        if (local_limit) {
            client_options.max_field_section_size_ = 512;
        } else {
            server_options.max_field_section_size_ = 512;
        }
        ruvia::test::http3_peers peers(server_options, client_options);
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(peers.to_client(client));
        const std::string large_value(1024, 'x');
        const std::array oversized{ruvia::http_header_view{"x-large", large_value}};
        ruvia::http3_regular_request_head_view ordinary{.authority_ = "example.test", .headers_ = oversized};
        ruvia::http3_connect_request_head_view connect{.authority_ = "example.test:443", .headers_ = oversized};
        ruvia::http3_extended_connect_request_head_view extended{.protocol_ = "connect-udp", .authority_ = "example.test", .target_ = "/ws", .headers_ = oversized};
        EXPECT_TRUE(peers.client_.submit_request_head(0, ordinary) == ruvia::http3_submit_status::invalid_message);
        EXPECT_TRUE(peers.client_.submit_request_head(4, connect) == ruvia::http3_submit_status::invalid_message);
        EXPECT_TRUE(peers.client_.submit_request_head(8, extended) == ruvia::http3_submit_status::invalid_message);
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.to_server(server));
        EXPECT_TRUE(server.headers_.empty());
        ordinary.headers_ = std::span<const ruvia::http_header_view>{};
        connect.headers_ = std::span<const ruvia::http_header_view>{};
        extended.headers_ = std::span<const ruvia::http_header_view>{};
        ASSERT_TRUE(peers.client_.submit_request_head(0, ordinary) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.client_.submit_request_head(4, connect) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.client_.submit_request_head(8, extended) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.to_server(server));
        ASSERT_EQ(server.headers_.size(), std::size_t{3});
        EXPECT_TRUE(server.ended(0));
        EXPECT_FALSE(server.ended(4));
        EXPECT_FALSE(server.ended(8));
        EXPECT_TRUE(server.headers_[1].field(":method") == "CONNECT");
        EXPECT_TRUE(server.headers_[1].field(":authority") == "example.test:443");
        EXPECT_FALSE(server.headers_[1].field(":scheme").has_value());
        EXPECT_FALSE(server.headers_[1].field(":path").has_value());
        EXPECT_TRUE(server.headers_[2].field(":protocol") == "connect-udp");
        EXPECT_TRUE(server.headers_[2].field(":path") == "/ws");
    }
}

TEST(Ruvia, http3_connect_request_submission_validates_authority_and_preserves_tunnel_bytes) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    for (const std::string_view authority : {"example.test", ":443", "user@example.test:443", "example.test:99999"}) {
        SCOPED_TRACE(authority);
        EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = authority}) == ruvia::http3_submit_status::invalid_message);
    }
    constexpr std::array framing{ruvia::http_header_view{"Content-Length", "3"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "example.test:443", .headers_ = framing}) == ruvia::http3_submit_status::invalid_message);
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "example.test:443"}) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ruvia::http_response accepted;
    ASSERT_EQ(peers.server_.submit_connect_response_head(0, accepted), ruvia::http3_submit_status::accepted);
    EXPECT_EQ(peers.server_.submit_connect_response_head(0, accepted), ruvia::http3_submit_status::invalid_state);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    EXPECT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("tunnel")).consumed_bytes_, std::size_t{6});
    ASSERT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":authority") == "example.test:443");
    EXPECT_FALSE(server.headers_[0].field("content-length").has_value());
    EXPECT_EQ(server.data(0), "tunnel");
    EXPECT_TRUE(server.ended(0));
}

TEST(Ruvia, http3_extended_connect_request_submission_requires_negotiation_and_valid_fields) {
    ruvia::http3_extended_connect_request_head_view request{.protocol_ = "websocket", .authority_ = "example.test", .target_ = "/ws"};
    {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(peers.to_client(client));
        EXPECT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::peer_capability_unavailable);
    }
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    ASSERT_TRUE(peers.bind());
    EXPECT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::peer_settings_pending);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    auto invalid = request;
    invalid.protocol_ = "bad protocol";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = request;
    invalid.target_ = "https://other.test/ws";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = request;
    invalid.scheme_ = "1https";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array fields{ruvia::http_header_view{"Sec-WebSocket-Version", "13"}};
    request.headers_ = fields;
    ASSERT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":method") == "CONNECT");
    EXPECT_TRUE(server.headers_[0].field(":protocol") == "websocket");
    EXPECT_TRUE(server.headers_[0].field("sec-websocket-version") == "13");
    EXPECT_FALSE(server.ended(0));
}

TEST(Ruvia, http3_typed_streaming_request_sends_validated_trailers_after_exact_content_length) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    constexpr std::array headers{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = headers, .content_ = ruvia::http_request_content_plan::streaming(3)}) == ruvia::http3_submit_status::accepted);
    constexpr std::array trailers{
        ruvia::http_header_view{"X-Checksum", "sha-256=abc"},
        ruvia::http_header_view{"Server-Timing", "db;dur=5"},
    };
    // Content short of the declared length rejects the trailers as a value.
    EXPECT_TRUE(peers.client_.finish_request(0, trailers) == ruvia::http3_finish_status::invalid_message);
    EXPECT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).consumed_bytes_, std::size_t{3});
    for (const std::string_view name : {":path", "Content-Length", "Connection", "Host"}) {
        SCOPED_TRACE(name);
        const std::array invalid{ruvia::http_header_view{name, "invalid"}};
        EXPECT_TRUE(peers.client_.finish_request(0, invalid) == ruvia::http3_finish_status::invalid_message);
    }
    ASSERT_TRUE(peers.client_.finish_request(0, trailers) == ruvia::http3_finish_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field("content-length") == "3");
    EXPECT_EQ(server.data(0), "abc");
    // The server receives the decoded trailer section with the message end.
    const auto* end = server.end(0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->trailers_.size(), std::size_t{2});
    EXPECT_TRUE(end->field("x-checksum") == "sha-256=abc");
    EXPECT_TRUE(end->field("server-timing") == "db;dur=5");
    EXPECT_EQ(server.sequence_, "HDE");
}

TEST(Ruvia, http3_connect_rejects_nonzero_content_length_with_open_stream) {
    EXPECT_TRUE(parses(
        {
            {":method", "CONNECT"},
            {":authority", "example.test:443"},
            {"content-length", "0"},
        },
        false));
    EXPECT_FALSE(parses(
        {
            {":method", "CONNECT"},
            {":authority", "example.test:443"},
            {"content-length", "5"},
        },
        false));

    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    ASSERT_TRUE(peers.bind());
    const std::array extended_nonzero{
        ruvia::test::http3_wire_field{":method", "CONNECT"},
        ruvia::test::http3_wire_field{":protocol", "connect-udp"},
        ruvia::test::http3_wire_field{":scheme", "https"},
        ruvia::test::http3_wire_field{":authority", "example.test"},
        ruvia::test::http3_wire_field{":path", "/.well-known/masque/udp/192.0.2.1/443/"},
        ruvia::test::http3_wire_field{"capsule-protocol", "?1"},
        ruvia::test::http3_wire_field{"content-length", "5"},
    };
    const auto received = receive_request(peers, extended_nonzero, false);
    EXPECT_TRUE(rejected(received, peers.server_));
}

namespace {

constexpr ruvia::http3_regular_request_head_view head_request_head{.method_ = "HEAD", .authority_ = "example.test", .target_ = "/resource"};

template <typename error_type>
[[nodiscard]] bool throws_as(auto&& operation) {
    try {
        operation();
    } catch (const error_type&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

[[nodiscard]] bool rejected_with(const auto& result, ruvia::http3_response_head_error error) {
    const auto* actual = std::get_if<ruvia::http3_response_head_error>(&result);
    return actual != nullptr && *actual == error;
}

using ruvia::test::append_http3_wire_frame;
using ruvia::test::append_http3_wire_headers;
using ruvia::test::has_local_message_error;
using ruvia::test::http3_wire_field;
using ruvia::test::rejected_http3_message;

[[nodiscard]] ruvia::http3_transport_result receive_http3_wire_response(ruvia::test::http3_peers& peers, std::int64_t stream_id, std::string_view bytes, bool fin, ruvia::test::http3_event_log& log) {
    return ruvia::test::receive_http3_wire(peers.client_, stream_id, bytes, fin, peers.timestamp_, log);
}

}  // namespace

// RFC 9114 §4.1-§4.2: a buffered response becomes :status, lowercase regular
// fields, a generated Date and the planned Content-Length, followed by DATA.
TEST(Ruvia, http3_buffered_response_head_maps_status_fields_and_content_length) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0));

    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.header("X-Trace-Id", "trace-7");
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(plan.send_body());
    EXPECT_EQ(plan.content_length(), std::uint64_t{11});

    const auto submitted = peers.server_.submit_response_head(0, response, plan);
    const auto* accepted = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted);
    EXPECT_TRUE(accepted != nullptr && accepted->content_length() == 11);
    // A second final head on the same stream is refused.
    EXPECT_TRUE(rejected_with(peers.server_.submit_response_head(0, response, plan), ruvia::http3_response_head_error::invalid_state));

    const auto data = peers.server_.submit_data(0, ruvia::test::http3_bytes(response.body()));
    EXPECT_TRUE(data.status_ == ruvia::http3_data_submit_status::complete);
    EXPECT_EQ(data.consumed_bytes_, std::size_t{11});
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HDE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{1});
    if (client.headers_.size() != 1) {
        return;
    }
    const auto& head = client.headers_[0];
    EXPECT_TRUE(!head.stream_ended_);
    EXPECT_TRUE(head.status_ && head.status_->value() == 200);
    EXPECT_FALSE(head.field(":status").has_value());
    EXPECT_TRUE(head.field("x-trace-id") == std::string_view("trace-7"));
    EXPECT_TRUE(!head.field("X-Trace-Id").has_value());
    EXPECT_TRUE(head.field("content-length") == std::string_view("11"));
    const auto date = head.field("date");
    EXPECT_TRUE(date.has_value() && date->ends_with(" GMT"));
    for (const auto& field : head.fields_) {
        for (const char byte : field.name_) {
            EXPECT_TRUE(byte < 'A' || byte > 'Z');
        }
    }
    EXPECT_EQ(client.data(0), std::string("hello world"));
    EXPECT_TRUE(client.ended(0));
}

TEST(Ruvia, http3_field_sections_keep_interleaved_names_and_values_after_submission) {
    for (const std::size_t extra_fields : {std::size_t{0}, std::size_t{34}}) {
        SCOPED_TRACE(extra_fields);
        ruvia::test::http3_peers peers;
        EXPECT_TRUE(peers.bind());
        ruvia::test::http3_event_log request;
        EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

        std::vector<std::string> names{std::string(80, 'a'), "X-Small", std::string(64, 'b'), std::string(31, 'c') + "Z" + std::string(31, 'c'), "Z" + std::string(63, 'd'), std::string(64, 'e') + "Z"};
        std::vector<std::string> normalized{std::string(80, 'a'), "x-small", std::string(64, 'b'), std::string(31, 'c') + "z" + std::string(31, 'c'), "z" + std::string(63, 'd'), std::string(64, 'e') + "z"};
        std::vector<std::string> values{"value-0", "value-1", "value-2", "value-3", "value-4", "value-5"};
        for (std::size_t index = 0; index < extra_fields; ++index) {
            names.push_back(std::string("X-Extra-") + std::to_string(index).c_str());
            normalized.push_back(std::string("x-extra-") + std::to_string(index).c_str());
            values.push_back(std::string("value-extra-") + std::to_string(index).c_str());
        }
        const auto expected_values = values;
        std::vector<ruvia::http_header_view> fields;
        for (std::size_t index = 0; index < names.size(); ++index) {
            fields.emplace_back(names[index], values[index]);
        }
        {
            const ruvia::http_interim_response_head hints(ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(fields));
            EXPECT_TRUE(peers.server_.submit_interim_response_head(0, hints) == ruvia::http3_submit_status::accepted);
            ruvia::http_response response;
            for (const auto& field : fields) {
                response.header_stable_view(field.name(), field.value());
            }
            const auto submitted = peers.server_.submit_streaming_response_head(0, std::move(response), ruvia::http_response_trailer_intent::present);
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(submitted));
            const auto data = peers.server_.submit_data(0, ruvia::test::http3_bytes("payload"));
            EXPECT_TRUE(data.status_ == ruvia::http3_data_submit_status::complete && data.consumed_bytes_ == 7);
            EXPECT_TRUE(peers.server_.finish_response(0, ruvia::validate_http_response_trailers(std::span<const ruvia::http_header_view>(fields))) == ruvia::http3_finish_status::accepted);
        }
        // QPACK encoding happens after every source has been replaced.
        for (auto& name : names) {
            name.assign(name.size(), 'q');
        }
        for (auto& value : values) {
            value.assign(value.size(), 'r');
        }
        ruvia::test::http3_event_log client;
        EXPECT_TRUE(peers.to_client(client));
        EXPECT_EQ(client.sequence_, std::string("HHDE"));
        EXPECT_EQ(client.data(0), std::string("payload"));
        const auto* end = client.end(0);
        ASSERT_NE(end, nullptr);
        EXPECT_EQ(client.headers_.size(), std::size_t{2});
        if (client.headers_.size() != 2) {
            return;
        }
        EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 103);
        EXPECT_TRUE(client.headers_[1].status_ && client.headers_[1].status_->value() == 200);
        for (std::size_t index = 0; index < normalized.size(); ++index) {
            for (const auto& section : client.headers_) {
                EXPECT_TRUE(section.field(normalized[index]) == expected_values[index]);
            }
            EXPECT_TRUE(end->field(normalized[index]) == expected_values[index]);
        }
    }
}

// RFC 9110 §9.3.2: a HEAD response is the head alone, ending the stream.
TEST(Ruvia, http3_buffered_response_head_to_head_request_ends_with_head) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, head_request_head, true, request));

    ruvia::http_response response;
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    EXPECT_TRUE(!plan.send_body());
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, plan)));
    // The head ended the stream: content after it is a caller ordering error.
    EXPECT_TRUE(throws_as<std::logic_error>([&] {
        (void)peers.server_.submit_data(0, ruvia::test::http3_bytes("x"));
    }));
    EXPECT_TRUE(throws_as<std::logic_error>([&] {
        (void)peers.server_.finish_stream(0);
    }));

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HE"));
    EXPECT_TRUE(client.headers_.size() == 1 && client.headers_[0].stream_ended_ && client.headers_[0].status_ && client.headers_[0].status_->value() == 200);
    EXPECT_TRUE(client.data(0).empty());
    EXPECT_TRUE(client.ended(0));
}

// RFC 9114 §4.2: connection-specific fields have no HTTP/3 meaning.
TEST(Ruvia, http3_response_head_rejects_connection_specific_fields) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    ruvia::http_response response;
    response.header("Connection", "close");
    response.body("bye");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(rejected_with(peers.server_.submit_response_head(0, response, plan), ruvia::http3_response_head_error::invalid_message));

    ruvia::http_response streamed;
    streamed.header("Keep-Alive", "timeout=5");
    EXPECT_TRUE(rejected_with(peers.server_.submit_streaming_response_head(0, std::move(streamed), ruvia::http_response_trailer_intent::none), ruvia::http3_response_head_error::invalid_message));

    // A rejected head sends nothing and leaves the stream able to answer.
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.empty());
    response.remove_header("Connection");
    const auto valid_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, valid_plan)));
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("bye")).consumed_bytes_ == 3);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && !client.headers_[0].field("connection").has_value());
    EXPECT_EQ(client.data(0), std::string("bye"));
    EXPECT_TRUE(client.ended(0));
}

// The planned Content-Length binds the DATA total exactly: excess is refused
// before anything is copied and an early end leaves the stream open, both as
// invalid_message values a forwarding caller can answer.
TEST(Ruvia, http3_response_content_length_mismatch_is_invalid_message) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_head, true, request));

    ruvia::http_response response;
    response.body("hello");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, plan)));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(4, response, plan)));

    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("hello!")).status_ == ruvia::http3_data_submit_status::invalid_message);
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("hello")).consumed_bytes_ == 5);
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("!")).status_ == ruvia::http3_data_submit_status::invalid_message);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);

    EXPECT_TRUE(peers.server_.submit_data(4, ruvia::test::http3_bytes("hel")).consumed_bytes_ == 3);
    EXPECT_TRUE(peers.server_.finish_stream(4) == ruvia::http3_finish_status::invalid_message);
    EXPECT_TRUE(peers.server_.submit_data(4, ruvia::test::http3_bytes("lo")).consumed_bytes_ == 2);
    EXPECT_TRUE(peers.server_.finish_stream(4) == ruvia::http3_finish_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.data(0), std::string("hello"));
    EXPECT_EQ(client.data(4), std::string("hello"));
    EXPECT_TRUE(client.ended(0) && client.ended(4));
    EXPECT_TRUE(!peers.client_.connection_error().has_value());
}

// RFC 9114 §4.1: a streaming response carries its trailer section in a
// HEADERS frame after the DATA frames.
TEST(Ruvia, http3_streaming_response_head_sends_trailers_after_data) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    ruvia::http_response head;
    head.header("Content-Type", "text/plain");
    const auto submitted = peers.server_.submit_streaming_response_head(0, std::move(head), ruvia::http_response_trailer_intent::present);
    const auto* commit = std::get_if<ruvia::http_response_stream_commit_plan>(&submitted);
    EXPECT_TRUE(commit != nullptr);
    if (commit == nullptr) {
        return;
    }
    EXPECT_TRUE(commit->framing() == ruvia::http_response_stream_framing::http3_frames);
    EXPECT_TRUE(commit->trailers_available());
    EXPECT_TRUE(commit->head_disposition() == ruvia::http_response_stream_head_disposition::body_open);

    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("chunk-1;")).consumed_bytes_ == 8);
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("chunk-2")).consumed_bytes_ == 7);
    const std::array trailer_fields{ruvia::http_header_view{"X-Checksum", "c2f1"}};
    EXPECT_TRUE(peers.server_.finish_response(0, ruvia::validate_http_response_trailers(std::span<const ruvia::http_header_view>(trailer_fields))) == ruvia::http3_finish_status::accepted);
    // The trailer section was the last thing the stream may carry.
    EXPECT_TRUE(throws_as<std::logic_error>([&] {
        (void)peers.server_.submit_data(0, ruvia::test::http3_bytes("late"));
    }));

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HDE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{1});
    if (client.headers_.size() != 1) {
        return;
    }
    EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 200);
    EXPECT_TRUE(client.headers_[0].field("content-type") == std::string_view("text/plain"));
    EXPECT_TRUE(!client.headers_[0].field("content-length").has_value());
    EXPECT_TRUE(!client.headers_[0].stream_ended_);
    EXPECT_EQ(client.data(0), std::string("chunk-1;chunk-2"));
    // The trailer section arrives with the message end.
    const auto* end = client.end(0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->trailers_.size(), std::size_t{1});
    EXPECT_TRUE(end->field("x-checksum") == std::string_view("c2f1"));
}

// RFC 9114 §4.1: interim responses precede the final head on the same stream.
TEST(Ruvia, http3_interim_response_precedes_final_head) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    const std::array links{ruvia::http_header_view{"Link", "</style.css>; rel=preload; as=style"}};
    const ruvia::http_interim_response_head early_hints(ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(links));
    EXPECT_TRUE(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_submit_status::accepted);

    ruvia::http_response response;
    response.body("page");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, plan)));
    // Once the final head is committed no interim response may follow.
    EXPECT_TRUE(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_submit_status::invalid_state);
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("page")).consumed_bytes_ == 4);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HHDE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{2});
    if (client.headers_.size() != 2) {
        return;
    }
    EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 103);
    EXPECT_TRUE(client.headers_[0].field("link") == std::string_view("</style.css>; rel=preload; as=style"));
    EXPECT_TRUE(!client.headers_[0].stream_ended_);
    EXPECT_TRUE(client.headers_[1].status_ && client.headers_[1].status_->value() == 200);
    EXPECT_TRUE(client.headers_[1].field("content-length") == std::string_view("4"));
    EXPECT_EQ(client.data(0), std::string("page"));
}

// submit_data() holds at most max_stream_output_bytes_ unacknowledged DATA per
// stream; acknowledgements free the budget and delivery stays in order.
TEST(Ruvia, http3_submit_data_is_bounded_by_unacknowledged_output) {
    ruvia::test::http3_peers peers({.max_stream_output_bytes_ = 8});
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    ruvia::http_response head;
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(peers.server_.submit_streaming_response_head(0, std::move(head), ruvia::http_response_trailer_intent::none)));

    constexpr std::string_view payload = "abcdefghijklmnopqrst";
    const auto first = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload));
    EXPECT_TRUE(first.status_ == ruvia::http3_data_submit_status::backpressured);
    EXPECT_EQ(first.consumed_bytes_, std::size_t{8});
    const auto blocked = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8)));
    EXPECT_TRUE(blocked.status_ == ruvia::http3_data_submit_status::backpressured);
    EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    // Delivery alone frees nothing: the budget waits for the acknowledgement.
    ruvia::test::http3_event_log client;
    std::vector<ruvia::test::http3_withheld_acknowledgement> withheld;
    EXPECT_TRUE(peers.to_client(client, &withheld));
    EXPECT_EQ(client.data(0), std::string(payload.substr(0, 8)));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8))).status_ == ruvia::http3_data_submit_status::backpressured);

    EXPECT_TRUE(ruvia::test::acknowledge_http3(peers.server_, withheld));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto second = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8)));
    EXPECT_TRUE(second.status_ == ruvia::http3_data_submit_status::backpressured);
    EXPECT_EQ(second.consumed_bytes_, std::size_t{8});
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto third = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(16)));
    EXPECT_TRUE(third.status_ == ruvia::http3_data_submit_status::complete);
    EXPECT_EQ(third.consumed_bytes_, std::size_t{4});
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_client(client));

    EXPECT_EQ(client.data(0), std::string(payload));
    EXPECT_TRUE(client.ended(0));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
}

TEST(Ruvia, http3_connect_response_has_no_content_framing_and_preserves_half_close) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "echo.test:443"}) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.header("content-length", "0");
    EXPECT_TRUE(peers.server_.submit_connect_response_head(0, response) == ruvia::http3_submit_status::invalid_message);
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(peers.server_.submit_connect_response_head(0, accepted) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && !client.headers_[0].stream_ended_ && !client.headers_[0].field("content-length") && !client.headers_[0].field("transfer-encoding"));
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("request")).consumed_bytes_ == 7);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_server(server));
    EXPECT_TRUE(server.data(0) == "request" && server.ended(0));
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("reply")).consumed_bytes_ == 5);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.data(0) == "reply" && client.ended(0));
}

TEST(Ruvia, http3_websocket_extended_connect_commits_negotiated_200) {
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    constexpr std::array fields{ruvia::http_header_view{"sec-websocket-version", "13"}};
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_extended_connect_request_head_view{.protocol_ = "websocket", .authority_ = "example.test", .target_ = "/ws", .headers_ = fields}) == ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            head.emplace(std::move(*received).take_head());
        }
    }));
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    const auto request = head->build_request();
    const auto validation = ruvia::validate_http3_websocket_handshake(*head, request);
    EXPECT_TRUE(validation.accepted() != nullptr);
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(peers.server_.submit_websocket_handshake(0, request, validation)));
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && client.headers_[0].status_ && client.headers_[0].status_->value() == 200 && !client.headers_[0].field("content-length") && !client.headers_[0].field("sec-websocket-accept"));
}

TEST(Ruvia, http3_client_processes_all_600_to_999_statuses_as_server_errors) {
    for (unsigned code = 600; code <= 999; ++code) {
        SCOPED_TRACE(code);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string digits(3, '0');
        digits[0] = static_cast<char>('0' + code / 100);
        digits[1] = static_cast<char>('0' + code / 10 % 10);
        digits[2] = static_cast<char>('0' + code % 10);
        std::string wire;
        append_http3_wire_headers(wire, {{":status", digits}, {"content-length", "3"}});
        append_http3_wire_frame(wire, 0, "err");
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, true, client).status_ == ruvia::http3_transport_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        ASSERT_TRUE(client.headers_[0].status_.has_value());
        EXPECT_EQ(client.headers_[0].status_->value(), code);
        EXPECT_TRUE(client.headers_[0].status_->status() == ruvia::http_status::internal_server_error);
        EXPECT_FALSE(client.headers_[0].field(":status").has_value());
        EXPECT_EQ(client.data(0), "err");
        EXPECT_TRUE(client.ended(0));
        EXPECT_TRUE(client.transport_actions_.empty());
        EXPECT_FALSE(peers.client_.connection_error().has_value());
    }
}

// The official nghttp3 dependency rejects 000..099 before the application
// callback. Keep this explicitly accepted dependency exception visible.
TEST(Ruvia, http3_official_nghttp3_rejects_000_to_099_before_publishing_client_heads) {
    for (const std::string_view digits : {"000", "099"}) {
        SCOPED_TRACE(digits);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", digits}});
        ruvia::test::http3_event_log client;
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_TRUE(client.headers_.empty());
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_rejects_invalid_response_heads_before_publication) {
    const std::vector<std::vector<http3_wire_field>> invalid_heads{
        {{":status", "101"}},
        {{":status", "20"}},
        {{":status", "2000"}},
        {{":status", "2x0"}},
        {{":status", " 200"}},
        {{"x-before", "value"}, {":status", "200"}},
        {{":status", "200"}, {":status", "204"}},
        {{":method", "GET"}, {":status", "200"}},
        {{"x-no-status", "value"}},
        {{":status", "200"}, {"connection", "close"}},
        {{":status", "200"}, {"transfer-encoding", "chunked"}},
        {{":status", "200"}, {"content-length", "bad"}},
        {{":status", "200"}, {"content-length", "2"}, {"content-length", "3"}},
        {{":status", "200"}, {"te", "trailers"}},
    };
    for (std::size_t index = 0; index < invalid_heads.size(); ++index) {
        SCOPED_TRACE(index);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, invalid_heads[index]);
        ruvia::test::http3_event_log client;
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_TRUE(client.headers_.empty());
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_validates_interim_final_and_retained_trailer_fields_on_wire) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    std::string wire;
    append_http3_wire_headers(wire, {{":status", "103"}, {"link", "</style.css>; rel=preload"}});
    append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "3"}, {"x-head", "valid"}, {"x-invalid", "bad\r\nvalue"}});
    append_http3_wire_frame(wire, 0, "abc");
    append_http3_wire_headers(wire, {{"content-length", "999"}, {"host", "wrong.test"}, {"x-checksum", "sha-256=abc"}, {"server-timing", "db;dur=5"}, {"x-invalid", "bad\r\nvalue"}});
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, true, client).status_ == ruvia::http3_transport_status::accepted);
    ASSERT_EQ(client.headers_.size(), std::size_t{2});
    ASSERT_TRUE(client.headers_[0].status_.has_value());
    ASSERT_TRUE(client.headers_[1].status_.has_value());
    EXPECT_EQ(client.headers_[0].status_->value(), 103);
    EXPECT_EQ(client.headers_[1].status_->value(), 200);
    EXPECT_TRUE(client.headers_[1].field("x-head") == "valid");
    EXPECT_FALSE(client.headers_[1].field("x-invalid").has_value());
    // RFC 9110 §6.5.1: fields forbidden in trailers are discarded.
    const auto* end = client.end(0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->trailers_.size(), std::size_t{2});
    EXPECT_FALSE(end->field("content-length").has_value());
    EXPECT_FALSE(end->field("host").has_value());
    EXPECT_FALSE(end->field("x-invalid").has_value());
    EXPECT_TRUE(end->field("x-checksum") == "sha-256=abc");
    EXPECT_TRUE(end->field("server-timing") == "db;dur=5");
    EXPECT_EQ(client.data(0), "abc");
    EXPECT_EQ(client.sequence_, "HHDE");
    EXPECT_TRUE(client.transport_actions_.empty());
}

TEST(Ruvia, http3_client_rejects_invalid_trailer_sections_without_publishing_partial_fields) {
    const std::vector<std::vector<http3_wire_field>> invalid_trailers{
        {{"x-checksum", "valid"}, {":status", "200"}},
        {{"x-checksum", "valid"}, {":path", "/"}},
        {{"x-checksum", "valid"}, {"te", "trailers"}},
    };
    for (std::size_t index = 0; index < invalid_trailers.size(); ++index) {
        SCOPED_TRACE(index);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "3"}});
        append_http3_wire_frame(wire, 0, "abc");
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, false, client).status_ == ruvia::http3_transport_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        wire.clear();
        append_http3_wire_headers(wire, invalid_trailers[index]);
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_EQ(client.headers_.size(), std::size_t{1});
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_rejects_short_and_excess_content_without_publishing_completion) {
    for (const std::string_view declared : {"2", "4"}) {
        SCOPED_TRACE(declared);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", declared}});
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, false, client).status_ == ruvia::http3_transport_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        wire.clear();
        append_http3_wire_frame(wire, 0, "abc");
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_core_response_validation_resets_only_the_malformed_stream) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    ASSERT_TRUE(peers.send_request(4, get_request_head, true, server));
    std::string wire;
    // nghttp3 permits request-style TE: trailers in a response. The core's
    // response-specific rule rejects it without invalidating the codec.
    append_http3_wire_headers(wire, {{":status", "200"}, {"te", "trailers"}});
    ruvia::test::http3_event_log client;
    const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
    EXPECT_TRUE(result.status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(has_local_message_error(client, 0));
    EXPECT_TRUE(client.headers_.empty());
    EXPECT_FALSE(client.ended(0));
    EXPECT_FALSE(peers.client_.connection_error().has_value());
    wire.clear();
    append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "2"}});
    append_http3_wire_frame(wire, 0, "ok");
    EXPECT_TRUE(receive_http3_wire_response(peers, 4, wire, true, client).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_EQ(client.data(4), "ok");
    EXPECT_TRUE(client.ended(4));
}

// RFC 9114 §4.1: a 1xx field section larger than the peer's
// SETTINGS_MAX_FIELD_SECTION_SIZE is a value the caller can answer, and the
// stream can still take its final response.
TEST(Ruvia, http3_interim_response_over_the_peer_field_limit_is_invalid_message) {
    ruvia::test::http3_peers peers({}, {.max_field_section_size_ = 256});
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, request));
    const std::string large(512, 'x');
    const std::array fields{ruvia::http_header_view{"link", large}};
    const ruvia::http_interim_response_head hints(ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(fields));
    EXPECT_EQ(peers.server_.submit_interim_response_head(0, hints), ruvia::http3_submit_status::invalid_message);
    ruvia::http_response response;
    response.status(ruvia::http_status::no_content);
    ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    ASSERT_EQ(client.headers_.size(), std::size_t{1});
    EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 204);
}

// The streaming head applies the method the stream received: a HEAD
// response ends the stream with its head.
TEST(Ruvia, http3_streaming_response_head_uses_the_received_request_method) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    ASSERT_TRUE(peers.send_request(0, head_request_head, true, request));
    ruvia::http_response head;
    head.header("content-length", "5");
    const auto submitted = peers.server_.submit_streaming_response_head(0, std::move(head));
    const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&submitted);
    ASSERT_NE(plan, nullptr);
    EXPECT_TRUE(plan->head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    ASSERT_EQ(client.headers_.size(), std::size_t{1});
    EXPECT_TRUE(client.headers_[0].stream_ended_);
    EXPECT_TRUE(client.headers_[0].field("content-length") == "5");
    EXPECT_TRUE(client.ended(0));
}

// RFC 9110 §10.1.1: the first 100 releases content held for
// Expect: 100-continue; a final head while it still waits ends the exchange.
TEST(Ruvia, http3_client_response_heads_carry_request_content_signals) {
    for (const bool continue_first : {true, false}) {
        SCOPED_TRACE(continue_first);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log request;
        ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming(3), .expectation_ = ruvia::http_client_request_expectation::continue_value}) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.to_server(request));
        if (continue_first) {
            ASSERT_EQ(peers.server_.submit_interim_response_head(0, ruvia::http_interim_response_head(ruvia::http_status::continue_value)), ruvia::http3_submit_status::accepted);
            ASSERT_EQ(peers.server_.submit_interim_response_head(0, ruvia::http_interim_response_head(ruvia::http_status::continue_value)), ruvia::http3_submit_status::accepted);
        }
        ruvia::http_response response;
        response.status(ruvia::http_status::no_content);
        ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(peers.server_.submit_response_head(0, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response))));
        std::vector<std::optional<ruvia::http_client_request_content_signal>> interim_signals;
        std::optional<ruvia::http_client_request_content_signal> final_signal;
        bool final_ended = false;
        ASSERT_TRUE(peers.to_client([&](ruvia::http3_event& event) {
            if (const auto* interim = event.informational_head()) {
                EXPECT_EQ(interim->head().protocol_version(), ruvia::http_protocol_version::http3);
                interim_signals.push_back(interim->request_content_signal());
            } else if (auto* head = event.response_head()) {
                final_signal = head->request_content_signal();
                final_ended = head->stream_ended();
                const auto taken = std::move(*head).take_head();
                EXPECT_EQ(taken.status(), ruvia::http_status::no_content);
            }
        }));
        EXPECT_TRUE(final_ended);
        if (continue_first) {
            ASSERT_EQ(interim_signals.size(), std::size_t{2});
            EXPECT_TRUE(interim_signals[0] == ruvia::http_client_request_content_signal::continue_value);
            EXPECT_FALSE(interim_signals[1].has_value());
            EXPECT_FALSE(final_signal.has_value());
        } else {
            EXPECT_TRUE(interim_signals.empty());
            EXPECT_TRUE(final_signal == ruvia::http_client_request_content_signal::exchange_complete);
        }
    }
}

// RFC 9220: the HTTP/3 convenience entry sends the negotiation's fields and
// the protocol-neutral validator accepts the server's selection.
TEST(Ruvia, http3_websocket_client_request_round_trips_the_negotiation) {
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    ASSERT_TRUE(peers.bind());
    constexpr std::array<std::string_view, 1> subprotocols{"chat.v1"};
    const ruvia::websocket_client_negotiation negotiation({.subprotocols_ = subprotocols});
    EXPECT_EQ(ruvia::submit_http3_websocket_client_request(peers.client_, 0, negotiation, "https", "example.test", "/ws"), ruvia::http3_submit_status::peer_settings_pending);
    ruvia::test::http3_event_log settings;
    ASSERT_TRUE(peers.to_client(settings));
    ASSERT_EQ(ruvia::submit_http3_websocket_client_request(peers.client_, 0, negotiation, "https", "example.test", "/ws"), ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            head.emplace(std::move(*received).take_head());
        }
    }));
    ASSERT_TRUE(head.has_value());
    const auto request = head->build_request();
    const auto validation = ruvia::validate_http3_websocket_handshake(*head, request);
    ASSERT_NE(validation.accepted(), nullptr);
    ASSERT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(peers.server_.submit_websocket_handshake(0, request, validation, {.supported_subprotocols_ = subprotocols})));
    bool validated = false;
    ASSERT_TRUE(peers.to_client([&](ruvia::http3_event& event) {
        if (const auto* response = event.response_head()) {
            const auto selection = ruvia::validate_extended_connect_websocket_client_response(negotiation, response->head(), !response->stream_ended());
            const auto* selected = std::get_if<ruvia::websocket_client_negotiation_result_view>(&selection);
            validated = selected != nullptr && selected->selected_subprotocol_ == "chat.v1";
        }
    }));
    EXPECT_TRUE(validated);
}

TEST(Ruvia, http3_response_submission_validates_method_plan_and_connect_state) {
    {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "HEAD", .authority_ = "example.test", .target_ = "/doc"}), ruvia::http3_submit_status::accepted);
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.to_server(server));

        ruvia::http_response response;
        response.status(ruvia::http_status::ok);
        response.static_body("hello");
        const auto mismatched_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        const auto result = peers.server_.submit_response_head(0, response, mismatched_plan);
        const auto* err = std::get_if<ruvia::http3_response_head_error>(&result);
        ASSERT_NE(err, nullptr);
        EXPECT_EQ(*err, ruvia::http3_response_head_error::response_plan_mismatch);
    }
    {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "example.test:443"}), ruvia::http3_submit_status::accepted);
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.to_server(server));

        ruvia::http_response ok_response;
        ok_response.status(ruvia::http_status::ok);
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, ok_response);
        const auto buffered = peers.server_.submit_response_head(0, ok_response, plan);
        const auto* buffered_err = std::get_if<ruvia::http3_response_head_error>(&buffered);
        ASSERT_NE(buffered_err, nullptr);
        EXPECT_EQ(*buffered_err, ruvia::http3_response_head_error::invalid_state);

        const auto streaming = peers.server_.submit_streaming_response_head(0, std::move(ok_response));
        const auto* streaming_err = std::get_if<ruvia::http3_response_head_error>(&streaming);
        ASSERT_NE(streaming_err, nullptr);
        EXPECT_EQ(*streaming_err, ruvia::http3_response_head_error::invalid_state);

        ruvia::http_response tunnel_ok;
        tunnel_ok.status(ruvia::http_status::ok);
        EXPECT_EQ(peers.server_.submit_connect_response_head(0, tunnel_ok), ruvia::http3_submit_status::accepted);
    }
}
