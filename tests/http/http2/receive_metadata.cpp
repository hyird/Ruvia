#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http2_connection.h"
namespace {

template <typename value_type>
concept settings_borrowable = requires(value_type&& value) {
    std::forward<value_type>(value).settings();
};
static_assert(settings_borrowable<const ruvia::http2_frame_metadata&>);
static_assert(!settings_borrowable<ruvia::http2_frame_metadata>);

enum class received_kind : std::uint8_t { settings,
    window_update,
    priority,
    request_head };

struct received_record final {
    received_kind kind_{received_kind::settings};
    std::array<ruvia::http2_setting_entry, 8> settings_{};
    std::size_t settings_count_{0};
    bool checked_bounds_{false};
    std::uint32_t stream_id_{0};
    std::uint32_t value_{0};
    std::uint16_t weight_{0};
    bool exclusive_{false};
    bool request_matches_{false};
    std::array<ruvia::http2_request_pseudo_header, 5> pseudo_order_{};
    std::size_t pseudo_count_{0};
};

// 按 next_event() 顺序记录元数据与请求头事件，其余事件不在本文件观察范围内。
// 请求头租约保留到日志析构，使对应流保持存活。
struct received_log final {
    std::array<received_record, 16> records_{};
    std::size_t count_{0};
    bool overflow_{false};
    std::vector<ruvia::http2_request_head_event> requests_;

    void drain(ruvia::http2_connection& connection) {
        while (auto event = connection.next_event()) {
            append(*event);
        }
    }

private:
    void append(ruvia::http2_event& event) {
        const auto* settings = event.received_settings();
        const auto* window = event.received_window_update();
        const auto* priority = event.received_priority();
        auto* request = event.request_head();
        if (settings == nullptr && window == nullptr && priority == nullptr && request == nullptr) {
            return;
        }
        if (count_ == records_.size()) {
            overflow_ = true;
            return;
        }
        auto& record = records_[count_++];
        if (settings != nullptr) {
            record.kind_ = received_kind::settings;
            record.settings_count_ = settings->size();
            if (record.settings_count_ > record.settings_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.settings_count_; ++index) {
                record.settings_[index] = settings->at(index);
            }
            try {
                (void)settings->at(settings->size());
            } catch (const std::out_of_range&) {
                record.checked_bounds_ = true;
            }
        } else if (window != nullptr) {
            record.kind_ = received_kind::window_update;
            record.stream_id_ = window->stream_id_;
            record.value_ = window->increment_;
        } else if (priority != nullptr) {
            record.kind_ = received_kind::priority;
            record.stream_id_ = priority->stream_id_;
            record.value_ = priority->stream_dependency_;
            record.weight_ = priority->weight_;
            record.exclusive_ = priority->exclusive_;
        } else {
            record.kind_ = received_kind::request_head;
            record.stream_id_ = request->stream_id();
            const auto view = request->request_view();
            record.request_matches_ = view.method_ == "GET" && view.scheme_ == "https" &&
                                      view.authority_ == "example.test" && view.path_ == "/metadata";
            record.pseudo_count_ = view.pseudo_header_order_.size();
            if (record.pseudo_count_ > record.pseudo_order_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.pseudo_count_; ++index) {
                record.pseudo_order_[index] = view.pseudo_header_order_[index];
            }
            requests_.push_back(std::move(*request));
        }
    }
};

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload = {}) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void append_setting(std::string& payload, std::uint16_t id, std::uint32_t value) {
    const std::array<char, 6> entry{static_cast<char>(id >> 8), static_cast<char>(id),
        static_cast<char>(value >> 24), static_cast<char>(value >> 16),
        static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(entry.data(), entry.size());
}

std::string window_payload(std::uint32_t increment) {
    const std::array<char, 4> bytes{static_cast<char>(increment >> 24), static_cast<char>(increment >> 16),
        static_cast<char>(increment >> 8), static_cast<char>(increment)};
    return std::string(bytes.data(), bytes.size());
}

std::string priority_payload(std::uint32_t dependency, unsigned weight, bool exclusive) {
    auto payload = window_payload(dependency | (exclusive ? 0x80000000U : 0));
    payload.push_back(static_cast<char>(weight - 1));
    return payload;
}

std::string request_block() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/metadata");
    return block;
}

std::string server_preface() {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
    return wire;
}

}  // namespace

TEST(Ruvia, http2_receive_frame_metadata_off_produces_no_metadata_events) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server()
                                                            : ruvia::http2_connection::client();
        std::string settings;
        append_setting(settings, 3, 13);
        append_setting(settings, 0xfefe, 1);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 16, false));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
        if (role == ruvia::http2_role::server) {
            const auto block = request_block();
            auto headers = priority_payload(0, 32, true);
            headers.append(block.data(), block.size());
            append_frame(wire, ruvia::http2_frame_type::headers, 0x24, 1, headers);
            append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(5));
        }
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        received_log log;
        log.drain(connection);
        EXPECT_TRUE(!log.overflow_);
        if (role == ruvia::http2_role::server) {
            EXPECT_EQ(log.count_, 1u);
            EXPECT_TRUE(log.records_[0].kind_ == received_kind::request_head);
            EXPECT_TRUE(log.records_[0].request_matches_);
        } else {
            EXPECT_EQ(log.count_, 0u);
        }
        EXPECT_TRUE(!connection.connection_error());
    }
}

// nghttp2 folds a repeated known identifier into its first position with the
// last value, keeps unknown identifiers in wire order, and owns the decoded
// entries, so the event outlives the input.
TEST(Ruvia, http2_receive_settings_fold_known_duplicates_and_keep_unknown_entries) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        const ruvia::http2_connection_options options{.receive_frame_metadata_ = true};
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server(options)
                                                            : ruvia::http2_connection::client(options);
        std::string payload;
        append_setting(payload, 0xfefe, 0xabcdef01);
        append_setting(payload, 3, 13);
        append_setting(payload, 3, 7);
        append_setting(payload, 0xfefe, 0);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
        append_frame(wire, ruvia::http2_frame_type::settings, 1, 0);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        wire.assign(wire.size(), '#');
        received_log log;
        log.drain(connection);
        EXPECT_EQ(log.count_, 2u);
        EXPECT_TRUE(!log.overflow_);
        const auto& first = log.records_[0];
        EXPECT_TRUE(first.kind_ == received_kind::settings);
        EXPECT_EQ(first.settings_count_, 3u);
        EXPECT_TRUE(first.checked_bounds_);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[0].id_), 0xfefe);
        EXPECT_EQ(first.settings_[0].value_, 0xabcdef01u);
        EXPECT_TRUE(first.settings_[1].id_ == ruvia::http2_setting_id::max_concurrent_streams);
        EXPECT_EQ(first.settings_[1].value_, 7u);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[2].id_), 0xfefe);
        EXPECT_EQ(first.settings_[2].value_, 0u);
        EXPECT_TRUE(log.records_[1].kind_ == received_kind::settings);
        EXPECT_EQ(log.records_[1].settings_count_, 0u);
        EXPECT_TRUE(log.records_[1].checked_bounds_);
    }
    const ruvia::http2_received_settings empty;
    bool threw = false;
    try {
        (void)empty.at((std::numeric_limits<std::size_t>::max)());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, http2_receive_invalid_later_setting_does_not_publish_partial_frame) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    std::string payload;
    append_setting(payload, 3, 19);
    append_setting(payload, 4, 0x80000000U);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 1, false));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_EQ(log.records_[0].settings_count_, 0u);
}

TEST(Ruvia, http2_receive_byte_fragments_publish_only_complete_frames) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    std::string wire = server_preface();
    const auto settings_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
    const auto first_window_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(11));
    for (std::size_t index = 0; index != wire.size(); ++index) {
        const auto status = server.feed(std::string_view(wire).substr(index, 1));
        EXPECT_TRUE(status == ruvia::http2_feed_result::accepted);
        log.drain(server);
        const auto expected = static_cast<std::size_t>(index + 1 >= settings_end) +
                              static_cast<std::size_t>(index + 1 >= first_window_end) +
                              static_cast<std::size_t>(index + 1 == wire.size());
        EXPECT_EQ(log.count_, expected);
    }
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].value_, 5u);
    EXPECT_TRUE(log.records_[2].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[2].value_, 11u);
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_receive_padded_headers_priority_precedes_continued_request_and_later_metadata) {
    for (const bool continued : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        received_log log;
        auto block = request_block();
        const auto split = continued ? block.size() / 2 : block.size();
        std::string payload(1, '\x03');
        payload += priority_payload(0, 256, true);
        payload.append(block.data(), split);
        payload.append(3, '\0');
        std::string wire = server_preface();
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(3));
        append_frame(wire, ruvia::http2_frame_type::headers, continued ? 0x29 : 0x2d, 1, payload);
        if (continued) {
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            log.drain(server);
            EXPECT_EQ(log.count_, 3u);
            EXPECT_TRUE(server.header_block_in_progress());
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::continuation, 4, 1, std::string_view(block).substr(split));
        }
        // A PRIORITY frame publishes nothing (RFC 9113 §5.3.2 deprecates it).
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 5, priority_payload(0, 1, false));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(9));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, 5u);
        EXPECT_TRUE(!log.overflow_);
        const std::array kinds{received_kind::settings, received_kind::window_update, received_kind::priority,
            received_kind::request_head, received_kind::window_update};
        for (std::size_t index = 0; index != kinds.size(); ++index) {
            EXPECT_TRUE(log.records_[index].kind_ == kinds[index]);
        }
        EXPECT_EQ(log.records_[1].value_, 3u);
        EXPECT_EQ(log.records_[2].stream_id_, 1u);
        EXPECT_EQ(log.records_[2].value_, 0u);
        EXPECT_EQ(log.records_[2].weight_, 256u);
        EXPECT_TRUE(log.records_[2].exclusive_);
        const auto& request = log.records_[3];
        EXPECT_TRUE(request.request_matches_);
        EXPECT_EQ(request.stream_id_, 1u);
        EXPECT_EQ(request.pseudo_count_, 4u);
        const std::array expected{ruvia::http2_request_pseudo_header::method, ruvia::http2_request_pseudo_header::scheme,
            ruvia::http2_request_pseudo_header::authority, ruvia::http2_request_pseudo_header::path};
        for (std::size_t index = 0; index != expected.size(); ++index) {
            EXPECT_TRUE(request.pseudo_order_[index] == expected[index]);
        }
        EXPECT_EQ(log.records_[4].value_, 9u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_window_updates_only_report_applied_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    auto block = request_block();
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::headers, 5, 3, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::request_head);
    log.count_ = 0;
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(0));  // 跳过的闭流：忽略。
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(17));
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(23));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[0].stream_id_, 3u);
    EXPECT_EQ(log.records_[0].value_, 17u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].stream_id_, 0u);
    EXPECT_EQ(log.records_[1].value_, 23u);
    EXPECT_TRUE(!server.connection_error());
    // nghttp2 treats a zero increment on a live stream as a connection error.
    log.count_ = 0;
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    log.drain(server);
    EXPECT_EQ(log.count_, 0u);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

TEST(Ruvia, http2_receive_rejected_connection_window_and_malformed_priority_are_not_published) {
    for (const auto type : {ruvia::http2_frame_type::window_update, ruvia::http2_frame_type::priority}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        append_frame(wire, type, 0, type == ruvia::http2_frame_type::window_update ? 0 : 1, window_payload(0));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == (type == ruvia::http2_frame_type::window_update
                                                         ? ruvia::http2_error_code::protocol_error
                                                         : ruvia::http2_error_code::frame_size_error));
    }
}

TEST(Ruvia, http2_receive_control_metadata_needs_no_dynamic_allocation) {
    // PRIORITY frames are deprecated (RFC 9113 §5.3.2) and publish nothing.
    for (const bool publish : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = publish});
        received_log log;
        const auto preface = server_preface();
        EXPECT_TRUE(server.feed(preface) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        log.count_ = 0;
        std::string priority;
        append_frame(priority, ruvia::http2_frame_type::priority, 0, 13, priority_payload(5, 19, false));
        std::string window;
        append_frame(window, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7));
        EXPECT_TRUE(server.feed(priority) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_TRUE(server.feed(window) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, publish ? 1u : 0u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_malformed_headers_do_not_publish_priority_or_request) {
    // Invalid padding and a payload too short for the PRIORITY fields are both
    // reported by nghttp2 as connection PROTOCOL_ERROR.
    for (const bool missing_priority : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        std::string payload(1, missing_priority ? '\0' : '\x08');
        payload += priority_payload(0, 1, false);
        if (missing_priority) {
            payload.pop_back();
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x2d, 1, payload);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

TEST(Ruvia, http2_receive_window_overflow_does_not_publish_rejected_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0x7fffffffU));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
}

namespace {
struct wire_observation_log final {
    std::vector<ruvia::http2_frame_metadata> frames_;
    struct field final {
        std::uint64_t first_;
        std::uint64_t frame_;
        std::size_t index_;
        std::string name_;
        std::string value_;
    };
    std::vector<field> fields_;
    std::size_t begins_{0};
    std::size_t ends_{0};

    static void frame(void* context, const ruvia::http2_frame_metadata& metadata) noexcept {
        static_cast<wire_observation_log*>(context)->frames_.push_back(metadata);
    }
    static void header(void* context, const ruvia::http2_header_metadata& metadata) noexcept {
        auto& log = *static_cast<wire_observation_log*>(context);
        switch (metadata.phase_) {
            case ruvia::http2_header_observation_phase::begin:
                ++log.begins_;
                break;
            case ruvia::http2_header_observation_phase::end:
                ++log.ends_;
                break;
            case ruvia::http2_header_observation_phase::field:
                log.fields_.push_back({metadata.first_frame_observation_sequence_, metadata.frame_observation_sequence_,
                    metadata.field_index_, std::string(metadata.name_), std::string(metadata.value_)});
                break;
        }
    }
};
}  // namespace

TEST(Ruvia, http2_wire_observers_preserve_raw_settings_priority_and_fragmented_hpack) {
    for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame,
            .header_observer_ = &wire_observation_log::header,
            .observer_context_ = &observed});
        std::string settings;
        append_setting(settings, 3, 17);
        append_setting(settings, 0xfafa, 123);
        append_setting(settings, 3, 29);
        append_setting(settings, 1, 1024);
        append_setting(settings, 1, 4096);
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7340033));
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 3, priority_payload(0, 256, true));
        // RFC 7541 C.4.1: Huffman-coded authority inserted at dynamic index 62.
        const std::string_view block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
        std::string payload(1, '\x03');
        payload += priority_payload(3, 42, false);
        payload += block.substr(0, 6);
        payload.append(3, '\0');
        append_frame(wire, ruvia::http2_frame_type::headers, 0x29, 1, payload);
        append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, 1, block.substr(6));
        // A second block uses that dynamic-table entry in a different order.
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3,
            std::string_view("\xbe\x84\x87\x82", 4));
        received_log requests;
        for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(cursor, fragment)) ==
                        ruvia::http2_feed_result::accepted);
            requests.drain(server);
        }
        ASSERT_EQ(observed.frames_.size(), 6u);
        for (std::size_t i = 0; i < observed.frames_.size(); ++i) {
            EXPECT_EQ(observed.frames_[i].observation_sequence_, i);
        }
        const auto& raw = observed.frames_[0];
        ASSERT_EQ(raw.settings().size(), 5u);
        EXPECT_TRUE(raw.settings_complete_);
        EXPECT_EQ(static_cast<std::uint16_t>(raw.settings()[1].id_), 0xfafa);
        EXPECT_EQ(raw.settings()[0].value_, 17u);
        EXPECT_EQ(raw.settings()[2].value_, 29u);
        EXPECT_EQ(raw.settings()[3].value_, 1024u);
        EXPECT_EQ(raw.settings()[4].value_, 4096u);
        ASSERT_TRUE(observed.frames_[1].window_increment_);
        EXPECT_EQ(*observed.frames_[1].window_increment_, 7340033u);
        ASSERT_TRUE(observed.frames_[2].priority_);
        EXPECT_EQ(observed.frames_[2].priority_->weight_, 256u);
        EXPECT_TRUE(observed.frames_[2].priority_->exclusive_);
        ASSERT_TRUE(observed.frames_[3].priority_);
        EXPECT_EQ(observed.frames_[3].priority_->stream_dependency_, 3u);
        EXPECT_EQ(observed.frames_[3].priority_->weight_, 42u);
        EXPECT_TRUE(observed.frames_[4].type_ == ruvia::http2_frame_type::continuation);
        ASSERT_EQ(observed.fields_.size(), 8u);
        EXPECT_EQ(observed.fields_[0].name_, ":method");
        EXPECT_EQ(observed.fields_[1].name_, ":scheme");
        EXPECT_EQ(observed.fields_[2].name_, ":path");
        EXPECT_EQ(observed.fields_[3].name_, ":authority");
        EXPECT_EQ(observed.fields_[3].value_, "www.example.com");
        EXPECT_EQ(observed.fields_[3].first_, 3u);
        EXPECT_EQ(observed.fields_[3].frame_, 4u);
        EXPECT_EQ(observed.fields_[4].name_, ":authority");
        EXPECT_EQ(observed.fields_[5].name_, ":path");
        EXPECT_EQ(observed.fields_[6].name_, ":scheme");
        EXPECT_EQ(observed.fields_[7].name_, ":method");
        EXPECT_EQ(observed.fields_[7].index_, 3u);
        EXPECT_EQ(observed.begins_, 2u);
        EXPECT_EQ(observed.ends_, 2u);
        EXPECT_FALSE(server.connection_error().has_value());
    }
}

TEST(Ruvia, http2_header_observer_sees_extended_connect_order_before_semantic_rejection) {
    for (const bool malformed : {false, true}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.header_observer_ = &wire_observation_log::header, .observer_context_ = &observed});
        std::string block;
        for (const auto& field : std::array{
                 std::pair{":protocol", "websocket"}, std::pair{":path", "/chat"},
                 std::pair{":scheme", "https"}, std::pair{":authority", "example.test:443"},
                 std::pair{":method", "CONNECT"}}) {
            ruvia::hpack_encoder::encode_header(block, field.first, field.second);
        }
        if (malformed) {
            ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        }
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        auto wire = server_preface();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        ASSERT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        bool request_received = false;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                request_received = true;
                const auto order = request->request_view().pseudo_header_order_;
                ASSERT_EQ(order.size(), 5u);
                EXPECT_TRUE(order[0] == ruvia::http2_request_pseudo_header::protocol);
                EXPECT_TRUE(order[4] == ruvia::http2_request_pseudo_header::method);
            }
        }
        EXPECT_EQ(request_received, !malformed);
        ASSERT_EQ(observed.fields_.size(), 6u);
        EXPECT_EQ(observed.fields_[0].name_, ":protocol");
        EXPECT_EQ(observed.fields_[4].name_, ":method");
        EXPECT_EQ(observed.fields_[5].name_, malformed ? ":method" : "sec-websocket-version");
        EXPECT_EQ(observed.fields_[0].first_, 1u);
        EXPECT_EQ(observed.frames_.size(), 0u);
    }
}

TEST(Ruvia, http2_frame_observer_includes_unknown_extensions_without_advertisement_semantics) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        wire_observation_log observed;
        const ruvia::http2_connection_options options{
            .frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed};
        auto connection = role == ruvia::http2_role::server
                              ? ruvia::http2_connection::server(options)
                              : ruvia::http2_connection::client(options);
        std::string wire = role == ruvia::http2_role::server
                               ? std::string(ruvia::http2_client_preface)
                               : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "unknown");
        // ORIGIN is not enabled, and servers never process ALTSVC. These
        // malformed opaque payloads must remain ignored with observation on.
        append_frame(wire, ruvia::http2_frame_type::origin, 0, 0, "not an origin list");
        if (role == ruvia::http2_role::server) {
            append_frame(wire, ruvia::http2_frame_type::alternative_service, 0, 0, "x");
        }
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(19));
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_FALSE(connection.connection_error().has_value());
        while (auto event = connection.next_event()) {
            EXPECT_EQ(event->origin_advertisement(), nullptr);
            EXPECT_EQ(event->alternative_service_advertisement(), nullptr);
        }
        ASSERT_EQ(observed.frames_.size(), role == ruvia::http2_role::server ? 5u : 4u);
        EXPECT_EQ(static_cast<unsigned>(observed.frames_[1].type_), 0xeeu);
        EXPECT_EQ(observed.frames_.back().observation_sequence_, observed.frames_.size() - 1);
        ASSERT_TRUE(observed.frames_.back().window_increment_);
        EXPECT_EQ(*observed.frames_.back().window_increment_, 19u);
    }
}

TEST(Ruvia, http2_frame_observation_does_not_weaken_mandatory_protocol_rejection) {
    wire_observation_log observed;
    auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed});
    auto wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    ASSERT_EQ(observed.frames_.size(), 2u);
    ASSERT_TRUE(observed.frames_[1].window_increment_);
    EXPECT_EQ(*observed.frames_[1].window_increment_, 0u);
}

TEST(Ruvia, http2_frame_observer_preserves_enabled_origin_and_alternative_service_decoding) {
    for (const bool observe : {false, true}) {
        for (const auto fragment : {std::size_t{1}, std::size_t{4096}}) {
            wire_observation_log observed;
            auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true,
                .frame_observer_ = observe ? &wire_observation_log::frame : nullptr,
                .observer_context_ = &observed});
            const std::array<std::string_view, 1> origins{"https://example.test"};
            auto origin_frame = ruvia::encode_http2_origin_frame(origins);
            auto alternative_frame = ruvia::encode_http2_alternative_service_frame(0, origins[0], "h3=\":443\"");
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(origin_frame));
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(alternative_frame));
            const auto& origin_bytes = std::get<std::vector<char>>(origin_frame);
            const auto& alternative_bytes = std::get<std::vector<char>>(alternative_frame);
            std::string wire;
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
            wire.append(origin_bytes.data(), origin_bytes.size());
            append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "discard me");
            wire.append(alternative_bytes.data(), alternative_bytes.size());
            std::size_t origin_count = 0;
            std::size_t alternative_count = 0;
            for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
                ASSERT_TRUE(client.feed(std::string_view(wire).substr(cursor, fragment)) ==
                            ruvia::http2_feed_result::accepted);
                while (auto event = client.next_event()) {
                    if (const auto* origin = event->origin_advertisement()) {
                        ++origin_count;
                        ASSERT_EQ(origin->origins_.size(), 1u);
                        EXPECT_TRUE(origin->origins_[0] == origins[0]);
                    }
                    if (const auto* alternative = event->alternative_service_advertisement()) {
                        ++alternative_count;
                        EXPECT_EQ(alternative->stream_id_, 0u);
                        EXPECT_TRUE(alternative->origin_ == origins[0]);
                        EXPECT_TRUE(alternative->field_value_ == "h3=\":443\"");
                    }
                }
            }
            EXPECT_EQ(origin_count, 1u);
            EXPECT_EQ(alternative_count, 1u);
            EXPECT_FALSE(client.connection_error().has_value());
            ASSERT_EQ(observed.frames_.size(), observe ? 4u : 0u);
            if (observe) {
                EXPECT_TRUE(observed.frames_[1].type_ == ruvia::http2_frame_type::origin);
                EXPECT_EQ(observed.frames_[1].observation_sequence_, 1u);
                EXPECT_TRUE(observed.frames_[3].type_ == ruvia::http2_frame_type::alternative_service);
                EXPECT_EQ(observed.frames_[3].observation_sequence_, 3u);
            }
        }
    }
}
