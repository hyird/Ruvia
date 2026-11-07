#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

namespace {

class receive_memory_resource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> allocation_budget_{};
    std::size_t allocations_{0};
    std::size_t live_{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (allocation_budget_) {
            if (*allocation_budget_ == 0) {
                throw std::bad_alloc();
            }
            --*allocation_budget_;
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations_;
        ++live_;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        --live_;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

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

std::pmr::string request_block() {
    std::pmr::string block;
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
        receive_memory_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource, .receive_frame_metadata_ = publish});
        received_log log;
        const auto preface = server_preface();
        EXPECT_TRUE(server.feed(preface) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        log.count_ = 0;
        std::string priority;
        append_frame(priority, ruvia::http2_frame_type::priority, 0, 13, priority_payload(5, 19, false));
        std::string window;
        append_frame(window, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7));
        const auto allocations = resource.allocations_;
        resource.allocation_budget_ = 0;
        EXPECT_TRUE(server.feed(priority) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_TRUE(server.feed(window) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        resource.allocation_budget_.reset();
        EXPECT_EQ(resource.allocations_, allocations);
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
