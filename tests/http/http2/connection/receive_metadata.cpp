#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "test_harness.h"

namespace {

class receive_memory_resource final : public std::pmr::memory_resource {
public:
    bool reject_{false};
    std::size_t allocations_{0};
    std::size_t live_{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // STL 调试代理可能在 noexcept 构造中分配，不向这些代理注入失败。
        if (reject_ && bytes >= 32) {
            throw std::bad_alloc();
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

struct received_record final {
    std::size_t kind_{0};
    std::array<ruvia::http2_setting_entry, 16> settings_{};
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

struct receive_observations final {
    std::array<received_record, 64> records_{};
    std::size_t count_{0};
    bool overflow_{false};
    receive_memory_resource* reject_after_priority_{nullptr};

    static void observe(void* context, const ruvia::http2_receive_metadata& metadata) noexcept {
        auto& self = *static_cast<receive_observations*>(context);
        if (self.count_ == self.records_.size()) {
            self.overflow_ = true;
            return;
        }
        auto& record = self.records_[self.count_++];
        record.kind_ = metadata.index();
        if (const auto* settings = std::get_if<ruvia::http2_received_settings>(&metadata)) {
            record.settings_count_ = settings->size();
            if (settings->size() > record.settings_.size()) {
                self.overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != settings->size(); ++index) {
                record.settings_[index] = settings->at(index);
            }
            try {
                (void)settings->at(settings->size());
            } catch (const std::out_of_range&) {
                record.checked_bounds_ = true;
            }
        } else if (const auto* window = std::get_if<ruvia::http2_received_window_update>(&metadata)) {
            record.stream_id_ = window->stream_id_;
            record.value_ = window->increment_;
        } else if (const auto* priority = std::get_if<ruvia::http2_received_priority>(&metadata)) {
            record.stream_id_ = priority->stream_id_;
            record.value_ = priority->stream_dependency_;
            record.weight_ = priority->weight_;
            record.exclusive_ = priority->exclusive_;
            if (self.reject_after_priority_ != nullptr) {
                self.reject_after_priority_->reject_ = true;
            }
        } else if (const auto* request = std::get_if<ruvia::http2_received_request_head>(&metadata)) {
            record.stream_id_ = request->stream_id_;
            record.request_matches_ = request->request_.method_ == "GET" &&
                                      request->request_.scheme_ == "https" &&
                                      request->request_.authority_ == "example.test" &&
                                      request->request_.path_ == "/metadata";
            record.pseudo_count_ = request->request_.pseudo_header_order_.size();
            if (record.pseudo_count_ > record.pseudo_order_.size()) {
                self.overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.pseudo_count_; ++index) {
                record.pseudo_order_[index] = request->request_.pseudo_header_order_[index];
            }
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

RUVIA_TEST(http2_receive_settings_preserve_unknown_duplicate_empty_and_bounds) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        receive_observations observed;
        const ruvia::http2_connection_options options{
            .receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed};
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
        RUVIA_CHECK(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK_EQ(observed.count_, 2u);
        RUVIA_CHECK(!observed.overflow_);
        const auto& first = observed.records_[0];
        RUVIA_CHECK_EQ(first.settings_count_, 4u);
        RUVIA_CHECK(first.checked_bounds_);
        RUVIA_CHECK_EQ(static_cast<std::uint16_t>(first.settings_[0].id_), 0xfefe);
        RUVIA_CHECK_EQ(first.settings_[0].value_, 0xabcdef01u);
        RUVIA_CHECK(first.settings_[1].id_ == ruvia::http2_setting_id::max_concurrent_streams);
        RUVIA_CHECK_EQ(first.settings_[1].value_, 13u);
        RUVIA_CHECK_EQ(first.settings_[2].value_, 7u);
        RUVIA_CHECK_EQ(static_cast<std::uint16_t>(first.settings_[3].id_), 0xfefe);
        RUVIA_CHECK_EQ(first.settings_[3].value_, 0u);
        RUVIA_CHECK_EQ(observed.records_[1].settings_count_, 0u);
        RUVIA_CHECK(observed.records_[1].checked_bounds_);
    }
    const ruvia::http2_received_settings empty;
    bool threw = false;
    try {
        (void)empty.at((std::numeric_limits<std::size_t>::max)());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(http2_receive_invalid_later_setting_does_not_publish_partial_frame) {
    receive_observations observed;
    auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
    std::string wire = server_preface();
    std::string payload;
    append_setting(payload, 3, 19);
    append_setting(payload, 4, 0x80000000U);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 1, false));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::flow_control_error);
    RUVIA_CHECK_EQ(observed.count_, 1u);
    RUVIA_CHECK_EQ(observed.records_[0].settings_count_, 0u);
}

RUVIA_TEST(http2_receive_byte_fragments_notify_only_complete_frames) {
    receive_observations observed;
    auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
    std::string wire = server_preface();
    const auto settings_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(7, 256, true));
    const auto priority_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(11));
    for (std::size_t index = 0; index != wire.size(); ++index) {
        const auto status = server.feed(std::string_view(wire).substr(index, 1));
        RUVIA_CHECK(status == ruvia::http2_feed_result::accepted || status == ruvia::http2_feed_result::need_input);
        const auto expected = static_cast<std::size_t>(index + 1 >= settings_end) +
                              static_cast<std::size_t>(index + 1 >= priority_end) +
                              static_cast<std::size_t>(index + 1 == wire.size());
        RUVIA_CHECK_EQ(observed.count_, expected);
    }
    RUVIA_CHECK_EQ(observed.records_[1].stream_id_, 7u);
    RUVIA_CHECK_EQ(observed.records_[1].value_, 7u);
    RUVIA_CHECK_EQ(observed.records_[1].weight_, 256u);
    RUVIA_CHECK(observed.records_[1].exclusive_);
    RUVIA_CHECK_EQ(observed.records_[2].value_, 11u);
    RUVIA_CHECK(!server.connection_error());
}

RUVIA_TEST(http2_receive_padded_headers_priority_precedes_continued_request_and_later_metadata) {
    for (const bool continued : {false, true}) {
        receive_observations observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
        auto block = request_block();
        const auto split = continued ? block.size() / 2 : block.size();
        std::string payload(1, '\x03');
        payload += priority_payload(1, 256, true);  // 自依赖仍按废弃 priority 的既有规则容忍。
        payload.append(block.data(), split);
        payload.append(3, '\0');
        std::string wire = server_preface();
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(3));
        append_frame(wire, ruvia::http2_frame_type::headers, continued ? 0x29 : 0x2d, 1, payload);
        if (continued) {
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK_EQ(observed.count_, 3u);
            RUVIA_CHECK(server.header_block_in_progress());
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::continuation, 4, 1, std::string_view(block).substr(split));
        }
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 5, priority_payload(0, 1, false));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(9));
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK_EQ(observed.count_, 6u);
        const std::array<std::size_t, 6> kinds{0, 1, 2, 3, 2, 1};
        for (std::size_t index = 0; index != kinds.size(); ++index) {
            RUVIA_CHECK_EQ(observed.records_[index].kind_, kinds[index]);
        }
        RUVIA_CHECK_EQ(observed.records_[2].value_, 1u);
        RUVIA_CHECK_EQ(observed.records_[2].weight_, 256u);
        RUVIA_CHECK(observed.records_[2].exclusive_);
        const auto& request = observed.records_[3];
        RUVIA_CHECK(request.request_matches_);
        RUVIA_CHECK_EQ(request.stream_id_, 1u);
        RUVIA_CHECK_EQ(request.pseudo_count_, 4u);
        const std::array expected{ruvia::http2_request_pseudo_header::method, ruvia::http2_request_pseudo_header::scheme,
            ruvia::http2_request_pseudo_header::authority, ruvia::http2_request_pseudo_header::path};
        for (std::size_t index = 0; index != expected.size(); ++index) {
            RUVIA_CHECK(request.pseudo_order_[index] == expected[index]);
        }
        auto event = server.next_event();
        RUVIA_CHECK(event && event->request_head());
        RUVIA_CHECK(!server.connection_error());
    }
}

RUVIA_TEST(http2_receive_window_updates_only_report_applied_credit) {
    receive_observations observed;
    auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
    auto block = request_block();
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::headers, 5, 3, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head());
    while (server.next_event()) {
    }
    observed.count_ = 0;
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(0));  // 跳过的闭流。
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(17));
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(23));
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(0));  // 流错误不通知。
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(9));  // 本地 reset 后忽略。
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(observed.count_, 2u);
    RUVIA_CHECK_EQ(observed.records_[0].stream_id_, 3u);
    RUVIA_CHECK_EQ(observed.records_[0].value_, 17u);
    RUVIA_CHECK_EQ(observed.records_[1].stream_id_, 0u);
    RUVIA_CHECK_EQ(observed.records_[1].value_, 23u);
    RUVIA_CHECK(!server.connection_error());
}

RUVIA_TEST(http2_receive_rejected_connection_window_and_malformed_priority_are_not_observed) {
    for (const auto type : {ruvia::http2_frame_type::window_update, ruvia::http2_frame_type::priority}) {
        receive_observations observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
        std::string wire = server_preface();
        append_frame(wire, type, 0, type == ruvia::http2_frame_type::window_update ? 0 : 1, window_payload(0));
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK_EQ(observed.count_, 1u);
        RUVIA_CHECK(server.connection_error() == (type == ruvia::http2_frame_type::window_update
                                                         ? ruvia::http2_error_code::protocol_error
                                                         : ruvia::http2_error_code::frame_size_error));
    }
}

RUVIA_TEST(http2_receive_pmr_retry_does_not_replay_committed_prefix) {
    receive_memory_resource resource;
    receive_observations observed;
    {
        auto server = ruvia::http2_connection::server({.resource_ = &resource,
            .receive_observer_ = &receive_observations::observe,
            .receive_observer_context_ = &observed});
        const auto preface = server_preface();
        RUVIA_CHECK(server.feed(preface) == ruvia::http2_feed_result::accepted);
        observed.count_ = 0;
        auto block = request_block();
        ruvia::hpack_encoder::encode_header(block, "x-large", std::string(4096, 'a'));
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 1, priority_payload(0, 12, false));
        append_frame(wire, ruvia::http2_frame_type::headers, 5, 1, block);
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(31));
        observed.reject_after_priority_ = &resource;
        bool threw = false;
        try {
            (void)server.feed(wire);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(observed.count_, 1u);
        RUVIA_CHECK_EQ(observed.records_[0].kind_, 2u);
        observed.reject_after_priority_ = nullptr;
        resource.reject_ = false;
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK_EQ(observed.count_, 3u);
        RUVIA_CHECK_EQ(observed.records_[1].kind_, 3u);
        RUVIA_CHECK(observed.records_[1].request_matches_);
        RUVIA_CHECK_EQ(observed.records_[2].kind_, 1u);
        RUVIA_CHECK_EQ(observed.records_[2].value_, 31u);
        RUVIA_CHECK(!server.connection_error());
    }
    RUVIA_CHECK_EQ(resource.live_, 0u);
}

RUVIA_TEST(http2_receive_control_metadata_needs_no_dynamic_allocation) {
    for (const bool observe : {false, true}) {
        receive_memory_resource resource;
        receive_observations observed;
        auto server = ruvia::http2_connection::server({.resource_ = &resource,
            .receive_observer_ = observe ? &receive_observations::observe : nullptr,
            .receive_observer_context_ = &observed});
        const auto preface = server_preface();
        RUVIA_CHECK(server.feed(preface) == ruvia::http2_feed_result::accepted);
        observed.count_ = 0;
        std::string priority;
        append_frame(priority, ruvia::http2_frame_type::priority, 0, 13, priority_payload(5, 19, false));
        std::string window;
        append_frame(window, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7));
        const auto allocations = resource.allocations_;
        resource.reject_ = true;
        RUVIA_CHECK(server.feed(priority) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(server.feed(window) == ruvia::http2_feed_result::accepted);
        resource.reject_ = false;
        RUVIA_CHECK_EQ(resource.allocations_, allocations);
        RUVIA_CHECK_EQ(observed.count_, observe ? 2u : 0u);
        RUVIA_CHECK(!server.connection_error());
    }
}

RUVIA_TEST(http2_receive_malformed_headers_do_not_publish_priority_or_request) {
    for (const bool missing_priority : {false, true}) {
        receive_observations observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
        std::string wire = server_preface();
        std::string payload(1, missing_priority ? '\0' : '\x08');
        payload += priority_payload(0, 1, false);
        if (missing_priority) {
            payload.pop_back();
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x2d, 1, payload);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK_EQ(observed.count_, 1u);
        RUVIA_CHECK(server.connection_error() == (missing_priority ? ruvia::http2_error_code::frame_size_error
                                                                   : ruvia::http2_error_code::protocol_error));
    }
}

RUVIA_TEST(http2_receive_window_overflow_does_not_publish_rejected_credit) {
    receive_observations observed;
    auto server = ruvia::http2_connection::server({.receive_observer_ = &receive_observations::observe, .receive_observer_context_ = &observed});
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0x7fffffffU));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK_EQ(observed.count_, 1u);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::flow_control_error);
}
