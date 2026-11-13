#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <gtest/gtest.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <filesystem>
#include <fstream>
#include <semaphore>
#include <set>

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "ruvia/http/http3_connection.h"
#include "ruvia/web/app.h"

#include "http_server_fixture.h"

namespace {

using namespace std::chrono_literals;

ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) {
    return static_cast<ruvia::http3_quic_connection*>(reference->user_data)
        ->native_handle();
}

// RFC 9114 §3.1: the server offers HTTP/3 only, over TLS 1.3 only (RFC 9001
// §4.2).
int select_h3(SSL*, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) {
    for (unsigned int offset = 0; offset < in_length;) {
        const unsigned int size = in[offset];
        if (size > in_length - offset - 1) {
            break;
        }
        if (size == 2 && in[offset + 1] == 'h' && in[offset + 2] == '3') {
            *out = in + offset + 1;
            *out_length = 2;
            return SSL_TLSEXT_ERR_OK;
        }
        offset += 1 + size;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

void configure_h3_server(SSL_CTX* context) {
    if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1) {
        throw std::runtime_error("cannot restrict the test server to TLS 1.3");
    }
    SSL_CTX_set_alpn_select_cb(context, select_h3, nullptr);
}

constexpr std::array<std::uint8_t, 32> stateless_reset_secret{
    0x52, 0x75, 0x76, 0x69, 0x61, 0x2d, 0x68, 0x33, 0x2d, 0x74, 0x65, 0x73,
    0x74, 0x2d, 0x72, 0x65, 0x73, 0x65, 0x74, 0x2d, 0x73, 0x65, 0x63, 0x72,
    0x65, 0x74, 0x2d, 0x30, 0x31, 0x32, 0x33, 0x34};

// One server connection accepted from a client's first Initial packet the
// way a listener does it (RFC 9000 §7.2, §7.3): a fresh random Source
// Connection ID, the client's chosen Destination Connection ID as
// original_destination_connection_id, its stateless reset token, and no
// active migration.
class quic_server_session final {
public:
    quic_server_session(SSL_CTX* context, const ngtcp2_path& path,
        std::span<const std::byte> initial, ngtcp2_tstamp now) {
        ngtcp2_pkt_hd header{};
        if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(initial.data()), initial.size()) != 0) {
            throw std::invalid_argument("datagram is not an acceptable QUIC Initial");
        }
        const auto source = ruvia::test::http3_test_random_connection_id(16);
        if (ngtcp2_crypto_generate_stateless_reset_token(stateless_reset_token_.data(),
                stateless_reset_secret.data(), stateless_reset_secret.size(), &source) != 0) {
            throw std::runtime_error("cannot derive a stateless reset token");
        }
        tls_.reset(SSL_new(context));
        if (!tls_ || ngtcp2_crypto_ossl_configure_server_session(tls_.get()) != 0) {
            throw std::runtime_error("cannot create the server TLS session");
        }
        SSL_set_app_data(tls_.get(), &reference_);
        SSL_set_accept_state(tls_.get());
        ngtcp2_crypto_ossl_ctx* crypto = nullptr;
        if (ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
            throw std::runtime_error("cannot create the server QUIC crypto context");
        }
        crypto_.reset(crypto);
        connection_.emplace(ruvia::http3_quic_connection::server({
            .crypto_callbacks_ = ruvia::test::http3_test_crypto_callbacks(ruvia::http3_role::server),
            .owner_callbacks_ = {.get_new_connection_id_ = ruvia::test::http3_test_new_connection_id},
            .source_connection_id_ = source,
            .destination_connection_id_ = header.scid,
            .original_destination_connection_id_ = header.dcid,
            .stateless_reset_token_ = stateless_reset_token_,
            .disable_active_migration_ = true,
            .initial_path_ = path,
            .version_ = header.version,
            .initial_timestamp_ = now,
        }));
        reference_.get_conn = get_connection;
        reference_.user_data = &*connection_;
        connection_->set_tls_native_handle(crypto_.get());
    }

    ~quic_server_session() {
        connection_.reset();
        SSL_set_app_data(tls_.get(), nullptr);
    }

    quic_server_session(const quic_server_session&) = delete;
    quic_server_session& operator=(const quic_server_session&) = delete;
    quic_server_session(quic_server_session&&) = delete;
    quic_server_session& operator=(quic_server_session&&) = delete;

    [[nodiscard]] ruvia::http3_quic_connection& connection() noexcept {
        return *connection_;
    }
    [[nodiscard]] const ruvia::http3_quic_stateless_reset_token& stateless_reset_token() const noexcept {
        return stateless_reset_token_;
    }
    [[nodiscard]] bool h3_selected() const noexcept {
        const unsigned char* protocol = nullptr;
        unsigned int protocol_size = 0;
        SSL_get0_alpn_selected(tls_.get(), &protocol, &protocol_size);
        return std::string_view(reinterpret_cast<const char*>(protocol), protocol_size) == "h3";
    }

private:
    struct free_tls final {
        void operator()(SSL* tls) const noexcept {
            SSL_free(tls);
        }
    };
    struct free_crypto final {
        void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
            ngtcp2_crypto_ossl_ctx_del(crypto);
        }
    };

    ruvia::http3_quic_stateless_reset_token stateless_reset_token_{};
    std::unique_ptr<SSL, free_tls> tls_;
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref reference_{};
    std::optional<ruvia::http3_quic_connection> connection_;
};

void drain_events(ruvia::http3_quic_connection& connection, std::list<ruvia::http3_event>& events) {
    while (auto event = connection.next_event()) {
        events.push_back(std::move(*event));
    }
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

// A client and a server connection joined by an in-memory datagram relay on
// a manually advanced clock: no sockets, no wall time, no loss.
class in_memory_h3_pair final {
public:
    explicit in_memory_h3_pair(ruvia::test::tls_identity& identity)
        : client_address_(loopback_address(50000)),
          server_address_(loopback_address(443)),
          client_path_{
              .local = {reinterpret_cast<ngtcp2_sockaddr*>(&client_address_), sizeof(client_address_)},
              .remote = {reinterpret_cast<ngtcp2_sockaddr*>(&server_address_), sizeof(server_address_)},
              .user_data = nullptr,
          },
          server_path_{
              .local = {reinterpret_cast<ngtcp2_sockaddr*>(&server_address_), sizeof(server_address_)},
              .remote = {reinterpret_cast<ngtcp2_sockaddr*>(&client_address_), sizeof(client_address_)},
              .user_data = nullptr,
          },
          client_initial_destination_(ruvia::test::http3_test_random_connection_id(18)),
          client_tls_(identity.ca_file_, "localhost") {
        client_.emplace(ruvia::http3_quic_connection::client({
            .crypto_callbacks_ = ruvia::test::http3_test_crypto_callbacks(ruvia::http3_role::client),
            .owner_callbacks_ = {.get_new_connection_id_ = ruvia::test::http3_test_new_connection_id},
            .source_connection_id_ = ruvia::test::http3_test_random_connection_id(16),
            .destination_connection_id_ = client_initial_destination_,
            .initial_path_ = client_path_,
            .initial_timestamp_ = now_,
        }));
        client_tls_.bind(*client_);

        ruvia::http3_quic_datagram initial;
        if (client_->next_datagram(1350, now_, initial).status_ != ruvia::http3_quic_status::accepted) {
            throw std::runtime_error("client wrote no Initial");
        }
        drain_events(*client_, client_events_);
        server_.emplace(identity.context_.native_handle(), server_path_, initial.bytes_, now_);
        last_server_receive_ = server().receive_datagram(server_path_, initial.ecn_, initial.bytes_, now_).status_;
        drain_events(server(), server_events_);
    }

    in_memory_h3_pair(const in_memory_h3_pair&) = delete;
    in_memory_h3_pair& operator=(const in_memory_h3_pair&) = delete;

    // Relays datagrams both ways until neither side writes one, advancing
    // the clock by one millisecond per round.
    void exchange() {
        for (int round = 0; round < 256; ++round) {
            now_ += NGTCP2_MILLISECONDS;
            const auto moved = relay(*client_, client_events_, server(), server_path_, server_events_, last_server_receive_) +
                               relay(server(), server_events_, *client_, client_path_, client_events_, last_client_receive_);
            if (moved == 0) {
                return;
            }
        }
        throw std::runtime_error("in-memory QUIC exchange did not settle");
    }

    [[nodiscard]] ruvia::http3_quic_connection& client() noexcept {
        return *client_;
    }
    [[nodiscard]] ruvia::http3_quic_connection& server() noexcept {
        return server_->connection();
    }
    [[nodiscard]] quic_server_session& server_session() noexcept {
        return *server_;
    }

    sockaddr_in client_address_;
    sockaddr_in server_address_;
    ngtcp2_path client_path_;
    ngtcp2_path server_path_;
    ngtcp2_tstamp now_{NGTCP2_SECONDS};
    ngtcp2_cid client_initial_destination_;
    ruvia::test::http3_test_client_tls client_tls_;
    std::optional<ruvia::http3_quic_connection> client_;
    std::optional<quic_server_session> server_;
    std::list<ruvia::http3_event> client_events_{};
    std::list<ruvia::http3_event> server_events_{};
    ruvia::http3_quic_status last_client_receive_{ruvia::http3_quic_status::accepted};
    ruvia::http3_quic_status last_server_receive_{ruvia::http3_quic_status::accepted};

private:
    std::size_t relay(ruvia::http3_quic_connection& from, std::list<ruvia::http3_event>& from_events,
        ruvia::http3_quic_connection& to, const ngtcp2_path& to_path, std::list<ruvia::http3_event>& to_events,
        ruvia::http3_quic_status& last_receive) {
        std::size_t moved = 0;
        ruvia::http3_quic_datagram datagram;
        for (;;) {
            const auto written = from.next_datagram(1350, now_, datagram);
            drain_events(from, from_events);
            if (written.status_ != ruvia::http3_quic_status::accepted || datagram.bytes_.empty()) {
                return moved;
            }
            ++moved;
            last_receive = to.receive_datagram(to_path, datagram.ecn_, datagram.bytes_, now_).status_;
            drain_events(to, to_events);
        }
    }
};

// Removes and returns the validated request head the server published for
// `stream_id`.
std::optional<ruvia::http3_request_head> take_request_head(
    std::list<ruvia::http3_event>& events, std::int64_t stream_id) {
    for (auto event = events.begin(); event != events.end(); ++event) {
        if (auto* head = event->request_head(); head != nullptr && head->stream_id() == stream_id) {
            std::optional<ruvia::http3_request_head> taken(std::move(*head).take_head());
            events.erase(event);
            return taken;
        }
    }
    return std::nullopt;
}

std::optional<ruvia::http3_response_head_event> take_response_head(
    std::list<ruvia::http3_event>& events, std::int64_t stream_id) {
    for (auto event = events.begin(); event != events.end(); ++event) {
        if (auto* head = event->response_head(); head != nullptr && head->stream_id() == stream_id) {
            std::optional<ruvia::http3_response_head_event> taken(std::move(*head));
            events.erase(event);
            return taken;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> field(const ruvia::http3_response_head_event& event, std::string_view name) {
    for (const auto& entry : event.head().headers()) {
        if (entry.name() == name) {
            return entry.value();
        }
    }
    return std::nullopt;
}

// Takes the DATA of `stream_id` and returns its receive credit at once.
std::string take_body(std::list<ruvia::http3_event>& events, std::int64_t stream_id) {
    std::string body{};
    for (auto& event : events) {
        if (auto* data = event.data(); data != nullptr && data->stream_id() == stream_id) {
            body.append(data->bytes());
            (void)data->take_credit();
        }
    }
    return body;
}

bool stream_ended(const std::list<ruvia::http3_event>& events, std::int64_t stream_id) {
    return std::any_of(events.begin(), events.end(), [stream_id](const ruvia::http3_event& event) {
        const auto* end = event.message_end();
        return end != nullptr && end->stream_id() == stream_id;
    });
}

std::vector<std::optional<std::int64_t>> goaway_ids(const std::list<ruvia::http3_event>& events) {
    std::vector<std::optional<std::int64_t>> ids{};
    for (const auto& event : events) {
        if (const auto* goaway = event.goaway()) {
            ids.push_back(goaway->id());
        }
    }
    return ids;
}

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Submits a buffered response through the server mapping: head, the whole
// body as DATA within the output budget, then the end of the stream.
bool submit_buffered_response(ruvia::http3_connection& codec, std::int64_t stream_id,
    ruvia::http_known_method method, const ruvia::http_response& response) {
    const auto plan = ruvia::plan_buffered_http_response_write(method, response);
    const auto head = codec.submit_response_head(stream_id, response, plan);
    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head)) {
        return false;
    }
    if (plan.send_body()) {
        const auto submitted = codec.submit_data(stream_id, as_bytes(response.body()));
        if (submitted.status_ != ruvia::http3_data_submit_status::complete) {
            return false;
        }
        return codec.finish_stream(stream_id) == ruvia::http3_finish_status::accepted;
    }
    return true;
}

struct served_get final {
    bool request_parsed_{false};
    bool response_submitted_{false};
    std::optional<ruvia::http3_response_head_event> response_head_;
    std::string response_body_{};
    bool response_ended_{false};
};

// Client GET on a fresh request stream, answered by the server through
// http3_request_head and the buffered response mapping.
served_get serve_get(in_memory_h3_pair& pair, std::int64_t stream_id, std::string_view path, std::string_view body) {
    served_get result;
    const ruvia::http3_regular_request_head_view request_head{
        .authority_ = "localhost", .target_ = path};
    if (pair.client().http3().submit_request_head(stream_id, request_head) != ruvia::http3_submit_status::accepted) {
        return result;
    }
    pair.exchange();

    const auto head = take_request_head(pair.server_events_, stream_id);
    if (!head) {
        return result;
    }
    const auto request = head->build_request();
    result.request_parsed_ = head->method() == "GET" && head->path() == path && head->authority() == "localhost" &&
                             head->stream_ended() && request.protocol_version() == ruvia::http_protocol_version::http3 &&
                             request.path() == path;

    ruvia::http_response response;
    response.header("x-served-by", "ruvia-http3");
    response.body(body);
    result.response_submitted_ = submit_buffered_response(pair.server().http3(), stream_id, ruvia::http_known_method::get, response);
    pair.exchange();

    if (auto head = take_response_head(pair.client_events_, stream_id)) {
        result.response_head_.emplace(std::move(*head));
    }
    result.response_body_ = take_body(pair.client_events_, stream_id);
    result.response_ended_ = stream_ended(pair.client_events_, stream_id);
    return result;
}

}  // namespace

TEST(Ruvia, http3_ngtcp2_openssl_client_emits_a_real_quic_initial_packet) {
    using ssl_context_ptr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    using ssl_ptr = std::unique_ptr<SSL, decltype(&SSL_free)>;

    ssl_context_ptr context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    EXPECT_TRUE(context != nullptr);
    if (!context) {
        return;
    }
    ssl_ptr tls(SSL_new(context.get()), SSL_free);
    EXPECT_TRUE(tls != nullptr);
    if (!tls) {
        return;
    }
    SSL_set_connect_state(tls.get());
    constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
    EXPECT_TRUE(SSL_set_alpn_protos(tls.get(), h3_alpn.data(),
                    static_cast<unsigned int>(h3_alpn.size())) == 0);

    ngtcp2_crypto_conn_ref connection_reference{
        .get_conn = get_connection,
        .user_data = nullptr,
    };
    SSL_set_app_data(tls.get(), &connection_reference);
    EXPECT_TRUE(ngtcp2_crypto_ossl_configure_client_session(tls.get()) == 0);
    ngtcp2_crypto_ossl_ctx* crypto_raw = nullptr;
    EXPECT_TRUE(ngtcp2_crypto_ossl_ctx_new(&crypto_raw, tls.get()) == 0);
    std::unique_ptr<ngtcp2_crypto_ossl_ctx,
        decltype(&ngtcp2_crypto_ossl_ctx_del)>
        crypto(crypto_raw, ngtcp2_crypto_ossl_ctx_del);
    if (!crypto) {
        return;
    }

    auto local = loopback_address(44300);
    auto remote = loopback_address(443);
    const ngtcp2_path path{
        .local = {reinterpret_cast<ngtcp2_sockaddr*>(&local), sizeof(local)},
        .remote = {reinterpret_cast<ngtcp2_sockaddr*>(&remote), sizeof(remote)},
        .user_data = nullptr,
    };

    constexpr std::array<std::uint8_t, 8> source_id{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    constexpr std::array<std::uint8_t, 8> destination_id{
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
    ngtcp2_cid source_connection_id;
    ngtcp2_cid destination_connection_id;
    ngtcp2_cid_init(&source_connection_id, source_id.data(), source_id.size());
    ngtcp2_cid_init(&destination_connection_id, destination_id.data(),
        destination_id.size());

    constexpr ngtcp2_tstamp timestamp = NGTCP2_SECONDS;
    auto connection = ruvia::http3_quic_connection::client({
        .crypto_callbacks_ = ruvia::test::http3_test_crypto_callbacks(ruvia::http3_role::client),
        .owner_callbacks_ = {.get_new_connection_id_ = ruvia::test::http3_test_new_connection_id},
        .source_connection_id_ = source_connection_id,
        .destination_connection_id_ = destination_connection_id,
        .initial_path_ = path,
        .initial_timestamp_ = timestamp,
    });
    connection_reference.user_data = &connection;
    connection.set_tls_native_handle(crypto.get());

    ruvia::http3_quic_datagram datagram;
    const auto result = connection.next_datagram(1350, timestamp, datagram);
    EXPECT_TRUE(result.status_ == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(datagram.bytes_.size() >= 1200);
    EXPECT_TRUE(datagram.path_.path.remote.addrlen == sizeof(remote));
    EXPECT_TRUE(datagram.path_.path.remote.addr ==
                reinterpret_cast<ngtcp2_sockaddr*>(
                    &datagram.path_.remote_addrbuf));
    EXPECT_TRUE(connection.expiry() > timestamp);

    SSL_set_app_data(tls.get(), nullptr);
}

TEST(Ruvia, http3_quic_server_accepts_a_client_initial_and_serves_a_request_in_memory) {
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    in_memory_h3_pair pair(identity);
    pair.exchange();

    EXPECT_TRUE(pair.last_server_receive_ == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(pair.last_client_receive_ == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(pair.client().handshake_completed());
    EXPECT_TRUE(pair.server().handshake_completed());
    EXPECT_TRUE(pair.client_tls_.verify_result() == X509_V_OK);
    EXPECT_TRUE(pair.client_tls_.h3_selected());
    EXPECT_TRUE(pair.server_session().h3_selected());

    // The server's transport parameters, as the client received them.
    const auto* parameters = ngtcp2_conn_get_remote_transport_params2(pair.client().native_handle());
    EXPECT_TRUE(parameters != nullptr);
    if (parameters != nullptr) {
        EXPECT_TRUE(parameters->disable_active_migration == 1);
        EXPECT_TRUE(parameters->original_dcid_present == 1);
        EXPECT_TRUE(ngtcp2_cid_eq(&parameters->original_dcid, &pair.client_initial_destination_) != 0);
        EXPECT_TRUE(parameters->stateless_reset_token_present == 1);
        const auto& token = pair.server_session().stateless_reset_token();
        EXPECT_TRUE(std::equal(token.begin(), token.end(), std::begin(parameters->stateless_reset_token)));
    }

    EXPECT_TRUE(pair.client().start_http3() == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(pair.server().start_http3() == ruvia::http3_quic_status::accepted);
    pair.exchange();
    const auto stream_id = pair.client().open_request_stream();
    EXPECT_TRUE(stream_id == std::int64_t{0});
    if (!stream_id) {
        return;
    }

    constexpr std::string_view body = "hello over http/3";
    auto served = serve_get(pair, *stream_id, "/hello", body);
    EXPECT_TRUE(served.request_parsed_);
    EXPECT_TRUE(served.response_submitted_);
    EXPECT_TRUE(served.response_head_.has_value());
    if (served.response_head_) {
        EXPECT_TRUE(served.response_head_->head().status() == ruvia::http_status::ok);
        EXPECT_TRUE(field(*served.response_head_, "content-length") == std::string(std::to_string(body.size())));
        EXPECT_TRUE(field(*served.response_head_, "x-served-by") == std::string_view("ruvia-http3"));
        EXPECT_TRUE(field(*served.response_head_, "date").has_value());
    }
    EXPECT_TRUE(served.response_body_ == body);
    EXPECT_TRUE(served.response_ended_);
    EXPECT_TRUE(!pair.client().closed());
    EXPECT_TRUE(!pair.server().closed());
}

TEST(Ruvia, http3_quic_server_drains_with_goaway_then_closes_the_connection_gracefully) {
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    in_memory_h3_pair pair(identity);
    pair.exchange();
    EXPECT_TRUE(pair.client().start_http3() == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(pair.server().start_http3() == ruvia::http3_quic_status::accepted);
    pair.exchange();
    const auto stream_id = pair.client().open_request_stream();
    EXPECT_TRUE(stream_id.has_value());
    if (!stream_id) {
        return;
    }
    const auto served = serve_get(pair, *stream_id, "/before-drain", "admitted");
    EXPECT_TRUE(served.response_ended_);
    EXPECT_TRUE(served.response_body_ == "admitted");

    // RFC 9114 §5.2: a shutdown notice first, the final GOAWAY a round trip
    // later naming the first request stream that was not processed.
    EXPECT_TRUE(pair.server().http3().begin_drain() == ruvia::http3_submit_status::accepted);
    pair.exchange();
    EXPECT_TRUE(goaway_ids(pair.client_events_) == (std::vector<std::optional<std::int64_t>>{std::nullopt}));
    EXPECT_TRUE(pair.server().http3().finish_drain() == ruvia::http3_submit_status::accepted);
    pair.exchange();
    EXPECT_TRUE(pair.server().http3().drain_final());
    EXPECT_TRUE(goaway_ids(pair.client_events_) ==
                (std::vector<std::optional<std::int64_t>>{std::nullopt, *stream_id + 4}));

    pair.server().close(ruvia::http3_error_code::no_error);
    ruvia::http3_quic_datagram close_datagram;
    const auto written = pair.server().next_datagram(1350, pair.now_, close_datagram);
    EXPECT_TRUE(written.status_ == ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(!close_datagram.bytes_.empty());
    EXPECT_TRUE(pair.server().closed());
    drain_events(pair.server(), pair.server_events_);

    const auto received = pair.client().receive_datagram(pair.client_path_, close_datagram.ecn_, close_datagram.bytes_, pair.now_);
    drain_events(pair.client(), pair.client_events_);
    EXPECT_TRUE(received.status_ == ruvia::http3_quic_status::closed);
    EXPECT_EQ(received.application_error_code_, std::optional(ruvia::http3_error_code::no_error));
    EXPECT_FALSE(received.transport_error_code_.has_value());
    EXPECT_TRUE(pair.client().closed());
    // The peer's CONNECTION_CLOSE carried H3_NO_ERROR (RFC 9114 §5.2).
    EXPECT_EQ(pair.client().application_error(), std::optional(ruvia::http3_error_code::no_error));
    EXPECT_FALSE(pair.client().transport_error().has_value());
    EXPECT_EQ(pair.server().application_error(), std::optional(ruvia::http3_error_code::no_error));

    // The CONNECTION_CLOSE is written once; the closing period sends nothing.
    ruvia::http3_quic_datagram after_close;
    EXPECT_TRUE(pair.server().next_datagram(1350, pair.now_, after_close).status_ == ruvia::http3_quic_status::closed);
    EXPECT_TRUE(after_close.bytes_.empty());
}

template <typename address_type>
[[nodiscard]] bool same_address(std::span<const std::byte> bytes, const address_type& address) {
    return std::ranges::equal(bytes, std::as_bytes(std::span(&address, 1)));
}

// RFC 9000 §7.4.1: a client remembers the server's transport parameters with
// its session ticket only once they are authenticated, and a later
// connection restores them for 0-RTT. current_path() reports the addresses
// the connection sends on (RFC 9000 §9).
TEST(Ruvia, http3_quic_client_encodes_early_transport_parameters_after_the_handshake) {
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    in_memory_h3_pair pair(identity);
    std::array<std::byte, 512> parameters{};
    EXPECT_FALSE(pair.client().handshake_completed());
    EXPECT_FALSE(pair.client().encode_early_transport_parameters(parameters).has_value());

    const auto client_path = pair.client().current_path();
    EXPECT_TRUE(same_address(client_path.local_, pair.client_address_));
    EXPECT_TRUE(same_address(client_path.remote_, pair.server_address_));
    const auto server_path = pair.server().current_path();
    EXPECT_TRUE(same_address(server_path.local_, pair.server_address_));
    EXPECT_TRUE(same_address(server_path.remote_, pair.client_address_));

    pair.exchange();
    ASSERT_TRUE(pair.client().handshake_completed());
    EXPECT_FALSE(pair.client().early_data_rejected());
    ASSERT_EQ(pair.client().start_http3(), ruvia::http3_quic_status::accepted);
    ASSERT_EQ(pair.server().start_http3(), ruvia::http3_quic_status::accepted);
    pair.exchange();
    const auto settings = pair.client().http3().peer_settings();
    ASSERT_TRUE(settings.has_value());

    const auto size = pair.client().encode_early_transport_parameters(parameters);
    ASSERT_TRUE(size.has_value());
    ASSERT_GT(*size, std::size_t{0});
    // A buffer one byte short holds nothing.
    EXPECT_FALSE(pair.client().encode_early_transport_parameters(std::span(parameters.data(), *size - 1)).has_value());
    const auto again = pair.client().encode_early_transport_parameters(parameters);
    EXPECT_EQ(again, size);
    EXPECT_TRUE(same_address(pair.client().current_path().local_, pair.client_address_));

    // A new connection to the same server restores them before 0-RTT.
    auto resumed = ruvia::http3_quic_connection::client({
        .crypto_callbacks_ = ruvia::test::http3_test_crypto_callbacks(ruvia::http3_role::client),
        .owner_callbacks_ = {.get_new_connection_id_ = ruvia::test::http3_test_new_connection_id},
        .source_connection_id_ = ruvia::test::http3_test_random_connection_id(16),
        .destination_connection_id_ = ruvia::test::http3_test_random_connection_id(18),
        .initial_path_ = pair.client_path_,
        .initial_timestamp_ = pair.now_,
    });
    EXPECT_EQ(resumed.start_early_http3(std::span<const std::byte>(parameters.data(), *size), *settings),
        ruvia::http3_quic_status::accepted);
    EXPECT_TRUE(resumed.http3_started());
    EXPECT_FALSE(resumed.early_data_rejected());
}

namespace {

// A one-connection HTTP/3 server on a real loopback UDP socket, built from the
// same server path as the in-memory tests and run on its own thread. Routes:
// GET /hello, POST /echo (the request content back) and GET /shutdown, which
// answers and then drains (GOAWAY notice, final GOAWAY a round trip later)
// and closes with H3_NO_ERROR once every admitted stream has closed. The
// thread ends when its connection closes, on stop, or after a bounded
// lifetime.
class udp_h3_test_server final {
public:
    udp_h3_test_server(SSL_CTX* context, asio::ip::udp::endpoint endpoint)
        : context_(context),
          endpoint_(endpoint),
          socket_(io_) {
        // Bound before the client can send, so its Initial is never refused.
        socket_.open(endpoint_.protocol());
#if !defined(_WIN32)
        socket_.set_option(asio::socket_base::reuse_address(true));
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, asio::system_category(), "cannot share the reserved UDP port");
        }
#endif
        socket_.bind(endpoint_);
        socket_.non_blocking(true);
        thread_ = std::thread([this] {
            try {
                serve();
            } catch (const std::exception& error) {
                failure_ = error.what();
            }
        });
    }

    ~udp_h3_test_server() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    udp_h3_test_server(const udp_h3_test_server&) = delete;
    udp_h3_test_server& operator=(const udp_h3_test_server&) = delete;

    // Waits for the server thread; returns its failure, empty when none.
    [[nodiscard]] std::string join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        return std::string(failure_);
    }

private:
    struct served_stream final {
        std::optional<ruvia::http3_request_head> head_;
        std::string request_body_{};
        std::string response_body_{};
        std::size_t response_offset_{0};
        bool responding_{false};
        bool finished_{false};
    };

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(endpoint_.data()), static_cast<ngtcp2_socklen>(endpoint_.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(peer_.data()), static_cast<ngtcp2_socklen>(peer_.size())},
            .user_data = nullptr,
        };
    }

    void serve() {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!stop_ && std::chrono::steady_clock::now() < deadline) {
            auto wake = std::chrono::steady_clock::now() + 20ms;
            if (session_) {
                const auto expiry = session_->connection().expiry();
                if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
                    wake = (std::min)(wake, std::chrono::steady_clock::time_point(
                                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                    std::chrono::nanoseconds(static_cast<std::int64_t>(expiry)))));
                }
            }
            socket_.async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
            io_.restart();
            io_.run_until(wake);
            socket_.cancel();
            io_.restart();
            io_.run();

            const bool received = receive_available();
            if (!session_) {
                continue;
            }
            auto& connection = session_->connection();
            if (!connection.closed() && timestamp() >= connection.expiry()) {
                static_cast<void>(connection.handle_expiry(timestamp()));
                handle_events();
            }
            if (!connection.closed()) {
                continue_responses();
                advance_drain(received);
            }
            flush();
            if (connection.closed()) {
                return;
            }
        }
    }

    bool receive_available() {
        bool received = false;
        for (;;) {
            asio::ip::udp::endpoint sender;
            std::error_code error;
            const auto size = socket_.receive_from(asio::buffer(input_), sender, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return received;
            }
            if (error) {
                // ICMP errors for a previous send surface here on some
                // platforms; they carry no datagram.
                continue;
            }
            const std::span<const std::byte> bytes(input_.data(), size);
            if (!session_) {
                peer_ = sender;
                try {
                    session_.emplace(context_, path(), bytes, timestamp());
                } catch (const std::invalid_argument&) {
                    continue;
                }
            } else if (sender != peer_) {
                continue;
            }
            if (session_->connection().closed()) {
                continue;
            }
            received = true;
            static_cast<void>(session_->connection().receive_datagram(path(), NGTCP2_ECN_NOT_ECT, bytes, timestamp()));
            handle_events();
        }
    }

    void flush() {
        auto& connection = session_->connection();
        for (;;) {
            const auto result = connection.next_datagram(1350, timestamp(), output_);
            handle_events();
            if (result.status_ != ruvia::http3_quic_status::accepted || output_.bytes_.empty()) {
                if (drain_notice_pending_flush_) {
                    drain_notice_pending_flush_ = false;
                    drain_notice_sent_ = true;
                }
                return;
            }
            std::error_code error;
            socket_.send_to(asio::buffer(output_.bytes_.data(), output_.bytes_.size()), peer_, 0, error);
        }
    }

    void handle_events() {
        auto& connection = session_->connection();
        // A single receive batch can complete the handshake and deliver the
        // first request. Bind QPACK before handling its response, not after
        // receive_available() has drained the socket.
        if (!connection.closed() && !connection.http3_started() && connection.handshake_completed()) {
            if (connection.start_http3() != ruvia::http3_quic_status::accepted) {
                throw std::runtime_error("server cannot start HTTP/3");
            }
        }
        // The connection itself resets a malformed request (RFC 9114
        // §4.1.2); only validated heads arrive here.
        while (auto event = connection.next_event()) {
            if (auto* request = event->request_head()) {
                const auto stream_id = request->stream_id();
                auto& stream = streams_[stream_id];
                if (stream.head_) {
                    continue;
                }
                stream.head_.emplace(std::move(*request).take_head());
                if (stream.head_->stream_ended()) {
                    respond(stream_id, stream);
                }
            } else if (const auto* data = event->data()) {
                // The event's credit returns as it is destroyed.
                if (const auto found = streams_.find(data->stream_id()); found != streams_.end()) {
                    found->second.request_body_.append(data->bytes());
                }
            } else if (const auto* end = event->message_end()) {
                if (const auto found = streams_.find(end->stream_id()); found != streams_.end() && !found->second.responding_) {
                    respond(end->stream_id(), found->second);
                }
            } else if (const auto* closed = event->stream_close()) {
                streams_.erase(closed->stream_id());
            }
        }
    }

    void respond(std::int64_t stream_id, served_stream& stream) {
        const auto method = stream.head_->method();
        const auto target = stream.head_->path();
        ruvia::http_response response;
        response.header("x-request-method", method);
        if (method == "GET" && target == "/hello") {
            stream.response_body_ = "hello over http/3";
        } else if (method == "POST" && target == "/echo") {
            stream.response_body_ = std::move(stream.request_body_);
        } else if (method == "GET" && target == "/shutdown") {
            stream.response_body_ = "draining";
            drain_requested_ = true;
        } else {
            response.status(ruvia::http_status::not_found);
        }
        response.body(stream.response_body_);
        const auto known_method = method == "POST" ? ruvia::http_known_method::post : ruvia::http_known_method::get;
        const auto plan = ruvia::plan_buffered_http_response_write(known_method, response);
        const auto head = session_->connection().http3().submit_response_head(stream_id, response, plan);
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head)) {
            throw std::runtime_error("server cannot submit a response head");
        }
        stream.responding_ = true;
        stream.finished_ = !plan.send_body();
    }

    // Submits response content as the per-stream output budget frees up.
    void continue_responses() {
        auto& codec = session_->connection().http3();
        for (auto& [stream_id, stream] : streams_) {
            if (!stream.responding_ || stream.finished_) {
                continue;
            }
            while (stream.response_offset_ < stream.response_body_.size()) {
                const auto submitted = codec.submit_data(
                    stream_id, as_bytes(std::string_view(stream.response_body_).substr(stream.response_offset_)));
                stream.response_offset_ += submitted.consumed_bytes_;
                if (submitted.status_ != ruvia::http3_data_submit_status::complete) {
                    break;
                }
            }
            if (stream.response_offset_ == stream.response_body_.size()) {
                if (codec.finish_stream(stream_id) != ruvia::http3_finish_status::accepted) {
                    throw std::runtime_error("server cannot finish a response");
                }
                stream.finished_ = true;
            }
        }
    }

    void advance_drain(bool received) {
        auto& connection = session_->connection();
        auto& codec = connection.http3();
        if (drain_requested_ && !drain_begun_) {
            if (codec.begin_drain() != ruvia::http3_submit_status::accepted) {
                throw std::runtime_error("server cannot begin draining");
            }
            drain_begun_ = true;
            drain_notice_pending_flush_ = true;
        } else if (drain_notice_sent_ && received && !codec.drain_final()) {
            // The peer has answered since the notice left: a round trip.
            if (codec.finish_drain() != ruvia::http3_submit_status::accepted) {
                throw std::runtime_error("server cannot finish draining");
            }
        } else if (codec.drain_final() && codec.drained()) {
            connection.close(ruvia::http3_error_code::no_error);
        }
    }

    SSL_CTX* context_;
    asio::ip::udp::endpoint endpoint_;
    asio::ip::udp::endpoint peer_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    std::vector<std::byte> input_ = std::vector<std::byte>(64 * 1024);
    ruvia::http3_quic_datagram output_;
    std::optional<quic_server_session> session_;
    std::map<std::int64_t, served_stream> streams_{};
    bool drain_requested_{false};
    bool drain_begun_{false};
    bool drain_notice_pending_flush_{false};
    bool drain_notice_sent_{false};
    std::atomic<bool> stop_{false};
    std::string failure_{};
    // Started last: every member above is ready before the thread runs.
    std::thread thread_;
};

}  // namespace

TEST(Ruvia, http3_test_client_exchanges_requests_with_a_udp_loopback_server) {
    asio::io_context io;
    ruvia::test::udp_port_reservation reservation(io);
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    udp_h3_test_server server(identity.context_.native_handle(), reservation.endpoint());

    {
        ruvia::test::http3_test_client client(reservation.endpoint(), identity.ca_file_, "localhost", 5s);
        const auto* parameters = ngtcp2_conn_get_remote_transport_params2(client.native_handle());
        EXPECT_TRUE(parameters != nullptr && parameters->disable_active_migration == 1);

        const auto hello = client.request("GET", "/hello");
        EXPECT_TRUE(hello.status_ == 200);
        EXPECT_TRUE(hello.body_ == "hello over http/3");
        EXPECT_TRUE(hello.header("Content-Length") == std::string("17"));
        EXPECT_TRUE(hello.header("x-request-method") == std::string("GET"));
        EXPECT_TRUE(!hello.reset_error_.has_value());

        // Larger than both the client's and the server's per-stream output
        // budget (256 KiB) and the initial stream window (1 MiB): the upload
        // and the echo only complete if DATA credit is returned as consumed.
        std::string upload(1536 * 1024, '\0');
        for (std::size_t index = 0; index < upload.size(); ++index) {
            upload[index] = static_cast<char>('a' + index % 23);
        }
        const auto echo = client.request("POST", "/echo", {{"content-type", "application/octet-stream"}}, upload);
        EXPECT_TRUE(echo.status_ == 200);
        EXPECT_TRUE(echo.header("content-length") == std::string(std::to_string(upload.size())));
        EXPECT_TRUE(echo.body_ == upload);

        // Incremental reading: chunks taken one at a time reassemble the body.
        const std::string streamed_upload(96 * 1024, 's');
        const auto stream_id = client.start_request(
            "POST", "/echo", {{"content-type", "application/octet-stream"}}, streamed_upload);
        EXPECT_TRUE(client.wait_for_response_head(stream_id).status_ == 200);
        std::string streamed{};
        while (auto chunk = client.read_body_chunk(stream_id, 5s)) {
            EXPECT_TRUE(!chunk->empty());
            streamed += *chunk;
        }
        EXPECT_TRUE(streamed == streamed_upload);
        const auto streamed_response = client.finish_response(stream_id);
        EXPECT_TRUE(streamed_response.status_ == 200);
        EXPECT_TRUE(streamed_response.body_.empty());

        const auto missing = client.request("GET", "/missing");
        EXPECT_TRUE(missing.status_ == 404);

        const auto shutdown = client.request("GET", "/shutdown");
        EXPECT_TRUE(shutdown.status_ == 200);
        EXPECT_TRUE(shutdown.body_ == "draining");
        EXPECT_TRUE(client.wait_until([&] { return client.closed(); }, 5s));
        // Streams 0, 4, 8, 12 and 16 were processed; 20 is the first that was not.
        EXPECT_TRUE(client.goaway_ids() == (std::vector<std::optional<std::int64_t>>{std::nullopt, 20}));
        EXPECT_EQ(client.application_error(), std::optional(ruvia::http3_error_code::no_error));
        EXPECT_FALSE(client.transport_error().has_value());
    }
    EXPECT_TRUE(server.join().empty());
}

TEST(Ruvia, http3_test_client_rejects_a_certificate_for_another_server_name) {
    asio::io_context io;
    ruvia::test::udp_port_reservation reservation(io);
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    udp_h3_test_server server(identity.context_.native_handle(), reservation.endpoint());

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        ruvia::test::http3_test_client client(reservation.endpoint(), identity.ca_file_, "other.example", 5s);
    }));
    // The client's CONNECTION_CLOSE ends the server's only connection.
    EXPECT_TRUE(server.join().empty());
}

// The QUIC owner alone decides where DATA credit returns: its receive windows.
TEST(Ruvia, http3_quic_connection_rejects_a_caller_credit_sink) {
    constexpr ruvia::http3_received_data_credit_sink_type sink = [](void*, std::int64_t, std::size_t) noexcept {};
    EXPECT_THROW((void)ruvia::http3_quic_connection::client({.http3_ = {.received_data_credit_sink_ = sink}}),
        std::invalid_argument);
}

TEST(Ruvia, http3_test_client_fails_within_its_timeout_without_a_server) {
    asio::io_context io;
    ruvia::test::udp_port_reservation reservation(io);
    ruvia::test::tls_identity identity("localhost");
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        ruvia::test::http3_test_client client(reservation.endpoint(), identity.ca_file_, "localhost", 1s);
    }));
    EXPECT_TRUE(std::chrono::steady_clock::now() - started < 5s);
}

namespace http3_resumption_test {

using namespace std::chrono_literals;

std::atomic<std::size_t> unsafe_requests{0};
std::atomic<std::size_t> early_retries{0};

class routes final {
public:
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/resumption/safe", safe);
    RUVIA_POST("/resumption/unsafe", unsafe);
    RUVIA_GET("/resumption/worker", worker);
    RUVIA_GET("/resumption/retry", retry);
    RUVIA_GET("/resumption/body", body);
    RUVIA_GET("/resumption/advertise", advertise);
    RUVIA_GET("/resumption/reprioritize", reprioritize);
    RUVIA_GET("/resumption/route", route);
    RUVIA_ROUTES_END

    // What the origin saw of an outbound request route: TLS server name,
    // :authority, the QUIC connection and whether the request was early data.
    ruvia::task<ruvia::http_response> route(ruvia::context& context) {
        const auto* tls = context.conn().tls();
        std::string result(tls != nullptr ? tls->server_name() : std::string_view());
        result.append("|").append(context.req().authority()).append("|");
        result.append(std::to_string(context.conn().connection_id())).append("|");
        result.append(context.early_data() ? "early" : "one-rtt");
        co_return context.text(std::move(result));
    }

    ruvia::task<ruvia::http_response> safe(ruvia::context& context) {
        co_return context.text(context.early_data() ? "early" : "one-rtt");
    }

    ruvia::task<ruvia::http_response> unsafe(ruvia::context& context) {
        ++unsafe_requests;
        co_return context.text(context.early_data() ? "unsafe-early" : "unsafe-one-rtt");
    }

    ruvia::task<ruvia::http_response> retry(ruvia::context& context) {
        if (context.early_data()) {
            ++early_retries;
            context.draft().status(ruvia::http_status::too_early);
            co_return context.text("retry after handshake");
        }
        co_return context.text("retried");
    }

    ruvia::task<ruvia::http_response> body(ruvia::context& context) {
        const auto bytes = co_await context.req().text();
        std::string result(context.early_data() ? "early:" : "one-rtt:");
        result.append(bytes);
        co_return context.text(std::move(result));
    }

    ruvia::task<ruvia::http_response> worker(ruvia::context& context) {
        const auto id = std::string(std::to_string(context.worker().id()));
        co_return context.text(std::string(id.data(), id.size()));
    }

    ruvia::task<ruvia::http_response> advertise(ruvia::context& context) {
        constexpr std::array<std::string_view, 2> origins{
            "https://localhost:8443",
            "https://api.localhost:8443",
        };
        context.advertise_origins(origins);
        context.draft().priority({.urgency_ = ruvia::http_priority_urgency(2), .incremental_ = true});
        const auto pri = context.req().priority();
        std::string text("u=" + std::string(std::to_string(pri.urgency_.value())) + ",i=" + (pri.incremental_ ? "1" : "0"));
        co_return context.text(std::string(text.data(), text.size()));
    }

    ruvia::task<void> reprioritize(ruvia::context& context) {
        auto& stream = context.stream_text();
        if (co_await stream.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        const auto initial = context.req().priority();
        std::string first("u=" + std::string(std::to_string(initial.urgency_.value())) + ",i=" + (initial.incremental_ ? "1" : "0") + "\n");
        co_await stream.write(first);
        for (int poll = 0; poll < 50; ++poll) {
            const auto current = context.req().priority();
            if (current.urgency_.value() == 1 && current.incremental_) {
                break;
            }
            (void)co_await ruvia::sleep_for(context.worker(), 10ms, context.stop_token());
        }
        const auto updated = context.req().priority();
        std::string second("u=" + std::string(std::to_string(updated.urgency_.value())) + ",i=" + (updated.incremental_ ? "1" : "0"));
        co_await stream.write(second);
        co_await stream.end();
    }
};

// One application and two independently keyed listeners. This also makes a
// ticket from the opt-in listener a real rejected-0RTT input on the default one.
class server final {
public:
    server()
        : enabled_tcp_(io_),
          disabled_tcp_(io_),
          retry_tcp_(io_),
          enabled_udp_(io_),
          disabled_udp_(io_),
          retry_udp_(io_),
          base_url_("https://localhost:" + std::string(std::to_string(enabled_udp_.endpoint().port()))),
          retry_base_url_("https://localhost:" + std::string(std::to_string(retry_udp_.endpoint().port()))),
          identity_("localhost", "DNS:localhost,DNS:a.localhost,DNS:b.localhost,DNS:c.localhost"),
          app_(ruvia::app()) {
        const auto& key = identity_.private_key_file();
        app_.server({.worker_count_ = 4})
            .listen({{
                         .address_ = "127.0.0.1",
                         .https_ = enabled_tcp_.endpoint().port(),
                         .http3_ = enabled_udp_.endpoint().port(),
                         .http3_early_data_ = true,
                         .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}},
                     },
                {
                    .address_ = "127.0.0.1",
                    .https_ = disabled_tcp_.endpoint().port(),
                    .http3_ = disabled_udp_.endpoint().port(),
                    .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}},
                },
                {
                    .address_ = "127.0.0.1",
                    .https_ = retry_tcp_.endpoint().port(),
                    .http3_ = retry_udp_.endpoint().port(),
                    .http3_retry_ = true,
                    .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}},
                }})
            .on_start([this] {
                started_.set_value();
            });
        auto ready = started_.get_future();
        run_.emplace(app_);
        ruvia::test::wait_until([&] {
            if (const auto failure = run_->failure()) {
                std::rethrow_exception(failure);
            }
            return ready.wait_for(0s) == std::future_status::ready;
        },
            10s);
        ready.get();
    }

    ~server() = default;

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    [[nodiscard]] ruvia::test::http3_test_client connect(bool enabled = true,
        const ruvia::test::http3_test_resumption* ticket = nullptr, bool early = false) {
        if (const auto failure = run_->failure()) {
            std::rethrow_exception(failure);
        }
        return ruvia::test::http3_test_client(
            enabled ? enabled_udp_.endpoint() : disabled_udp_.endpoint(),
            identity_.ca_file_, "localhost", 5s, ticket, early);
    }

    [[nodiscard]] ruvia::test::http3_test_client connect_retry() {
        if (const auto failure = run_->failure()) {
            std::rethrow_exception(failure);
        }
        return ruvia::test::http3_test_client(
            retry_udp_.endpoint(), identity_.ca_file_, "localhost", 5s);
    }

    [[nodiscard]] asio::ip::udp::endpoint retry_endpoint() const noexcept {
        return retry_udp_.endpoint();
    }

    [[nodiscard]] std::uint16_t enabled_tcp_port() const noexcept {
        return enabled_tcp_.endpoint().port();
    }

    [[nodiscard]] asio::ip::udp::endpoint enabled_endpoint() const noexcept {
        return enabled_udp_.endpoint();
    }

    [[nodiscard]] ruvia::http_client_config retry_outbound_config() const {
        auto config = outbound_config();
        config.base_url_ = retry_base_url_;
        config.http3_.enable_early_data_ = false;
        return config;
    }

    [[nodiscard]] ruvia::http_client_config outbound_config() const {
        ruvia::http_client_config config{.base_url_ = base_url_};
        config.versions_ = ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true};
        config.http3_.enable_early_data_ = true;
        config.idle_timeout_ = 100ms;
        config.tls_.ca_file_ = identity_.ca_file_;
        return config;
    }

    void stop() {
        if (run_.has_value()) {
            app_.stop();
            run_->join();
            run_.reset();
        }
    }

    [[nodiscard]] static server& shared() {
        static server instance;
        return instance;
    }

private:
    asio::io_context io_;
    ruvia::test::tcp_port_reservation enabled_tcp_;
    ruvia::test::tcp_port_reservation disabled_tcp_;
    ruvia::test::tcp_port_reservation retry_tcp_;
    ruvia::test::udp_port_reservation enabled_udp_;
    ruvia::test::udp_port_reservation disabled_udp_;
    ruvia::test::udp_port_reservation retry_udp_;
    std::string base_url_{};
    std::string retry_base_url_{};
    ruvia::test::tls_identity identity_;
    ruvia::application& app_;
    std::promise<void> started_;
    std::optional<ruvia::test::app_run> run_;
};

ruvia::test::http3_test_resumption ticket(bool enabled = true) {
    auto client = server::shared().connect(enabled);
    const auto response = client.request("GET", "/resumption/safe");
    if (response.status_ != 200 || response.body_ != "one-rtt") {
        throw std::runtime_error("HTTP/3 resumption setup did not complete a regular request");
    }
    return client.capture_resumption();
}

ruvia::task<void> exercise_outbound_resumption(ruvia::event_loop loop,
    ruvia::http_client_config config) {
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        auto client = owner.client();
        {
            auto response = co_await client.get("/resumption/safe").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            EXPECT_EQ(text.text(), "one-rtt");
        }
        // Let the pool expire the transport while retaining its TLS ticket.
        (void)co_await ruvia::sleep_for(owner.worker(), 250ms);
        {
            auto response = co_await client.get("/resumption/safe").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            // TLS resumption can succeed while pacing delays HEADERS until 1-RTT.
            // The ordered early-flight tests below verify actual 0-RTT delivery.
            EXPECT_TRUE(text.text() == "early" || text.text() == "one-rtt");
        }
        (void)co_await ruvia::sleep_for(owner.worker(), 250ms);
        {
            const auto before = early_retries.load();
            auto response = co_await client.get("/resumption/retry").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            EXPECT_EQ(text.text(), "retried");
            EXPECT_TRUE(early_retries.load() - before <= 1);
        }
        (void)co_await ruvia::sleep_for(owner.worker(), 250ms);
        {
            auto response = co_await client.get("/resumption/body").body("buffered").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            EXPECT_EQ(text.text(), "one-rtt:buffered");
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// A real UDP relay gates encrypted packets after the public client has received
// an authenticated response. A migration cannot receive a path-validation reply
// while blocked, independent of scheduler and network timing.
class migration_relay final {
public:
    migration_relay(const ruvia::event_loop& loop, asio::ip::udp::endpoint server)
        : socket_(loop.io_context(), {asio::ip::address_v4::loopback(), 0}),
          server_(server),
          done_(loop.worker()) {
        asio::co_spawn(loop.executor(), serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            complete_ = true;
            done_.notify();
        });
    }
    [[nodiscard]] std::uint16_t port() const {
        return socket_.local_endpoint().port();
    }
    void block() noexcept {
        blocked_ = true;
    }
    [[nodiscard]] ruvia::task<void> shutdown() {
        std::error_code ignored;
        socket_.close(ignored);
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> serve() {
        std::array<std::byte, 65536> bytes;
        for (;;) {
            asio::ip::udp::endpoint sender;
            std::error_code error;
            const auto size = co_await socket_.async_receive_from(asio::buffer(bytes), sender,
                asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::error::operation_aborted || error == asio::error::bad_descriptor) {
                co_return;
            }
            if (error) {
                throw std::system_error(error);
            }
            if (blocked_) {
                continue;
            }
            if (sender != server_) {
                client_ = sender;
            }
            co_await socket_.async_send_to(asio::buffer(bytes.data(), size),
                sender == server_ ? client_ : server_, asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::error::operation_aborted || error == asio::error::bad_descriptor) {
                co_return;
            }
            if (error) {
                throw std::system_error(error);
            }
        }
    }
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint server_;
    asio::ip::udp::endpoint client_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool blocked_{};
    bool complete_{};
};

ruvia::task<void> exercise_blocked_migration(ruvia::event_loop_attachment& attachment,
    ruvia::http_client_config config, asio::ip::udp::endpoint remote, int mode) {
    const auto loop = attachment.loop();
    migration_relay relay(loop, remote);
    const std::string base_url("https://localhost:" +
                               std::string(std::to_string(relay.port())));
    config.base_url_ = base_url;
    config.idle_timeout_ = 30s;
    config.connect_timeout_ = mode == 2 ? 100ms : 2s;
    config.http3_.enable_early_data_ = false;
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        auto client = owner.client();
        {
            auto warm = co_await client.get("/resumption/safe").send();
            EXPECT_TRUE(warm.ok());
            (void)co_await warm.bytes();
        }
        relay.block();
        ruvia::stop_source stop;
        ruvia::operation_options options{.timeout_ = mode == 1 ? 20ms : 5s, .stop_token_ = stop.token()};
        std::optional<std::error_code> outcome;
        bool complete = false;
        const auto migrate = [&]() -> ruvia::task<void> {
            try {
                co_await client.with_options(options).migrate("127.0.0.1");
            } catch (const ruvia::http_client_error& error) {
                outcome = error.code();
            }
            complete = true;
        };
        ruvia::task_scope tasks(loop.worker());
        const auto started = std::chrono::steady_clock::now();
        tasks.spawn(migrate());
        (void)co_await ruvia::sleep_for(loop.worker(), 5ms);
        if (mode == 0 || mode == 3 || mode == 4) {
            EXPECT_FALSE(complete);
        }
        if (mode == 0) {
            stop.request_stop();
        } else if (mode == 3) {
            owner.close();
        } else if (mode == 4) {
            attachment.stop();
        }
        co_await tasks.join();
        if (mode == 1 || mode == 2) {
            EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
        }
        EXPECT_TRUE(complete);
        EXPECT_TRUE(outcome.has_value());
        if (outcome) {
            EXPECT_EQ(*outcome, mode == 0                ? std::make_error_code(std::errc::operation_canceled)
                                : mode == 3 || mode == 4 ? ruvia::make_error_code(ruvia::http_client_errc::closing)
                                                         : std::make_error_code(std::errc::timed_out));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    co_await relay.shutdown();
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ruvia::task<void> exercise_outbound_migration(ruvia::event_loop loop,
    ruvia::http_client_config config) {
    config.idle_timeout_ = 30s;
    config.http3_.enable_early_data_ = false;
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        auto client = owner.client();
        std::string worker{};
        for (int change = 0; change <= 16; ++change) {
            if (change != 0) {
                co_await client.migrate("127.0.0.1");
            }
            auto response = co_await client.get("/resumption/worker").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            if (change == 0) {
                worker = text.text();
            } else {
                EXPECT_EQ(text.text(), worker);
            }
            if (change == 0) {
                ruvia::stop_source stop;
                stop.request_stop();
                bool cancelled = false;
                try {
                    co_await client.with_options({.timeout_ = 1ms, .stop_token_ = stop.token()}).migrate("127.0.0.1");
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(cancelled);
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ruvia::task<void> exercise_outbound_features(ruvia::event_loop loop,
    ruvia::http_client_config config, ruvia::http_client_config retry_config) {
    config.idle_timeout_ = 30s;
    config.http3_.enable_early_data_ = false;
    config.advertisements_.receive_origins_ = true;
    ruvia::http_client owner(loop, config);
    ruvia::http_client retry_owner(loop, retry_config);
    std::exception_ptr failure;
    try {
        auto client = owner.client();
        {
            auto response = co_await client.get("/resumption/advertise")
                                .header("priority", "u=4, i")
                                .send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            EXPECT_EQ(text.text(), "u=4,i=1");
            EXPECT_EQ(response.header("priority"), std::optional<std::string_view>("u=2, i"));
            auto advertisement = client.next_advertisement();
            EXPECT_TRUE(advertisement.has_value());
            if (advertisement) {
                EXPECT_TRUE(advertisement->protocol_version() == ruvia::http_protocol_version::http3);
                const auto* origins = advertisement->origins();
                EXPECT_TRUE(origins != nullptr);
                if (origins != nullptr) {
                    EXPECT_EQ(origins->origins_.size(), std::size_t{2});
                    if (origins->origins_.size() == 2) {
                        EXPECT_EQ(origins->origins_[0], "https://localhost:8443");
                        EXPECT_EQ(origins->origins_[1], "https://api.localhost:8443");
                    }
                }
            }
        }
        {
            auto response = co_await client.get("/resumption/reprioritize")
                                .header("priority", "u=6")
                                .send();
            EXPECT_TRUE(response.ok());
            auto first = co_await response.stream().text();
            EXPECT_TRUE(first.has_value() && *first == "u=6,i=0\n");
            response.reprioritize({.urgency_ = ruvia::http_priority_urgency(1), .incremental_ = true});
            std::string remainder{};
            while (auto chunk = co_await response.stream().text()) {
                remainder.append(*chunk);
            }
            EXPECT_EQ(remainder, "u=1,i=1");
        }
        {
            auto retry_client = retry_owner.client();
            auto response = co_await retry_client.get("/resumption/safe").send();
            auto text = co_await response.text();
            EXPECT_TRUE(response.ok());
            EXPECT_EQ(text.text(), "one-rtt");
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await retry_owner.shutdown();
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ruvia::task<void> run_resumption_lane(ruvia::http_client_ref c,
    ruvia::http_client_ref rc, int lane) {
    for (int round = 0; round < 4; ++round) {
        auto r1 = co_await c.get("/resumption/retry").send();
        EXPECT_TRUE(r1.ok());
        auto t1 = co_await r1.text();
        EXPECT_EQ(t1.text(), "retried");

        const std::string payload("payload-" + std::string(std::to_string(lane)) + "-" + std::string(std::to_string(round)));
        auto r2 = co_await c.get("/resumption/body").body(payload).send();
        EXPECT_TRUE(r2.ok());
        auto t2 = co_await r2.text();
        EXPECT_TRUE(t2.text() == std::string("early:") + payload || t2.text() == std::string("one-rtt:") + payload);

        auto r3 = co_await rc.get("/resumption/safe").send();
        EXPECT_TRUE(r3.ok());
        auto t3 = co_await r3.text();
        EXPECT_EQ(t3.text(), "one-rtt");
    }
}

ruvia::task<void> exercise_concurrent_early_data_and_retry_worker(ruvia::event_loop loop,
    ruvia::http_client_config config, ruvia::http_client_config retry_config) {
    config.connection_limits_.max_open_ = 2;
    config.http3_.enable_early_data_ = true;
    config.advertisements_.receive_origins_ = true;
    retry_config.connection_limits_.max_open_ = 2;
    ruvia::http_client owner(loop, config);
    ruvia::http_client retry_owner(loop, retry_config);
    std::exception_ptr err;
    try {
        {
            auto seed = co_await owner.client().get("/resumption/safe").send();
            EXPECT_TRUE(seed.ok());
            (void)co_await seed.text();
        }
        ruvia::task_scope scope(loop.worker());
        for (int lane = 0; lane < 8; ++lane) {
            scope.spawn(run_resumption_lane(owner.client(), retry_owner.client(), lane));
        }
        co_await scope.join();
    } catch (...) {
        err = std::current_exception();
    }
    co_await retry_owner.shutdown();
    co_await owner.shutdown();
    if (err) {
        std::rethrow_exception(err);
    }
}

struct route_observation final {
    std::string server_name_;
    std::string authority_;
    std::string origin_connection_;
    bool early_{false};
    std::uint64_t client_connection_{};
    bool reused_{false};
};

ruvia::task<route_observation> observe_route(ruvia::http_client_ref client, ruvia::http_client_request_options options) {
    auto response = co_await client.get("/resumption/route").configure(std::move(options)).send();
    EXPECT_TRUE(response.ok());
    EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http3);
    const auto body = co_await response.text();
    const auto text = body.text();
    const auto first = text.find('|');
    const auto second = text.find('|', first + 1);
    const auto third = text.find('|', second + 1);
    route_observation observed;
    observed.server_name_ = text.substr(0, first);
    observed.authority_ = text.substr(first + 1, second - first - 1);
    observed.origin_connection_ = text.substr(second + 1, third - second - 1);
    observed.early_ = text.substr(third + 1) == "early";
    observed.client_connection_ = response.connection_info().id_;
    observed.reused_ = response.connection_info().reused_;
    co_return observed;
}

[[nodiscard]] ruvia::http_client_request_options route_options(std::string_view authority,
    std::optional<std::string_view> server_name = std::nullopt, std::optional<std::string_view> dial_host = std::nullopt) {
    ruvia::http_client_request_options options;
    options.authority_.emplace(authority);
    if (server_name) {
        options.server_name_.emplace(*server_name);
    }
    if (dial_host) {
        options.dial_host_.emplace(*dial_host);
    }
    return options;
}

ruvia::task<void> exercise_outbound_routes(ruvia::event_loop loop, ruvia::http_client_config config, std::uint16_t port) {
    config.http3_.enable_early_data_ = false;
    config.idle_timeout_ = 10s;
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        const auto client = owner.client();
        const auto authority = [port](std::string_view host) {
            return std::string(host).append(":").append(std::to_string(port));
        };
        const auto a = co_await observe_route(client, route_options(authority("a.localhost")));
        EXPECT_EQ(a.server_name_, "a.localhost");
        EXPECT_EQ(a.authority_, authority("a.localhost"));
        EXPECT_FALSE(a.reused_);
        const auto again = co_await observe_route(client, route_options(authority("a.localhost")));
        EXPECT_TRUE(again.reused_);
        EXPECT_EQ(again.client_connection_, a.client_connection_);
        EXPECT_EQ(again.origin_connection_, a.origin_connection_);
        // :authority is not part of the connection key: the same server name
        // carries another authority on the same QUIC connection.
        const auto shared = co_await observe_route(client, route_options(authority("c.localhost"), "a.localhost"));
        EXPECT_EQ(shared.server_name_, "a.localhost");
        EXPECT_EQ(shared.authority_, authority("c.localhost"));
        EXPECT_EQ(shared.client_connection_, a.client_connection_);
        EXPECT_EQ(shared.origin_connection_, a.origin_connection_);
        const auto b = co_await observe_route(client, route_options(authority("b.localhost")));
        EXPECT_EQ(b.server_name_, "b.localhost");
        EXPECT_EQ(b.authority_, authority("b.localhost"));
        EXPECT_FALSE(b.reused_);
        EXPECT_NE(b.client_connection_, a.client_connection_);
        EXPECT_NE(b.origin_connection_, a.origin_connection_);
        EXPECT_EQ(client.stats().open_connections_, 2U);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Each idle period retires the QUIC connection and lets the pool reclaim its
// owner; resumption state survives only in the pool's per-route ticket cache.
ruvia::task<route_observation> observe_after_idle(ruvia::event_loop& loop, ruvia::http_client_ref client,
    ruvia::http_client_request_options options) {
    (void)co_await ruvia::sleep_for(loop.worker(), 250ms);
    co_return co_await observe_route(client, std::move(options));
}

ruvia::task<void> exercise_route_tickets(ruvia::event_loop loop, ruvia::http_client_config config, std::uint16_t port) {
    config.http3_.enable_early_data_ = true;
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        const auto client = owner.client();
        const auto authority = std::string("a.localhost:").append(std::to_string(port));
        const auto first = co_await observe_route(client, route_options(authority));
        EXPECT_FALSE(first.early_);
        // The route's own ticket resumes it with 0-RTT on a new connection.
        bool resumed = false;
        for (int attempt = 0; attempt != 8 && !resumed; ++attempt) {
            resumed = (co_await observe_after_idle(loop, client, route_options(authority))).early_;
        }
        EXPECT_TRUE(resumed);
        // The same server name and endpoint reached through another dial host
        // is another route: route A's ticket is never offered, so its first
        // request cannot be early data even though the origin would accept it.
        const auto other_dial = co_await observe_after_idle(loop, client, route_options(authority, std::nullopt, "127.0.0.1"));
        EXPECT_EQ(other_dial.server_name_, "a.localhost");
        EXPECT_FALSE(other_dial.early_);
        // Another server name on the same dial target starts without a ticket too.
        const auto other_name = co_await observe_after_idle(loop, client,
            route_options(std::string("b.localhost:").append(std::to_string(port))));
        EXPECT_EQ(other_name.server_name_, "b.localhost");
        EXPECT_FALSE(other_name.early_);
        // Route A still resumes from its own ticket afterwards.
        resumed = false;
        for (int attempt = 0; attempt != 8 && !resumed; ++attempt) {
            resumed = (co_await observe_after_idle(loop, client, route_options(authority))).early_;
        }
        EXPECT_TRUE(resumed);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Alt-Svc is learned and used per route (dial host, dial port, server name).
ruvia::task<void> exercise_route_alt_svc(ruvia::event_loop loop, ruvia::http_client_config config, std::uint16_t tcp_port) {
    const std::string base_url("https://localhost:" + std::string(std::to_string(tcp_port)));
    config.base_url_ = base_url;
    config.versions_ = ruvia::http_client_versions{.http3_ = true};
    config.http3_.enable_early_data_ = false;
    config.idle_timeout_ = 10s;
    ruvia::http_client owner(loop, config);
    std::exception_ptr failure;
    try {
        const auto client = owner.client();
        const auto authority = [tcp_port](std::string_view host) {
            return std::string(host).append(":").append(std::to_string(tcp_port));
        };
        const auto fetch = [&](std::string_view host) -> ruvia::task<std::pair<ruvia::http_protocol_version, std::string>> {
            auto response = co_await client.get("/resumption/route").configure(route_options(authority(host))).send();
            EXPECT_TRUE(response.ok());
            const auto body = co_await response.text();
            co_return std::pair{response.protocol_version(), std::string(body.text())};
        };
        const auto learned = co_await fetch("a.localhost");
        EXPECT_TRUE(learned.first != ruvia::http_protocol_version::http3);
        const auto upgraded = co_await fetch("a.localhost");
        EXPECT_TRUE(upgraded.first == ruvia::http_protocol_version::http3);
        EXPECT_TRUE(upgraded.second.starts_with("a.localhost|"));
        // Another route has not learned the alternative yet.
        const auto other = co_await fetch("b.localhost");
        EXPECT_TRUE(other.first != ruvia::http_protocol_version::http3);
        const auto other_upgraded = co_await fetch("b.localhost");
        EXPECT_TRUE(other_upgraded.first == ruvia::http_protocol_version::http3);
        EXPECT_TRUE(other_upgraded.second.starts_with("b.localhost|"));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http3_resumption_test

TEST(Ruvia, http3_outbound_alt_svc_is_learned_and_used_per_route) {
    auto& server = http3_resumption_test::server::shared();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        loops.loop(0).start(http3_resumption_test::exercise_route_alt_svc(loops.loop(0), server.outbound_config(), server.enabled_tcp_port())).get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_request_routes_send_their_authority_and_server_name_per_connection) {
    auto& server = http3_resumption_test::server::shared();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        loops.loop(0).start(http3_resumption_test::exercise_outbound_routes(loops.loop(0), server.outbound_config(), server.enabled_endpoint().port())).get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_session_tickets_resume_only_their_route) {
    auto& server = http3_resumption_test::server::shared();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        loops.loop(0).start(http3_resumption_test::exercise_route_tickets(loops.loop(0), server.outbound_config(), server.enabled_endpoint().port())).get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_idle_reconnect_serves_requests_and_bounds_425_retries) {
    const auto config = http3_resumption_test::server::shared().outbound_config();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        auto loop = loops.loop(0);
        loop.start(http3_resumption_test::exercise_outbound_resumption(loop, config)).get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_consecutive_migrations_wait_for_connection_ids) {
    const auto config = http3_resumption_test::server::shared().outbound_config();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        auto loop = loops.loop(0);
        loop.start(http3_resumption_test::exercise_outbound_migration(loop, config)).get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_migration_cancellation_and_deadlines_cover_path_validation) {
    auto& server = http3_resumption_test::server::shared();
    ruvia::blocking_pool blocking;
    for (int mode = 0; mode != 5; ++mode) {
        SCOPED_TRACE(mode);
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blocking});
        auto root = attachment.loop().start(http3_resumption_test::exercise_blocked_migration(
            attachment, server.outbound_config(), server.enabled_endpoint(), mode));
        attachment.run();
        std::move(root).get();
    }
}

TEST(Ruvia, http3_resumes_tls_tickets_across_workers_with_early_data_negotiated) {
    const auto ticket = http3_resumption_test::ticket();
    EXPECT_TRUE(ticket.permits_early_data());
    // Fresh source ports distribute resumptions across the four workers;
    // all workers decrypt tickets using the listener's shared rotating keys.
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto client = http3_resumption_test::server::shared().connect(true, &ticket, true);
        EXPECT_TRUE(client.early_data_attempted());
        const auto stream = client.start_early_request("GET", "/resumption/safe");
        const auto response = client.finish_response(stream);
        client.complete_handshake();
        EXPECT_TRUE(client.resumed());
        EXPECT_TRUE(client.early_data_accepted());
        EXPECT_TRUE(!client.early_data_rejected());
        EXPECT_EQ(response.status_, 200);
        EXPECT_EQ(response.body_, "early");
    }
}

TEST(Ruvia, http3_rejects_unsafe_zero_rtt_with_425_then_accepts_one_rtt) {
    const auto ticket = http3_resumption_test::ticket();
    const auto before = http3_resumption_test::unsafe_requests.load();
    auto client = http3_resumption_test::server::shared().connect(true, &ticket, true);
    const auto early = client.start_early_request("POST", "/resumption/unsafe");
    const auto rejected = client.finish_response(early);
    client.complete_handshake();
    EXPECT_TRUE(client.resumed());
    EXPECT_TRUE(client.early_data_accepted());
    EXPECT_EQ(rejected.status_, 425);
    EXPECT_EQ(http3_resumption_test::unsafe_requests.load(), before);
    const auto retried = client.request("POST", "/resumption/unsafe");
    EXPECT_EQ(retried.status_, 200);
    EXPECT_EQ(retried.body_, "unsafe-one-rtt");
    EXPECT_EQ(http3_resumption_test::unsafe_requests.load(), before + 1);
}

TEST(Ruvia, http3_default_listener_resumes_without_enabling_early_data) {
    const auto ticket = http3_resumption_test::ticket(false);
    EXPECT_TRUE(!ticket.permits_early_data());
    auto client = http3_resumption_test::server::shared().connect(false, &ticket, true);
    EXPECT_TRUE(client.resumed());
    EXPECT_TRUE(!client.early_data_attempted());
    EXPECT_TRUE(!client.early_data_accepted());
    const auto response = client.request("GET", "/resumption/safe");
    EXPECT_EQ(response.status_, 200);
    EXPECT_EQ(response.body_, "one-rtt");
}

TEST(Ruvia, http3_rejected_early_ticket_recovers_without_replaying_request) {
    const auto ticket = http3_resumption_test::ticket();
    auto client = http3_resumption_test::server::shared().connect(false, &ticket, true);
    const auto stream = client.start_request("GET", "/resumption/safe");
    client.complete_handshake();
    EXPECT_TRUE(!client.resumed());
    EXPECT_TRUE(client.early_data_attempted());
    EXPECT_TRUE(client.early_data_rejected());
    EXPECT_TRUE(!client.early_data_accepted());
    const auto rejected = client.finish_response(stream);
    EXPECT_EQ(rejected.status_, 0);
    EXPECT_TRUE(rejected.reset_error_.has_value());
    const auto response = client.request("GET", "/resumption/safe");
    EXPECT_EQ(response.status_, 200);
    EXPECT_EQ(response.body_, "one-rtt");
}

TEST(Ruvia, http3_udp_rebinding_preserves_the_connection_worker) {
    auto client = http3_resumption_test::server::shared().connect();
    const auto original = client.request("GET", "/resumption/worker");
    EXPECT_EQ(original.status_, 200);
    EXPECT_TRUE(!original.body_.empty());
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto port = client.local_port();
        client.rebind();
        EXPECT_TRUE(client.local_port() != port);
        const auto response = client.request("GET", "/resumption/worker");
        EXPECT_EQ(response.status_, 200);
        EXPECT_EQ(response.body_, original.body_);
        EXPECT_TRUE(!client.closed());
    }
}

TEST(Ruvia, http3_quic_stateless_reset_closes_client_and_enforces_size_bounds) {
    ruvia::test::tls_identity identity("localhost");
    configure_h3_server(identity.context_.native_handle());
    in_memory_h3_pair pair(identity);
    pair.exchange();
    EXPECT_TRUE(pair.client().handshake_completed());
    EXPECT_TRUE(!pair.client().stateless_reset_received());

    const auto& token = pair.server_session().stateless_reset_token();
    std::array<std::uint8_t, 32> random_bytes{};
    EXPECT_TRUE(RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) == 1);
    std::array<std::byte, 64> reset_packet{};
    // Packets of 21 bytes or less cannot form a valid Stateless Reset.
    EXPECT_EQ(
        ruvia::write_http3_quic_stateless_reset(token, random_bytes, reset_packet, 21),
        std::size_t{0});
    const auto written =
        ruvia::write_http3_quic_stateless_reset(token, random_bytes, reset_packet, 44);
    EXPECT_TRUE(written >= 22 && written <= 44);

    const auto status = pair.client().receive_datagram(
        pair.client_path_, NGTCP2_ECN_NOT_ECT,
        std::span<const std::byte>(reset_packet.data(), written), pair.now_);
    EXPECT_TRUE(status.status_ == ruvia::http3_quic_status::closed);
    EXPECT_TRUE(pair.client().closed());
    EXPECT_TRUE(pair.client().stateless_reset_received());
}

TEST(Ruvia, http3_retry_validates_client_address_and_stateless_reset_retires_unknown_short_packets) {
    auto retry_client = http3_resumption_test::server::shared().connect_retry();
    EXPECT_TRUE(retry_client.after_retry());
    const auto served = retry_client.request("GET", "/resumption/safe");
    EXPECT_EQ(served.status_, 200);
    EXPECT_EQ(served.body_, "one-rtt");

    // Capture the server-issued CID and its advertised stateless reset token,
    // close the connection, wait out the server's closing period, and verify
    // that a short-header packet for that retired CID elicits a Stateless Reset.
    const ngtcp2_cid server_cid = *ngtcp2_conn_get_dcid2(retry_client.native_handle());
    const auto* params = ngtcp2_conn_get_remote_transport_params2(retry_client.native_handle());
    EXPECT_TRUE(params != nullptr && params->stateless_reset_token_present != 0);
    std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN> expected_token{};
    if (params != nullptr) {
        std::memcpy(expected_token.data(), params->stateless_reset_token, expected_token.size());
    }
    retry_client.close();

    asio::io_context io;
    asio::ip::udp::socket probe(io, asio::ip::udp::v4());
    probe.connect(http3_resumption_test::server::shared().retry_endpoint());
    probe.non_blocking(true);
    std::array<std::uint8_t, 48> short_packet{};
    EXPECT_TRUE(RAND_bytes(short_packet.data(), static_cast<int>(short_packet.size())) == 1);
    short_packet[0] = static_cast<std::uint8_t>(0x40U | (short_packet[0] & 0x3fU));
    std::memcpy(short_packet.data() + 1, server_cid.data, server_cid.datalen);

    std::array<std::uint8_t, 128> reply{};
    std::size_t reply_size = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code send_error;
        (void)probe.send(asio::buffer(short_packet), 0, send_error);
        std::error_code error;
        reply_size = probe.receive(asio::buffer(reply), 0, error);
        if (!error && reply_size >= 22) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(reply_size >= 22 && reply_size < short_packet.size());
    if (reply_size >= NGTCP2_STATELESS_RESET_TOKENLEN) {
        EXPECT_TRUE(std::memcmp(
                        reply.data() + reply_size - NGTCP2_STATELESS_RESET_TOKENLEN,
                        expected_token.data(), expected_token.size()) == 0);
    }
}

TEST(Ruvia, http3_outbound_advertises_origins_and_reprioritizes_active_streams) {
    const auto config = http3_resumption_test::server::shared().outbound_config();
    const auto retry_config = http3_resumption_test::server::shared().retry_outbound_config();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &blocking});
    loops.start();
    try {
        auto loop = loops.loop(0);
        loop.start(http3_resumption_test::exercise_outbound_features(
                       loop, config, retry_config))
            .get();
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

TEST(Ruvia, http3_outbound_concurrent_early_data_retry_and_goaway_drain_stress) {
    const auto config = http3_resumption_test::server::shared().outbound_config();
    const auto retry_config = http3_resumption_test::server::shared().retry_outbound_config();
    ruvia::blocking_pool blocking;
    ruvia::event_loop_pool loops({.loop_count_ = 4, .blocking_pool_ = &blocking});
    loops.start();
    try {
        std::vector<ruvia::root_task<void>> futures{};
        futures.reserve(4);
        for (std::size_t idx = 0; idx < 4; ++idx) {
            auto loop = loops.loop(idx);
            futures.push_back(loop.start(http3_resumption_test::exercise_concurrent_early_data_and_retry_worker(
                loop, config, retry_config)));
        }
        for (auto& f : futures) {
            std::move(f).get();
        }
    } catch (...) {
        loops.stop();
        loops.join();
        throw;
    }
    loops.stop();
    loops.join();
}

// End to end over loopback UDP: a real application with an HTTPS listener and
// an HTTP/3 service on the same port number, four workers, and an ngtcp2 +
// nghttp3 client (http_server_fixture.h).

namespace http3_serving_test {
using namespace std::chrono_literals;

constexpr auto client_timeout = 5s;
constexpr std::size_t echo_bytes = 1536 * 1024;
constexpr std::size_t file_bytes = 300 * 1024;
constexpr int stream_chunks = 8;

std::atomic<std::uint16_t> tcp_echo_port{};
std::atomic<std::uint16_t> udp_echo_port{};

std::span<const std::byte> byte_view(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string udp_capsule(std::string_view payload) {
    std::array<char, 16> prefix{};
    const auto encoded = ruvia::encode_http_capsule_header(prefix, ruvia::http_datagram_capsule_type, payload.size() + 1);
    std::string result(prefix.data(), std::get<std::size_t>(encoded));
    result.push_back('\0');
    result.append(payload);
    return result;
}

std::string masked_frame(unsigned char opcode, std::string_view payload) {
    if (payload.size() > 125) {
        throw std::length_error("test frame exceeds short payload");
    }
    std::string frame{};
    frame.push_back(static_cast<char>(0x80U | opcode));
    frame.push_back(static_cast<char>(0x80U | payload.size()));
    constexpr std::array<char, 4> mask{1, 2, 3, 4};
    frame.append(mask.data(), mask.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        frame.push_back(static_cast<char>(payload[index] ^ mask[index % mask.size()]));
    }
    return frame;
}

std::string read_tunnel_bytes(ruvia::test::http3_test_client& client, std::int64_t stream, std::size_t size) {
    std::string bytes{};
    while (bytes.size() < size) {
        auto chunk = client.read_body_chunk(stream, client_timeout);
        if (!chunk) {
            throw std::runtime_error("tunnel ended before echo");
        }
        bytes.append(*chunk);
    }
    return bytes;
}

ruvia::task<void> await_backend(ruvia::context& context, std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline ||
        co_await ruvia::sleep_for(context.worker(), 1ms, context.stop_token()) != ruvia::timer_sleep_result::elapsed) {
        throw std::runtime_error("echo backend deadline");
    }
}

// Real loopback TCP and UDP peers, independent of the application's workers.
class echo_backends final {
public:
    echo_backends()
        : tcp_(io_, {asio::ip::address_v4::loopback(), 0}),
          udp_(io_, {asio::ip::address_v4::loopback(), 0}) {
        tcp_echo_port = tcp_.local_endpoint().port();
        udp_echo_port = udp_.local_endpoint().port();
        asio::co_spawn(io_, serve_tcp(), [this](std::exception_ptr failure) { tcp_failure_ = failure; });
        asio::co_spawn(io_, serve_udp(), [this](std::exception_ptr failure) { udp_failure_ = failure; });
        thread_ = std::thread([this] { io_.run(); });
    }
    ~echo_backends() {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void finish() {
        io_.stop();
        thread_.join();
        if (tcp_failure_) {
            std::rethrow_exception(tcp_failure_);
        }
        if (udp_failure_) {
            std::rethrow_exception(udp_failure_);
        }
    }

private:
    static asio::awaitable<void> echo_tcp_client(asio::ip::tcp::socket socket) {
        std::array<char, 16384> buffer{};
        std::error_code error;
        for (;;) {
            const auto count = co_await socket.async_read_some(asio::buffer(buffer),
                asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::error::eof || error == asio::error::connection_reset) {
                break;
            }
            if (error) {
                throw std::system_error(error);
            }
            co_await asio::async_write(socket, asio::buffer(buffer.data(), count),
                asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, error);
    }
    asio::awaitable<void> serve_tcp() {
        for (;;) {
            auto socket = co_await tcp_.async_accept(asio::use_awaitable);
            asio::co_spawn(io_, echo_tcp_client(std::move(socket)), [this](std::exception_ptr failure) {
                if (failure && !tcp_failure_) {
                    tcp_failure_ = failure;
                }
            });
        }
    }
    asio::awaitable<void> serve_udp() {
        std::array<char, 65536> buffer{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            const auto count = co_await udp_.async_receive_from(asio::buffer(buffer), sender, asio::use_awaitable);
            co_await udp_.async_send_to(asio::buffer(buffer.data(), count), sender, asio::use_awaitable);
        }
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor tcp_;
    asio::ip::udp::socket udp_;
    std::exception_ptr tcp_failure_;
    std::exception_ptr udp_failure_;
    std::thread thread_;
};

std::atomic<bool>& slow_entered() {
    static std::atomic<bool> value{false};
    return value;
}

std::filesystem::path& file_path() {
    static std::filesystem::path value;
    return value;
}

std::string file_content() {
    std::string content(file_bytes, '\0');
    for (std::size_t index = 0; index < content.size(); ++index) {
        content[index] = static_cast<char>('a' + index % 26);
    }
    return content;
}

class http3_routes final {
public:
    RUVIA_ROUTES_BEGIN(http3_routes)
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/dial-authority", dial_authority);
    RUVIA_GET("/connection-hints", connection_hints);
    RUVIA_GET("/connection-silent", connection_silent);
    RUVIA_POST("/echo", echo);
    RUVIA_GET("/stream", stream);
    RUVIA_POST("/upload-trailer", upload_trailer, ruvia::stream_body);
    RUVIA_GET("/events", events);
    RUVIA_GET("/push-and-origin", push_and_origin);
    RUVIA_GET("/file", file);
    RUVIA_GET("/worker", worker);
    RUVIA_GET("/slow", slow);
    RUVIA_CONNECT("echo.test:443", tcp_tunnel);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/echo.test/53/", udp_tunnel);
    RUVIA_GET_WS("/websocket", websocket,
        (ruvia::websocket_route_config{.deflate_ = {.enabled_ = true}}));
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text("hello over http/3");
    }
    ruvia::task<ruvia::http_response> dial_authority(ruvia::context& context) {
        context.draft().header("set-cookie", "logical=1; Domain=localhost; Path=/; Secure");
        const auto authority = context.req().authority();
        std::string text(authority.empty() ? context.req().header("host").value_or("") : authority);
        text.push_back('|');
        text.append(context.req().header("cookie").value_or(""));
        co_return context.text(text);
    }
    ruvia::task<ruvia::http_response> connection_hints(ruvia::context& context) {
        const std::array fields{ruvia::http_header_view("x-hint", "connection-controls")};
        co_await context.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields));
        co_return context.text("hints complete");
    }
    ruvia::task<ruvia::http_response> connection_silent(ruvia::context& context) {
        (void)co_await ruvia::sleep_for(context.worker(), 1s, context.stop_token());
        co_return context.text("delayed");
    }
    ruvia::task<ruvia::http_response> echo(ruvia::context& context) {
        const auto body = co_await context.req().text();
        co_return context.text(std::string(body.data(), body.size()));
    }
    ruvia::task<void> stream(ruvia::context& context) {
        auto& writer = context.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int chunk = 0; chunk < stream_chunks; ++chunk) {
            co_await writer.write("chunk-" + std::string(std::to_string(chunk)) + ";");
            (void)co_await writer.sleep(2ms);
        }
        co_await writer.end();
    }
    ruvia::task<void> upload_trailer(ruvia::context& context) {
        const std::array<ruvia::http_header_view, 1> hint_fields{{{"link", "</app.css>; rel=preload"}}};
        co_await context.inform(
            ruvia::http_interim_response_head(ruvia::http_status::early_hints, hint_fields));
        std::size_t bytes = 0;
        auto& reader = context.req().body_reader();
        while (auto chunk = co_await reader.read()) {
            bytes += chunk->size();
        }
        const auto checksum = context.req().trailer("x-upload-checksum").value_or("missing");
        context.draft().header("Trailer", "x-response-trailer");
        auto& out = context.stream_text();
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("uploaded:" + std::string(std::to_string(bytes)) + ":" + std::string(checksum));
        const ruvia::http_header_view trailers[] = {{"x-response-trailer", "verified"}};
        co_await out.end(trailers);
    }
    ruvia::task<void> events(ruvia::context& context) {
        auto writer = context.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int idx = 0; idx < 3; ++idx) {
            const std::string id = std::string(std::to_string(idx));
            const std::string data(std::string("item-") + id);
            co_await writer.write({.data_ = data, .event_ = "tick", .id_ = id});
        }
        co_await writer.end();
    }
    ruvia::task<ruvia::http_response> push_and_origin(ruvia::context& context) {
        const std::string origin("https://" + std::string(context.req().authority()));
        const std::array<std::string_view, 1> origins{origin};
        context.advertise_origins(origins);
        if (context.req().protocol_version() == ruvia::http_protocol_version::http2) {
            const std::array<ruvia::http_header_view, 1> push_headers{{{"x-pushed", "1"}}};
            (void)context.push({.scheme_ = "https",
                .authority_ = context.req().authority(),
                .path_ = "/hello",
                .headers_ = push_headers});
        }
        co_return context.text("origin-and-push");
    }
    ruvia::task<ruvia::http_response> file(ruvia::context& context) {
        co_return co_await context.file({.path_ = file_path()});
    }
    // Names the worker that serves the connection.
    ruvia::task<ruvia::http_response> worker(ruvia::context& context) {
        const auto id = std::string(std::to_string(context.worker().id()));
        co_return context.text(std::string(id.data(), id.size()));
    }
    // Ends only on its stop token.
    ruvia::task<ruvia::http_response> slow(ruvia::context& context) {
        slow_entered() = true;
        (void)co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token());
        co_return context.text("slow-done");
    }
    ruvia::task<void> tcp_tunnel(ruvia::context& context) {
        asio::io_context io;
        asio::ip::tcp::socket socket(io);
        socket.connect({asio::ip::address_v4::loopback(), tcp_echo_port.load()});
        socket.non_blocking(true);
        auto& tunnel = context.tunnel();
        std::array<char, 16384> buffer{};
        while (auto chunk = co_await tunnel.read()) {
            const auto deadline = std::chrono::steady_clock::now() + client_timeout;
            std::size_t sent = 0;
            while (sent < chunk->size()) {
                std::error_code error;
                sent += socket.write_some(asio::buffer(chunk->data() + sent, chunk->size() - sent), error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    co_await await_backend(context, deadline);
                } else if (error) {
                    throw std::system_error(error);
                }
            }
            std::size_t received = 0;
            while (received < chunk->size()) {
                std::error_code error;
                const auto count = socket.read_some(asio::buffer(buffer.data(),
                                                        (std::min)(buffer.size(), chunk->size() - received)),
                    error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    co_await await_backend(context, deadline);
                    continue;
                }
                if (error) {
                    throw std::system_error(error);
                }
                received += count;
                co_await tunnel.write(std::string_view(buffer.data(), count));
            }
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        co_await tunnel.finish();
    }
    ruvia::task<void> websocket(ruvia::context& context) {
        auto& socket = context.websocket();
        while (auto message = co_await socket.read()) {
            co_await socket.send(std::move(*message));
        }
    }
    ruvia::task<void> udp_tunnel(ruvia::context& context) {
        asio::io_context io;
        asio::ip::udp::socket socket(io, asio::ip::udp::v4());
        socket.connect({asio::ip::address_v4::loopback(), udp_echo_port.load()});
        socket.non_blocking(true);
        auto tunnel = context.tunnel().udp();
        std::array<char, 65536> buffer{};
        while (auto packet = co_await tunnel.read()) {
            const auto deadline = std::chrono::steady_clock::now() + client_timeout;
            for (;;) {
                std::error_code error;
                socket.send(asio::buffer(packet->text().data(), packet->text().size()), 0, error);
                if (!error) {
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error);
                }
                co_await await_backend(context, deadline);
            }
            for (;;) {
                std::error_code error;
                const auto count = socket.receive(asio::buffer(buffer), 0, error);
                if (!error) {
                    co_await tunnel.send(std::string_view(buffer.data(), count));
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error);
                }
                co_await await_backend(context, deadline);
            }
        }
        co_await tunnel.finish();
    }
};

// One loopback port number free for both TCP (HTTPS) and UDP (HTTP/3).
class shared_port_reservation final {
public:
    explicit shared_port_reservation(asio::io_context& io)
        : tcp_(io) {
        for (int attempt = 0; attempt < 64; ++attempt) {
            udp_.emplace(io);
            const auto port = udp_->endpoint().port();
            std::error_code error;
            tcp_.open(asio::ip::tcp::v4(), error);
            if (!error) {
                // Bound exclusively first, so no other socket holds the port;
                // then shared with the server's reuseport listeners.
                tcp_.bind({asio::ip::address_v4::loopback(), port}, error);
            }
            if (!error) {
                tcp_.set_option(asio::socket_base::reuse_address(true));
#if !defined(_WIN32)
                const int enabled = 1;
                if (::setsockopt(tcp_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
                    throw std::system_error(errno, asio::system_category(), "cannot share the reserved TCP port");
                }
#else
                // Windows fixes sharing at bind time: the named UDP lease
                // keeps other fixtures off the number instead.
                tcp_.close();
#endif
                port_ = port;
                return;
            }
            tcp_.close(error);
        }
        throw std::runtime_error("no loopback port is free for both TCP and UDP");
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

private:
    std::optional<ruvia::test::udp_port_reservation> udp_;
    asio::ip::tcp::socket tcp_;
    std::uint16_t port_{0};
};

// One HTTP/1.1 request over TLS on TCP; returns the response head.
std::string https_head(asio::io_context& io, std::uint16_t port, const std::filesystem::path& ca_file) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(ca_file.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_CTX_set_alpn_protos(client.native_handle(), alpn, sizeof(alpn)) != 0) {
        throw std::runtime_error("cannot offer http/1.1");
    }
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot offer SNI");
    }
    stream.next_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const std::string request("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    asio::write(stream, asio::buffer(request));
    std::string response{};
    std::error_code error;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), count);
        if (error || response.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    return std::string(std::string_view(response).substr(0, response.find("\r\n\r\n")));
}

ruvia::task<void> run_protocol_lane(ruvia::http_client_ref client,
    ruvia::http_protocol_version expected_version, std::size_t loop_idx, int lane,
    const std::string& expected_stream, const std::string& expected_file,
    std::atomic<std::size_t>& completed_ops) {
    for (int round = 0; round < 3; ++round) {
        {
            auto resp = co_await client.get("/hello").send();
            EXPECT_TRUE(resp.ok());
            EXPECT_TRUE(resp.protocol_version() == expected_version);
            auto body = co_await resp.text();
            EXPECT_EQ(body.text(), "hello over http/3");
        }
        {
            const std::string payload(4096 + static_cast<std::size_t>(lane * 256 + round * 17),
                static_cast<char>('A' + (lane + round) % 26));
            auto resp = co_await client.post("/echo").body(payload).send();
            EXPECT_TRUE(resp.ok());
            auto echoed = co_await resp.text();
            EXPECT_EQ(echoed.text(), payload);
        }
        {
            auto resp = co_await client.get("/stream")
                            .header("priority", "u=5")
                            .send();
            EXPECT_TRUE(resp.ok());
            if (expected_version != ruvia::http_protocol_version::http11) {
                resp.reprioritize({.urgency_ = ruvia::http_priority_urgency(2), .incremental_ = true});
            }
            std::string collected{};
            while (auto chunk = co_await resp.stream().text()) {
                collected.append(*chunk);
            }
            EXPECT_EQ(collected, expected_stream);
        }
        if (round == 0) {
            auto resp = co_await client.get("/file").send();
            EXPECT_TRUE(resp.ok());
            auto body = co_await resp.text();
            EXPECT_EQ(body.text().size(), expected_file.size());
            EXPECT_TRUE(body.text() == expected_file);

            // Mid-stream response abandonment: read one chunk and drop the response.
            {
                auto partial = co_await client.get("/stream").send();
                EXPECT_TRUE(partial.ok());
                auto first_chunk = co_await partial.stream().text();
                EXPECT_TRUE(first_chunk.has_value());
            }

            auto sse_resp = co_await client.get("/events").send();
            EXPECT_TRUE(sse_resp.ok());
            std::string sse_collected{};
            while (auto chunk = co_await sse_resp.stream().text()) {
                sse_collected.append(*chunk);
            }
            EXPECT_TRUE(sse_collected.find("event: tick\nid: 0\ndata: item-0\n\n") != std::string::npos);
            EXPECT_TRUE(sse_collected.find("event: tick\nid: 2\ndata: item-2\n\n") != std::string::npos);

            if (expected_version != ruvia::http_protocol_version::http11) {
                auto po_resp = co_await client.get("/push-and-origin").send();
                EXPECT_TRUE(po_resp.ok());
                auto po_body = co_await po_resp.text();
                EXPECT_EQ(po_body.text(), "origin-and-push");
                while (auto pushed = client.next_push()) {
                    auto p_resp = co_await pushed->response();
                    EXPECT_TRUE(p_resp.ok());
                    auto p_text = co_await p_resp.text();
                    EXPECT_EQ(p_text.text(), "hello over http/3");
                }
            }
        }
        if (round == 1) {
            // Mid-upload abandonment: write one chunk and drop the exchange.
            {
                auto abandoned = co_await client.post("/upload-trailer").upload({});
                co_await abandoned.body().write("abandoned-chunk");
            }
            {
                auto exchange = co_await client.post("/upload-trailer")
                                    .header("trailer", "x-upload-checksum")
                                    .upload({.expectation_ =
                                                 ruvia::http_client_request_expectation::continue_value});
                co_await exchange.body().write("part-1:");
                co_await exchange.body().write("part-2");
                const std::array<ruvia::http_header_view, 1> req_trailers{
                    {{"x-upload-checksum", "crc-ok"}}};
                co_await exchange.body().end(req_trailers);
                auto resp = co_await exchange.response();
                EXPECT_TRUE(resp.ok());
                EXPECT_TRUE(resp.informational_responses().size() >= std::size_t{1});
                std::string uploaded_text{};
                while (auto chunk = co_await resp.stream().text()) {
                    uploaded_text.append(*chunk);
                }
                EXPECT_EQ(uploaded_text, "uploaded:13:crc-ok");
                EXPECT_EQ(
                    resp.trailer("x-upload-checksum").has_value(), false);
                EXPECT_EQ(
                    resp.trailer("x-response-trailer"), std::optional<std::string_view>{"verified"});
            }
            auto ws = co_await client.websocket("/websocket")
                          .deflate({.compression_ = {.enabled_ = true}})
                          .connect();
            EXPECT_TRUE(ws.connected());
            for (int msg_idx = 0; msg_idx < 3; ++msg_idx) {
                const std::string msg(std::string(512, static_cast<char>('a' + msg_idx)) +
                                      "-ws-" + std::string(std::to_string(loop_idx)) + "-" +
                                      std::string(std::to_string(lane)) + "-" + std::string(std::to_string(msg_idx)));
                co_await ws.text(msg, {.compress_ = (msg_idx % 2 == 0)});
                auto reply = co_await ws.read();
                EXPECT_TRUE(reply.has_value());
                if (reply) {
                    EXPECT_EQ(reply->payload(), msg);
                }
            }
            co_await ws.close();
        }
        if (round == 2) {
            auto tunnel_res = co_await client.connect("echo.test:443").open();
            auto* tunnel = tunnel_res.tunnel();
            EXPECT_TRUE(tunnel != nullptr);
            if (tunnel != nullptr) {
                const std::string msg(8192, static_cast<char>('0' + lane % 10));
                co_await tunnel->write(msg);
                co_await tunnel->finish();
                std::string echoed{};
                while (auto chunk = co_await tunnel->text()) {
                    echoed.append(*chunk);
                }
                EXPECT_EQ(echoed, msg);
            }
            if (expected_version != ruvia::http_protocol_version::http11) {
                auto udp_res = co_await client.udp("/.well-known/masque/udp/echo.test/53/").open();
                auto* raw_tunnel = udp_res.tunnel();
                EXPECT_TRUE(raw_tunnel != nullptr);
                if (raw_tunnel != nullptr) {
                    auto udp = std::move(*raw_tunnel).udp();
                    const std::string dgram("udp-" + std::string(std::to_string(loop_idx)) + "-" + std::string(std::to_string(lane)));
                    co_await udp.send(dgram);
                    auto echoed_dgram = co_await udp.read();
                    EXPECT_TRUE(echoed_dgram.has_value());
                    if (echoed_dgram) {
                        EXPECT_EQ(echoed_dgram->text(), dgram);
                    }
                    co_await udp.finish();
                    EXPECT_TRUE(!(co_await udp.read()).has_value());
                }
            }
        }
        ++completed_ops;
    }
}

ruvia::task<void> run_cancelled_slow_request(ruvia::http_client_ref client,
    ruvia::stop_token token) {
    bool cancelled = false;
    try {
        (void)co_await client.with_options({.stop_token_ = std::move(token)})
            .get("/slow")
            .send();
    } catch (const ruvia::http_client_error& err) {
        cancelled = err.code() == std::make_error_code(std::errc::operation_canceled);
    }
    EXPECT_TRUE(cancelled);
}

ruvia::task<void> run_cancellation_and_migration_lane(ruvia::worker_ref worker,
    std::optional<ruvia::http_client_ref> h2_client, ruvia::http_client_ref h3_client) {
    for (int i = 0; i < 3; ++i) {
        ruvia::stop_source cancel_h2;
        ruvia::stop_source cancel_h3;
        ruvia::task_scope cancel_scope(worker);
        if (h2_client) {
            cancel_scope.spawn(run_cancelled_slow_request(*h2_client, cancel_h2.token()));
        }
        cancel_scope.spawn(run_cancelled_slow_request(h3_client, cancel_h3.token()));
        (void)co_await ruvia::sleep_for(worker, 5ms);
        cancel_h2.request_stop();
        cancel_h3.request_stop();
        co_await cancel_scope.join();
        co_await h3_client.migrate("127.0.0.1", 0);
    }
}

ruvia::task<void> run_negotiate_upgrade_lane(
    ruvia::http_client_ref client) {
    auto first = co_await client.get("/hello").send();
    EXPECT_TRUE(first.ok());
    (void)co_await first.text();
    auto second = co_await client.get("/hello").send();
    EXPECT_TRUE(second.ok());
    EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http3);
    auto text = co_await second.text();
    EXPECT_EQ(text.text(), "hello over http/3");
}

// negotiate learns nothing from Alt-Svc: every request stays on TCP ALPN.
ruvia::task<void> run_negotiate_tcp_lane(ruvia::http_client_ref client) {
    for (int request = 0; request != 3; ++request) {
        auto response = co_await client.get("/hello").send();
        EXPECT_TRUE(response.ok());
        EXPECT_TRUE(response.protocol_version() != ruvia::http_protocol_version::http3);
        (void)co_await response.text();
    }
}

ruvia::task<void> exercise_independent_informational(ruvia::event_loop loop, ruvia::http_client_ref client) {
    bool entered = false;
    bool released = false;
    bool completed = false;
    auto request = client.get("/connection-hints");
    request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> fields,
                                             ruvia::stop_token stop) -> ruvia::task<void> {
        EXPECT_EQ(status.value(), 103);
        EXPECT_TRUE(std::ranges::any_of(fields, [](const ruvia::http_header& field) {
            return field.name() == "x-hint" && field.value() == "connection-controls";
        }));
        entered = true;
        while (!released && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(loop.worker(), 1ms, stop);
        }
    }});
    auto first = [&]() -> ruvia::task<void> {
        auto response = co_await request.send();
        const auto text = co_await response.text();
        EXPECT_EQ(text.text(), "hints complete");
        completed = true;
    };
    ruvia::task_scope tasks(loop.worker());
    tasks.spawn(first());
    std::exception_ptr failure;
    try {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!entered) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("informational callback did not start");
            }
            (void)co_await ruvia::sleep_for(loop.worker(), 1ms);
        }
        auto independent = co_await client.get("/hello").send();
        (void)co_await independent.text();
        EXPECT_FALSE(completed);
    } catch (...) {
        failure = std::current_exception();
    }
    released = true;
    co_await tasks.join();
    EXPECT_TRUE(completed);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ruvia::task<void> exercise_connection_controls(ruvia::event_loop loop, std::uint16_t port,
    const std::filesystem::path& ca_file, ruvia::http_client_versions protocol) {
    bool allowed = true;
    std::size_t validations = 0;
    ruvia::http_client_config config{.base_url_ = "https://localhost",
        .dial_host_ = "127.0.0.1",
        .dial_port_ = port};
    config.versions_ = protocol;
    // Unbounded open connections: fresh connections then never evict the
    // pooled one this test expects to reuse.
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = ca_file;
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([&](const ruvia::ip_address& ip, std::uint16_t dial_port) {
        ++validations;
        EXPECT_EQ(dial_port, port);
        const auto text = ip.to_string();
        return allowed && (text == "127.0.0.1" || text == "127.0.0.2");
    });
    ruvia::http_client owner(loop, config);
    auto client = owner.client();
    std::exception_ptr failure;
    std::string_view stage = "first-head";
    int finish_attempt = -1;
    try {
        EXPECT_EQ(client.host(), "localhost");
        EXPECT_EQ(client.port(), 443);
        auto first = co_await client.get("/dial-authority").send();
        const auto first_info = first.connection_info();
        EXPECT_NE(first_info.id_, 0U);
        EXPECT_EQ(first_info.peer_ip_.to_string(), "127.0.0.1");
        EXPECT_EQ(first_info.peer_port_, port);
        EXPECT_FALSE(first_info.reused_);
        EXPECT_GT(first_info.tls_duration_, std::chrono::steady_clock::duration::zero());
        stage = "first-body";
        const auto first_text = co_await first.text();
        EXPECT_EQ(first_text.text(), "localhost|");
        stage = "second-head";
        auto second = co_await client.get("/dial-authority").send();
        EXPECT_EQ(second.connection_info().id_, first_info.id_);
        EXPECT_TRUE(second.connection_info().reused_);
        stage = "second-body";
        const auto second_text = co_await second.text();
        EXPECT_EQ(second_text.text(), "localhost|logical=1");
        std::uint64_t last_fresh = 0;
        for (int attempt = 0; attempt != 2; ++attempt) {
            stage = "fresh-head";
            auto fresh = co_await client.get("/hello").configure({.fresh_connection_ = true}).send();
            EXPECT_FALSE(fresh.connection_info().reused_);
            EXPECT_NE(fresh.connection_info().id_, first_info.id_);
            EXPECT_NE(fresh.connection_info().id_, last_fresh);
            last_fresh = fresh.connection_info().id_;
            stage = "fresh-body";
            (void)co_await fresh.text();
        }
        stage = "pooled-reuse";
        auto reused = co_await client.get("/hello").send();
        EXPECT_EQ(reused.connection_info().id_, first_info.id_);
        (void)co_await reused.text();
        EXPECT_TRUE(client.close_connection(first_info.id_));
        EXPECT_FALSE(client.close_connection(first_info.id_));
        stage = "replace-closed-connection";
        auto replaced = co_await client.get("/hello").send();
        const auto replacement_id = replaced.connection_info().id_;
        EXPECT_NE(replacement_id, first_info.id_);
        (void)co_await replaced.text();
        if (protocol != ruvia::http_client_versions{.http2_ = false}) {
            stage = "independent-informational";
            co_await exercise_independent_informational(loop, client);
        }
        if (protocol == ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}) {
            // The peer can finish both halves before the UDP send completion
            // resumes the producer that is awaiting its submitted trailers/FIN.
            for (int attempt = 0; attempt != 4; ++attempt) {
                finish_attempt = attempt;
                stage = "upload-open";
                auto upload = co_await client.post("/upload-trailer")
                                  .header("trailer", "x-upload-checksum")
                                  .configure({.fresh_connection_ = attempt % 2 == 0})
                                  .upload({.expectation_ = ruvia::http_client_request_expectation::continue_value});
                stage = "upload-write";
                co_await upload.body().write("last");
                const std::array fields{ruvia::http_header_view("x-upload-checksum", "finish-race")};
                stage = "upload-end";
                co_await upload.body().end(fields);
                stage = "upload-response";
                auto response = co_await upload.response();
                stage = "upload-response-body";
                const auto text = co_await response.text();
                EXPECT_EQ(text.text(), "uploaded:4:finish-race");
                EXPECT_EQ(response.trailer("x-response-trailer"), std::optional<std::string_view>{"verified"});
                stage = "tunnel-open";
                auto connected = co_await client.connect("echo.test:443").open();
                auto* tunnel = connected.tunnel();
                EXPECT_NE(tunnel, nullptr);
                if (!tunnel) {
                    throw std::runtime_error("HTTP/3 finish race requires an accepted tunnel");
                }
                stage = "tunnel-write";
                co_await tunnel->write("half-close");
                stage = "tunnel-finish";
                co_await tunnel->finish();
                stage = "tunnel-read-eof";
                std::string echoed;
                while (const auto chunk = co_await tunnel->text()) {
                    echoed.append(*chunk);
                }
                EXPECT_EQ(echoed, "half-close");
            }
        }
        bool timed_out = false;
        try {
            auto request = client.get("/connection-silent");
            request.configure({.timeouts_ = {.read_ = ruvia::http_client_timeout_override::after(10ms)}});
            (void)co_await request.send();
        } catch (const ruvia::http_client_error& error) {
            timed_out = error.code() == std::make_error_code(std::errc::timed_out);
            const auto* information = error.connection_info();
            EXPECT_NE(information, nullptr);
            if (information) {
                EXPECT_NE(information->id_, 0U);
                EXPECT_EQ(information->peer_ip_.to_string(), "127.0.0.1");
                EXPECT_EQ(information->peer_port_, port);
            }
        }
        EXPECT_TRUE(timed_out);
        stage = "dns-stream-head";
        auto streaming = co_await client.get("/stream").send();
        const auto draining_id = streaming.connection_info().id_;
        const std::array candidates{
            ruvia::http_client_endpoint_address{"127.0.0.2", port},
            ruvia::http_client_endpoint_address{"127.0.0.1", port}};
        client.update_dns(candidates);
        EXPECT_EQ(client.dns_snapshot().size(), 2U);
        std::exception_ptr replacement_failure;
        auto replace = [&]() -> ruvia::task<void> {
            try {
                auto updated = co_await client.get("/hello").send();
                EXPECT_NE(updated.connection_info().id_, draining_id);
                EXPECT_EQ(updated.connection_info().peer_ip_.to_string(), "127.0.0.1");
                EXPECT_EQ(updated.connection_info().peer_port_, port);
                (void)co_await updated.text();
            } catch (...) {
                replacement_failure = std::current_exception();
            }
        };
        ruvia::task_scope replacement_tasks(loop.worker());
        replacement_tasks.spawn(replace());
        // 旧响应仍占唯一连接容量；新准入与旧正文消费并行，而非相互等待。
        stage = "dns-draining-body";
        std::exception_ptr drain_failure;
        try {
            const auto streamed_text = co_await streaming.text();
            std::string expected;
            for (int chunk = 0; chunk < stream_chunks; ++chunk) {
                expected.append("chunk-");
                expected.append(std::to_string(chunk));
                expected.push_back(';');
            }
            EXPECT_EQ(streamed_text.text(), expected);
            EXPECT_EQ(streaming.connection_info().id_, draining_id);
        } catch (...) {
            drain_failure = std::current_exception();
        }
        co_await replacement_tasks.join();
        if (drain_failure) {
            std::rethrow_exception(drain_failure);
        }
        if (replacement_failure) {
            stage = "dns-replacement";
            std::rethrow_exception(replacement_failure);
        }
        stage = "dns-explicit-refresh";
        co_await client.refresh_dns();
        const auto snapshot = client.dns_snapshot();
        EXPECT_EQ(snapshot.size(), 1U);
        if (!snapshot.empty()) {
            EXPECT_EQ(snapshot.front().ip_.to_string(), "127.0.0.1");
            EXPECT_EQ(snapshot.front().port_.value(), port);
        }
        stage = "dns-policy-rejection";
        allowed = false;
        bool rejected = false;
        try {
            (void)co_await client.get("/hello").configure({.fresh_connection_ = true}).send();
        } catch (const ruvia::http_client_error& error) {
            rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::resolve_failed);
        }
        EXPECT_TRUE(rejected);
        EXPECT_GT(validations, 0U);
    } catch (const std::exception& error) {
        failure = std::make_exception_ptr(std::runtime_error(
            "connection controls versions=" + std::string(protocol.http1_ ? "1" : "") + (protocol.http2_ ? "2" : "") + (protocol.http3_ ? "3" : "") +
            " stage=" + std::string(stage) + " attempt=" + std::to_string(finish_attempt) + ": " + error.what()));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await owner.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
    allowed = true;
    config.dns_refresh_interval_ = 10ms;
    ruvia::http_client refreshing(loop, config);
    failure = nullptr;
    try {
        const std::array initial{ruvia::http_client_endpoint_address{"127.0.0.2", port}};
        auto refreshed = refreshing.client();
        refreshed.update_dns(initial);
        // Background refresh only looks up DNS dial names that still have a
        // live connection: this literal dial host is never queried, so the
        // published snapshot stays.
        (void)co_await ruvia::sleep_for(loop.worker(), 100ms);
        const auto snapshot = refreshed.dns_snapshot();
        EXPECT_TRUE(snapshot.size() == 1 && snapshot.front().ip_.to_string() == "127.0.0.2");
        EXPECT_EQ(refreshed.stats().dns_refreshes_, 0U);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await refreshing.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ruvia::task<void> exercise_mixed_protocol_worker(ruvia::event_loop loop, std::size_t loop_idx,
    std::uint16_t port, const std::filesystem::path& ca_file,
    const std::string& expected_stream, const std::string& expected_file,
    std::atomic<std::size_t>& completed_ops) {
    const std::string base_url("https://localhost:" + std::string(std::to_string(port)));
    const auto make_client_config = [&](ruvia::http_client_versions protocol) {
        ruvia::http_client_config cfg{.base_url_ = base_url};
        cfg.versions_ = protocol;
        cfg.connection_limits_.max_open_ = protocol == ruvia::http_client_versions{.http2_ = false} ? 4 : 1;
        cfg.connect_timeout_ = 5s;
        cfg.acquire_timeout_ = 5s;
        cfg.read_timeout_ = 5s;
        cfg.write_timeout_ = 5s;
        cfg.request_timeout_ = 10s;
        cfg.tls_.ca_file_ = ca_file;
        cfg.push_.enabled_ = protocol == ruvia::http_client_versions{.http1_ = false};
        cfg.advertisements_.receive_origins_ = protocol != ruvia::http_client_versions{.http2_ = false};
        return cfg;
    };

    ruvia::http_client h1_owner(loop, make_client_config(ruvia::http_client_versions{.http2_ = false}));
    std::optional<ruvia::http_client> h2_owner;
#ifdef RUVIA_ENABLE_HTTP2
    h2_owner.emplace(loop, make_client_config(ruvia::http_client_versions{.http1_ = false}));
#endif
    ruvia::http_client h3_owner(loop, make_client_config(ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}));
    ruvia::http_client neg_owner(loop, make_client_config(ruvia::http_client_versions{.http3_ = true}));
    ruvia::http_client tcp_owner(loop, make_client_config(ruvia::http_client_versions{}));
    std::exception_ptr worker_failure;
    try {
        if (loop_idx == 0) {
            co_await exercise_connection_controls(loop, port, ca_file, ruvia::http_client_versions{.http2_ = false});
#ifdef RUVIA_ENABLE_HTTP2
            co_await exercise_connection_controls(loop, port, ca_file, ruvia::http_client_versions{.http1_ = false});
#endif
            co_await exercise_connection_controls(loop, port, ca_file, ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true});
            co_await ruvia::test::client_operations::exercise(h3_owner.client(), loop.worker());
        }
        {
            if (h2_owner) {
                auto warm_h2 = co_await h2_owner->client().get("/hello").send();
                EXPECT_TRUE(warm_h2.ok());
                (void)co_await warm_h2.text();
            }
            auto warm_h3 = co_await h3_owner.client().get("/hello").send();
            EXPECT_TRUE(warm_h3.ok());
            (void)co_await warm_h3.text();
        }
        ruvia::task_scope tasks(loop.worker());
        for (int lane = 0; lane < 2; ++lane) {
            tasks.spawn(run_protocol_lane(h1_owner.client(), ruvia::http_protocol_version::http11,
                loop_idx, lane, expected_stream, expected_file, completed_ops));
        }
        for (int lane = 0; lane < 4; ++lane) {
            if (h2_owner) {
                tasks.spawn(run_protocol_lane(h2_owner->client(), ruvia::http_protocol_version::http2,
                    loop_idx, lane, expected_stream, expected_file, completed_ops));
            }
            tasks.spawn(run_protocol_lane(h3_owner.client(), ruvia::http_protocol_version::http3,
                loop_idx, lane, expected_stream, expected_file, completed_ops));
        }
        tasks.spawn(run_cancellation_and_migration_lane(
            loop.worker(), h2_owner ? std::optional(h2_owner->client()) : std::nullopt, h3_owner.client()));
        tasks.spawn(run_negotiate_upgrade_lane(neg_owner.client()));
        tasks.spawn(run_negotiate_tcp_lane(tcp_owner.client()));
        co_await tasks.join();
    } catch (...) {
        worker_failure = std::current_exception();
    }
    co_await neg_owner.shutdown();
    co_await tcp_owner.shutdown();
    co_await h3_owner.shutdown();
    if (h2_owner) {
        co_await h2_owner->shutdown();
    }
    co_await h1_owner.shutdown();
    if (worker_failure) {
        std::rethrow_exception(worker_failure);
    }
}

ruvia::task<void> run_goaway_reconnect_request(ruvia::http_client_ref client,
    ruvia::http_protocol_version expected_version) {
    auto resp = co_await client.get("/hello").send();
    EXPECT_TRUE(resp.ok());
    EXPECT_TRUE(resp.protocol_version() == expected_version);
    auto text = co_await resp.text();
    EXPECT_EQ(text.text(), "hello over http/3");
}

ruvia::task<void> exercise_capped_goaway_worker(ruvia::event_loop loop, std::uint16_t port,
    const std::filesystem::path& ca_file) {
    const std::string base_url("https://localhost:" + std::string(std::to_string(port)));
    const auto make_cfg = [&](ruvia::http_client_versions protocol) {
        ruvia::http_client_config cfg{.base_url_ = base_url};
        cfg.versions_ = protocol;
        cfg.connection_limits_.max_open_ = 1;
        cfg.connect_timeout_ = 5s;
        cfg.acquire_timeout_ = 5s;
        cfg.request_timeout_ = 10s;
        cfg.tls_.ca_file_ = ca_file;
        return cfg;
    };
    std::optional<ruvia::http_client> h2_owner;
#ifdef RUVIA_ENABLE_HTTP2
    h2_owner.emplace(loop, make_cfg(ruvia::http_client_versions{.http1_ = false}));
#endif
    ruvia::http_client h3_owner(loop, make_cfg(ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}));
    std::exception_ptr err;
    try {
        {
            if (h2_owner) {
                auto w2 = co_await h2_owner->client().get("/hello").send();
                EXPECT_TRUE(w2.ok());
                (void)co_await w2.text();
            }
            auto w3 = co_await h3_owner.client().get("/hello").send();
            EXPECT_TRUE(w3.ok());
            (void)co_await w3.text();
        }
        for (int round = 0; round < 4; ++round) {
            ruvia::task_scope tasks(loop.worker());
            for (int lane = 0; lane < 2; ++lane) {
                if (h2_owner) {
                    tasks.spawn(run_goaway_reconnect_request(
                        h2_owner->client(), ruvia::http_protocol_version::http2));
                }
                tasks.spawn(run_goaway_reconnect_request(
                    h3_owner.client(), ruvia::http_protocol_version::http3));
            }
            co_await tasks.join();
        }
    } catch (...) {
        err = std::current_exception();
    }
    co_await h3_owner.shutdown();
    if (h2_owner) {
        co_await h2_owner->shutdown();
    }
    if (err) {
        std::rethrow_exception(err);
    }
}

ruvia::test::http3_test_client connect(std::uint16_t port, const ruvia::test::tls_identity& identity) {
    return ruvia::test::http3_test_client(
        {asio::ip::address_v4::loopback(), port}, identity.ca_file_, "localhost", client_timeout);
}

ruvia::test::http3_raw_request_client connect_raw(std::uint16_t port, const ruvia::test::tls_identity& identity) {
    return ruvia::test::http3_raw_request_client(
        {asio::ip::address_v4::loopback(), port}, identity.ca_file_, "localhost", client_timeout);
}

}  // namespace http3_serving_test

TEST(Ruvia, a_multi_worker_app_serves_http3_and_closes_it_on_stop) {
    using namespace http3_serving_test;
    http3_resumption_test::server::shared().stop();
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto& private_key = identity.private_key_file();
    file_path() = identity.ca_file_.parent_path() / "body.txt";
    {
        std::ofstream output(file_path(), std::ios::binary);
        const auto content = file_content();
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("cannot write the HTTP/3 test file");
        }
    }

    asio::io_context io;
    shared_port_reservation reservation(io);
    const auto port = reservation.port();
    echo_backends backends;

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 4, .max_buffered_body_bytes_ = 4 * 1024 * 1024})
        .listen({
            .address_ = "127.0.0.1",
            .https_ = port,
            .http3_ = port,
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
        })
        .on_start([&started] { started.release(); });
    ruvia::test::app_run run(app);
    EXPECT_TRUE(started.try_acquire_for(10s));

    {
        auto client = connect(port, identity);

        // A buffered body.
        const auto hello = client.request("GET", "/hello");
        EXPECT_EQ(hello.status_, 200);
        EXPECT_EQ(hello.body_, "hello over http/3");
        EXPECT_TRUE(!hello.header("alt-svc").has_value());

        // Request content larger than every flow-control window, echoed back.
        std::string upload(echo_bytes, '\0');
        for (std::size_t index = 0; index < upload.size(); ++index) {
            upload[index] = static_cast<char>('0' + index % 10);
        }
        const auto echoed = client.request(
            "POST", "/echo", {{"content-type", "application/octet-stream"}}, upload);
        EXPECT_EQ(echoed.status_, 200);
        EXPECT_EQ(echoed.body_.size(), upload.size());
        EXPECT_TRUE(echoed.body_ == upload);

        // A response stream, read chunk by chunk.
        const auto stream_id = client.start_request("GET", "/stream");
        const auto& head = client.wait_for_response_head(stream_id);
        EXPECT_EQ(head.status_, 200);
        std::string streamed{};
        while (auto chunk = client.read_body_chunk(stream_id, client_timeout)) {
            streamed += *chunk;
        }
        const auto stream_end = client.finish_response(stream_id);
        streamed += stream_end.body_;
        std::string expected_stream{};
        for (int chunk = 0; chunk < stream_chunks; ++chunk) {
            expected_stream += "chunk-" + std::string(std::to_string(chunk)) + ";";
        }
        EXPECT_EQ(streamed, expected_stream);
        EXPECT_TRUE(!stream_end.reset_error_.has_value());

        // A file body larger than the stream's output budget.
        const auto file = client.request("GET", "/file");
        EXPECT_EQ(file.status_, 200);
        EXPECT_EQ(file.header("content-length").value_or(std::string{}), std::string(std::to_string(file_bytes)));
        EXPECT_TRUE(file.body_ == file_content());

        const auto missing = client.request("GET", "/missing");
        EXPECT_EQ(missing.status_, 404);

        // RFC 9114 §4.3.1: :authority and Host must agree. The malformed
        // request is a stream error of type H3_MESSAGE_ERROR (§4.1.2); the
        // connection keeps serving.
        {
            auto raw = connect_raw(port, identity);
            const auto authority = "localhost:" + std::string(std::to_string(port));
            const auto malformed = raw.request({{":method", "GET"}, {":scheme", "https"}, {":authority", authority},
                {":path", "/hello"}, {"host", "attacker.example"}});
            EXPECT_EQ(malformed.reset_error_, std::optional(ruvia::http3_error_code::message_error));
            EXPECT_EQ(malformed.status_, 0);
            const auto served = raw.request({{":method", "GET"}, {":scheme", "https"}, {":authority", authority}, {":path", "/hello"}});
            EXPECT_EQ(served.status_, 200);
        }
    }

    {
        auto client = connect(port, identity);
        EXPECT_TRUE(client.peer_settings().enable_connect_protocol_);
        EXPECT_TRUE(client.peer_settings().h3_datagram_);
        const auto tunnel = client.start_tunnel(ruvia::http3_connect_request_head_view{.authority_ = "echo.test:443"});
        EXPECT_EQ(client.wait_for_response_head(tunnel).status_, 200);
        const std::string payload(128 * 1024, 't');
        client.send_stream_data(tunnel, byte_view(payload), true);
        const auto echo = client.finish_response(tunnel);
        EXPECT_EQ(echo.body_, payload);
        EXPECT_TRUE(!echo.reset_error_);
        EXPECT_EQ(client.request("GET", "/hello").status_, 200);
        const auto cancelled = client.start_tunnel(ruvia::http3_connect_request_head_view{.authority_ = "echo.test:443"});
        EXPECT_EQ(client.wait_for_response_head(cancelled).status_, 200);
        client.reset_request(cancelled, ruvia::http3_error_code::request_cancelled);
        EXPECT_EQ(client.request("GET", "/hello").status_, 200);

        constexpr std::array websocket_fields{ruvia::http_header_view{"sec-websocket-version", "13"}};
        const auto websocket = client.start_tunnel(ruvia::http3_extended_connect_request_head_view{
            .protocol_ = "websocket", .authority_ = "localhost", .target_ = "/websocket", .headers_ = websocket_fields});
        const auto& head = client.wait_for_response_head(websocket);
        EXPECT_EQ(head.status_, 200);
        EXPECT_TRUE(!head.header("sec-websocket-accept"));
        EXPECT_TRUE(!head.header("upgrade"));
        const auto frame = masked_frame(1, "websocket-echo");
        client.send_stream_data(websocket, byte_view(frame));
        const auto echoed = read_tunnel_bytes(client, websocket, 16);
        EXPECT_EQ(echoed, std::string("\x81\x0e"
                                      "websocket-echo",
                              16));
        const auto close = masked_frame(8, std::string("\x03\xe8", 2));
        client.send_stream_data(websocket, byte_view(close), true);
        const auto closed = client.finish_response(websocket);
        EXPECT_EQ(closed.body_, std::string("\x88\x02\x03\xe8", 4));
        EXPECT_TRUE(!closed.reset_error_);
    }

    constexpr std::array udp_fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
    const ruvia::http3_extended_connect_request_head_view udp_head{
        .protocol_ = "connect-udp", .authority_ = "localhost", .target_ = "/.well-known/masque/udp/echo.test/53/", .headers_ = udp_fields};
    {
        auto client = connect(port, identity);
        const auto tunnel = client.start_tunnel(udp_head);
        const auto& head = client.wait_for_response_head(tunnel);
        EXPECT_EQ(head.status_, 200);
        EXPECT_EQ(head.header("capsule-protocol").value_or(std::string{}), "?1");
        const std::string unknown("\x01ignored", 8);
        client.send_datagram(tunnel, byte_view(unknown));

        // A native packet may interrupt an incomplete reliable capsule.
        const auto capsule = udp_capsule("capsule");
        client.send_stream_data(tunnel, byte_view(std::string_view(capsule).substr(0, 1)));
        const std::string native("\0native", 7);
        client.send_datagram(tunnel, byte_view(native));
        EXPECT_EQ(client.receive_datagram(tunnel), native);
        client.send_stream_data(tunnel, byte_view(std::string_view(capsule).substr(1)));
        EXPECT_EQ(client.receive_datagram(tunnel), std::string("\0capsule", 8));
        const std::string empty(1, '\0');
        client.send_datagram(tunnel, byte_view(empty));
        EXPECT_EQ(client.receive_datagram(tunnel), empty);

        // Oversize UDP packets use reliable capsules, never truncated QUIC DATAGRAMs.
        const std::string large(4096, 'u');
        const auto large_capsule = udp_capsule(large);
        client.send_stream_data(tunnel, byte_view(large_capsule));
        EXPECT_EQ(read_tunnel_bytes(client, tunnel, large_capsule.size()), large_capsule);
        client.send_stream_data(tunnel, {}, true);
        EXPECT_TRUE(!client.finish_response(tunnel).reset_error_);
        EXPECT_EQ(client.request("GET", "/hello").status_, 200);
        const auto malformed = client.start_tunnel(udp_head);
        EXPECT_EQ(client.wait_for_response_head(malformed).status_, 200);
        const std::string truncated_context(1, static_cast<char>(0xff));
        client.send_datagram(malformed, byte_view(truncated_context));
        EXPECT_TRUE(client.finish_response(malformed).reset_error_.has_value());
        const auto cancelled = client.start_tunnel(udp_head);
        EXPECT_EQ(client.wait_for_response_head(cancelled).status_, 200);
        client.reset_request(cancelled, ruvia::http3_error_code::request_cancelled);
        EXPECT_EQ(client.request("GET", "/hello").status_, 200);
    }
    {
        ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port},
            identity.ca_file_, "localhost", client_timeout, nullptr, false, false);
        const auto tunnel = client.start_tunnel(udp_head);
        EXPECT_EQ(client.wait_for_response_head(tunnel).status_, 200);
        const auto capsule = udp_capsule("capsule-only");
        client.send_stream_data(tunnel, byte_view(capsule), true);
        const auto echo = client.finish_response(tunnel);
        EXPECT_EQ(echo.body_, capsule);
        EXPECT_TRUE(!echo.reset_error_);
    }

    {
        // Keep one connection through consecutive active migrations, then NAT
        // rebindings. ngtcp2 1.25.0 could retire the fallback CID when the next
        // migration began after new-path validation but before old-path probing
        // ended. Do not wait for those probes to expire between path changes.
        auto client = connect(port, identity);
        const auto* connection = client.native_handle();
        auto previous_stream = client.start_request("GET", "/worker");
        const auto initial = client.finish_response(previous_stream);
        EXPECT_EQ(initial.status_, 200);
        EXPECT_TRUE(!initial.reset_error_);
        EXPECT_TRUE(!initial.body_.empty());
        std::set<std::uint16_t> dcid_prefixes{};
        const auto record_dcid_prefix = [&] {
            const auto* dcid = ngtcp2_conn_get_dcid2(connection);
            ASSERT_NE(dcid, nullptr);
            ASSERT_GE(dcid->datalen, std::size_t{2});
            dcid_prefixes.insert(static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(dcid->data[0]) << 8) | static_cast<std::uint16_t>(dcid->data[1])));
        };
        record_dcid_prefix();
        for (const bool active_migration : {true, false}) {
            for (int change = 0; change < 16; ++change) {
                const auto previous_port = client.local_port();
                if (active_migration) {
                    client.migrate();
                    record_dcid_prefix();
                } else {
                    client.rebind();
                }
                EXPECT_TRUE(client.local_port() != previous_port);
                const auto stream = client.start_request("GET", "/worker");
                EXPECT_EQ(stream, previous_stream + 4);
                const auto response = client.finish_response(stream);
                EXPECT_EQ(response.status_, 200);
                EXPECT_TRUE(!response.reset_error_);
                EXPECT_EQ(response.body_, initial.body_);
                EXPECT_TRUE(client.native_handle() == connection);
                EXPECT_TRUE(!client.closed());
                previous_stream = stream;
            }
        }
        // RFC 9000 §9.5: server-issued CIDs for the same worker mask the owner
        // index per CID so path changes share no static wire prefix.
        EXPECT_GT(dcid_prefixes.size(), std::size_t{1});
    }

    // nghttp3 treats a request it cannot frame (here a :path that is no
    // origin-form target) as a connection error of type H3_MESSAGE_ERROR, as
    // RFC 9114 §8 permits.
    {
        auto raw = connect_raw(port, identity);
        EXPECT_THROW((void)raw.request({{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "hello"}}),
            std::runtime_error);
        EXPECT_TRUE(raw.closed());
        const auto* close_error = ngtcp2_conn_get_ccerr2(raw.native_handle());
        EXPECT_TRUE(close_error != nullptr &&
                    close_error->error_code == static_cast<std::uint64_t>(ruvia::http3_error_code::message_error));
    }

    // The HTTPS listener advertises the HTTP/3 service (RFC 7838 §3).
    const auto https = https_head(io, port, identity.ca_file_);
    EXPECT_TRUE(https.starts_with("HTTP/1.1 200 OK"));
    EXPECT_TRUE(https.find("\r\nalt-svc: h3=\":" + std::string(std::to_string(port)) + "\"; ma=86400") != std::string::npos ||
                https.find("\r\nAlt-Svc: h3=\":" + std::string(std::to_string(port)) + "\"; ma=86400") != std::string::npos);

    // Fresh connections from fresh client ports: the kernel (POSIX) or the
    // acceptor (Windows) spreads them over the four workers, and each
    // connection's packets reach the worker that owns it.
    std::set<std::string> workers{};
    for (int connection = 0; connection < 50; ++connection) {
        auto client = connect(port, identity);
        const auto served = client.request("GET", "/worker");
        EXPECT_EQ(served.status_, 200);
        workers.insert(served.body_);
        client.close();
    }
    EXPECT_TRUE(workers.size() > 1);

    // High-concurrency mixed-protocol E2E stress across HTTP/1.1, HTTP/2,
    // HTTP/3, and Alt-Svc negotiation: buffered/streaming/file requests,
    // PRIORITY_UPDATE, WebSocket, TCP CONNECT, connect-udp datagrams,
    // mid-flight request cancellation, and live QUIC connection migration.
    {
        const std::string expected_file(file_content());
        std::string expected_stream{};
        for (int chunk = 0; chunk < stream_chunks; ++chunk) {
            expected_stream += "chunk-" + std::string(std::to_string(chunk)) + ";";
        }
        ruvia::blocking_pool client_blocking;
        ruvia::event_loop_pool client_loops({.loop_count_ = 4, .blocking_pool_ = &client_blocking});
        client_loops.start();
        try {
            std::atomic<std::size_t> completed_ops{0};
            std::vector<ruvia::root_task<void>> loop_futures{};
            loop_futures.reserve(4);
            for (std::size_t loop_idx = 0; loop_idx < 4; ++loop_idx) {
                auto loop = client_loops.loop(loop_idx);
                loop_futures.push_back(loop.start(exercise_mixed_protocol_worker(
                    loop, loop_idx, port, identity.ca_file_, expected_stream, expected_file,
                    completed_ops)));
            }
            for (auto& fut : loop_futures) {
                std::move(fut).get();
            }
#ifdef RUVIA_ENABLE_HTTP2
            constexpr std::size_t lanes_per_worker = 2 + 4 + 4;
#else
            constexpr std::size_t lanes_per_worker = 2 + 4;
#endif
            EXPECT_EQ(completed_ops.load(), std::size_t{4 * lanes_per_worker * 3});
        } catch (...) {
            client_loops.stop();
            client_loops.join();
            throw;
        }
        client_loops.stop();
        client_loops.join();
    }

    // Direct disconnect: the stop closes the connection at once with
    // CONNECTION_CLOSE(H3_NO_ERROR) while its request is still in flight,
    // without GOAWAY.
    {
        auto client = connect(port, identity);
        (void)client.start_request("GET", "/slow");
        // The stop begins while the request's handler runs.
        EXPECT_TRUE(client.wait_until([] { return slow_entered().load(); }, client_timeout));
        std::thread stopper([&app] { app.stop(); });
        EXPECT_TRUE(client.wait_until([&client] { return client.closed(); }, client_timeout));
        stopper.join();
        EXPECT_TRUE(client.goaway_ids().empty());
        EXPECT_EQ(client.application_error(), std::optional(ruvia::http3_error_code::no_error));
        EXPECT_FALSE(client.transport_error().has_value());
    }
    run.join();
    backends.finish();

    // A connection that reaches max_requests_per_connection_ still ends
    // gracefully (RFC 9114 §5.2): its requests are answered, the shutdown
    // notice and the final GOAWAY follow, then CONNECTION_CLOSE(H3_NO_ERROR).
    app.server({.worker_count_ = 4, .max_requests_per_connection_ = 2});
    ruvia::test::app_run capped(app);
    EXPECT_TRUE(started.try_acquire_for(10s));
    {
        auto client = connect(port, identity);
        EXPECT_EQ(client.request("GET", "/hello").status_, 200);
        const auto last = client.start_request("GET", "/hello");
        const auto response = client.finish_response(last);
        EXPECT_EQ(response.status_, 200);
        EXPECT_EQ(response.body_, "hello over http/3");
        EXPECT_TRUE(client.wait_until([&client] { return client.closed(); }, client_timeout));
        const auto& goaways = client.goaway_ids();
        EXPECT_EQ(goaways.size(), std::size_t{2});
        if (goaways.size() == 2) {
            EXPECT_FALSE(goaways.front().has_value());
            EXPECT_TRUE(goaways.back().has_value() && *goaways.back() > last);
        }
        EXPECT_EQ(client.application_error(), std::optional(ruvia::http3_error_code::no_error));
        EXPECT_FALSE(client.transport_error().has_value());
    }
    {
        ruvia::blocking_pool capped_blocking;
        ruvia::event_loop_pool capped_loops({.loop_count_ = 2, .blocking_pool_ = &capped_blocking});
        capped_loops.start();
        try {
            std::vector<ruvia::root_task<void>> futures{};
            futures.reserve(2);
            for (std::size_t idx = 0; idx < 2; ++idx) {
                auto loop = capped_loops.loop(idx);
                futures.push_back(loop.start(exercise_capped_goaway_worker(
                    loop, port, identity.ca_file_)));
            }
            for (auto& f : futures) {
                std::move(f).get();
            }
        } catch (...) {
            capped_loops.stop();
            capped_loops.join();
            throw;
        }
        capped_loops.stop();
        capped_loops.join();
    }
    app.stop();
    capped.join();
}
