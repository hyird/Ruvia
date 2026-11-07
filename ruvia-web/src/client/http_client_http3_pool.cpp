#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/http_tunnel.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {
#ifdef RUVIA_ENABLE_HTTP3
namespace {
std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}
// RFC 7838 field members: commas and semicolons inside quoted strings do not
// delimit members. Unsupported ALPN identifiers are skipped, never downgraded.
std::string_view take_component(std::string_view& text, char delimiter) noexcept {
    bool quoted = false, escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && value == delimiter) {
            const auto result = text.substr(0, index);
            text.remove_prefix(index + 1);
            return trim_ows(result);
        }
    }
    const auto result = trim_ows(text);
    text = {};
    return result;
}
}  // namespace
#endif

void http_client_pool::observe_alt_svc(std::span<const http_header> fields) {
#ifdef RUVIA_ENABLE_HTTP3
    if (config_.protocol_ != http_client_protocol::negotiate || config_.scheme_ != http_scheme::https) {
        return;
    }
    bool first = true;
    for (const auto& field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "alt-svc")) {
            continue;
        }
        if (first) {
            alternative_services_.clear();
            first = false;
        }
        auto value = trim_ows(field.value());
        if (value == "clear") {
            alternative_services_.clear();
            return;
        }
        while (!value.empty() && alternative_services_.size() < config_.http3_.max_alt_svc_entries_) {
            auto member = take_component(value, ',');
            auto authority = take_component(member, ';');
            const auto equals = authority.find('=');
            if (equals == std::string_view::npos || trim_ows(authority.substr(0, equals)) != "h3") {
                continue;
            }
            authority = trim_ows(authority.substr(equals + 1));
            if (authority.size() < 4 || authority.front() != '"' || authority.back() != '"') {
                continue;
            }
            authority.remove_prefix(1);
            authority.remove_suffix(1);
            if (authority.find_first_of("\\\" \t\r\n/@?#") != std::string_view::npos) {
                continue;
            }
            const auto colon = authority.rfind(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            auto host = authority.substr(0, colon);
            const auto port_text = authority.substr(colon + 1);
            unsigned port = 0;
            const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port == 0 || port > 65535) {
                continue;
            }
            if (!host.empty() && host.front() == '[' && host.back() == ']') {
                host.remove_prefix(1);
                host.remove_suffix(1);
            } else if (host.find(':') != std::string_view::npos) {
                continue;
            }
            if (host.empty()) {
                host = config_.host_;
            }
            if (host.size() > 253) {
                continue;
            }
            std::uint64_t age = 86400;
            bool valid = true;
            bool age_seen = false;
            while (!member.empty()) {
                const auto parameter = take_component(member, ';');
                const auto separator = parameter.find('=');
                if (separator == std::string_view::npos) {
                    continue;
                }
                if (!http_ascii_equals_ignore_case(trim_ows(parameter.substr(0, separator)), "ma")) {
                    continue;
                }
                if (age_seen) {
                    valid = false;
                    break;
                }
                age_seen = true;
                const auto digits = trim_ows(parameter.substr(separator + 1));
                const auto number = std::from_chars(digits.data(), digits.data() + digits.size(), age);
                if (number.ec != std::errc{} || number.ptr != digits.data() + digits.size()) {
                    valid = false;
                    break;
                }
            }
            if (!valid || age == 0) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::steady_clock::time_point::max)() - now).count();
            const auto seconds = (std::min)(age, static_cast<std::uint64_t>(maximum));
            alternative_services_.push_back({std::pmr::string(host, resource_), static_cast<std::uint16_t>(port), now + std::chrono::seconds(seconds)});
        }
    }
#else
    (void)fields;
#endif
}

#ifdef RUVIA_ENABLE_HTTP3
task<bool> http_client_pool::execute_http3(const http_client_request_storage& request,
    const operation_timeout& timeout, const operation_timeout& acquire_timeout, stop_token stop,
    http_client_response_state& state) {
    if (slots_.closing()) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is closed");
    }
    const bool required = config_.protocol_ == http_client_protocol::http3_only;
    if (!required && config_.protocol_ != http_client_protocol::negotiate) {
        co_return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(alternative_services_, [now](const alternative_service& service) { return service.expires_ <= now; });
    if (!required && alternative_services_.empty()) {
        co_return false;
    }
    // Another response can replace the cache while DNS or the handshake waits.
    // The selected endpoint is therefore owned across suspension; TLS always
    // verifies config_.host_, never this advertised transport name.
    std::pmr::string host(required ? std::string_view(config_.host_) : std::string_view(alternative_services_.front().host_), resource_);
    const auto port = required ? config_.port_ : alternative_services_.front().port_;
    if (http3_connections_.empty()) {
        http3_connections_.resize(config_.connection_count_);
    }
    std::size_t selected = 0;
    std::size_t capacity = 0;
    for (std::size_t index = 0; index < http3_connections_.size(); ++index) {
        const auto available = http3_connections_[index] ? http3_connections_[index]->available() : config_.http3_.max_concurrent_streams_;
        if (available > capacity) {
            capacity = available;
            selected = index;
        }
    }
    if (!http3_connections_[selected]) {
        http3_connections_[selected] = make_pmr_object<http3_client_connection>(resource_, *this, selected);
    }
    const auto used = co_await http3_connections_[selected]->execute(request, host, port, !required, timeout, acquire_timeout, stop, state);
    if (!used) {
        std::erase_if(alternative_services_, [&](const alternative_service& service) { return service.host_ == host && service.port_ == port; });
    }
    co_return used;
}

task<std::optional<http_tunnel_input>> http_client_pool::read_http3_tunnel_input(http_client_response_state& state) {
    auto lane = claim_operation_lane(state.body_operation_active_, "HTTP/3 tunnel input is already active");
    for (;;) {
        state.release_consumed_body_prefix();
        if (state.offset_ < state.buffered_.size()) {
            http_tunnel_input result{std::pmr::string(std::string_view(state.buffered_).substr(state.offset_), state.resource_), false};
            state.offset_ = state.buffered_.size();
            co_return result;
        }
        if (!state.pending_.empty()) {
            http_tunnel_input result{std::pmr::string(state.resource_), false};
            result.bytes_.swap(state.pending_);
            release_response_data(state);
            co_return result;
        }
        if (state.http3_datagrams_ && !state.http3_datagrams_->empty()) {
            http_tunnel_input result{std::move(state.http3_datagrams_->front()), true};
            state.http3_datagram_bytes_ -= result.bytes_.size();
            state.http3_datagrams_->pop_front();
            co_return result;
        }
        state.rethrow_response_failure();
        if (state.pool_closed_ || state.abandoned_) {
            throw http_client_error(http_client_error::code_type::closing, "HTTP/3 tunnel input closed");
        }
        if (state.receive_complete()) {
            co_return std::nullopt;
        }
        co_await state.data_signal_.wait();
    }
}

task<bool> http_client_pool::send_http3_datagram(http_client_response_state& state, std::string_view payload) {
    if (!state.tunnel_ || state.transport_ != http_client_response_transport::http3 || state.connection_index_ >= http3_connections_.size() || !http3_connections_[state.connection_index_]) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP/3 tunnel output closed");
    }
    auto lane = claim_operation_lane(state.tunnel_->output_.operation_active_, "HTTP/3 tunnel output is already active");
    co_return co_await http3_connections_[state.connection_index_]->send_datagram(state, std::as_bytes(std::span(payload.data(), payload.size())));
}
#endif

task<void> http_client_pool::migrate_http3(std::string_view local_address, std::uint16_t port) {
    return migrate_http3_owned(std::string(local_address), port);
}

task<void> http_client_pool::migrate_http3_owned(std::string local_address, std::uint16_t port) {
#ifdef RUVIA_ENABLE_HTTP3
    require_accepting();
    if (slots_.closing()) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is closed");
    }
    bool migrated = false;
    for (auto& connection : http3_connections_) {
        if (connection && connection->active()) {
            co_await connection->migrate(local_address, port);
            migrated = true;
        }
    }
    if (!migrated) {
        throw http_client_error(http_client_error::code_type::protocol_unavailable, "no active HTTP/3 connection to migrate");
    }
#else
    (void)local_address;
    (void)port;
    throw http_client_error(http_client_error::code_type::protocol_unavailable, "HTTP/3 is disabled");
    co_return;
#endif
}
}  // namespace ruvia::detail
