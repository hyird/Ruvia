#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/http/http2_connection.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
namespace http2_frame_observation_test {
using namespace std::chrono_literals;

class observer_controller final {
public:
    RUVIA_ROUTES_BEGIN(observer_controller)
    RUVIA_GET("/fingerprint", fingerprint);
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> fingerprint(ruvia::context& context) {
        co_return context.text(std::to_string(context.conn().connection_id()));
    }
};

struct observation final {
    std::vector<ruvia::http2_frame_metadata> frames_;
    std::vector<std::string> pseudo_names_;
    std::vector<std::uint64_t> first_frames_;
    std::size_t begins_{0};
    std::size_t ends_{0};
};

class observation_log final {
public:
    void frame(const ruvia::http2_frame_record& record) {
        std::lock_guard lock(mutex_);
        connections_[record.connection_id()].frames_.push_back(record.metadata());
    }
    void header(const ruvia::http2_headers_record& record) {
        std::lock_guard lock(mutex_);
        auto& result = connections_[record.connection_id()];
        const auto& metadata = record.metadata();
        if (metadata.phase_ == ruvia::http2_header_observation_phase::begin) {
            ++result.begins_;
        } else if (metadata.phase_ == ruvia::http2_header_observation_phase::end) {
            ++result.ends_;
        } else if (metadata.name_.starts_with(':')) {
            result.pseudo_names_.emplace_back(metadata.name_);
            result.first_frames_.push_back(metadata.first_frame_observation_sequence_);
        }
    }
    observation get(std::uint64_t id) const {
        std::lock_guard lock(mutex_);
        return connections_.at(id);
    }

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, observation> connections_;
};

void setting(std::string& payload, std::uint16_t id, std::uint32_t value) {
    const std::array<char, 6> bytes{static_cast<char>(id >> 8), static_cast<char>(id),
        static_cast<char>(value >> 24), static_cast<char>(value >> 16),
        static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(bytes.data(), bytes.size());
}

std::string opening(bool reverse, bool empty_initial) {
    std::string wire(ruvia::http2_client_preface);
    if (empty_initial) {
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    }
    std::string payload;
    setting(payload, 3, 17);
    setting(payload, 0xfafa, 101);
    setting(payload, 3, 29);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    // Exclusive dependency zero, weight 256: a standalone PRIORITY on idle 3.
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::priority, 0, 3,
        std::string_view("\x80\0\0\0\xff", 5));
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
        std::string_view("\0\x70\0\x01", 4));
    std::string block;
    const std::array names = reverse
                                 ? std::array{":path", ":authority", ":scheme", ":method"}
                                 : std::array{":method", ":scheme", ":authority", ":path"};
    for (const std::string_view name : names) {
        const auto value = name == ":method" ? "GET" : name == ":scheme"  ? "https"
                                                   : name == ":authority" ? "localhost"
                                                                          : "/fingerprint";
        ruvia::hpack_encoder::encode_header(block, name, value);
    }
    // PADDED | PRIORITY | END_STREAM, followed by two CONTINUATION frames.
    payload.assign(1, '\x02');
    payload.append("\0\0\0\x03\x29", 5);
    payload.append(block.data(), 1);
    payload.append(2, '\0');
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x29, 1, payload);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::continuation, 0, 1,
        std::string_view(block).substr(1, 2));
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, 1,
        std::string_view(block).substr(3));
    return wire;
}

std::uint64_t exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    bool reverse, bool empty_initial, std::size_t fragment) {
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
    static constexpr unsigned char alpn[]{2, 'h', '2'};
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 ||
        SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot configure TLS h2 test client");
    }
    stream.next_layer().connect(endpoint);
    stream.handshake(asio::ssl::stream_base::client);
    const unsigned char* selected = nullptr;
    unsigned selected_size = 0;
    SSL_get0_alpn_selected(stream.native_handle(), &selected, &selected_size);
    if (std::string_view(reinterpret_cast<const char*>(selected), selected_size) != "h2") {
        throw std::runtime_error("TLS client did not negotiate h2");
    }
    const auto wire = opening(reverse, empty_initial);
    for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
        asio::write(stream, asio::buffer(std::string_view(wire).substr(cursor, fragment)));
    }
    std::string body;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const auto frame = ruvia::test::read_frame(stream, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            body.append(frame.payload_);
            if ((frame.header_.flags_ & 1) != 0) {
                return std::stoull(body);
            }
        }
    }
}
}  // namespace http2_frame_observation_test

TEST(Ruvia, http2_typed_observers_fingerprint_tls_frames_independently_of_record_boundaries) {
    using namespace http2_frame_observation_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    std::binary_semaphore started{0};
    observation_log observations;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .https_ = endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}})
        .on_start([&] { started.release(); })
        .on_http2_frame([&](const ruvia::http2_frame_record& record) noexcept { observations.frame(record); })
        .on_http2_headers([&](const ruvia::http2_headers_record& record) noexcept { observations.header(record); });
    {
        ruvia::test::app_run running(app);
        ASSERT_TRUE(started.try_acquire_for(10s));
        EXPECT_THROW(app.on_http2_frame(nullptr), std::logic_error);
        EXPECT_THROW(app.on_http2_headers(nullptr), std::logic_error);
        for (const bool reverse : {false, true}) {
            for (const bool empty_initial : {false, true}) {
                for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
                    const auto id = exchange(io, endpoint, reverse, empty_initial, fragment);
                    const auto observed = observations.get(id);
                    ASSERT_EQ(observed.frames_.size(), empty_initial ? 7u : 6u);
                    for (std::size_t i = 0; i < observed.frames_.size(); ++i) {
                        EXPECT_EQ(observed.frames_[i].observation_sequence_, i);
                    }
                    const auto offset = empty_initial ? 1u : 0u;
                    if (empty_initial) {
                        EXPECT_EQ(observed.frames_[0].settings().size(), 0u);
                    }
                    const auto& settings = observed.frames_[offset];
                    ASSERT_EQ(settings.settings().size(), 3u);
                    EXPECT_TRUE(settings.settings_complete_);
                    EXPECT_EQ(settings.settings()[0].value_, 17u);
                    EXPECT_EQ(static_cast<std::uint16_t>(settings.settings()[1].id_), 0xfafa);
                    EXPECT_EQ(settings.settings()[1].value_, 101u);
                    EXPECT_EQ(settings.settings()[2].value_, 29u);
                    ASSERT_TRUE(observed.frames_[offset + 1].priority_);
                    EXPECT_EQ(observed.frames_[offset + 1].priority_->weight_, 256u);
                    EXPECT_TRUE(observed.frames_[offset + 1].priority_->exclusive_);
                    ASSERT_TRUE(observed.frames_[offset + 2].window_increment_);
                    EXPECT_EQ(*observed.frames_[offset + 2].window_increment_, 7340033u);
                    ASSERT_TRUE(observed.frames_[offset + 3].priority_);
                    EXPECT_EQ(observed.frames_[offset + 3].priority_->stream_dependency_, 3u);
                    EXPECT_EQ(observed.frames_[offset + 3].priority_->weight_, 42u);
                    EXPECT_TRUE(observed.frames_[offset + 4].type_ == ruvia::http2_frame_type::continuation);
                    EXPECT_TRUE(observed.frames_[offset + 5].type_ == ruvia::http2_frame_type::continuation);
                    const std::vector<std::string> expected = reverse
                                                                  ? std::vector<std::string>{":path", ":authority", ":scheme", ":method"}
                                                                  : std::vector<std::string>{":method", ":scheme", ":authority", ":path"};
                    EXPECT_EQ(observed.pseudo_names_, expected);
                    ASSERT_EQ(observed.first_frames_.size(), 4u);
                    for (const auto sequence : observed.first_frames_) {
                        EXPECT_EQ(sequence, offset + 3u);
                    }
                    EXPECT_EQ(observed.begins_, 1u);
                    EXPECT_EQ(observed.ends_, 1u);
                }
            }
        }
        app.stop();
        running.join();
    }
    app.on_http2_frame(nullptr).on_http2_headers(nullptr);
}
