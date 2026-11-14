// HTTP/2 sans-I/O connection core.
//
// A pure protocol state machine: it never touches a socket, a coroutine, a timer,
// or asio. Inbound bytes go to one nghttp2_session (nghttp2_session_mem_recv2),
// which owns framing, HPACK, stream states, flow control and SETTINGS/PING/GOAWAY
// handling; its callbacks feed Ruvia's message semantics (field validation,
// content accounting, CONNECT/tunnel phases, leases) and publish typed events.
// Every operation drains nghttp2_session_mem_send2 eagerly into the
// frame-segmented output buffer, so the owner sees whole frames through
// take_output_batch(). The I/O loop, concurrency model and timeouts live entirely
// in the caller.
//
// nghttp2 is a C library: exceptions never cross its frames. A callback that
// throws records the exception and fails the nghttp2 call; the connection is
// then failed and the exception rethrown once nghttp2 has returned. Allocation
// failure inside nghttp2 is likewise fatal for the connection.

#include "ruvia/http/http2_connection.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
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

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"

#include "http_connection_fields.h"

namespace ruvia::detail {

struct http2_stored_header_view final {
    std::string_view name_;
    std::string_view value_;
    http_field_kind kind_{http_field_kind::other};
};

class http2_header_list final {
public:
    http2_header_list() noexcept = default;

    http2_header_list(const http2_header_list&) = delete;
    http2_header_list& operator=(const http2_header_list&) = delete;
    http2_header_list(http2_header_list&&) = delete;
    http2_header_list& operator=(http2_header_list&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] bool full() const noexcept {
        return count_ == (std::numeric_limits<std::uint16_t>::max)();
    }

    [[nodiscard]] http2_stored_header_view at(std::size_t index) const& noexcept {
        const auto& field = index < inline_header_fields ? inline_fields_[index] : overflow_fields_[index - inline_header_fields];
        return http2_stored_header_view{.name_ = view(field.name_offset_, field.name_size_), .value_ = view(field.value_offset_, field.value_size_), .kind_ = field.kind_};
    }
    [[nodiscard]] http2_stored_header_view at(std::size_t) const&& = delete;

    [[nodiscard]] bool append(std::string_view name, std::string_view value, http_field_kind kind) {
        if (full() || name.size() > max_stored_header_view_size || value.size() > max_stored_header_view_size || storage_size_ > max_stored_header_view_size - name.size() || storage_size_ + name.size() > max_stored_header_view_size - value.size()) {
            return false;
        }

        if (count_ >= inline_header_fields) {
            reserve_geometric(overflow_fields_, overflow_fields_.size() + 1);
        }
        const auto field_bytes = name.size() + value.size();
        if (using_overflow_storage_ || field_bytes > inline_header_storage_bytes - storage_size_) {
            ensure_overflow_storage(field_bytes);
        }
        const auto name_offset = static_cast<std::uint32_t>(storage_size_);
        append_bytes(name);
        const auto value_offset = static_cast<std::uint32_t>(storage_size_);
        append_bytes(value);

        const auto field = header_field_type{.name_offset_ = name_offset, .name_size_ = static_cast<std::uint32_t>(name.size()), .value_offset_ = value_offset, .value_size_ = static_cast<std::uint32_t>(value.size()), .kind_ = kind};

        if (count_ < inline_header_fields) {
            inline_fields_[count_] = field;
        } else {
            overflow_fields_.push_back(field);
        }
        ++count_;
        return true;
    }

private:
    static constexpr std::size_t inline_header_fields = 16;
    static constexpr std::size_t inline_header_storage_bytes = 512;
    static constexpr std::size_t max_stored_header_view_size = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

    struct header_field_type final {
        std::uint32_t name_offset_{0};
        std::uint32_t name_size_{0};
        std::uint32_t value_offset_{0};
        std::uint32_t value_size_{0};
        http_field_kind kind_{http_field_kind::other};
    };

    [[nodiscard]] std::string_view view(std::uint32_t offset, std::uint32_t size) const noexcept {
        return std::string_view(storage_data() + offset, size);
    }

    [[nodiscard]] const char* storage_data() const noexcept {
        return using_overflow_storage_ ? overflow_storage_.data() : inline_storage_.data();
    }

    void append_bytes(std::string_view value) {
        if (value.empty()) {
            return;
        }
        if (!using_overflow_storage_) {
            std::memcpy(inline_storage_.data() + storage_size_, value.data(), value.size());
            storage_size_ += value.size();
            return;
        }
        overflow_storage_.append(value.data(), value.size());
        storage_size_ += value.size();
    }

    void ensure_overflow_storage(std::size_t additional_bytes) {
        reserve_geometric(overflow_storage_, storage_size_ + additional_bytes);
        if (using_overflow_storage_) {
            return;
        }
        overflow_storage_.append(inline_storage_.data(), storage_size_);
        using_overflow_storage_ = true;
    }

    std::array<char, inline_header_storage_bytes> inline_storage_;
    std::string overflow_storage_;
    std::array<header_field_type, inline_header_fields> inline_fields_;
    std::vector<header_field_type> overflow_fields_;
    std::size_t storage_size_{0};
    std::uint16_t count_{0};
    bool using_overflow_storage_{false};
};

class http2_stream_state;

// Only abnormal whole-stream termination owns a close source. This covers a local
// or peer RST_STREAM as well as a GOAWAY last-stream-id exclusion; GOAWAY is not a
// reset, so naming this alternative after RST_STREAM would be protocol-inaccurate.
// Open and normally ended states cannot expose a close source.
class http2_stream_aborted final {
public:
    [[nodiscard]] constexpr http2_stream_close_source source() const noexcept {
        return source_;
    }

private:
    friend class http2_local_send_state;

    explicit constexpr http2_stream_aborted(http2_stream_close_source source_value) noexcept :
        source_(source_value) {}

    http2_stream_close_source source_;
};

// Local frame permission is one exclusive protocol state. Request content,
// response content and tunnel DATA are deliberately distinct, so a separate
// message-kind enum cannot contradict the active phase.
class http2_local_send_state final {
    enum class phase_type : std::uint8_t {
        head_pending,
        request_content_open,
        response_content_open,
        connect_pending,
        tunnel_open,
        end_stream_committed,
    };
    friend class http2_stream_state;

    constexpr http2_local_send_state() noexcept = default;

    [[nodiscard]] bool begin_request_content() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::request_content_open;
        return true;
    }

    [[nodiscard]] bool begin_response_content() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::response_content_open;
        return true;
    }

    [[nodiscard]] bool begin_connect_request() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::connect_pending;
        return true;
    }

    // A client opens from connect-pending after a peer 2xx; a server opens from
    // head-pending when it submits that 2xx. The owning stream validates the
    // separate CONNECT semantic state before calling this transition.
    [[nodiscard]] bool open_tunnel() noexcept {
        if (head_pending() == nullptr && connect_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::tunnel_open;
        return true;
    }

    [[nodiscard]] bool commit_head_end_stream() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::end_stream_committed;
        return true;
    }

    [[nodiscard]] bool reject_connect() noexcept {
        if (connect_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::end_stream_committed;
        return true;
    }

    [[nodiscard]] bool commit_end_stream() noexcept {
        if (!content_open()) {
            return false;
        }
        state_ = phase_type::end_stream_committed;
        return true;
    }

    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        if (!http2_is_valid_stream_close_source(source_value) || aborted() != nullptr) {
            return false;
        }
        state_ = http2_stream_aborted(source_value);
        return true;
    }

public:
    [[nodiscard]] constexpr const void* head_pending() const& noexcept {
        return has_phase(phase_type::head_pending) ? this : nullptr;
    }
    const void* head_pending() const&& = delete;

    [[nodiscard]] constexpr const void* request_content_open() const& noexcept {
        return has_phase(phase_type::request_content_open) ? this : nullptr;
    }
    const void* request_content_open() const&& = delete;

    [[nodiscard]] constexpr const void* response_content_open() const& noexcept {
        return has_phase(phase_type::response_content_open) ? this : nullptr;
    }
    const void* response_content_open() const&& = delete;

    [[nodiscard]] constexpr const void* connect_pending() const& noexcept {
        return has_phase(phase_type::connect_pending) ? this : nullptr;
    }
    const void* connect_pending() const&& = delete;

    [[nodiscard]] constexpr const void* tunnel_open() const& noexcept {
        return has_phase(phase_type::tunnel_open) ? this : nullptr;
    }
    const void* tunnel_open() const&& = delete;

    [[nodiscard]] constexpr const void* end_stream_committed() const& noexcept {
        return has_phase(phase_type::end_stream_committed) ? this : nullptr;
    }
    const void* end_stream_committed() const&& = delete;

    [[nodiscard]] constexpr const http2_stream_aborted* aborted() const& noexcept {
        return std::get_if<http2_stream_aborted>(&state_);
    }
    const http2_stream_aborted* aborted() const&& = delete;

private:
    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&state_);
        return current != nullptr && *current == phase;
    }

    [[nodiscard]] bool content_open() const noexcept {
        return request_content_open() != nullptr || response_content_open() != nullptr || tunnel_open() != nullptr;
    }

    std::variant<phase_type, http2_stream_aborted> state_{phase_type::head_pending};
};

class http2_remote_content_allowed_without_length final {
public:
    [[nodiscard]] constexpr std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_allowed_without_length(std::size_t received_bytes = 0) noexcept :
        received_bytes_(received_bytes) {}

    std::size_t received_bytes_{0};
};

class http2_remote_content_allowed_known_length final {
public:
    [[nodiscard]] constexpr std::size_t declared_length() const noexcept {
        return declared_length_;
    }

    [[nodiscard]] constexpr std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_allowed_known_length(std::size_t declared_length, std::size_t received_bytes = 0) noexcept :
        declared_length_(declared_length),
        received_bytes_(received_bytes) {}

    std::size_t declared_length_;
    std::size_t received_bytes_{0};
};

class http2_remote_content_metadata_only_known_length final {
public:
    [[nodiscard]] constexpr std::size_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_metadata_only_known_length(std::size_t declared_length) noexcept :
        declared_length_(declared_length) {}

    std::size_t declared_length_;
};

enum class http2_remote_content_accounting_result : std::uint8_t { accepted,
    counter_overflow,
    declared_length_exceeded,
    content_forbidden };

// Content allowance, Content-Length ownership, and received-byte accounting are
// one exclusive state. HEAD/204/304 responses retain representation length
// metadata but cannot accidentally accept DATA as message content. account() is
// the only byte mutation: a rejected input leaves the active alternative intact.
class http2_remote_content_state final {
public:
    constexpr http2_remote_content_state() noexcept = default;

    [[nodiscard]] bool declare_known_length(std::size_t length) noexcept {
        if (const auto* known = allowed_known_length(); known != nullptr) {
            return known->declared_length() == length;
        }
        if (const auto* known = metadata_only_known_length(); known != nullptr) {
            return known->declared_length() == length;
        }
        if (const auto* allowed = allowed_without_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = http2_remote_content_allowed_known_length(length);
            return true;
        }
        if (metadata_only_without_length() != nullptr) {
            state_ = http2_remote_content_metadata_only_known_length(length);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool select_metadata_only() noexcept {
        if (metadata_only_without_length() != nullptr || metadata_only_known_length() != nullptr) {
            return true;
        }
        if (const auto* allowed = allowed_without_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = std::monostate{};
            return true;
        }
        if (const auto* allowed = allowed_known_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = http2_remote_content_metadata_only_known_length(allowed->declared_length());
            return true;
        }
        return false;
    }

    [[nodiscard]] http2_remote_content_accounting_result account(std::size_t bytes_value) noexcept {
        if (auto* allowed = std::get_if<http2_remote_content_allowed_without_length>(&state_); allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        if (auto* allowed = std::get_if<http2_remote_content_allowed_known_length>(&state_); allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            if (allowed->received_bytes_ > allowed->declared_length_ || bytes_value > allowed->declared_length_ - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::declared_length_exceeded;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        return bytes_value == 0 ? http2_remote_content_accounting_result::accepted : http2_remote_content_accounting_result::content_forbidden;
    }

    [[nodiscard]] constexpr const http2_remote_content_allowed_without_length* allowed_without_length() const& noexcept {
        return std::get_if<http2_remote_content_allowed_without_length>(&state_);
    }
    const http2_remote_content_allowed_without_length* allowed_without_length() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_allowed_known_length* allowed_known_length() const& noexcept {
        return std::get_if<http2_remote_content_allowed_known_length>(&state_);
    }
    const http2_remote_content_allowed_known_length* allowed_known_length() const&& = delete;

    [[nodiscard]] constexpr const void* metadata_only_without_length() const& noexcept {
        return std::holds_alternative<std::monostate>(state_) ? this : nullptr;
    }
    const void* metadata_only_without_length() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_metadata_only_known_length* metadata_only_known_length() const& noexcept {
        return std::get_if<http2_remote_content_metadata_only_known_length>(&state_);
    }
    const http2_remote_content_metadata_only_known_length* metadata_only_known_length() const&& = delete;

    [[nodiscard]] bool terminal_length_valid() const noexcept {
        const auto* known = allowed_known_length();
        return known == nullptr || known->received_bytes() == known->declared_length();
    }

private:
    std::variant<http2_remote_content_allowed_without_length, http2_remote_content_allowed_known_length, std::monostate, http2_remote_content_metadata_only_known_length> state_{http2_remote_content_allowed_without_length()};
};

// A server decoded CONNECT without END_STREAM and has not accepted or rejected it.
// DATA is forbidden until that decision selects tunnel or rejection semantics.
class http2_remote_connect_pending final {
public:
    [[nodiscard]] constexpr bool saw_tunnel_data() const noexcept {
        return saw_tunnel_data_;
    }

private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_connect_pending() noexcept = default;

    constexpr void mark_tunnel_data() noexcept {
        saw_tunnel_data_ = true;
    }

    bool saw_tunnel_data_{false};
};

// A server decoded CONNECT and then observed END_STREAM on its HEADERS or on a
// DATA frame. The peer send half is closed while the application still owns the
// accept/reject decision.
class http2_remote_connect_pending_end_stream final {
public:
    [[nodiscard]] constexpr bool ended_without_tunnel_data() const noexcept {
        return ended_without_tunnel_data_;
    }

private:
    friend class http2_remote_receive_state;

    explicit constexpr http2_remote_connect_pending_end_stream(bool ended_without_tunnel_data) noexcept :
        ended_without_tunnel_data_(ended_without_tunnel_data) {}

    bool ended_without_tunnel_data_{false};
};

// Remote frame permission and peer-half lifecycle are one exclusive protocol state.
// In particular, HTTP CONNECT content completion is not conflated with END_STREAM:
// an accepted tunnel can keep receiving DATA and replenishing its stream window, while
// a pending/rejected CONNECT accepts empty framing DATA until the peer sends END_STREAM.
class http2_remote_receive_state final {
    enum class phase_type : std::uint8_t {
        head_pending,
        head_end_stream_pending,
        content_open,
        connect_rejected_awaiting_end_stream,
        tunnel_open,
        end_stream,
        aborted,
    };
    friend class http2_stream_state;

    constexpr http2_remote_receive_state() noexcept = default;

    [[nodiscard]] bool record_head_end_stream() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = phase_type::head_end_stream_pending;
        return true;
    }

    [[nodiscard]] bool finalize_content_head() noexcept {
        if (head_pending() != nullptr) {
            state_ = phase_type::content_open;
            return true;
        }
        if (head_end_stream_pending() != nullptr) {
            state_ = phase_type::end_stream;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finalize_connect_head() noexcept {
        if (head_pending() != nullptr) {
            state_ = http2_remote_connect_pending();
            return true;
        }
        if (head_end_stream_pending() != nullptr) {
            state_ = http2_remote_connect_pending_end_stream(true);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_accept_connect() const noexcept {
        return head_pending() != nullptr || head_end_stream_pending() != nullptr || connect_pending() != nullptr || connect_pending_end_stream() != nullptr;
    }

    [[nodiscard]] bool accept_connect() noexcept {
        if (head_pending() != nullptr || connect_pending() != nullptr) {
            state_ = phase_type::tunnel_open;
            return true;
        }
        if (head_end_stream_pending() != nullptr || connect_pending_end_stream() != nullptr) {
            state_ = phase_type::end_stream;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_reject_connect() const noexcept {
        return can_accept_connect();
    }

    [[nodiscard]] bool reject_connect() noexcept {
        // Client role: a non-2xx CONNECT response resumes ordinary response-content
        // semantics. Server role: a rejected CONNECT request has no content, but its
        // peer send half can remain open until an empty DATA(END_STREAM) arrives.
        if (head_pending() != nullptr) {
            state_ = phase_type::content_open;
            return true;
        }
        if (head_end_stream_pending() != nullptr || connect_pending_end_stream() != nullptr) {
            state_ = phase_type::end_stream;
            return true;
        }
        if (connect_pending() != nullptr) {
            state_ = phase_type::connect_rejected_awaiting_end_stream;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finish_content() noexcept {
        if (content_open() == nullptr) {
            return false;
        }
        state_ = phase_type::end_stream;
        return true;
    }

    // RFC 8441 section 5 / RFC 6455 section 4.1: whether the peer half-closed
    // without ever sending tunnel data depends on the whole pending phase, not
    // on how the octets were split across DATA frames.
    [[nodiscard]] bool mark_connect_tunnel_data() noexcept {
        auto* pending = std::get_if<http2_remote_connect_pending>(&state_);
        if (pending == nullptr) {
            return false;
        }
        pending->mark_tunnel_data();
        return true;
    }

    [[nodiscard]] bool finish_pending_connect() noexcept {
        auto* pending = std::get_if<http2_remote_connect_pending>(&state_);
        if (pending == nullptr) {
            return false;
        }
        state_ = http2_remote_connect_pending_end_stream(!pending->saw_tunnel_data());
        return true;
    }

    [[nodiscard]] bool finish_tunnel() noexcept {
        if (tunnel_open() == nullptr) {
            return false;
        }
        state_ = phase_type::end_stream;
        return true;
    }

    [[nodiscard]] bool finish_rejected_connect() noexcept {
        if (connect_rejected_awaiting_end_stream() == nullptr) {
            return false;
        }
        state_ = phase_type::end_stream;
        return true;
    }

    [[nodiscard]] bool abort() noexcept {
        if (aborted() != nullptr) {
            return false;
        }
        state_ = phase_type::aborted;
        return true;
    }

public:
    [[nodiscard]] constexpr const void* head_pending() const& noexcept {
        return has_phase(phase_type::head_pending) ? this : nullptr;
    }
    const void* head_pending() const&& = delete;

    [[nodiscard]] constexpr const void* head_end_stream_pending() const& noexcept {
        return has_phase(phase_type::head_end_stream_pending) ? this : nullptr;
    }
    const void* head_end_stream_pending() const&& = delete;

    [[nodiscard]] constexpr const void* content_open() const& noexcept {
        return has_phase(phase_type::content_open) ? this : nullptr;
    }
    const void* content_open() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_connect_pending* connect_pending() const& noexcept {
        return std::get_if<http2_remote_connect_pending>(&state_);
    }
    const http2_remote_connect_pending* connect_pending() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_connect_pending_end_stream* connect_pending_end_stream() const& noexcept {
        return std::get_if<http2_remote_connect_pending_end_stream>(&state_);
    }
    const http2_remote_connect_pending_end_stream* connect_pending_end_stream() const&& = delete;

    [[nodiscard]] constexpr const void* connect_rejected_awaiting_end_stream() const& noexcept {
        return has_phase(phase_type::connect_rejected_awaiting_end_stream) ? this : nullptr;
    }
    const void* connect_rejected_awaiting_end_stream() const&& = delete;

    [[nodiscard]] constexpr const void* tunnel_open() const& noexcept {
        return has_phase(phase_type::tunnel_open) ? this : nullptr;
    }
    const void* tunnel_open() const&& = delete;

    [[nodiscard]] constexpr const void* end_stream() const& noexcept {
        return has_phase(phase_type::end_stream) ? this : nullptr;
    }
    const void* end_stream() const&& = delete;

    [[nodiscard]] constexpr const void* aborted() const& noexcept {
        return has_phase(phase_type::aborted) ? this : nullptr;
    }
    const void* aborted() const&& = delete;

private:
    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&state_);
        return current != nullptr && *current == phase;
    }

    std::variant<phase_type, http2_remote_connect_pending, http2_remote_connect_pending_end_stream> state_{phase_type::head_pending};
};

enum class http2_connect_form : std::uint8_t { standard,
    extended };

// The CONNECT form matters while validating/dispatching the request and choosing
// its dedicated acceptance path. After a final response, :protocol on the retained
// request state is the authoritative Extended CONNECT signal; rejected responses
// resume ordinary HTTP response-body semantics. Consequently, only pending owns a
// form payload.
class http2_connect_pending final {
public:
    [[nodiscard]] constexpr http2_connect_form form() const noexcept {
        return form_;
    }

private:
    friend class http2_tunnel_state;

    explicit constexpr http2_connect_pending(http2_connect_form form) noexcept :
        form_(form) {}

    http2_connect_form form_;
};

// CONNECT is exactly one of four protocol phases.
class http2_tunnel_state final {
    enum class phase_type : std::uint8_t { not_connect,
        open,
        rejected };

public:
    constexpr http2_tunnel_state() noexcept = default;

    [[nodiscard]] bool begin(http2_connect_form form) noexcept {
        if ((form != http2_connect_form::standard && form != http2_connect_form::extended) || not_connect() == nullptr) {
            return false;
        }
        state_ = http2_connect_pending(form);
        return true;
    }

    [[nodiscard]] bool accept() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = phase_type::open;
        return true;
    }

    [[nodiscard]] bool reject() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = phase_type::rejected;
        return true;
    }

    [[nodiscard]] constexpr const void* not_connect() const& noexcept {
        return has_phase(phase_type::not_connect) ? this : nullptr;
    }
    const void* not_connect() const&& = delete;

    [[nodiscard]] constexpr const http2_connect_pending* pending() const& noexcept {
        return std::get_if<http2_connect_pending>(&state_);
    }
    const http2_connect_pending* pending() const&& = delete;

    [[nodiscard]] constexpr const void* open() const& noexcept {
        return has_phase(phase_type::open) ? this : nullptr;
    }
    const void* open() const&& = delete;

    [[nodiscard]] constexpr const void* rejected() const& noexcept {
        return has_phase(phase_type::rejected) ? this : nullptr;
    }
    const void* rejected() const&& = delete;

private:
    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&state_);
        return current != nullptr && *current == phase;
    }

    std::variant<phase_type, http2_connect_pending> state_{phase_type::not_connect};
};

enum class http2_local_request_content_gate : std::uint8_t {
    open,
    awaiting_continue,
    canceled,
};

enum class http2_push_reservation : std::uint8_t { none,
    local,
    remote };

// Ruvia's message-level view of one stream: decoded request/response storage,
// content accounting and the semantic half-states. Framing, stream state and
// flow control belong to the connection's nghttp2 session; this record outlives
// the nghttp2 stream while an owner lease or delivered DATA credit still names it.
class http2_stream_state final {
    std::uint32_t id_{0};
    http2_push_reservation push_reservation_{http2_push_reservation::none};
    http2_remote_content_state remote_content_;
    http_local_content_state local_content_;
    http2_local_send_state local_send_;
    http2_remote_receive_state remote_receive_;
    bool peer_concurrency_slot_held_{false};
    http_request_expectations expectations_;
    std::array<http2_request_pseudo_header, 5> pseudo_header_order_{};
    std::uint8_t pseudo_header_count_{0};
    bool has_protocol_ : 1 {false};
    bool has_scheme_ : 1 {false};
    bool has_authority_ : 1 {false};
    bool has_path_ : 1 {false};
    bool has_host_ : 1 {false};
    bool has_cookie_ : 1 {false};
    bool regular_header_seen_ : 1 {false};
    bool has_informational_response_ : 1 {false};
    std::uint16_t scheme_default_port_{0};
    std::uint16_t host_header_index_{0};
    std::optional<http_received_status> response_status_;
    std::optional<http_priority_fields> priority_update_;
    http2_tunnel_state tunnel_state_;
    // Received DATA bytes delivered to the owner and not yet acknowledged.
    std::uint32_t window_debt_{0};
    std::string method_;
    http_known_method known_method_{http_known_method::unknown};
    std::string scheme_;
    std::string authority_;
    std::string path_;
    std::string protocol_;
    std::string cookie_;
    http2_header_list headers_;
    std::vector<http_header> trailers_;
    http2_local_request_content_gate local_request_content_gate_{http2_local_request_content_gate::open};
    bool pinned_{false};
    std::size_t pending_events_{0};

public:
    explicit http2_stream_state(std::uint32_t stream_id) noexcept :
        id_(stream_id) {}
    http2_stream_state(const http2_stream_state&) = delete;
    http2_stream_state& operator=(const http2_stream_state&) = delete;
    http2_stream_state(http2_stream_state&&) = delete;
    http2_stream_state& operator=(http2_stream_state&&) = delete;
    void reserve_push(http2_push_reservation reservation) noexcept {
        push_reservation_ = reservation;
    }
    void activate_push() noexcept {
        push_reservation_ = http2_push_reservation::none;
    }
    [[nodiscard]] http2_push_reservation push_reservation() const noexcept {
        return push_reservation_;
    }

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    [[nodiscard]] bool pinned() const noexcept {
        return pinned_;
    }

    void set_pinned(bool pinned_value) noexcept {
        pinned_ = pinned_value;
    }

    // Queued connection events naming this stream; an owner-lease release waits for zero.
    [[nodiscard]] bool has_pending_events() const noexcept {
        return pending_events_ != 0;
    }
    void add_pending_event() noexcept {
        ++pending_events_;
    }
    void remove_pending_event() noexcept {
        if (pending_events_ == 0) {
            std::terminate();
        }
        --pending_events_;
    }

    void add_window_debt(std::uint32_t bytes_value) noexcept {
        window_debt_ += bytes_value;
    }

    [[nodiscard]] std::uint32_t window_debt() const noexcept {
        return window_debt_;
    }

    [[nodiscard]] std::uint32_t take_window_debt() noexcept {
        return std::exchange(window_debt_, 0);
    }

    [[nodiscard]] bool take_window_debt(std::uint32_t bytes_value) noexcept {
        if (bytes_value == 0 || bytes_value > window_debt_) {
            return false;
        }
        window_debt_ -= bytes_value;
        return true;
    }

    [[nodiscard]] bool declare_remote_content_length(std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
        return remote_content_.declare_known_length(static_cast<std::size_t>(value));
    }

    [[nodiscard]] bool select_remote_content_metadata_only() noexcept {
        return remote_content_.select_metadata_only();
    }

    [[nodiscard]] http2_remote_content_accounting_result account_remote_content(std::size_t bytes_value) noexcept {
        return remote_content_.account(bytes_value);
    }

    [[nodiscard]] const http2_remote_content_state& remote_content() const& noexcept {
        return remote_content_;
    }
    [[nodiscard]] const http2_remote_content_state& remote_content() const&& = delete;

    void begin_local_content_forbidden() noexcept {
        local_content_.begin_forbidden();
    }

    void begin_local_content_unbounded() noexcept {
        local_content_.begin_unbounded();
    }

    void begin_local_content_known_length(std::uint64_t length) noexcept {
        local_content_.begin_known_length(length);
    }

    void commit_local_content(std::size_t bytes_value) noexcept {
        local_content_.commit(bytes_value);
    }

    [[nodiscard]] const http_local_content_state& local_content() const& noexcept {
        return local_content_;
    }
    [[nodiscard]] const http_local_content_state& local_content() const&& = delete;

    [[nodiscard]] bool is_aborted() const noexcept {
        return local_send_.aborted() != nullptr;
    }

    [[nodiscard]] const http2_local_send_state& local_send() const& noexcept {
        return local_send_;
    }
    [[nodiscard]] const http2_local_send_state& local_send() const&& = delete;

    [[nodiscard]] const http2_remote_receive_state& remote_receive() const& noexcept {
        return remote_receive_;
    }
    [[nodiscard]] const http2_remote_receive_state& remote_receive() const&& = delete;

    [[nodiscard]] bool hold_peer_concurrency_slot() noexcept {
        if (peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = true;
        return true;
    }

    [[nodiscard]] bool release_peer_concurrency_slot() noexcept {
        if (!peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = false;
        return true;
    }

    // Aborting closes both halves together; a stream is never half-aborted.
    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        if (remote_receive_.aborted() != nullptr) {
            return false;
        }
        if (!local_send_.abort(source_value)) {
            return false;
        }
        return remote_receive_.abort();
    }

    [[nodiscard]] bool record_remote_head_end_stream() noexcept {
        return remote_receive_.record_head_end_stream();
    }

    [[nodiscard]] bool finalize_remote_content_head() noexcept {
        if (tunnel_state_.not_connect() == nullptr) {
            return false;
        }
        return remote_receive_.finalize_content_head();
    }

    [[nodiscard]] bool finalize_remote_connect_head() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.finalize_connect_head();
    }

    [[nodiscard]] bool finish_remote_content() noexcept {
        return remote_receive_.finish_content();
    }

    [[nodiscard]] bool mark_remote_connect_tunnel_data() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.mark_connect_tunnel_data();
    }

    [[nodiscard]] bool finish_remote_pending_connect() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.finish_pending_connect();
    }

    [[nodiscard]] bool finish_remote_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return remote_receive_.finish_tunnel();
    }

    [[nodiscard]] bool finish_remote_rejected_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return remote_receive_.finish_rejected_connect();
    }

    [[nodiscard]] bool begin_local_request_content() noexcept {
        return local_send_.begin_request_content();
    }

    void await_request_continue() noexcept {
        local_request_content_gate_ = http2_local_request_content_gate::awaiting_continue;
    }

    [[nodiscard]] bool request_continue_pending() const noexcept {
        return local_request_content_gate_ == http2_local_request_content_gate::awaiting_continue;
    }

    [[nodiscard]] bool release_request_continue() noexcept {
        if (!request_continue_pending()) {
            return false;
        }
        local_request_content_gate_ = http2_local_request_content_gate::open;
        return true;
    }

    [[nodiscard]] bool cancel_pending_request_continue() noexcept {
        if (!request_continue_pending()) {
            return false;
        }
        local_request_content_gate_ = http2_local_request_content_gate::canceled;
        return true;
    }

    [[nodiscard]] bool request_content_canceled() const noexcept {
        return local_request_content_gate_ == http2_local_request_content_gate::canceled;
    }

    [[nodiscard]] bool begin_local_response_content() noexcept {
        return local_send_.begin_response_content();
    }

    [[nodiscard]] bool commit_local_head_end_stream() noexcept {
        return local_send_.commit_head_end_stream();
    }

    [[nodiscard]] bool begin_local_connect_request() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return local_send_.begin_connect_request();
    }

    [[nodiscard]] bool open_local_connect_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return local_send_.open_tunnel();
    }

    [[nodiscard]] bool reject_local_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return local_send_.reject_connect();
    }

    [[nodiscard]] bool commit_local_end_stream() noexcept {
        return local_send_.commit_end_stream();
    }

    void parse_request_expectation_field(std::string_view value) noexcept {
        expectations_.parse_field(value);
    }

    [[nodiscard]] http_request_expectations request_expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_request_content_indication request_content_indication() const noexcept {
        const auto* known_length = remote_content_.allowed_known_length();
        // An open receive half can still be metadata-only, known-empty, or
        // awaiting only an empty END_STREAM frame.
        const bool content_can_follow = remote_receive_.content_open() != nullptr && (remote_content_.allowed_without_length() != nullptr || (known_length != nullptr && known_length->received_bytes() < known_length->declared_length()));
        return content_can_follow ? http_request_content_indication::will_follow : http_request_content_indication::no_content;
    }

    void record_request_pseudo_header(http2_request_pseudo_header header) noexcept {
        assert(pseudo_header_count_ < pseudo_header_order_.size());
        pseudo_header_order_[pseudo_header_count_++] = header;
    }

    [[nodiscard]] std::span<const http2_request_pseudo_header> request_pseudo_header_order() const& noexcept {
        return {pseudo_header_order_.data(), pseudo_header_count_};
    }
    std::span<const http2_request_pseudo_header> request_pseudo_header_order() const&& = delete;

    [[nodiscard]] std::string_view request_method() const& noexcept {
        return known_method_ != http_known_method::unknown ? known_http_method_token(known_method_) : std::string_view(method_);
    }
    std::string_view request_method() const&& = delete;

    [[nodiscard]] http_known_method request_known_method() const noexcept {
        return known_method_;
    }

    void assign_request_method(std::string_view method) {
        const auto known = classify_http_method(method);
        if (known != http_known_method::unknown) {
            method_.clear();
            known_method_ = known;
            return;
        }
        method_.assign(method.data(), method.size());
        known_method_ = http_known_method::unknown;
    }

    [[nodiscard]] std::string_view request_authority() const& noexcept {
        return authority_;
    }
    std::string_view request_authority() const&& = delete;

    void assign_request_authority(std::string_view value) {
        authority_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view request_path() const& noexcept {
        return path_;
    }
    std::string_view request_path() const&& = delete;

    void assign_request_path(std::string_view value) {
        path_.assign(value.data(), value.size());
    }

    void take_request_target_from(http2_stream_state& other) noexcept {
        method_.swap(other.method_);
        using std::swap;
        swap(known_method_, other.known_method_);
        scheme_.swap(other.scheme_);
        authority_.swap(other.authority_);
        path_.swap(other.path_);
    }

    [[nodiscard]] std::string_view request_protocol() const& noexcept {
        return protocol_;
    }
    std::string_view request_protocol() const&& = delete;

    [[nodiscard]] std::string_view request_cookie() const& noexcept {
        return cookie_;
    }
    std::string_view request_cookie() const&& = delete;

    [[nodiscard]] bool append_request_cookie_header_value(std::string_view value, bool has_existing_cookie) {
        return append_binary_cookie_crumb(cookie_, value, has_existing_cookie);
    }

    [[nodiscard]] std::size_t remote_header_count() const noexcept {
        return headers_.size();
    }

    [[nodiscard]] http2_stored_header_view remote_header_at(std::size_t index) const& noexcept {
        return headers_.at(index);
    }
    http2_stored_header_view remote_header_at(std::size_t) const&& = delete;

    [[nodiscard]] bool append_remote_header(std::string_view name, std::string_view value, http_field_kind kind) {
        return headers_.append(name, value, kind);
    }
    [[nodiscard]] bool append_remote_trailer(std::string_view name, std::string_view value) {
        trailers_.emplace_back(name, value);
        return true;
    }
    [[nodiscard]] std::vector<http_header> take_remote_trailers() & noexcept {
        return std::move(trailers_);
    }

    [[nodiscard]] bool has_method() const noexcept {
        return known_method_ != http_known_method::unknown || !method_.empty();
    }

    [[nodiscard]] bool has_protocol() const noexcept {
        return has_protocol_;
    }

    [[nodiscard]] bool protocol_is_websocket() const noexcept {
        return http_ascii_equals_ignore_case(request_protocol(), "websocket");
    }

    void set_protocol(std::string_view value) {
        protocol_.assign(value.data(), value.size());
        has_protocol_ = true;
    }

    [[nodiscard]] bool has_scheme() const noexcept {
        return has_scheme_;
    }

    [[nodiscard]] std::string_view request_scheme() const& noexcept {
        return scheme_;
    }
    std::string_view request_scheme() const&& = delete;

    void assign_request_scheme(std::string_view value) {
        scheme_.assign(value.data(), value.size());
    }

    void mark_scheme(std::uint16_t default_port) noexcept {
        has_scheme_ = true;
        scheme_default_port_ = default_port;
    }

    [[nodiscard]] std::uint16_t scheme_default_port() const noexcept {
        return scheme_default_port_;
    }

    [[nodiscard]] bool has_authority() const noexcept {
        return has_authority_;
    }

    void mark_authority() noexcept {
        has_authority_ = true;
    }

    [[nodiscard]] bool has_path() const noexcept {
        return has_path_;
    }

    void mark_path() noexcept {
        has_path_ = true;
    }

    [[nodiscard]] bool has_host() const noexcept {
        return has_host_;
    }

    void mark_host(std::size_t index) noexcept {
        has_host_ = true;
        host_header_index_ = static_cast<std::uint16_t>(index);
    }

    [[nodiscard]] std::size_t host_header_index() const noexcept {
        return host_header_index_;
    }

    [[nodiscard]] bool has_cookie() const noexcept {
        return has_cookie_;
    }

    void mark_cookie() noexcept {
        has_cookie_ = true;
    }

    [[nodiscard]] bool regular_header_seen() const noexcept {
        return regular_header_seen_;
    }

    void mark_regular_header_seen() noexcept {
        regular_header_seen_ = true;
    }

    [[nodiscard]] bool begin_standard_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::standard);
    }

    [[nodiscard]] bool begin_extended_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::extended);
    }

    [[nodiscard]] bool accept_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !remote_receive_.can_accept_connect()) {
            return false;
        }
        if (!tunnel_state_.accept()) {
            return false;
        }
        return remote_receive_.accept_connect();
    }

    [[nodiscard]] bool reject_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !remote_receive_.can_reject_connect()) {
            return false;
        }
        if (!tunnel_state_.reject()) {
            return false;
        }
        return remote_receive_.reject_connect();
    }

    [[nodiscard]] const http2_tunnel_state& tunnel() const& noexcept {
        return tunnel_state_;
    }
    [[nodiscard]] const http_received_status* response_status() const& noexcept {
        return response_status_ ? &*response_status_ : nullptr;
    }
    const http_received_status* response_status() const&& = delete;

    [[nodiscard]] bool set_response_status(http_received_status status) noexcept {
        if (response_status_) {
            return false;
        }
        response_status_ = status;
        return true;
    }

    [[nodiscard]] bool has_informational_response() const noexcept {
        return has_informational_response_;
    }

    void mark_informational_response() noexcept {
        has_informational_response_ = true;
    }

    [[nodiscard]] const std::optional<http_priority_fields>& priority_update() const& noexcept {
        return priority_update_;
    }

    void set_priority_update(http_priority_fields fields) noexcept {
        priority_update_ = fields;
    }
};

// Cross-phase queries over the exclusive remote-receive alternatives.
[[nodiscard]] inline bool http2_remote_final_head_decoded(const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    return remote.head_pending() == nullptr && remote.head_end_stream_pending() == nullptr;
}

[[nodiscard]] inline bool http2_remote_peer_half_closed(const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    return remote.head_end_stream_pending() != nullptr || remote.connect_pending_end_stream() != nullptr || remote.end_stream() != nullptr || remote.aborted() != nullptr;
}

// Protocol closure is independent of whether request-view storage remains pinned.
// A reset closes both halves immediately; otherwise both peer END_STREAM and a
// committed local END_STREAM are required.
[[nodiscard]] inline bool http2_stream_is_closed(const http2_stream_state& stream) noexcept {
    return stream.is_aborted() || (http2_remote_peer_half_closed(stream) && stream.local_send().end_stream_committed() != nullptr);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The field section handed to one nghttp2 HEADERS/PUSH_PROMISE submission.
// Entries either borrow caller text that outlives the submission or own a copy
// (lowercased names, formatted values). nghttp2 copies every name and value
// while submitting, so nothing here is retained past that call; nghttp2's
// session deflater is the only HPACK encoder.
class http2_field_list final {
public:
    http2_field_list() noexcept = default;

    void clear() noexcept {
        owned_.clear();
        entries_.clear();
        nv_.clear();
    }

    // Keep common sections warm; return oversized storage so one large field
    // section cannot pin descriptor buffers to an idle connection.
    void release() noexcept {
        http_clear_string_retaining_small(owned_);
        if (entries_.capacity() > http_retained_string_bytes / sizeof(entry_type)) {
            entries_ = std::vector<entry_type>();
        } else {
            entries_.clear();
        }
        if (nv_.capacity() > http_retained_string_bytes / sizeof(nghttp2_nv)) {
            nv_ = std::vector<nghttp2_nv>();
        } else {
            nv_.clear();
        }
    }

    void reserve(std::size_t fields) {
        entries_.reserve(entries_.size() + fields);
        nv_.reserve(entries_.size() + fields);
    }

    // `name` must already be lowercase; both views must outlive the submission.
    void add(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name), .value_ = text_type::borrowed(value), .sensitive_ = http_field_never_indexed(name)});
    }

    // Copies `value`, which may be a temporary formatting buffer.
    void add_owned_value(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto owned_value = own(value);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name), .value_ = owned_value, .sensitive_ = http_field_never_indexed(name)});
    }

    // HTTP/2 field names are lowercase on the wire (RFC 9113 §8.2.1).
    void add_lowercase(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto offset = owned_.size();
        const auto lower = http_lower_header_name(name, [this, offset](std::size_t size) {
            reserve_geometric(owned_, offset + size);
            owned_.resize(offset + size);
            return owned_.data() + offset;
        });
        const auto entry_name = lower.data() == name.data() ? text_type::borrowed(name) : text_type{.data_ = nullptr, .offset_ = offset, .size_ = lower.size()};
        entries_.push_back(entry_type{.name_ = entry_name, .value_ = text_type::borrowed(value), .sensitive_ = http_field_never_indexed(lower)});
    }

    template <typename field_range_type>
    void add_lowercase_fields(const field_range_type& fields) {
        for (const auto& field : fields) {
            add_lowercase(field.name(), field.value());
        }
    }

    // Resolves owned offsets once every entry is final.
    [[nodiscard]] std::span<const nghttp2_nv> nv() {
        nv_.clear();
        nv_.reserve(entries_.size());
        for (const auto& entry : entries_) {
            const auto name = resolve(entry.name_);
            const auto value = resolve(entry.value_);
            nv_.push_back(nghttp2_nv{
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = static_cast<std::uint8_t>(entry.sensitive_ ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
            });
        }
        return nv_;
    }

private:
    struct text_type final {
        const char* data_{nullptr};
        std::size_t offset_{0};
        std::size_t size_{0};

        [[nodiscard]] static text_type borrowed(std::string_view text) noexcept {
            return text_type{.data_ = text.data(), .offset_ = 0, .size_ = text.size()};
        }
    };
    struct entry_type final {
        text_type name_;
        text_type value_;
        bool sensitive_{false};
    };

    [[nodiscard]] text_type own(std::string_view text) {
        const auto offset = owned_.size();
        reserve_geometric(owned_, offset + text.size());
        owned_.append(text.data(), text.size());
        return text_type{.data_ = nullptr, .offset_ = offset, .size_ = text.size()};
    }

    [[nodiscard]] std::string_view resolve(const text_type& text) const noexcept {
        return text.data_ != nullptr ? std::string_view(text.data_, text.size_) : std::string_view(owned_).substr(text.offset_, text.size_);
    }

    std::string owned_;
    std::vector<entry_type> entries_;
    std::vector<nghttp2_nv> nv_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. Huffman coding can spend up to 30 bits per decoded
// octet, so four encoded bytes per decoded-budget byte bound a valid block while
// keeping a hard memory limit for incomplete CONTINUATION sequences.
inline constexpr std::size_t max_http2_encoded_header_block_bytes = 4 * max_http_header_bytes;

// Request TE values are validated with the other request-field semantics
// (validate_request_field: only "trailers", RFC 9113 Section 8.2.2).
[[nodiscard]] inline bool http2_is_valid_regular_header(std::string_view name, std::string_view value) noexcept {
    return is_valid_binary_field_name(name) && is_valid_http_header_value(value) && !is_forbidden_http_binary_connection_field(name);
}

struct http2_header_decode_context final {
    explicit http2_header_decode_context(http2_stream_state& stream_value) noexcept :
        stream_(stream_value) {}

    http2_stream_state& stream_;
    binary_field_section_size decoded_header_list_size_{max_http_header_bytes};
    request_field_state request_fields_{request_field_framing::binary};
};

// Call only after authority admission (or validated local push submission).
// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool http2_request_synthesizes_host(const http2_stream_state& stream) noexcept {
    return binary_request_synthesizes_host(stream.has_host(), stream.has_authority(), stream.request_scheme(), stream.request_authority());
}

[[nodiscard]] inline std::string_view http2_request_host_field(const http2_stream_state& stream) noexcept {
    if (!stream.has_host()) {
        return {};
    }
    const auto index = stream.host_header_index();
    if (index < stream.remote_header_count()) {
        const auto header_value = stream.remote_header_at(index);
        if (header_value.kind_ == http_field_kind::host) {
            return header_value.value_;
        }
    }
    return {};
}

// Wire :authority if present; otherwise the Host field of a translated
// origin-form request (RFC 9113 §8.3.1).
[[nodiscard]] inline std::string_view http2_effective_request_authority(const http2_stream_state& stream) noexcept {
    return stream.has_authority() ? stream.request_authority() : http2_request_host_field(stream);
}

[[nodiscard]] inline bool http2_has_required_request_authority(const http2_stream_state& stream) noexcept {
    if (!binary_regular_request_requires_authority(stream.request_scheme(), stream.request_path())) {
        return true;
    }
    if (stream.has_authority()) {
        return true;
    }
    return is_valid_binary_request_authority(stream.request_scheme(), http2_request_host_field(stream));
}

[[nodiscard]] inline bool http2_accumulate_header_list_bytes(http2_header_decode_context& context_value, std::string_view name, std::string_view value) noexcept {
    return context_value.decoded_header_list_size_.add(name, value);
}

[[nodiscard]] inline bool http2_append_cookie_header_value(http2_stream_state& stream, std::string_view value) {
    // RFC 9113 section 8.2.3 joins cookie-pairs with "; ". An empty crumb is
    // not a cookie-pair (RFC 6265 section 4.2.1) and would leave a leading or
    // trailing separator in the joined value, so it contributes nothing.
    if (value.empty()) {
        return true;
    }
    if (!stream.append_request_cookie_header_value(value, stream.has_cookie())) {
        return false;
    }
    stream.mark_cookie();
    return true;
}

[[nodiscard]] inline bool http2_on_decoded_initial_header(http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    auto& stream = context_value.stream_;
    if (name.empty()) {
        return false;
    }

    if (name.front() == ':') {
        if (stream.regular_header_seen()) {
            return false;
        }
        if (name == ":method") {
            if (stream.has_method() || !is_valid_http_method_token(value)) {
                return false;
            }
            stream.assign_request_method(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::method);
            return true;
        }
        if (name == ":protocol") {
            if (stream.has_protocol() || !is_valid_http_header_name(value)) {
                return false;
            }
            stream.set_protocol(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::protocol);
            return true;
        }
        if (name == ":scheme") {
            if (stream.has_scheme() || !is_valid_uri_scheme(value)) {
                return false;
            }
            stream.assign_request_scheme(value);
            stream.mark_scheme(http_uri_scheme_default_port(value));
            stream.record_request_pseudo_header(http2_request_pseudo_header::scheme);
            return true;
        }
        if (name == ":authority") {
            if (stream.has_authority() || !is_valid_uri_authority(value)) {
                return false;
            }
            stream.assign_request_authority(value);
            stream.mark_authority();
            stream.record_request_pseudo_header(http2_request_pseudo_header::authority);
            return true;
        }
        if (name == ":path") {
            if (stream.has_path() || value.empty()) {
                return false;
            }
            stream.assign_request_path(value);
            stream.mark_path();
            stream.record_request_pseudo_header(http2_request_pseudo_header::path);
            return true;
        }
        return false;
    }

    if (!http2_is_valid_regular_header(name, value)) {
        return false;
    }
    stream.mark_regular_header_seen();
    const auto kind = classify_http_field(name);
    if (validate_request_field(kind, value, http_field_list_role::recipient, context_value.request_fields_) != request_field_status::ok) {
        return false;
    }
    switch (kind) {
        case http_field_kind::host:
            if (stream.has_authority() && !authority_matches_host(stream.request_authority(), value, stream.scheme_default_port())) {
                return false;
            }
            stream.mark_host(stream.remote_header_count());
            break;
        case http_field_kind::cookie:
            return http2_append_cookie_header_value(stream, value);
        case http_field_kind::expect:
            // Expect is an extensible semantic list. Preserve unsupported but
            // syntactically valid members for the Web product's 417 policy.
            stream.parse_request_expectation_field(value);
            break;
        case http_field_kind::content_length: {
            http_content_length_state content_length;
            if (content_length.parse_field(value) != http_content_length_parse_status::ok || !stream.declare_remote_content_length(*content_length.value())) {
                return false;
            }
            break;
        }
        default:
            break;
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] inline bool http2_on_decoded_request_trailer(http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    // Pseudo-header and connection-specific fields, including a TE value other
    // than "trailers", are malformed (RFC 9113 sections 8.1 and 8.2.2). A field
    // merely not permitted as a trailer binds only the sender (RFC 9110 section
    // 6.5.1) and is discarded.
    if (!http2_is_valid_regular_header(name, value) || (name == "te" && !is_valid_binary_http_te_field_value(value, http_field_list_role::recipient))) {
        return false;
    }
    return is_forbidden_http_request_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool http2_is_pending_websocket_connect(const http2_stream_state& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr && pending->form() == http2_connect_form::extended && stream.protocol_is_websocket();
}

[[nodiscard]] inline bool http2_websocket_connect_remote_head_ended(const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    const auto* pending_end = remote.connect_pending_end_stream();
    return remote.head_end_stream_pending() != nullptr || (pending_end != nullptr && pending_end->ended_without_tunnel_data());
}

[[nodiscard]] inline bool http2_websocket_connect_content_acceptable(const http2_stream_state& stream) noexcept {
    const auto* known = stream.remote_content().allowed_known_length();
    return stream.remote_content().allowed_without_length() != nullptr || (known != nullptr && known->declared_length() == 0);
}

[[nodiscard]] inline websocket_handshake_validation_result validate_http2_websocket_handshake(const http2_stream_state& stream, const http_request& request, websocket_handshake_validation_options options = {}) noexcept {
    if (!http2_is_pending_websocket_connect(stream) || !http2_remote_final_head_decoded(stream) || http2_websocket_connect_remote_head_ended(stream) || !http2_websocket_connect_content_acceptable(stream)) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    // RFC 8441 section 5: :protocol supersedes the RFC 6455 key exchange, so
    // Sec-WebSocket-Key and Sec-WebSocket-Accept are not processed here.
    return validate_extended_connect_websocket_headers(request, options);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Queue records for the events whose public form the facade materializes from
// stream storage or binds to its owner endpoint when the event is taken.
struct http2_message_head_record final {
    std::uint32_t stream_id_{0};
    std::optional<http_client_request_content_signal> request_content_signal_;
    // The HEADERS frame left the peer send half open. Server side: message
    // content events follow this head (a pending CONNECT reads as closed).
    // Client side: the final response head did not carry END_STREAM. Captured
    // when the head is published; the same input batch may already have
    // delivered later frames to the core.
    bool body_open_{false};
    bool initial_end_stream_{false};
};

// The bytes borrow the accepted feed() input until the next consuming feed().
struct http2_message_data_record final {
    std::uint32_t stream_id_{0};
    std::string_view bytes_;
    std::uint32_t flow_control_bytes_{0};
};

struct http2_tunnel_data_record final {
    std::uint32_t stream_id_{0};
    std::string_view bytes_;
    std::uint32_t flow_control_bytes_{0};
};

struct http2_message_end_record final {
    std::uint32_t stream_id_{0};
};

// One queued protocol event. Events that need no owner binding are stored in
// their public form and moved out unchanged. There is deliberately no none
// alternative: an empty queue is represented by peek_event() returning null.
class http2_event final {
public:
    using value_type = std::variant<http2_informational_head_event, http2_message_head_record, http2_message_data_record, http2_message_end_record, http2_tunnel_data_record, http2_tunnel_end_event, http2_stream_closed_event, http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event, http_priority_update, http_origin_advertisement, http_alternative_service_advertisement, http2_received_settings, http2_received_window_update, http2_received_priority>;

    template <typename event_type>
        requires(!std::is_same_v<event_type, http2_event>)
    explicit http2_event(event_type event) noexcept :
        value_(std::move(event)) {}
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;

    // Visits each stream id whose storage the owner may read when taking this event.
    template <typename visitor_type>
    void for_each_referenced_stream(visitor_type&& visitor) const noexcept {
        std::visit([&visitor](const auto& event) noexcept {
            using event_type = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<event_type, http2_push_promise_event>) {
                visitor(event.associated_stream_id_);
                visitor(event.promised_stream_id_);
            } else if constexpr (requires { event.stream_id(); }) {
                visitor(event.stream_id());
            } else if constexpr (std::is_same_v<event_type, http2_message_head_record> || std::is_same_v<event_type, http2_message_data_record> || std::is_same_v<event_type, http2_message_end_record> || std::is_same_v<event_type, http2_tunnel_data_record>) {
                visitor(event.stream_id_);
            }
        }, value_);
    }

    [[nodiscard]] value_type& value() & noexcept {
        return value_;
    }
    value_type& value() && = delete;

private:
    value_type value_;
};

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. nghttp2
// serializes frames; the connection appends each frame it emits as one segment
// so batches stay frame-aligned and DATA payload bytes stay attributable.
class http2_output_buffer final {
public:
    struct segment_type final {
        // DATA payload spans [begin + http2_frame_header_bytes, end). Raw byte
        // segments have no DATA metadata.
        std::size_t begin_;
        std::size_t end_;
        std::uint32_t stream_id_;
        bool is_data_;
    };
    http2_output_buffer() noexcept = default;

    [[nodiscard]] bool wants_write() const noexcept {
        return consumed_ < bytes_.size();
    }

    // Appends one complete frame (or the client connection preface) as a
    // segment; both reservations precede the copy, so a throwing allocation
    // leaves the pending output unchanged.
    void append_segment(std::string_view bytes_value, std::uint32_t stream_id, bool is_data) {
        if (bytes_value.empty()) {
            return;
        }
        // Reclaim an amortizable prefix before new frames grow the live suffix.
        compact_consumed_prefix();
        reserve_segment();
        if (bytes_value.size() > bytes_.max_size() - bytes_.size()) {
            throw std::length_error("HTTP/2 output buffer size overflow");
        }
        reserve_geometric(bytes_, bytes_.size() + bytes_value.size());
        const auto begin = bytes_.size();
        bytes_.append(bytes_value.data(), bytes_value.size());
        segments_.push_back(segment_type{begin, bytes_.size(), stream_id, is_data});
    }

    [[nodiscard]] http2_output_batch_result take_batch(std::size_t max_bytes, std::string& into, http2_data_output_observer_type observer, void* observer_context) {
        if (consumed_ == bytes_.size()) {
            return {http2_output_batch_status::empty, 0};
        }
        // Every append records a segment, and the cursor only advances by whole
        // segments, so the first pending segment always begins at consumed_.
        std::size_t end = consumed_;
        std::size_t segment_count = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment_count != 0 && segment.end_ - consumed_ > max_bytes) {
                break;
            }
            end = segment.end_;
            ++segment_count;
            if (end - consumed_ >= max_bytes) {
                break;
            }
        }
        // Only a storage handoff benefits from compacting this batch in place.
        const auto batch_bytes = end - consumed_;
        const bool complete_batch = end == bytes_.size();
        const bool transferable = into.empty();
        if (complete_batch && transferable) {
            compact_consumed_prefix();
        }
        const auto first_segment = segment_offset_;
        const bool transfer_storage = consumed_ == 0 && complete_batch && transferable;
        if (transfer_storage) {
            into.swap(bytes_);
        } else {
            into.append(bytes_.data() + consumed_, batch_bytes);
        }
        if (observer != nullptr) {
            ++observer_depth_;
        }
        struct observer_depth_guard final {
            http2_output_buffer& buffer_;
            bool active_;
            ~observer_depth_guard() {
                if (!active_) {
                    return;
                }
                --buffer_.observer_depth_;
                if (buffer_.observer_depth_ == 0 && buffer_.segment_offset_ == buffer_.segments_.size()) {
                    buffer_.segments_.clear();
                    buffer_.segment_offset_ = 0;
                }
            }
        } guard{*this, observer != nullptr};
        if (transfer_storage) {
            reset_after_drain();
        } else {
            consume_batch(batch_bytes, segment_count);
        }
        if (observer != nullptr) {
            // A callback may append or drain more frames. Their vector may
            // reallocate, so retain indices and snapshot each entry by value.
            for (std::size_t i = 0; i < segment_count; ++i) {
                const auto segment = segments_[first_segment + i];
                if (segment.is_data_) {
                    observer(observer_context, segment.stream_id_, segment.end_ - segment.begin_ - http2_frame_header_bytes);
                }
            }
        }
        return {http2_output_batch_status::taken, batch_bytes};
    }

    [[nodiscard]] std::size_t pending_data_bytes(std::uint32_t stream_id) const noexcept {
        std::size_t total = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment.is_data_ && segment.stream_id_ == stream_id) {
                total += segment.end_ - segment.begin_ - http2_frame_header_bytes;
            }
        }
        return total;
    }

private:
    // Advances the cursor past whole segments; the cursor always stays on a
    // segment boundary, so no partial-segment bookkeeping exists.
    void consume_batch(std::size_t bytes_value, std::size_t segment_count) noexcept {
        if (bytes_value == bytes_.size() - consumed_) {
            reset_after_drain();
            return;
        }
        consumed_ += bytes_value;
        segment_offset_ += segment_count;
    }

    void reset_after_drain() noexcept {
        bytes_.clear();
        if (observer_depth_ == 0) {
            segments_.clear();
            segment_offset_ = 0;
        } else {
            segment_offset_ = segments_.size();
        }
        consumed_ = 0;
    }

    void compact_consumed_prefix() noexcept {
        if (observer_depth_ != 0 || consumed_ < 64 * 1024 || consumed_ < bytes_.size() - consumed_) {
            return;
        }
        const auto prefix = consumed_;
        bytes_.erase(0, prefix);
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            auto& segment = segments_[index];
            segment.begin_ = segment.begin_ > prefix ? segment.begin_ - prefix : 0;
            segment.end_ -= prefix;
        }
        segments_.erase(segments_.begin(), segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_));
        segment_offset_ = 0;
        consumed_ = 0;
    }

    void reserve_segment() {
        if (observer_depth_ == 0 && segment_offset_ >= 32 && segment_offset_ >= segments_.size() - segment_offset_) {
            const auto remaining = segments_.size() - segment_offset_;
            std::move(segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_), segments_.end(), segments_.begin());
            segments_.resize(remaining);
            segment_offset_ = 0;
        }
        // A burst of frames shares one geometric growth policy.
        reserve_geometric(segments_, segments_.size() + 1);
    }

    std::string bytes_;
    std::vector<segment_type> segments_;
    std::size_t segment_offset_{0};
    std::size_t consumed_{0};
    std::size_t observer_depth_{0};
};

// RFC 9113 §5.1.1: stream identifiers are 31-bit values.
inline constexpr std::uint32_t http2_max_stream_id = 0x7fffffffU;

// One compile-time source of truth for every local receive constraint. These values
// drive both the advertised SETTINGS and the matching session options and fixed
// capacities; they are intentionally not runtime knobs that can diverge.
struct http2_local_settings final {
    static constexpr std::uint32_t header_table_size = 4096;
    static constexpr std::uint32_t max_concurrent_streams = 128;
    static constexpr std::uint32_t initial_window_size = 1024 * 1024;
    static constexpr std::uint32_t max_frame_size = http2_default_max_frame_size;
    static constexpr std::uint32_t max_header_list_size = static_cast<std::uint32_t>(max_http_header_bytes);
    static constexpr std::uint32_t enable_connect_protocol = 1;
};

static_assert(http2_local_settings::initial_window_size <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));

class http2_stream_table final {
public:
    // Hard upper bound on live stream records. Reserved push promises do not hold a
    // concurrency slot, so this can exceed SETTINGS_MAX_CONCURRENT_STREAMS.
    static constexpr std::size_t max_streams = static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 4;
    static constexpr std::size_t max_push_streams = max_streams - static_cast<std::size_t>(http2_local_settings::max_concurrent_streams);

    http2_stream_table() noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http2_stream_state* find(std::uint32_t stream_id) & noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] http2_stream_state* find(std::uint32_t) && = delete;

    [[nodiscard]] const http2_stream_state* find(std::uint32_t stream_id) const& noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (const auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] const http2_stream_state* find(std::uint32_t) const&& = delete;

    // NOTE on slot reuse: a closed stream's http2_stream_state is destroyed here (inline
    // slot .reset() / overflow erase), freeing its per-stream strings. We do NOT
    // pool the storage and reset-in-place to retain capacity, deliberately. A correct
    // reset_for_reuse would have to reinitialise ~85 fields across 8 sub-objects, and a
    // single missed field would leak one request's decoded headers / routing / body
    // into the next reused slot -- a cross-request data-disclosure risk not worth taking
    // for a micro-optimisation off the measured hot path.
    [[nodiscard]] http2_stream_state* create(std::uint32_t stream_id) & {
        if (auto* existing = find(stream_id); existing != nullptr) {
            return existing;
        }
        const bool is_push = (stream_id & 1U) == 0;
        if (size_ >= max_streams || (is_push && push_size_ >= max_push_streams)) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id);
                ++size_;
                if (is_push) {
                    ++push_size_;
                }
                return &*slot;
            }
        }
        auto stream = std::make_unique<http2_stream_state>(stream_id);
        reserve_geometric(overflow_ids_, overflow_ids_.size() + 1);
        reserve_geometric(overflow_, overflow_.size() + 1);
        auto* result_value = stream.get();
        overflow_ids_.push_back(stream_id);
        overflow_.push_back(std::move(stream));
        ++size_;
        if (is_push) {
            ++push_size_;
        }
        return result_value;
    }
    [[nodiscard]] http2_stream_state* create(std::uint32_t) && = delete;

    bool remove(std::uint32_t stream_id) noexcept {
        if (stream_id == 0) {
            return false;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                slot.reset();
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                erase_overflow_at(i);
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        return false;
    }

    template <typename callback>
    void for_each(callback&& callback_value) {
        snapshot_iteration_guard_type guard_value(*this);
        for (auto& slot : inline_) {
            if (slot) {
                callback_value(*slot);
            }
        }
        const auto overflow_end = overflow_.size();
        for (std::size_t i = 0; i < overflow_end; ++i) {
            auto& stream = overflow_[i];
            if (stream != nullptr) {
                callback_value(*stream);
            }
        }
    }

private:
    // Two inline slots cover the typical request/response cadence without
    // paying the full concurrent-stream budget up front: each slot embeds a
    // ~1.3 KB http2_stream_state, and the table lives in the connection object
    // for the connection's whole lifetime, so the two slots put ~2.6 KB into
    // every connection. Deeper multiplexing spills to the overflow path.
    static constexpr std::size_t inline_capacity = 2;
    using overflow_stream_type = std::unique_ptr<http2_stream_state>;

    class snapshot_iteration_guard_type final {
    public:
        explicit snapshot_iteration_guard_type(http2_stream_table& table_value) noexcept :
            table_(table_value) {
            ++table_.snapshot_iteration_depth_;
        }

        snapshot_iteration_guard_type(const snapshot_iteration_guard_type&) = delete;
        snapshot_iteration_guard_type& operator=(const snapshot_iteration_guard_type&) = delete;

        ~snapshot_iteration_guard_type() {
            --table_.snapshot_iteration_depth_;
            table_.compact_overflow_if_idle();
        }

    private:
        http2_stream_table& table_;
    };

    void erase_overflow_at(std::size_t index) noexcept {
        if (snapshot_iteration_depth_ != 0) {
            overflow_ids_[index] = 0;
            overflow_[index].reset();
            overflow_needs_compact_ = true;
            return;
        }
        if (index + 1 != overflow_.size()) {
            overflow_ids_[index] = overflow_ids_.back();
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_ids_.pop_back();
        overflow_.pop_back();
    }

    void compact_overflow_if_idle() noexcept {
        if (snapshot_iteration_depth_ != 0 || !overflow_needs_compact_) {
            return;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            if (overflow_[i] == nullptr) {
                if (i + 1 != overflow_.size()) {
                    overflow_ids_[i] = overflow_ids_.back();
                    overflow_[i] = std::move(overflow_.back());
                }
                overflow_ids_.pop_back();
                overflow_.pop_back();
                continue;
            }
            ++i;
        }
        overflow_needs_compact_ = false;
    }

    std::array<std::optional<http2_stream_state>, inline_capacity> inline_{};
    std::vector<std::uint32_t> overflow_ids_;
    std::vector<overflow_stream_type> overflow_;
    std::size_t size_{0};
    std::size_t push_size_{0};
    std::size_t snapshot_iteration_depth_{0};
    bool overflow_needs_compact_{false};
};

// The local SETTINGS frame of the connection preface. Only a client may
// advertise ENABLE_PUSH=1 (RFC 9113 §6.5.2).
[[nodiscard]] inline std::array<nghttp2_settings_entry, 8> http2_local_settings_entries(bool enable_push) noexcept {
    return {{
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, http2_local_settings::header_table_size},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, enable_push ? 1U : 0U},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, http2_local_settings::max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, http2_local_settings::initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, http2_local_settings::max_frame_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, http2_local_settings::max_header_list_size},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, http2_local_settings::enable_connect_protocol},
        {NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES, 1},
    }};
}

// The ALTSVC (RFC 7838 §4) encoder. The size function validates the frame against
// `max_frame_size` and returns its payload size; the write function fills
// exactly that many bytes in the caller's destination.
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size);
void http2_write_alternative_service_payload(std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept;

class http2_local_connection_open final {};

// A server drains in two phases (RFC 9113 section 6.8). During the round trip the
// advertised boundary is 2^31-1 and peer streams are still admitted up to
// round_trip_stream_limit(); after the PING ACK (or that limit) the boundary is the
// real last peer stream id and is final. A client drains in the final phase at once.
class http2_local_connection_graceful_drain final {
public:
    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }
    [[nodiscard]] constexpr bool awaiting_round_trip() const noexcept {
        return awaiting_round_trip_;
    }
    // Highest peer stream id still admitted while awaiting the round trip.
    [[nodiscard]] constexpr std::uint32_t round_trip_stream_limit() const noexcept {
        return round_trip_stream_limit_;
    }

private:
    friend class http2_local_connection_state;

    constexpr http2_local_connection_graceful_drain(std::uint32_t last_stream_id, bool awaiting_round_trip, std::uint32_t round_trip_stream_limit) noexcept :
        last_stream_id_(last_stream_id),
        round_trip_stream_limit_(round_trip_stream_limit),
        awaiting_round_trip_(awaiting_round_trip) {}

    std::uint32_t last_stream_id_;
    std::uint32_t round_trip_stream_limit_;
    bool awaiting_round_trip_;
};

class http2_local_connection_fatal_failure final {
public:
    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class http2_local_connection_state;

    explicit constexpr http2_local_connection_fatal_failure(http2_error_code error) noexcept :
        error_(error) {}

    http2_error_code error_;
};

// The locally initiated connection lifecycle owns its GOAWAY meaning. A graceful
// drain alone carries the advertised stream boundary; a fatal protocol failure
// alone carries an error code and atomically supersedes any earlier drain.
// Peer GOAWAY and preface progress are directional/orthogonal state elsewhere.
class http2_local_connection_state final {
public:
    [[nodiscard]] constexpr const http2_local_connection_open* open() const& noexcept {
        return std::get_if<http2_local_connection_open>(&state_);
    }
    const http2_local_connection_open* open() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_graceful_drain* graceful_drain() const& noexcept {
        return std::get_if<http2_local_connection_graceful_drain>(&state_);
    }
    const http2_local_connection_graceful_drain* graceful_drain() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_fatal_failure* fatal_failure() const& noexcept {
        return std::get_if<http2_local_connection_fatal_failure>(&state_);
    }
    const http2_local_connection_fatal_failure* fatal_failure() const&& = delete;

    [[nodiscard]] bool begin_graceful_drain(std::uint32_t last_stream_id) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    [[nodiscard]] bool begin_round_trip_drain(std::uint32_t advertised_stream_id, std::uint32_t round_trip_stream_limit) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(advertised_stream_id, true, round_trip_stream_limit));
        return true;
    }

    [[nodiscard]] bool finish_round_trip_drain(std::uint32_t last_stream_id) noexcept {
        const auto* drain = graceful_drain();
        if (drain == nullptr || !drain->awaiting_round_trip()) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    void fail(http2_error_code error) noexcept {
        state_ = state_type(http2_local_connection_fatal_failure(error));
    }

private:
    using state_type = std::variant<http2_local_connection_open, http2_local_connection_graceful_drain, http2_local_connection_fatal_failure>;

    state_type state_;
};

// Owner-lease releases and DATA credits that could not be applied yet. A pinned
// stream is released only after its delivered DATA credit and queued events are
// gone; a release or credit whose application failed is retried later. Both sets
// are bounded by the stream-table capacity, so they never allocate.
class http2_stream_lease_ledger final {
public:
    enum class release_kind_type : std::uint8_t {
        after_credits,
        after_remote_end,
        abandon,
    };
    struct release_type final {
        std::uint32_t stream_id_;
        release_kind_type kind_;
    };
    struct credit_type final {
        std::uint32_t stream_id_;
        std::uint32_t bytes_;
    };

    [[nodiscard]] const release_type* release(std::uint32_t stream_id) const noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                return &releases_[index];
            }
        }
        return nullptr;
    }

    // abandon and after_remote_end override a pending release; after_credits only
    // replaces abandon, whose reset has then been sent.
    void defer_release(std::uint32_t stream_id, release_kind_type kind) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            auto& existing = releases_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (kind != release_kind_type::after_credits || existing.kind_ == release_kind_type::abandon) {
                existing.kind_ = kind;
            }
            return;
        }
        if (release_count_ == releases_.size()) {
            std::terminate();
        }
        releases_[release_count_++] = release_type{stream_id, kind};
    }

    void erase_release(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                releases_[index] = releases_[--release_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t release_count() const noexcept {
        return release_count_;
    }
    [[nodiscard]] const release_type& release_at(std::size_t index) const noexcept {
        return releases_[index];
    }

    void defer_credit(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            auto& existing = credits_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (bytes > (std::numeric_limits<std::uint32_t>::max)() - existing.bytes_) {
                std::terminate();
            }
            existing.bytes_ += bytes;
            return;
        }
        if (credit_count_ == credits_.size()) {
            std::terminate();
        }
        credits_[credit_count_++] = credit_type{stream_id, bytes};
    }

    void erase_credit(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            if (credits_[index].stream_id_ == stream_id) {
                credits_[index] = credits_[--credit_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t credit_count() const noexcept {
        return credit_count_;
    }
    [[nodiscard]] credit_type credit_at(std::size_t index) const noexcept {
        return credits_[index];
    }

private:
    std::array<release_type, http2_stream_table::max_streams> releases_;
    std::size_t release_count_{0};
    std::array<credit_type, http2_stream_table::max_streams> credits_;
    std::size_t credit_count_{0};
};

// An ORIGIN or ALTSVC payload handed to nghttp2_submit_extension(). It lives on
// the submitting call's stack; nghttp2 packs it while that call drains the
// session, and the single in-tree payload encoder writes exactly size_ bytes.
struct http2_extension_payload final {
    std::size_t size_{0};
    void (*write_)(const http2_extension_payload&, std::span<char>) noexcept {nullptr};
    std::span<const std::string_view> origins_{};
    std::string_view origin_{};
    std::string_view field_value_{};
};

class http2_connection final {
public:
    explicit http2_connection(http2_role role = http2_role::server, bool enable_push = false, bool receive_origin_advertisements = false, bool receive_frame_metadata = false, http2_frame_observer_type frame_observer = nullptr, http2_header_observer_type header_observer = nullptr, void* observer_context = nullptr);
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;

    [[nodiscard]] http2_role role() const noexcept {
        return role_;
    }

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer. nghttp2 consumes the whole span and
    // buffers any incomplete frame itself. Calling while events remain returns
    // events_pending and consumes nothing; drain peek_event()/consume_event()
    // first. protocol_failure is terminal. DATA events borrow the accepted span
    // until the next consuming feed().
    [[nodiscard]] http2_feed_result feed(std::string_view in);
    template <http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    // The owner materializes the front event before consuming it, so an exception
    // never consumes the only protocol notification for a stream.
    [[nodiscard]] http2_event* peek_event() & noexcept;
    [[nodiscard]] http2_event* peek_event() && = delete;
    void consume_event() noexcept;

    // Access an assembled request head / stream for the owner to build an http_request.
    [[nodiscard]] http2_stream_state* stream(std::uint32_t stream_id) & noexcept;
    [[nodiscard]] http2_stream_state* stream(std::uint32_t) && = delete;
    // New outbound work is closed after a fatal connection error. Owner cleanup
    // uses the raw stream lookup so outstanding leases remain releasable.
    [[nodiscard]] http2_stream_state* find_outbound_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream_receive_status stream_receive_status(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_server_request_view> server_request_view(std::uint32_t stream_id) const noexcept;

    // --- outbound --------------------------------------------------------------
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes, std::string& into, http2_data_output_observer_type observer, void* observer_context);
    [[nodiscard]] bool wants_write() const noexcept {
        return output_.wants_write();
    }

    // A stale/mismatched plan is rejected distinctly before any field or stream
    // mutation. Informational status codes and HTTP/2-unrepresentable control
    // semantics are rejected as well.
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http2_response_head_submit_error> submit_response_head(std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan);
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http2_response_head_submit_error> submit_streaming_response_head(std::uint32_t stream_id, http_response head, http_response_trailer_intent trailer_intent);
    [[nodiscard]] http2_data_submit_result submit_data(std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream, std::size_t max_payload_bytes);
    [[nodiscard]] http2_submit_status submit_interim_response_head(std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error> submit_websocket_handshake(std::uint32_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options);
    [[nodiscard]] http2_submit_status submit_connect_response_head(std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] std::variant<std::uint32_t, http2_push_submit_error> submit_push_promise(std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] http2_finish_status finish_request(std::uint32_t stream_id, std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_finish_status finish_response(std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id, std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http2_submit_status submit_ping();
    [[nodiscard]] http2_submit_status submit_max_concurrent_streams(std::uint32_t limit);

    // --- lifecycle -------------------------------------------------------------
    // A connection error is terminal: its GOAWAY has been queued (unless the
    // failure was an allocation failure that left no room for it) and the I/O
    // owner must close the transport after flushing it. Graceful local/peer
    // GOAWAY is deliberately absent from this value.
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept {
        const auto* failure = local_connection_state_.fatal_failure();
        return failure != nullptr ? std::optional<http2_error_code>(failure->error()) : std::nullopt;
    }
    void begin_drain();
    void finish_drain();
    void terminate(http2_error_code error);
    [[nodiscard]] bool draining() const noexcept {
        return local_connection_state_.graceful_drain() != nullptr;
    }
    [[nodiscard]] bool drain_final() const noexcept {
        const auto* drain = local_connection_state_.graceful_drain();
        return drain != nullptr && !drain->awaiting_round_trip();
    }
    [[nodiscard]] bool header_block_in_progress() const noexcept {
        return header_block_in_progress_ || header_block_.has_value();
    }
    // Queues the role-specific connection preface (client magic, SETTINGS and the
    // connection WINDOW_UPDATE) exactly once.
    void begin_connection();

    // --- client role -------------------------------------------------------------
    [[nodiscard]] std::size_t available_local_request_streams(std::size_t local_limit) const noexcept;
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_regular_request_head(std::string_view method, std::string_view scheme, std::optional<std::string_view> authority, std::string_view path, std::span<const http_header_view> headers, http_request_content_plan content, http_client_request_expectation expectation = http_client_request_expectation::none, http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_connect_request_head(std::string_view authority, std::span<const http_header_view> headers = {}, http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_extended_connect_request_head(std::string_view protocol, std::string_view scheme, std::string_view authority, std::string_view path, std::span<const http_header_view> headers = {}, http_request_field_source source = http_request_field_source::generated);

    // A DATA event borrows bytes from the accepted input and retains the matching
    // receive-window debt; destroying its credit hands the bytes to
    // nghttp2_session_consume(). A no-op once the stream or its debt is gone;
    // a failed release is deferred to the lease ledger. Never throws.
    void return_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept;
    [[nodiscard]] http2_request_content_release_status release_request_content(std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool received_peer_settings() const noexcept {
        return received_peer_settings_;
    }

    // A request/response built from a stream holds views into its decoded
    // storage, so the stream must outlive an in-flight handler. While pinned, a
    // peer reset only marks it aborted (emitting stream_closed) instead of
    // freeing it.
    void pin_stream(std::uint32_t stream_id) noexcept;
    void lease_submitted_request(const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept;
    // Ends an owner lease. The stream is unpinned once its delivered DATA credit
    // and queued events are gone; until then the release stays deferred.
    void release_stream_lease(std::uint32_t stream_id);
    // Response/tunnel EOF releases the client lease only after both halves close.
    void release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept;
    // Destruction path of an unreleased lease: resets a live stream, then
    // releases it. Never throws; a failure is retried later.
    void abandon_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_leases() noexcept;
    // Sends whatever nghttp2 queued outside a flushing operation (WINDOW_UPDATE
    // after a credit, RST_STREAM after an abandoned lease). Failures have
    // already failed the connection and are observed by the next operation.
    void flush_output() noexcept;

private:
    friend struct http2_session_callbacks;

    struct session_deleter final {
        void operator()(nghttp2_session* session) const noexcept {
            nghttp2_session_del(session);
        }
    };

    // The field block nghttp2 is currently delivering through on_header.
    enum class header_block_kind : std::uint8_t {
        request,
        response,
        request_trailers,
        response_trailers,
        push_promise,
    };
    struct header_block_type final {
        header_block_type(header_block_kind kind, http2_stream_state& stream) noexcept :
            kind_(kind),
            context_(stream) {}

        header_block_kind kind_;
        http2_header_decode_context context_;
        // Response heads: a 1xx keeps its fields separately from the final head.
        std::vector<http_header> informational_fields_;
        std::optional<http_received_status> status_;
        http_response_content_semantics content_semantics_{http_response_content_semantics::with_content};
        bool saw_regular_{false};
        // PUSH_PROMISE on a connected tunnel (RFC 9113 §8.5): decoded, then refused.
        bool reject_connected_tunnel_{false};
    };

    // One outbound DATA submission. It lives on submit_data()'s stack and
    // nghttp2 copies from it while the same call drains the session.
    struct outbound_data_type final {
        std::string_view bytes_;
        bool finished_{false};
    };

    // --- nghttp2 plumbing (callbacks run inside mem_recv2/mem_send2) ----------
    void create_session();
    // Drains every frame nghttp2 is ready to send into the output buffer and
    // surfaces any failure recorded by a callback.
    void flush_session();
    // Fails the connection for a negative nghttp2 return code and rethrows a
    // recorded callback exception or allocation failure.
    void fail_on_nghttp2_error(int error);
    // Submission results: fatal codes fail the connection and throw; any other
    // rejection is a caller contract violation (std::logic_error).
    void check_nghttp2(int result);
    // A local connection error: GOAWAY(error) through nghttp2, then terminal state.
    void fail_connection(http2_error_code error);
    void record_callback_failure() noexcept;

    int on_begin_frame(const nghttp2_frame_hd& header);
    nghttp2_ssize receive_observed(std::string_view input);
    void observe_frame_payload(std::string_view input) noexcept;
    void observe_header(http2_header_observation_phase phase, std::string_view name = {}, std::string_view value = {}) noexcept;
    int on_begin_headers(const nghttp2_frame& frame);
    int on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value);
    int on_frame_recv(const nghttp2_frame& frame);
    int on_data_chunk_recv(std::uint8_t flags, std::uint32_t stream_id, std::string_view data);
    int on_stream_close(std::uint32_t stream_id, std::uint32_t error_code);
    int before_frame_send(const nghttp2_frame& frame);
    int on_frame_send(const nghttp2_frame& frame);
    int on_extension_chunk(const nghttp2_frame_hd& header, std::string_view data);

    int begin_request_headers(const nghttp2_frame& frame);
    int begin_response_headers(const nghttp2_frame& frame);
    int begin_push_promise(const nghttp2_frame& frame);
    // Validates one decoded field against the active block; false is malformed.
    [[nodiscard]] bool accept_header_field(std::string_view name, std::string_view value);
    void finish_header_block(const nghttp2_frame& frame);
    [[nodiscard]] bool finish_request_head(http2_stream_state& stream);
    [[nodiscard]] bool finish_response_head(http2_stream_state& stream, header_block_type& block);
    [[nodiscard]] bool finish_trailers(http2_stream_state& stream);
    void finish_push_promise(const nghttp2_frame& frame, header_block_type& block);
    // nghttp2 discards a PUSH_PROMISE whose associated stream closes before the
    // block ends; the promised stream it already reserved is cancelled here.
    void cancel_push_promise_on(std::uint32_t associated_stream_id);
    void finish_data_frame(http2_stream_state& stream);
    void process_rst_stream(const nghttp2_frame& frame);
    void process_goaway(const nghttp2_frame& frame);
    void process_settings(const nghttp2_frame& frame);
    void process_ping(const nghttp2_frame& frame);
    void process_priority_update(const nghttp2_frame& frame);
    void process_advertisement(const nghttp2_frame_hd& header);
    // A malformed field block: RST_STREAM(PROTOCOL_ERROR), plus the malformed
    // request-head budget for a server's initial head.
    void reject_header_block(std::uint32_t stream_id, bool malformed_request_head);

    // Emit message_head (and message_end when the peer already ended the stream).
    void emit_request_headers(http2_stream_state& stream);

    // --- stream lifecycle -------------------------------------------------------
    [[nodiscard]] http2_stream_state* create_stream(std::uint32_t stream_id);
    [[nodiscard]] std::optional<http2_request_head_submit_error> local_request_admission_error() const noexcept;
    // Queues the staged request fields as the next client stream's HEADERS.
    // A failure consumes neither a stream identifier nor a concurrency slot.
    template <typename prepare_type>
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_local_request_head(http2_end_stream end_stream, const prepare_type& prepare);
    void release_local_request_stream_if_closed(http2_stream_state& stream) noexcept;
    void release_local_request_stream(http2_stream_state& stream) noexcept;
    void unpin_stream(std::uint32_t stream_id);
    [[nodiscard]] bool release_received_data(std::uint32_t stream_id, std::uint32_t bytes);
    void abandon_stream_lease_checked(std::uint32_t stream_id);
    // Return consumed receive credit to nghttp2's WINDOW_UPDATE accounting.
    void consume_received(std::uint32_t stream_id, std::size_t bytes);
    // Preserve a removed stream's unacknowledged debt as connection credit.
    void flush_window_debt(http2_stream_state& stream);

    // Submits HEADERS for `stream` from the staged field list.
    void submit_header_frames(http2_stream_state& stream, http2_end_stream end_stream);
    [[nodiscard]] std::optional<http2_response_head_submit_error> response_head_admission_error(const http2_stream_state* stream, http_status_code status) const noexcept;
    [[nodiscard]] bool commit_final_response_head(http2_stream_state& stream, const http_response& response, const http_framed_response_head_plan& head_plan, http2_end_stream end_stream, std::optional<std::uint64_t> content_length);
    void commit_connect_response_head(http2_stream_state& stream, bool terminal_remote_half);
    void apply_response_priority(http2_stream_state& stream, const http_response_headers& headers);
    // Sends `bytes` (already clamped to both send windows) as DATA within this
    // call; nghttp2 copies them while the session drains.
    void send_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream);
    // Commits the local END_STREAM, then sends it as empty DATA or with the
    // staged trailer fields.
    void end_local_stream(http2_stream_state& stream, bool empty_data);
    [[nodiscard]] http2_submit_status submit_extension_frame(http2_frame_type type, std::uint32_t stream_id, http2_extension_payload& payload);
    // nghttp2 DATA source: copies the submitting call's bytes. A null source is
    // the empty END_STREAM a rejected client CONNECT owes, which may wait for
    // flow-control credit inside nghttp2.
    static nghttp2_ssize read_outbound_data(nghttp2_session* session, std::int32_t stream_id, std::uint8_t* buffer, std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source, void* user_data) noexcept;
    // An empty END_STREAM DATA frame needs positive connection and stream send
    // windows (nghttp2 defers zero-window DATA).
    [[nodiscard]] bool can_send_empty_end_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::int32_t connection_send_window() const noexcept;
    [[nodiscard]] std::uint32_t peer_setting(nghttp2_settings_id id) const noexcept;

    // Appends an event into reserved capacity and counts it on every stream it names.
    void publish_event(http2_event event);

    // Peer-induced output is bounded without a clock; every limit ends in
    // GOAWAY(ENHANCE_YOUR_CALM). PING ACK, SETTINGS ACK and each RST_STREAM that
    // answers a peer frame count until output fully drains (Go x/net/http2
    // maxQueuedControlFrames); such resets also consume a lifetime budget, as h2
    // max_local_error_reset_streams.
    static constexpr std::uint32_t max_undrained_control_frames = 1000;
    static constexpr std::uint32_t max_peer_error_resets = 1024;
    // Rapid reset (CVE-2023-44487): peer resets may run this far ahead of the
    // responses this connection let complete.
    static constexpr std::uint32_t max_unprocessed_resets = 1000;
    // Pingora's lifetime bound on malformed server request heads.
    static constexpr std::uint32_t max_malformed_request_streams = 32;
    // CONTINUATION frames following one HEADERS/PUSH_PROMISE (CVE-2024-27316).
    static constexpr std::size_t max_continuation_frames = 1024;

    // Owner/local resets do not count against peer-induced budgets.
    void submit_owner_reset(std::uint32_t stream_id, http2_error_code error);
    // A stream error found in Ruvia's semantics: RST_STREAM and a local close event.
    void reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error);

    enum class close_notification_type : std::uint8_t { emit_event,
        owner_already_knows };
    bool close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source, http2_error_code error, close_notification_type notification);
    bool close_stream(std::uint32_t stream_id, http2_stream_close_source source, http2_error_code error);
    void finish_drain_round_trip();
    void begin_final_drain();
    void submit_goaway(std::uint32_t last_stream_id);

    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    std::unique_ptr<nghttp2_session, session_deleter> session_;
    http2_output_buffer output_;
    http2_field_list fields_;
    http2_stream_table streams_;
    // RFC 9218 priority signals received for streams the peer has not opened yet.
    struct idle_priority_entry final {
        std::uint32_t stream_id_{0};
        http_priority_fields fields_{};
    };
    std::vector<idle_priority_entry> priorities_;
    std::vector<http2_event> events_;
    std::size_t event_offset_{0};
    http2_stream_lease_ledger leases_;
    std::vector<std::uint32_t> owner_resets_;
    std::string extension_payload_;
    std::optional<header_block_type> header_block_;
    // A PUSH_PROMISE's request decodes into scratch storage; the promised
    // stream's own receive state awaits the pushed response.
    std::optional<http2_stream_state> push_scratch_;
    std::uint32_t push_associated_stream_id_{0};
    std::exception_ptr callback_failure_;

    http2_local_connection_state local_connection_state_;
    http2_role role_{http2_role::server};
    bool enable_push_{false};
    bool receive_origin_advertisements_{false};
    bool receive_frame_metadata_{false};
    http2_frame_observer_type frame_observer_{nullptr};
    http2_header_observer_type header_observer_{nullptr};
    void* observer_context_{nullptr};
    http2_frame_metadata observed_frame_{};
    std::uint64_t observed_next_sequence_{0};
    std::uint64_t observed_header_sequence_{0};
    std::size_t observed_field_index_{0};
    std::uint32_t observed_header_stream_{0};
    std::size_t observed_payload_remaining_{0};
    std::size_t observed_payload_offset_{0};
    std::array<unsigned char, 6> observed_scalar_{};
    std::size_t observed_scalar_size_{0};
    bool observed_headers_padding_{false};
    bool started_{false};
    bool received_peer_settings_{false};
    bool header_block_in_progress_{false};
    bool liveness_ping_outstanding_{false};
    std::uint32_t last_peer_stream_id_{0};
    std::uint32_t highest_peer_stream_id_{0};
    std::uint32_t active_local_request_streams_{0};
    std::optional<http2_goaway_event> peer_goaway_;
    std::size_t header_block_bytes_{0};

    std::uint32_t malformed_request_streams_{0};
    std::uint64_t peer_reset_streams_{0};
    std::uint64_t completed_responses_{0};
    std::uint32_t undrained_control_frames_{0};
    std::uint32_t peer_error_resets_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

constexpr std::string_view http2_drain_debug = "connection draining";
// Opaque payloads of the locally originated PINGs (RFC 9113 §6.7 echoes them
// unchanged); distinct values tell the graceful-drain round trip from liveness.
constexpr std::string_view http2_drain_ping_payload = "ruviadrn";
constexpr std::string_view http2_liveness_ping_payload = "ruvialiv";
static_assert(http2_drain_ping_payload.size() == 8 && http2_liveness_ping_payload.size() == 8);

[[nodiscard]] std::string_view bytes_view(const std::uint8_t* data, std::size_t size) noexcept {
    return size == 0 ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(data), size);
}

}  // namespace

// Static entry points registered with nghttp2. Each one is noexcept: an
// exception is recorded on the connection and reported to nghttp2 as a fatal
// callback failure, then rethrown after nghttp2 returns.
struct http2_session_callbacks final {
    template <typename function_type>
    static int guard(void* user_data, function_type&& function) noexcept {
        auto& connection = *static_cast<http2_connection*>(user_data);
        try {
            return std::forward<function_type>(function)(connection);
        } catch (...) {
            connection.record_callback_failure();
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int on_begin_frame(nghttp2_session*, const nghttp2_frame_hd* header, void* user_data) {
        return guard(user_data, [header](http2_connection& connection) {
            return connection.on_begin_frame(*header);
        });
    }
    static int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_begin_headers(*frame);
        });
    }
    static int on_header(nghttp2_session*, const nghttp2_frame* frame, nghttp2_rcbuf* name, nghttp2_rcbuf* value, std::uint8_t, void* user_data) {
        return guard(user_data, [frame, name, value](http2_connection& connection) {
            const auto name_buffer = nghttp2_rcbuf_get_buf(name);
            const auto value_buffer = nghttp2_rcbuf_get_buf(value);
            return connection.on_header(*frame, bytes_view(name_buffer.base, name_buffer.len), bytes_view(value_buffer.base, value_buffer.len));
        });
    }
    static int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_frame_recv(*frame);
        });
    }
    static int on_data_chunk_recv(nghttp2_session*, std::uint8_t flags, std::int32_t stream_id, const std::uint8_t* data, std::size_t length, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_data_chunk_recv(flags, static_cast<std::uint32_t>(stream_id), bytes_view(data, length));
        });
    }
    static int on_stream_close(nghttp2_session*, std::int32_t stream_id, std::uint32_t error_code, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_stream_close(static_cast<std::uint32_t>(stream_id), error_code);
        });
    }
    static int before_frame_send(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.before_frame_send(*frame);
        });
    }
    static int on_frame_send(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_frame_send(*frame);
        });
    }
    static int on_extension_chunk(nghttp2_session*, const nghttp2_frame_hd* header, const std::uint8_t* data, std::size_t length, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_extension_chunk(*header, bytes_view(data, length));
        });
    }
    // ORIGIN and ALTSVC payloads are decoded from the accumulated chunks in
    // on_frame_recv, so no unpacked object is handed back to nghttp2.
    static int unpack_extension(nghttp2_session*, void** payload, const nghttp2_frame_hd*, void*) noexcept {
        *payload = nullptr;
        return 0;
    }
    static nghttp2_ssize pack_extension(nghttp2_session*, std::uint8_t* buffer, std::size_t length, const nghttp2_frame* frame, void*) noexcept {
        const auto& payload = *static_cast<const http2_extension_payload*>(frame->ext.payload);
        if (payload.size_ > length) {
            return NGHTTP2_ERR_CANCEL;
        }
        payload.write_(payload, std::span<char>(reinterpret_cast<char*>(buffer), payload.size_));
        return static_cast<nghttp2_ssize>(payload.size_);
    }
    // DATA frames may use the peer's whole SETTINGS_MAX_FRAME_SIZE (RFC 9113
    // §4.2) instead of nghttp2's 16 KiB default.
    static nghttp2_ssize read_length(nghttp2_session*, std::uint8_t, std::int32_t, std::int32_t session_window, std::int32_t stream_window, std::uint32_t remote_max_frame_size, void*) noexcept {
        const auto window = (std::min)(session_window, stream_window);
        return static_cast<nghttp2_ssize>((std::max)(std::int64_t{1}, (std::min)(static_cast<std::int64_t>(remote_max_frame_size), static_cast<std::int64_t>(window))));
    }
};

nghttp2_ssize http2_connection::read_outbound_data(nghttp2_session*, std::int32_t, std::uint8_t* buffer, std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source, void*) noexcept {
    auto* data = static_cast<outbound_data_type*>(source->ptr);
    if (data == nullptr) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    const auto copied = (std::min)(length, data->bytes_.size());
    if (copied != 0) {
        std::memcpy(buffer, data->bytes_.data(), copied);
        data->bytes_.remove_prefix(copied);
    }
    if (data->bytes_.empty()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        data->finished_ = true;
    }
    return static_cast<nghttp2_ssize>(copied);
}

http2_connection::http2_connection(http2_role role, bool enable_push, bool receive_origin_advertisements, bool receive_frame_metadata, http2_frame_observer_type frame_observer, http2_header_observer_type header_observer, void* observer_context) :
    memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)),
    role_(role) {
    enable_push_ = role == http2_role::client && enable_push;
    receive_origin_advertisements_ = role == http2_role::client && receive_origin_advertisements;
    receive_frame_metadata_ = receive_frame_metadata;
    frame_observer_ = frame_observer;
    header_observer_ = header_observer;
    observer_context_ = observer_context;
    create_session();
}

http2_connection::~http2_connection() = default;

void http2_connection::create_session() {
    struct callbacks_deleter final {
        void operator()(nghttp2_session_callbacks* value) const noexcept {
            nghttp2_session_callbacks_del(value);
        }
    };
    struct option_deleter final {
        void operator()(nghttp2_option* value) const noexcept {
            nghttp2_option_del(value);
        }
    };
    nghttp2_session_callbacks* raw_callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&raw_callbacks) != 0) {
        throw std::bad_alloc();
    }
    const std::unique_ptr<nghttp2_session_callbacks, callbacks_deleter> callbacks(raw_callbacks);
    nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks.get(), &http2_session_callbacks::on_begin_frame);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks.get(), &http2_session_callbacks::on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback2(callbacks.get(), &http2_session_callbacks::on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks.get(), &http2_session_callbacks::on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks.get(), &http2_session_callbacks::on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks.get(), &http2_session_callbacks::on_stream_close);
    nghttp2_session_callbacks_set_before_frame_send_callback(callbacks.get(), &http2_session_callbacks::before_frame_send);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks.get(), &http2_session_callbacks::on_frame_send);
    nghttp2_session_callbacks_set_on_extension_chunk_recv_callback(callbacks.get(), &http2_session_callbacks::on_extension_chunk);
    nghttp2_session_callbacks_set_unpack_extension_callback(callbacks.get(), &http2_session_callbacks::unpack_extension);
    nghttp2_session_callbacks_set_pack_extension_callback2(callbacks.get(), &http2_session_callbacks::pack_extension);
    nghttp2_session_callbacks_set_data_source_read_length_callback2(callbacks.get(), &http2_session_callbacks::read_length);

    nghttp2_option* raw_option = nullptr;
    if (nghttp2_option_new(&raw_option) != 0) {
        throw std::bad_alloc();
    }
    const std::unique_ptr<nghttp2_option, option_deleter> option(raw_option);
    // Received DATA is credited only when the owner consumes it.
    nghttp2_option_set_no_auto_window_update(option.get(), 1);
    // Ruvia's request/response validators own HTTP message semantics.
    nghttp2_option_set_no_http_messaging(option.get(), 1);
    // RFC 9113 §6.5.2: no concurrency limit applies until the peer sets one.
    nghttp2_option_set_peer_max_concurrent_streams(option.get(), (std::numeric_limits<std::uint32_t>::max)());
    nghttp2_option_set_max_reserved_remote_streams(option.get(), static_cast<std::uint32_t>(http2_stream_table::max_push_streams));
    nghttp2_option_set_max_send_header_block_length(option.get(), max_http2_encoded_header_block_bytes);
    // The encoder never inserts dynamic-table entries.
    nghttp2_option_set_max_deflate_dynamic_table_size(option.get(), 0);
    nghttp2_option_set_max_settings(option.get(), http2_received_settings::max_frame_entries);
    nghttp2_option_set_max_continuations(option.get(), max_continuation_frames);
    nghttp2_option_set_max_outbound_ack(option.get(), max_undrained_control_frames);
    // The core has no clock: nghttp2's time-based limiters stay out of reach and
    // the deterministic budgets in this class bound abuse instead.
    constexpr auto unlimited = (std::numeric_limits<std::uint64_t>::max)();
    nghttp2_option_set_stream_reset_rate_limit(option.get(), unlimited, 0);
    nghttp2_option_set_glitch_rate_limit(option.get(), unlimited, 0);
    if (role_ == http2_role::client) {
        nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(http2_frame_type::alternative_service));
        if (receive_origin_advertisements_) {
            nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(http2_frame_type::origin));
        }
    }
    nghttp2_option_set_builtin_recv_extension_type(option.get(), NGHTTP2_PRIORITY_UPDATE);
    // Unknown extensions remain semantically ignored, but use nghttp2's same
    // framing callbacks when a wire observer needs their position.
    if (frame_observer_ != nullptr) {
        for (unsigned type = 0x0a; type <= 0xff; ++type) {
            if (type != NGHTTP2_PRIORITY_UPDATE) {
                nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(type));
            }
        }
    }

    nghttp2_session* raw_session = nullptr;
    const int result = role_ == http2_role::client ? nghttp2_session_client_new3(&raw_session, callbacks.get(), this, option.get(), &memory_table_) : nghttp2_session_server_new3(&raw_session, callbacks.get(), this, option.get(), &memory_table_);
    if (result == NGHTTP2_ERR_NOMEM) {
        memory_.rethrow_allocation_failure();
    }
    if (result != 0) {
        throw std::runtime_error("nghttp2 failed to create an HTTP/2 session");
    }
    session_.reset(raw_session);
}

void http2_connection::record_callback_failure() noexcept {
    if (!callback_failure_) {
        callback_failure_ = std::current_exception();
    }
}

void http2_connection::fail_connection(http2_error_code error) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    header_block_in_progress_ = false;
    header_block_.reset();
    push_scratch_.reset();
    local_connection_state_.fail(error);
    // Queues GOAWAY(error) and makes nghttp2 ignore every later inbound frame.
    // An allocation failure here only loses the GOAWAY; the state is already terminal.
    (void)nghttp2_session_terminate_session(session_.get(), static_cast<std::uint32_t>(error));
}

void http2_connection::fail_on_nghttp2_error(int error) {
    auto failure = std::exchange(callback_failure_, nullptr);
    auto code = http2_error_code::internal_error;
    switch (error) {
        case NGHTTP2_ERR_FLOODED:
        case NGHTTP2_ERR_TOO_MANY_CONTINUATIONS:
        case NGHTTP2_ERR_SETTINGS_EXPECTED:
            code = error == NGHTTP2_ERR_SETTINGS_EXPECTED ? http2_error_code::protocol_error : http2_error_code::enhance_your_calm;
            break;
        case NGHTTP2_ERR_BAD_CLIENT_MAGIC:
            code = http2_error_code::protocol_error;
            break;
        default:
            break;
    }
    fail_connection(code);
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (error == NGHTTP2_ERR_NOMEM) {
        memory_.rethrow_allocation_failure();
    }
}

void http2_connection::check_nghttp2(int result) {
    if (result >= 0) {
        return;
    }
    if (result == NGHTTP2_ERR_NOMEM || nghttp2_is_fatal(result)) {
        fail_on_nghttp2_error(result);
        throw std::runtime_error(nghttp2_strerror(result));
    }
    throw std::logic_error(nghttp2_strerror(result));
}

void http2_connection::flush_session() {
    for (;;) {
        const std::uint8_t* data = nullptr;
        const auto produced = nghttp2_session_mem_send2(session_.get(), &data);
        if (produced < 0) {
            // A failed mem_send2 leaves nghttp2's outbound state indeterminate,
            // so nothing more is sent on this session.
            const auto error = static_cast<int>(produced);
            if (local_connection_state_.fatal_failure() == nullptr) {
                local_connection_state_.fail(http2_error_code::internal_error);
            }
            if (auto failure = std::exchange(callback_failure_, nullptr)) {
                std::rethrow_exception(failure);
            }
            if (error == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::runtime_error(nghttp2_strerror(error));
        }
        if (produced == 0) {
            break;
        }
        const auto bytes = bytes_view(data, static_cast<std::size_t>(produced));
        const auto header = parse_http2_frame_header(std::span<const char>(bytes.data(), bytes.size()));
        const bool preface = bytes.size() == http2_client_preface.size() && bytes == http2_client_preface;
        const bool is_data = !preface && header && header->type_ == static_cast<std::uint8_t>(http2_frame_type::data);
        try {
            output_.append_segment(bytes, is_data ? header->stream_id_ : 0, is_data);
        } catch (...) {
            // nghttp2 already counts these bytes as sent; the wire can no longer
            // be made consistent.
            if (local_connection_state_.fatal_failure() == nullptr) {
                local_connection_state_.fail(http2_error_code::internal_error);
            }
            throw;
        }
    }
}

void http2_connection::begin_connection() {
    if (started_) {
        return;
    }
    const auto entries = http2_local_settings_entries(enable_push_);
    check_nghttp2(nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, entries.data(), entries.size()));
    check_nghttp2(nghttp2_session_set_local_window_size(session_.get(), NGHTTP2_FLAG_NONE, 0, static_cast<std::int32_t>(http2_local_settings::initial_window_size)));
    started_ = true;
    flush_session();
}

// --- outbound byte buffer (batched writes) ------------------------------------

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes, std::string& into, http2_data_output_observer_type observer, void* observer_context) {
    const auto result_value = output_.take_batch(max_bytes, into, observer, observer_context);
    if (result_value.status_ == http2_output_batch_status::taken && !output_.wants_write()) {
        undrained_control_frames_ = 0;
    }
    return result_value;
}

// --- event queue --------------------------------------------------------------

http2_event* http2_connection::peek_event() & noexcept {
    if (event_offset_ < events_.size()) {
        return &events_[event_offset_];
    }
    events_.clear();
    event_offset_ = 0;
    return nullptr;
}

void http2_connection::consume_event() noexcept {
    if (event_offset_ >= events_.size()) {
        std::terminate();
    }
    // Snapshot the ids first: a lease retry may publish events and reallocate the queue.
    std::array<std::uint32_t, 2> stream_ids{};
    std::size_t stream_count = 0;
    events_[event_offset_++].for_each_referenced_stream([&](std::uint32_t stream_id) noexcept {
        stream_ids[stream_count++] = stream_id;
    });
    // Only the consumed event's streams can newly satisfy a deferred lease release.
    for (std::size_t index = 0; index < stream_count; ++index) {
        if (auto* stream = streams_.find(stream_ids[index])) {
            stream->remove_pending_event();
        }
        retry_deferred_stream_lease(stream_ids[index]);
    }
    flush_output();
}

void http2_connection::publish_event(http2_event event) {
    reserve_geometric(events_, events_.size() + 1);
    events_.push_back(std::move(event));
    events_.back().for_each_referenced_stream([this](std::uint32_t stream_id) noexcept {
        if (auto* stream = streams_.find(stream_id)) {
            stream->add_pending_event();
        }
    });
}

// --- inbound -----------------------------------------------------------------

http2_feed_result http2_connection::feed(std::string_view in) {
    // The facade begins the connection before it offers any input.
    if (!started_) {
        std::terminate();
    }
    // An unread event may carry a zero-copy view into the previous input, so
    // accepting another span here would invalidate its bytes.
    if (event_offset_ < events_.size()) {
        return http2_feed_result::events_pending;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_feed_result::protocol_failure;
    }
    events_.clear();
    event_offset_ = 0;
    const auto consumed = frame_observer_ != nullptr ? receive_observed(in) : nghttp2_session_mem_recv2(session_.get(), reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
    if (consumed < 0) {
        try {
            fail_on_nghttp2_error(static_cast<int>(consumed));
        } catch (...) {
            try {
                flush_session();
                // NOLINTNEXTLINE(bugprone-empty-catch): the original failure wins.
            } catch (...) {
            }
            throw;
        }
    }
    flush_session();
    return local_connection_state_.fatal_failure() != nullptr ? http2_feed_result::protocol_failure : http2_feed_result::accepted;
}

nghttp2_ssize http2_connection::receive_observed(std::string_view input) {
    const auto original_size = input.size();
    while (!input.empty()) {
        // nghttp2 is the only frame-header parser. Until on_begin_frame gives
        // us a payload length, offer one octet; afterwards offer whole payload
        // fragments. No magic/header bytes are interpreted or retained here.
        const auto count = observed_payload_remaining_ == 0 ? std::size_t{1} : (std::min)(input.size(), observed_payload_remaining_);
        if (observed_payload_remaining_ != 0) {
            observe_frame_payload(input.substr(0, count));
            observed_payload_remaining_ -= count;
            if (observed_payload_remaining_ == 0) {
                frame_observer_(observer_context_, observed_frame_);
            }
        }
        const auto consumed = nghttp2_session_mem_recv2(session_.get(), reinterpret_cast<const std::uint8_t*>(input.data()), count);
        if (consumed < 0) {
            return consumed;
        }
        if (consumed == 0 || local_connection_state_.fatal_failure() != nullptr) {
            break;
        }
        input.remove_prefix(static_cast<std::size_t>(consumed));
    }
    return static_cast<nghttp2_ssize>(original_size);
}

void http2_connection::observe_frame_payload(std::string_view input) noexcept {
    const auto type = observed_frame_.type_;
    const bool settings = type == http2_frame_type::settings;
    const bool window = type == http2_frame_type::window_update;
    const bool priority = type == http2_frame_type::priority || (type == http2_frame_type::headers && (observed_frame_.flags_ & NGHTTP2_FLAG_PRIORITY) != 0);
    if (!settings && !window && !priority) {
        return;
    }
    const auto scalar_bytes = settings ? std::size_t{6} : (window ? std::size_t{4} : std::size_t{5});
    for (const unsigned char byte : input) {
        const auto offset = observed_payload_offset_++;
        if (observed_headers_padding_ && offset == 0) {
            continue;
        }
        if (!settings && observed_scalar_size_ == scalar_bytes) {
            break;
        }
        observed_scalar_[observed_scalar_size_++] = byte;
        if (observed_scalar_size_ != scalar_bytes) {
            continue;
        }
        const auto read_u32 = [this](std::size_t start) noexcept {
            return (std::uint32_t{observed_scalar_[start]} << 24) | (std::uint32_t{observed_scalar_[start + 1]} << 16) | (std::uint32_t{observed_scalar_[start + 2]} << 8) | std::uint32_t{observed_scalar_[start + 3]};
        };
        if (settings) {
            if (observed_frame_.settings_count_ < observed_frame_.settings_.size()) {
                observed_frame_.settings_[observed_frame_.settings_count_++] = {static_cast<http2_setting_id>((std::uint16_t{observed_scalar_[0]} << 8) | observed_scalar_[1]), read_u32(2)};
            }
            observed_scalar_size_ = 0;
        } else if (window) {
            observed_frame_.window_increment_ = read_u32(0) & 0x7fffffffU;
        } else {
            const auto dependency = read_u32(0);
            observed_frame_.priority_ = http2_received_priority{.stream_id_ = observed_frame_.stream_id_, .stream_dependency_ = dependency & 0x7fffffffU, .weight_ = static_cast<std::uint16_t>(observed_scalar_[4] + 1), .exclusive_ = (dependency & 0x80000000U) != 0};
        }
    }
}

void http2_connection::observe_header(http2_header_observation_phase phase, std::string_view name, std::string_view value) noexcept {
    if (header_observer_ == nullptr) {
        return;
    }
    const http2_header_metadata metadata{.first_frame_observation_sequence_ = observed_header_sequence_, .frame_observation_sequence_ = observed_frame_.observation_sequence_, .stream_id_ = observed_header_stream_, .field_index_ = observed_field_index_, .phase_ = phase, .name_ = name, .value_ = value};
    header_observer_(observer_context_, metadata);
    if (phase == http2_header_observation_phase::field) {
        ++observed_field_index_;
    }
}

int http2_connection::on_begin_frame(const nghttp2_frame_hd& header) {
    if (frame_observer_ != nullptr || header_observer_ != nullptr) {
        observed_frame_ = {};
        observed_frame_.observation_sequence_ = observed_next_sequence_++;
        observed_frame_.payload_length_ = static_cast<std::uint32_t>(header.length);
        observed_frame_.stream_id_ = static_cast<std::uint32_t>(header.stream_id);
        observed_frame_.type_ = static_cast<http2_frame_type>(header.type);
        observed_frame_.flags_ = header.flags;
        observed_payload_remaining_ = header.length;
        observed_payload_offset_ = 0;
        observed_scalar_size_ = 0;
        observed_headers_padding_ = header.type == NGHTTP2_HEADERS && (header.flags & NGHTTP2_FLAG_PADDED) != 0;
        if (header.type == NGHTTP2_SETTINGS) {
            observed_frame_.settings_complete_ = header.length % 6 == 0 && header.length / 6 <= observed_frame_.settings_.size();
        }
        if (frame_observer_ != nullptr && header.length == 0) {
            frame_observer_(observer_context_, observed_frame_);
        }
    }
    switch (header.type) {
        case NGHTTP2_HEADERS:
            if (role_ == http2_role::server && (header.stream_id & 1) != 0) {
                highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, static_cast<std::uint32_t>(header.stream_id));
            }
            header_block_bytes_ = header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            break;
        case NGHTTP2_PUSH_PROMISE:
            header_block_bytes_ = header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            break;
        case NGHTTP2_CONTINUATION:
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            if (header.length > max_http2_encoded_header_block_bytes - header_block_bytes_) {
                // A partial block cannot be skipped without desynchronizing the
                // connection-wide HPACK state (RFC 9113 §4.3).
                fail_connection(http2_error_code::compression_error);
                break;
            }
            header_block_bytes_ += header.length;
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            header_block_in_progress_ = false;
            extension_payload_.clear();
            break;
        default:
            header_block_in_progress_ = false;
            break;
    }
    return 0;
}

int http2_connection::on_extension_chunk(const nghttp2_frame_hd& header, std::string_view data) {
    if (role_ == http2_role::client && (header.type == static_cast<std::uint8_t>(http2_frame_type::alternative_service) || (header.type == static_cast<std::uint8_t>(http2_frame_type::origin) && receive_origin_advertisements_))) {
        extension_payload_.append(data.data(), data.size());
    }
    return 0;
}

int http2_connection::on_frame_recv(const nghttp2_frame& frame) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    switch (frame.hd.type) {
        case NGHTTP2_DATA:
            if ((frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
                if (auto* stream = streams_.find(frame.hd.stream_id); stream != nullptr && !stream->is_aborted()) {
                    finish_data_frame(*stream);
                }
            }
            break;
        case NGHTTP2_HEADERS:
        case NGHTTP2_PUSH_PROMISE:
            header_block_in_progress_ = false;
            finish_header_block(frame);
            break;
        case NGHTTP2_RST_STREAM:
            process_rst_stream(frame);
            break;
        case NGHTTP2_SETTINGS:
            if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
                process_settings(frame);
            }
            break;
        case NGHTTP2_PING:
            process_ping(frame);
            break;
        case NGHTTP2_GOAWAY:
            process_goaway(frame);
            break;
        case NGHTTP2_WINDOW_UPDATE:
            if (receive_frame_metadata_) {
                publish_event(http2_event(http2_received_window_update{.stream_id_ = static_cast<std::uint32_t>(frame.hd.stream_id), .increment_ = static_cast<std::uint32_t>(frame.window_update.window_size_increment)}));
            }
            break;
        case NGHTTP2_PRIORITY_UPDATE:
            process_priority_update(frame);
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            if (role_ == http2_role::client && (frame.hd.type == static_cast<std::uint8_t>(http2_frame_type::alternative_service) || receive_origin_advertisements_)) {
                process_advertisement(frame.hd);
            }
            break;
        default:
            break;
    }
    return 0;
}

void http2_connection::process_settings(const nghttp2_frame& frame) {
    received_peer_settings_ = true;
    if (!receive_frame_metadata_) {
        return;
    }
    http2_received_settings settings;
    const auto count = (std::min)(frame.settings.niv, http2_received_settings::max_entries);
    for (std::size_t index = 0; index < count; ++index) {
        settings.entries_[index] = http2_setting_entry{.id_ = static_cast<http2_setting_id>(frame.settings.iv[index].settings_id), .value_ = frame.settings.iv[index].value};
    }
    settings.size_ = count;
    publish_event(http2_event(settings));
}

void http2_connection::process_ping(const nghttp2_frame& frame) {
    if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        return;
    }
    const auto payload = bytes_view(frame.ping.opaque_data, sizeof(frame.ping.opaque_data));
    // The drain PING's ACK completes the round trip that in-flight stream
    // creation was given (RFC 9113 §6.8).
    if (payload == http2_drain_ping_payload) {
        finish_drain_round_trip();
    } else if (payload == http2_liveness_ping_payload) {
        liveness_ping_outstanding_ = false;
    }
}

void http2_connection::process_goaway(const nghttp2_frame& frame) {
    const http2_goaway_event goaway(static_cast<std::uint32_t>(frame.goaway.last_stream_id), static_cast<http2_error_code>(frame.goaway.error_code));
    // A GOAWAY boundary can cover more live streams than the concurrency budget:
    // reserved push promises hold no concurrency slot (RFC 9113 §5.1.2, §8.4.2)
    // yet remain open and must be closed here.
    std::array<std::uint32_t, http2_stream_table::max_streams> unprocessed_stream_ids;
    std::size_t unprocessed_count = 0;
    if (role_ == http2_role::client) {
        bool response_started_above_last = false;
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 || stream.id() <= goaway.last_stream_id() || stream.is_aborted()) {
                return;
            }
            // A response head proves that the peer acted on this request; calling
            // it unprocessed would make replay unsafe (RFC 9113 §8.7).
            if (http2_remote_final_head_decoded(stream) || stream.has_informational_response()) {
                response_started_above_last = true;
                return;
            }
            unprocessed_stream_ids[unprocessed_count++] = stream.id();
        });
        if (response_started_above_last) {
            fail_connection(http2_error_code::protocol_error);
            return;
        }
    } else {
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 && stream.id() > goaway.last_stream_id() && !http2_stream_is_closed(stream)) {
                unprocessed_stream_ids[unprocessed_count++] = stream.id();
            }
        });
    }
    std::ranges::sort(std::span(unprocessed_stream_ids).first(unprocessed_count));

    // A valid peer GOAWAY is graceful shutdown, not a local connection error. Its
    // sender opens no further streams (RFC 9113 §6.8), so our directional GOAWAY
    // follows at once without a round trip.
    if (local_connection_state_.open() != nullptr) {
        begin_final_drain();
    } else {
        finish_drain_round_trip();
    }
    peer_goaway_ = goaway;
    publish_event(http2_event(goaway));
    for (std::size_t i = 0; i < unprocessed_count; ++i) {
        const auto stream_id = unprocessed_stream_ids[i];
        if (role_ == http2_role::server) {
            (void)close_stream(stream_id, http2_stream_close_source::peer_goaway, goaway.error());
            continue;
        }
        // Higher locally initiated streams were never processed and can be
        // replayed on a new connection (RFC 9113 §8.7).
        if (close_stream_impl(stream_id, http2_stream_close_source::peer_goaway, goaway.error(), close_notification_type::owner_already_knows)) {
            publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
        }
    }
}

void http2_connection::process_advertisement(const nghttp2_frame_hd& header) {
    struct payload_guard final {
        std::string& payload_;
        ~payload_guard() {
            http_clear_string_retaining_small(payload_);
        }
    } guard{extension_payload_};
    if (role_ != http2_role::client) {
        return;
    }
    const auto payload = std::span<const char>(extension_payload_.data(), extension_payload_.size());
    if (header.type == static_cast<std::uint8_t>(http2_frame_type::origin)) {
        if (!receive_origin_advertisements_ || header.stream_id != 0 || (header.flags & 0xf) != 0) {
            return;
        }
        auto advertisement = decode_http_origin_advertisement(payload);
        if (advertisement.index() == 0) {
            publish_event(http2_event(std::move(std::get<0>(advertisement))));
        }
        return;
    }
    const auto stream_id = static_cast<std::uint32_t>(header.stream_id);
    if (stream_id != 0) {
        // ALTSVC is ignored on a CONNECT tunnel or a still-reserved push stream (RFC 7838 §4).
        const auto* stream = streams_.find(stream_id);
        if (stream == nullptr || http2_stream_is_closed(*stream) || stream->push_reservation() != http2_push_reservation::none || stream->tunnel().not_connect() == nullptr) {
            return;
        }
    }
    auto advertisement = decode_http2_alternative_service(stream_id, payload);
    if (advertisement.index() == 0) {
        publish_event(http2_event(std::move(std::get<0>(advertisement))));
    }
}

void http2_connection::process_priority_update(const nghttp2_frame& frame) {
    // nghttp2 accepts PRIORITY_UPDATE only on a server and already enforced its
    // push-stream and idle-stream rules; a Prioritized Stream ID of zero is left
    // to this layer (RFC 9218 §7.1).
    const auto& update = *static_cast<const nghttp2_ext_priority_update*>(frame.ext.payload);
    const auto id = static_cast<std::uint32_t>(update.stream_id);
    if (id == 0) {
        fail_connection(http2_error_code::protocol_error);
        return;
    }
    const auto parsed = parse_http_priority(bytes_view(update.field_value, update.field_value_len));
    if (parsed.index() != 0) {
        return;  // A malformed Priority field does not prioritize a stream.
    }
    const auto fields = std::get<0>(parsed);
    auto* stream = streams_.find(id);
    const bool idle = stream == nullptr && (id & 1U) != 0 && id > highest_peer_stream_id_;
    if (stream != nullptr) {
        if (http2_stream_is_closed(*stream) || stream->local_send().end_stream_committed() != nullptr) {
            return;
        }
        stream->set_priority_update(fields);
    } else if (idle) {
        const auto existing = std::ranges::find_if(priorities_, [id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == id;
        });
        if (existing != priorities_.end()) {
            existing->fields_ = fields;
        } else {
            // Signals for identifiers the peer skipped become unreachable once a
            // higher stream opens; drop those only when the table would grow.
            std::erase_if(priorities_, [this](const auto& entry_value) noexcept {
                return entry_value.stream_id_ <= highest_peer_stream_id_;
            });
            if (priorities_.size() >= http2_local_settings::max_concurrent_streams) {
                return;
            }
            priorities_.push_back({id, fields});
        }
    } else {
        return;
    }
    publish_event(http2_event(http_priority_update{id, (id & 1U) == 0, fields}));
}

void http2_connection::process_rst_stream(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const auto error = static_cast<http2_error_code>(frame.rst_stream.error_code);
    auto* stream = streams_.find(stream_id);
    // RFC 9113 §8.7: REFUSED_STREAM on a stream this client opened means the
    // server did no processing; the owner may replay the request. A refusal
    // after a response head contradicts that and stays a reset.
    const bool refused_request = role_ == http2_role::client && error == http2_error_code::refused_stream && (stream_id & 1U) != 0 && stream != nullptr && stream->response_status() == nullptr && !stream->has_informational_response();
    if (stream != nullptr && !stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        if (refused_request) {
            if (close_stream_impl(stream_id, http2_stream_close_source::peer, error, close_notification_type::owner_already_knows)) {
                publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
            }
        } else {
            (void)close_stream(stream_id, http2_stream_close_source::peer, error);
        }
    }
    // Rapid reset (CVE-2023-44487): a peer that keeps resetting streams far ahead
    // of the responses it lets complete is cut off.
    ++peer_reset_streams_;
    if (static_cast<std::uint64_t>(peer_reset_streams_) > static_cast<std::uint64_t>(completed_responses_) + max_unprocessed_resets) {
        fail_connection(http2_error_code::enhance_your_calm);
    }
}

int http2_connection::on_stream_close(std::uint32_t stream_id, std::uint32_t error_code) {
    if (const auto owner_reset = std::ranges::find(owner_resets_, stream_id); owner_reset != owner_resets_.end()) {
        owner_resets_.erase(owner_reset);
    }
    if (header_block_.has_value() && &header_block_->context_.stream_ == streams_.find(stream_id)) {
        header_block_.reset();
    }
    auto* stream = streams_.find(stream_id);
    // After a connection error the owner learns of every stream's end from
    // connection_error(); nghttp2 closing streams while tearing down (for
    // example on the GOAWAY that caused the error) is not a stream event.
    if (stream == nullptr || stream->is_aborted() || http2_stream_is_closed(*stream) || local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    // nghttp2 ended a stream Ruvia still considers open: a stream error it
    // detected itself (flow control, frame on a closed half, refused stream).
    (void)close_stream(stream_id, http2_stream_close_source::local, static_cast<http2_error_code>(error_code));
    return 0;
}

int http2_connection::before_frame_send(const nghttp2_frame& frame) {
    const bool ack = (frame.hd.flags & NGHTTP2_FLAG_ACK) != 0;
    if ((frame.hd.type == NGHTTP2_PING || frame.hd.type == NGHTTP2_SETTINGS) && ack) {
        if (undrained_control_frames_ >= max_undrained_control_frames) {
            fail_connection(http2_error_code::enhance_your_calm);
            return NGHTTP2_ERR_CANCEL;
        }
        ++undrained_control_frames_;
        return 0;
    }
    if (frame.hd.type != NGHTTP2_RST_STREAM) {
        return 0;
    }
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    if (const auto owner_reset = std::ranges::find(owner_resets_, stream_id); owner_reset != owner_resets_.end()) {
        owner_resets_.erase(owner_reset);
        return 0;
    }
    // Every other RST_STREAM answers a peer frame.
    if (peer_error_resets_ >= max_peer_error_resets || undrained_control_frames_ >= max_undrained_control_frames) {
        fail_connection(http2_error_code::enhance_your_calm);
        return NGHTTP2_ERR_CANCEL;
    }
    ++peer_error_resets_;
    ++undrained_control_frames_;
    return 0;
}

int http2_connection::on_frame_send(const nghttp2_frame& frame) {
    // A GOAWAY carrying an error is nghttp2's own connection error (or ours,
    // already recorded); graceful drain GOAWAYs carry NO_ERROR.
    if (frame.hd.type == NGHTTP2_GOAWAY && frame.goaway.error_code != NGHTTP2_NO_ERROR && local_connection_state_.fatal_failure() == nullptr) {
        header_block_in_progress_ = false;
        header_block_.reset();
        push_scratch_.reset();
        local_connection_state_.fail(static_cast<http2_error_code>(frame.goaway.error_code));
    } else if (frame.hd.type == NGHTTP2_DATA && (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
        const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
        if (auto* stream = streams_.find(stream_id); stream != nullptr && stream->tunnel().rejected() != nullptr && stream->local_send().connect_pending() != nullptr) {
            (void)stream->reject_local_connect();
            release_local_request_stream_if_closed(*stream);
            retry_deferred_stream_lease(stream_id);
        }
    }
    return 0;
}

// --- local control frames ---------------------------------------------------

void http2_connection::submit_goaway(std::uint32_t last_stream_id) {
    check_nghttp2(nghttp2_submit_goaway(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(last_stream_id), NGHTTP2_NO_ERROR, reinterpret_cast<const std::uint8_t*>(http2_drain_debug.data()), http2_drain_debug.size()));
}

void http2_connection::begin_drain() {
    // Graceful drain (RFC 9113 §6.8) without a connection error: established
    // streams keep running. A server first advertises 2^31-1 and keeps admitting
    // the streams the client may already have in flight; the final boundary
    // follows the ACK of a PING queued right behind that GOAWAY.
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    if (role_ != http2_role::server) {
        begin_final_drain();
        flush_session();
        return;
    }
    // A client that ignores the first GOAWAY and withholds the ACK still cannot
    // extend the round trip indefinitely: one concurrency window of further
    // streams is admitted, then the boundary becomes final.
    const auto stream_budget = std::uint32_t{2} * http2_local_settings::max_concurrent_streams;
    const auto round_trip_limit = last_peer_stream_id_ > http2_max_stream_id - stream_budget ? http2_max_stream_id : last_peer_stream_id_ + stream_budget;
    // nghttp2's shutdown notice is the 2^31-1 GOAWAY that, unlike a final
    // GOAWAY, leaves new peer streams admitted and input read while no stream
    // is active.
    check_nghttp2(nghttp2_submit_shutdown_notice(session_.get()));
    // The PING's ACK proves the peer read the GOAWAY only if the PING follows
    // it on the wire. nghttp2 sends a queued PING ahead of a queued GOAWAY, so
    // the GOAWAY is drained first.
    flush_session();
    check_nghttp2(nghttp2_submit_ping(session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_drain_ping_payload.data())));
    (void)local_connection_state_.begin_round_trip_drain(http2_max_stream_id, round_trip_limit);
    flush_session();
}

void http2_connection::finish_drain() {
    if (local_connection_state_.open() != nullptr) {
        begin_final_drain();
    } else {
        finish_drain_round_trip();
    }
    flush_session();
}

void http2_connection::begin_final_drain() {
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    submit_goaway(last_peer_stream_id_);
    (void)local_connection_state_.begin_graceful_drain(last_peer_stream_id_);
}

void http2_connection::finish_drain_round_trip() {
    const auto* drain = local_connection_state_.graceful_drain();
    if (drain == nullptr || !drain->awaiting_round_trip()) {
        return;
    }
    submit_goaway(last_peer_stream_id_);
    (void)local_connection_state_.finish_round_trip_drain(last_peer_stream_id_);
}

void http2_connection::terminate(http2_error_code error) {
    // The same terminal path as a detected connection error: GOAWAY(error)
    // names the last peer stream nghttp2 processed (RFC 9113 section 6.8).
    fail_connection(error);
    flush_session();
}

http2_submit_status http2_connection::submit_ping() {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (liveness_ping_outstanding_) {
        return http2_submit_status::accepted;
    }
    check_nghttp2(nghttp2_submit_ping(session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_liveness_ping_payload.data())));
    liveness_ping_outstanding_ = true;
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_max_concurrent_streams(std::uint32_t limit) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (limit > http2_local_settings::max_concurrent_streams) {
        return http2_submit_status::invalid_message;
    }
    const nghttp2_settings_entry entry{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, limit};
    check_nghttp2(nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, &entry, 1));
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    if (role_ != http2_role::client || local_connection_state_.open() == nullptr || stream_id == 0 || stream_id > http2_max_stream_id || ((stream_id & 1U) == 0 && stream_id > highest_peer_stream_id_)) {
        return http2_submit_status::invalid_state;
    }
    std::array<char, 32> encoded{};
    const auto size = encode_http_priority(encoded, fields_value);
    if (size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    // nghttp2 sends PRIORITY_UPDATE only to a peer that advertised
    // SETTINGS_NO_RFC7540_PRIORITIES=1 (RFC 9218 §2.1).
    if (peer_setting(NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES) == 0) {
        return http2_submit_status::peer_capability_unavailable;
    }
    check_nghttp2(nghttp2_submit_priority_update(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), reinterpret_cast<const std::uint8_t*>(encoded.data()), std::get<0>(size)));
    flush_session();
    return http2_submit_status::accepted;
}

std::uint32_t http2_connection::peer_setting(nghttp2_settings_id id) const noexcept {
    return nghttp2_session_get_remote_settings(session_.get(), id);
}

std::int32_t http2_connection::connection_send_window() const noexcept {
    return nghttp2_session_get_remote_window_size(session_.get());
}

// --- flow control & send window -----------------------------------------------

void http2_connection::consume_received(std::uint32_t stream_id, std::size_t bytes) {
    if (bytes == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    check_nghttp2(nghttp2_session_consume(session_.get(), static_cast<std::int32_t>(stream_id), bytes));
}

void http2_connection::flush_window_debt(http2_stream_state& stream) {
    const auto debt = stream.take_window_debt();
    if (debt == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    check_nghttp2(nghttp2_session_consume_connection(session_.get(), debt));
}

bool http2_connection::release_received_data(std::uint32_t stream_id, std::uint32_t bytes_value) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || bytes_value == 0 || bytes_value > stream->window_debt()) {
        return false;
    }
    consume_received(stream_id, bytes_value);
    (void)stream->take_window_debt(bytes_value);
    return true;
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return output_.pending_data_bytes(stream_id);
}

std::optional<http2_send_window_state> http2_connection::send_window_state(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr || http2_stream_is_closed(*stream)) {
        return std::nullopt;
    }
    const auto stream_window = nghttp2_session_get_stream_remote_window_size(session_.get(), static_cast<std::int32_t>(stream_id));
    if (stream_window < 0 && nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        return std::nullopt;
    }
    const auto connection_window = connection_send_window();
    return http2_send_window_state{
        .connection_window_ = connection_window,
        .stream_window_ = stream_window,
        .available_ = static_cast<std::uint32_t>((std::max)(0, (std::min)(connection_window, stream_window))),
    };
}

bool http2_connection::can_send_empty_end_stream(std::uint32_t stream_id) const noexcept {
    const auto window = send_window_state(stream_id);
    return window.has_value() && window->connection_window_ > 0 && window->stream_window_ > 0;
}

bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    return stream == nullptr || stream->is_aborted();
}

// --- stream leases ------------------------------------------------------------

void http2_connection::lease_submitted_request(const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept {
    const auto* submitted = std::get_if<std::uint32_t>(&result);
    if (submitted == nullptr) {
        return;
    }
    pin_stream(*submitted);
}

void http2_connection::release_stream_lease(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr && (stream->window_debt() != 0 || stream->has_pending_events())) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_credits);
        return;
    }
    unpin_stream(stream_id);
    leases_.erase_release(stream_id);
}

void http2_connection::release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept {
    leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_remote_end);
}

void http2_connection::abandon_stream_lease_checked(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        leases_.erase_release(stream_id);
        return;
    }
    if (!stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        const auto error = stream->local_send().end_stream_committed() != nullptr ? http2_error_code::no_error : http2_error_code::cancel;
        (void)submit_reset(stream_id, error);
    }
    release_stream_lease(stream_id);
}

void http2_connection::return_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
    try {
        (void)release_received_data(stream_id, bytes);
        retry_deferred_stream_lease(stream_id);
    } catch (...) {
        leases_.defer_credit(stream_id, bytes);
    }
    flush_output();
}

void http2_connection::abandon_stream_lease(std::uint32_t stream_id) noexcept {
    try {
        abandon_stream_lease_checked(stream_id);
    } catch (...) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::abandon);
    }
    flush_output();
}

void http2_connection::flush_output() noexcept {
    try {
        flush_session();
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_lease(std::uint32_t stream_id) noexcept {
    const auto* pending = leases_.release(stream_id);
    if (pending == nullptr) {
        return;
    }
    using kind_type = http2_stream_lease_ledger::release_kind_type;
    if (pending->kind_ == kind_type::abandon) {
        try {
            abandon_stream_lease_checked(stream_id);
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
        return;
    }
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr && (stream->window_debt() != 0 || stream->has_pending_events() || (pending->kind_ == kind_type::after_remote_end && !connection_error() && !http2_stream_is_closed(*stream)))) {
        return;
    }
    try {
        unpin_stream(stream_id);
        leases_.erase_release(stream_id);
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_leases() noexcept {
    std::size_t index = 0;
    while (index < leases_.credit_count()) {
        const auto pending = leases_.credit_at(index);
        try {
            (void)release_received_data(pending.stream_id_, pending.bytes_);
            leases_.erase_credit(pending.stream_id_);
            retry_deferred_stream_lease(pending.stream_id_);
        } catch (...) {
            ++index;
        }
    }
    index = 0;
    while (index < leases_.release_count()) {
        const auto release = leases_.release_at(index);
        retry_deferred_stream_lease(release.stream_id_);
        if (index < leases_.release_count() && leases_.release_at(index).stream_id_ == release.stream_id_) {
            ++index;
        }
    }
    flush_output();
}

// --- stream records & lifecycle -----------------------------------------------

void http2_connection::pin_stream(std::uint32_t stream_id) noexcept {
    if (auto* const stream = streams_.find(stream_id)) {
        stream->set_pinned(true);
    }
}

void http2_connection::unpin_stream(std::uint32_t stream_id) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        return;
    }
    if (stream->is_aborted()) {
        flush_window_debt(*stream);
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        release_local_request_stream(*stream);
        streams_.remove(stream_id);
        return;
    }
    if (http2_stream_is_closed(*stream)) {
        flush_window_debt(*stream);
        ++completed_responses_;
        release_local_request_stream_if_closed(*stream);
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        streams_.remove(stream_id);
        return;
    }
    const auto error = stream->local_send().end_stream_committed() != nullptr ? http2_error_code::no_error : http2_error_code::cancel;
    (void)submit_reset(stream_id, error);
    if (auto* retained_stream = streams_.find(stream_id)) {
        flush_window_debt(*retained_stream);
        release_local_request_stream(*retained_stream);
        streams_.remove(stream_id);
    }
}

bool http2_connection::close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error, close_notification_type notification) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return false;
    }
    if (header_block_.has_value() && &header_block_->context_.stream_ == stream) {
        header_block_.reset();
    }
    const bool pinned = stream->pinned();
    release_local_request_stream(*stream);
    std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
        return entry_value.stream_id_ == stream_id;
    });
    (void)stream->abort(source_value);
    if (notification == close_notification_type::emit_event) {
        publish_event(http2_event(http2_stream_closed_event(stream_id, source_value, error)));
    }
    if (!pinned) {
        flush_window_debt(*stream);
        streams_.remove(stream_id);
    }
    return true;
}

bool http2_connection::close_stream(std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) {
    return close_stream_impl(stream_id, source_value, error, close_notification_type::emit_event);
}

void http2_connection::submit_owner_reset(std::uint32_t stream_id, http2_error_code error) {
    if (nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        return;
    }
    reserve_geometric(owner_resets_, owner_resets_.size() + 1);
    owner_resets_.push_back(stream_id);
    check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
}

void http2_connection::reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error) {
    check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
    (void)close_stream(stream_id, http2_stream_close_source::local, error);
}

http2_stream_state* http2_connection::find_outbound_stream(std::uint32_t stream_id) noexcept {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return nullptr;
    }
    auto* stream = streams_.find(stream_id);
    return stream != nullptr && !stream->is_aborted() ? stream : nullptr;
}

http2_stream_state* http2_connection::create_stream(std::uint32_t stream_id) {
    return streams_.create(stream_id);
}

std::optional<http2_request_head_submit_error> http2_connection::local_request_admission_error() const noexcept {
    if (role_ != http2_role::client) {
        return http2_request_head_submit_error::invalid_state;
    }
    if (!started_) {
        std::terminate();
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_.has_value() || nghttp2_session_get_next_stream_id(session_.get()) > http2_max_stream_id) {
        return http2_request_head_submit_error::connection_unavailable;
    }
    if (active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_request_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

std::size_t http2_connection::available_local_request_streams(std::size_t local_limit) const noexcept {
    if (draining() || local_request_admission_error().has_value()) {
        return 0;
    }
    auto limit = (std::min)(local_limit, static_cast<std::size_t>(http2_local_settings::max_concurrent_streams));
    limit = (std::min)(limit, static_cast<std::size_t>(peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)));
    return limit > active_local_request_streams_ ? limit - active_local_request_streams_ : 0;
}

void http2_connection::release_local_request_stream(http2_stream_state& stream) noexcept {
    if (stream.release_peer_concurrency_slot() && active_local_request_streams_ != 0) {
        --active_local_request_streams_;
    }
}

void http2_connection::release_local_request_stream_if_closed(http2_stream_state& stream) noexcept {
    if (http2_stream_is_closed(stream)) {
        release_local_request_stream(stream);
    }
}

http2_stream_state* http2_connection::stream(std::uint32_t stream_id) & noexcept {
    return streams_.find(stream_id);
}

http2_stream_receive_status http2_connection::stream_receive_status(std::uint32_t stream_id) const noexcept {
    const auto* current = streams_.find(stream_id);
    if (current == nullptr || current->is_aborted()) {
        return http2_stream_receive_status::closed;
    }
    return current->remote_receive().end_stream() != nullptr ? http2_stream_receive_status::ended : http2_stream_receive_status::open;
}

std::optional<http2_server_request_view> http2_connection::server_request_view(std::uint32_t stream_id) const noexcept {
    if (role_ != http2_role::server) {
        return std::nullopt;
    }
    const auto* stream_state = streams_.find(stream_id);
    if (stream_state == nullptr || stream_state->request_method().empty()) {
        return std::nullopt;
    }
    return http2_server_request_view{
        .method_ = stream_state->request_method(),
        .scheme_ = stream_state->request_scheme(),
        .path_ = stream_state->request_path(),
        .authority_ = http2_effective_request_authority(*stream_state),
        .protocol_ = stream_state->request_protocol(),
        .pseudo_header_order_ = stream_state->request_pseudo_header_order(),
    };
}
}  // namespace ruvia::detail

namespace ruvia {
http2_akamai_fingerprint_collector::http2_akamai_fingerprint_collector(http2_akamai_fingerprint_options options) :
    max_frames_(options.max_frames_),
    max_bytes_(options.max_bytes_) {}

void http2_akamai_fingerprint_collector::reset() noexcept {
    settings_.clear();
    priorities_.clear();
    pseudo_headers_.clear();
    fingerprint_.clear();
    frames_seen_ = 0;
    bytes_seen_ = 0;
    window_update_ = 0;
    first_request_stream_id_ = 0;
    has_window_update_ = false;
    saw_regular_header_ = false;
    phase_ = phase::collecting;
}

void http2_akamai_fingerprint_collector::observe_frame(const http2_frame_metadata& frame) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        ++frames_seen_;
        const std::size_t frame_bytes = 9U + static_cast<std::size_t>(frame.payload_length_);
        if (frames_seen_ > max_frames_ || frame_bytes > max_bytes_ - bytes_seen_) {
            phase_ = phase::failed;
            return;
        }
        bytes_seen_ += frame_bytes;

        const auto append_priority = [&](const http2_received_priority& pri) {
            if (!priorities_.empty()) {
                priorities_.push_back(',');
            }
            http_append_unsigned(priorities_, pri.stream_id_);
            priorities_.push_back(':');
            priorities_.push_back(pri.exclusive_ ? '1' : '0');
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.stream_dependency_);
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.weight_);
        };

        switch (frame.type_) {
            case http2_frame_type::settings:
                if ((frame.flags_ & 0x01U) != 0) {
                    break;
                }
                if (!frame.settings_complete_ || frame.stream_id_ != 0) {
                    phase_ = phase::failed;
                    return;
                }
                for (const auto& entry : frame.settings()) {
                    if (!settings_.empty()) {
                        settings_.push_back(';');
                    }
                    http_append_unsigned(settings_, static_cast<std::uint16_t>(entry.id_));
                    settings_.push_back(':');
                    http_append_unsigned(settings_, entry.value_);
                }
                break;
            case http2_frame_type::window_update:
                if (frame.stream_id_ == 0 && frame.window_increment_.has_value()) {
                    window_update_ = *frame.window_increment_;
                    has_window_update_ = true;
                }
                break;
            case http2_frame_type::priority:
                if (frame.priority_.has_value()) {
                    append_priority(*frame.priority_);
                }
                break;
            case http2_frame_type::headers:
                if (frame.priority_.has_value() && (first_request_stream_id_ == 0 || frame.stream_id_ == first_request_stream_id_)) {
                    append_priority(*frame.priority_);
                }
                break;
            default:
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

void http2_akamai_fingerprint_collector::observe_header(const http2_header_metadata& header) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        switch (header.phase_) {
            case http2_header_observation_phase::begin:
                if (header.stream_id_ == 0) {
                    phase_ = phase::failed;
                    return;
                }
                if (first_request_stream_id_ == 0) {
                    first_request_stream_id_ = header.stream_id_;
                }
                break;
            case http2_header_observation_phase::field: {
                if (first_request_stream_id_ == 0) {
                    if (header.stream_id_ == 0) {
                        phase_ = phase::failed;
                        return;
                    }
                    first_request_stream_id_ = header.stream_id_;
                }
                if (header.stream_id_ != first_request_stream_id_) {
                    return;
                }
                const std::size_t field_bytes = header.name_.size() + header.value_.size();
                if (field_bytes > max_bytes_ - bytes_seen_) {
                    phase_ = phase::failed;
                    return;
                }
                bytes_seen_ += field_bytes;
                if (!saw_regular_header_ && !header.name_.empty() && header.name_.front() == ':') {
                    std::string_view token{};
                    if (header.name_ == ":method") {
                        token = "m";
                    } else if (header.name_ == ":authority") {
                        token = "a";
                    } else if (header.name_ == ":scheme") {
                        token = "s";
                    } else if (header.name_ == ":path") {
                        token = "p";
                    } else if (header.name_.size() > 1U) {
                        token = header.name_.substr(1, 1);
                    }
                    if (!token.empty()) {
                        if (!pseudo_headers_.empty()) {
                            pseudo_headers_.push_back(',');
                        }
                        pseudo_headers_.append(token);
                    }
                } else {
                    saw_regular_header_ = true;
                }
                break;
            }
            case http2_header_observation_phase::end:
                if (header.stream_id_ != first_request_stream_id_ || first_request_stream_id_ == 0) {
                    return;
                }
                if (pseudo_headers_.empty()) {
                    phase_ = phase::failed;
                    return;
                }
                fingerprint_.clear();
                fingerprint_.append(settings_);
                fingerprint_.push_back('|');
                if (has_window_update_) {
                    http_append_unsigned(fingerprint_, window_update_);
                } else {
                    fingerprint_.append("00");
                }
                fingerprint_.push_back('|');
                if (priorities_.empty()) {
                    fingerprint_.push_back('0');
                } else {
                    fingerprint_.append(priorities_);
                }
                fingerprint_.push_back('|');
                fingerprint_.append(pseudo_headers_);
                phase_ = phase::complete;
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

// nghttp2 packs an extension payload into a buffer of at least 16 KiB
// (nghttp2_pack_extension_callback2), so larger peer frame limits do not apply.
[[nodiscard]] std::uint32_t extension_payload_limit(std::uint32_t peer_max_frame_size) noexcept {
    return (std::min)(peer_max_frame_size, http2_default_max_frame_size);
}

}  // namespace

void http2_connection::apply_response_priority(http2_stream_state& stream, const http_response_headers& headers) {
    const auto parsed = parse_http_priority(headers);
    const auto* fields = std::get_if<http_priority_fields>(&parsed);
    if (fields == nullptr || (!fields->urgency_ && !fields->incremental_)) {
        return;
    }
    nghttp2_extpri priority{NGHTTP2_EXTPRI_DEFAULT_URGENCY, 0};
    if (nghttp2_session_get_extpri_stream_priority(session_.get(), &priority, static_cast<std::int32_t>(stream.id())) != 0) {
        return;
    }
    // Response parameters override only their specified request parameters.
    if (fields->urgency_) {
        priority.urgency = fields->urgency_->value();
    }
    if (fields->incremental_) {
        priority.inc = *fields->incremental_ ? 1 : 0;
    }
    check_nghttp2(nghttp2_session_change_extpri_stream_priority(session_.get(), static_cast<std::int32_t>(stream.id()), &priority, 0));
}

void http2_connection::submit_header_frames(http2_stream_state& stream, http2_end_stream end_stream) {
    const auto fields = fields_.nv();
    check_nghttp2(nghttp2_submit_headers(session_.get(), static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE), static_cast<std::int32_t>(stream.id()), nullptr, fields.data(), fields.size(), nullptr));
    fields_.release();
    if (stream.push_reservation() == http2_push_reservation::local && stream.hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    stream.activate_push();
}

void http2_connection::send_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream) {
    // The caller clamped `bytes` to both send windows, so nghttp2 serializes
    // all of it while this call drains the session; nothing is retained.
    outbound_data_type data{.bytes_ = bytes};
    nghttp2_data_provider2 provider{};
    provider.source.ptr = &data;
    provider.read_callback = &http2_connection::read_outbound_data;
    check_nghttp2(nghttp2_submit_data2(session_.get(), static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE), static_cast<std::int32_t>(stream_id), &provider));
    flush_session();
    if (!data.finished_) {
        // nghttp2 kept a pointer to this frame's source; the session cannot continue.
        fail_connection(http2_error_code::internal_error);
        flush_output();
        throw std::logic_error("nghttp2 deferred DATA within its send window");
    }
}

void http2_connection::commit_connect_response_head(http2_stream_state& stream, bool terminal_remote_half) {
    submit_header_frames(stream, http2_end_stream::keep_open);
    (void)stream.accept_connect();
    stream.begin_local_content_unbounded();
    (void)stream.open_local_connect_tunnel();
    if (terminal_remote_half) {
        publish_event(http2_event(http2_tunnel_end_event(stream.id())));
    }
    flush_session();
}

std::optional<http2_response_head_submit_error> http2_connection::response_head_admission_error(const http2_stream_state* stream, http_status_code status) const noexcept {
    if (stream == nullptr) {
        return http2_response_head_submit_error::closed;
    }
    // A 2xx answer to a pending CONNECT opens the tunnel; only the CONNECT and
    // WebSocket handshake submissions commit one.
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) || stream->local_send().head_pending() == nullptr || (status.is_successful() && stream->tunnel().pending() != nullptr)) {
        return http2_response_head_submit_error::invalid_state;
    }
    // RFC 9113 §8.4: a pushed response needs a slot under the client's
    // SETTINGS_MAX_CONCURRENT_STREAMS.
    if (stream->push_reservation() == http2_push_reservation::local && active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_response_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

bool http2_connection::commit_final_response_head(http2_stream_state& stream, const http_response& response, const http_framed_response_head_plan& head_plan, http2_end_stream end_stream, std::optional<std::uint64_t> content_length) {
    if (!append_http_binary_response_fields(fields_, response, head_plan, max_http_header_bytes)) {
        return false;
    }
    apply_response_priority(stream, response.headers());
    submit_header_frames(stream, end_stream);
    if (head_plan.body_plan().body_suppressed()) {
        stream.begin_local_content_forbidden();
    } else if (content_length.has_value()) {
        stream.begin_local_content_known_length(*content_length);
    } else {
        stream.begin_local_content_unbounded();
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream.commit_local_head_end_stream();
    } else {
        (void)stream.begin_local_response_content();
    }
    if (stream.tunnel().pending() != nullptr) {
        (void)stream.reject_connect();
    }
    release_local_request_stream_if_closed(stream);
    flush_session();
    return true;
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error> http2_connection::submit_response_head(std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, response.status())) {
        return *error;
    }
    if (write_plan.body_plan().request_method() != stream->request_known_method() || !write_plan.matches_response(response)) {
        return http2_response_head_submit_error::response_plan_mismatch;
    }
    if (http_binary_final_response_control_plan(response).has_value()) {
        return http2_response_head_submit_error::invalid_message;
    }
    const auto head_plan_result = http_framed_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        const auto error = std::get<http_framed_response_head_plan_error>(head_plan_result);
        const bool response_plan_mismatch = error == http_framed_response_head_plan_error::response_status_mismatch || error == http_framed_response_head_plan_error::response_representation_mismatch;
        return response_plan_mismatch ? http2_response_head_submit_error::response_plan_mismatch : http2_response_head_submit_error::invalid_message;
    }
    const auto end_stream = write_plan.send_body() ? http2_end_stream::keep_open : http2_end_stream::end_stream;
    if (!commit_final_response_head(*stream, response, *head_plan, end_stream, write_plan.content_length())) {
        return http2_response_head_submit_error::invalid_message;
    }
    return write_plan;
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error> http2_connection::submit_streaming_response_head(std::uint32_t stream_id, http_response head, http_response_trailer_intent trailer_intent) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, head.status())) {
        return *error;
    }
    auto prepared_commit_plan = plan_http_response_stream_commit(http_response_stream_framing::http2_frames, stream->request_known_method(), head.status(), trailer_intent);
    if (!prepared_commit_plan.trailer_intent_allowed()) {
        return http2_response_head_submit_error::invalid_message;
    }
    if (http_binary_final_response_control_plan(head).has_value()) {
        return http2_response_head_submit_error::invalid_message;
    }
    auto stream_head = prepare_http_response_stream_head(std::move(head), std::move(prepared_commit_plan));
    const auto& commit_plan = stream_head.commit_plan();
    // One prepared plan owns both the encoded Content-Length metadata and the
    // local DATA accounting contract.
    const auto head_plan_result = http_framed_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        return http2_response_head_submit_error::invalid_message;
    }
    const auto end_stream = commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended ? http2_end_stream::end_stream : http2_end_stream::keep_open;
    if (!commit_final_response_head(*stream, stream_head.response(), *head_plan, end_stream, head_plan->streaming_content_length())) {
        return http2_response_head_submit_error::invalid_message;
    }
    return commit_plan;
}

http2_submit_status http2_connection::submit_interim_response_head(std::uint32_t stream_id, const http_interim_response_head& response) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, response.status())) {
        switch (*error) {
            case http2_response_head_submit_error::closed:
                return http2_submit_status::closed;
            case http2_response_head_submit_error::peer_stream_limit_reached:
                return http2_submit_status::peer_capability_unavailable;
            default:
                return http2_submit_status::invalid_state;
        }
    }
    if (!append_http_binary_interim_response_fields(fields_, response, max_http_header_bytes)) {
        return http2_submit_status::invalid_message;
    }
    submit_header_frames(*stream, http2_end_stream::keep_open);
    flush_session();
    return http2_submit_status::accepted;
}

http2_data_submit_result http2_connection::submit_data(std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream, std::size_t max_payload_bytes) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return {0, http2_data_submit_status::closed};
    }
    const auto& local_send = stream->local_send();
    if (local_send.request_content_open() == nullptr && local_send.response_content_open() == nullptr && local_send.tunnel_open() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for DATA submission");
    }
    if (local_send.request_content_open() != nullptr) {
        if (stream->request_continue_pending()) {
            return {0, http2_data_submit_status::expectation_pending};
        }
        if (stream->request_content_canceled()) {
            throw std::logic_error("HTTP/2 request content was canceled");
        }
    }
    // Check the whole offered input before consuming a prefix. A terminal input
    // that cannot satisfy Content-Length must never partially reach the wire.
    switch (stream->local_content().check_accept(chunk.size(), http2_ends_stream(end_stream))) {
        case http_local_content_check::accepted:
            break;
        case http_local_content_check::not_started:
        case http_local_content_check::forbidden:
            throw std::logic_error("HTTP/2 stream content is not allowed");
        case http_local_content_check::length_exceeded:
        case http_local_content_check::length_incomplete:
            return {0, http2_data_submit_status::invalid_message};
    }
    const auto window = send_window_state(stream_id);
    const std::size_t available = window.has_value() ? window->available_ : 0;
    const auto consumed = (std::min)({chunk.size(), max_payload_bytes, available});
    const bool complete = consumed == chunk.size();
    const auto terminal = complete ? end_stream : http2_end_stream::keep_open;
    // An END_STREAM without payload is still a DATA frame, which nghttp2 sends
    // only with positive connection and stream windows.
    const bool empty_terminal = chunk.empty() && http2_ends_stream(terminal);
    if (empty_terminal && !can_send_empty_end_stream(stream_id)) {
        return {0, http2_data_submit_status::backpressured};
    }
    if (consumed != 0 || empty_terminal) {
        // Commit before nghttp2 sends the frame: an END_STREAM that completes the
        // stream closes it during the same drain.
        stream->commit_local_content(consumed);
        if (http2_ends_stream(terminal)) {
            (void)stream->commit_local_end_stream();
            release_local_request_stream_if_closed(*stream);
        }
        (void)send_data(stream_id, chunk.substr(0, consumed), terminal);
    }
    return {consumed, complete ? http2_data_submit_status::complete : http2_data_submit_status::backpressured};
}

http2_request_content_release_status http2_connection::release_request_content(std::uint32_t stream_id) noexcept {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_request_content_release_status::closed;
    }
    return stream->release_request_continue() ? http2_request_content_release_status::released : http2_request_content_release_status::not_pending;
}

http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_submit_status::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) || stream->local_send().head_pending() == nullptr || stream->tunnel().pending() == nullptr) {
        return http2_submit_status::invalid_state;
    }
    if (!is_valid_http_binary_connect_response_head(response) || http_binary_final_response_control_plan(response).has_value()) {
        return http2_submit_status::invalid_message;
    }
    const auto head_plan_result = http_framed_connect_response_head_plan(plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr || !append_http_binary_response_fields(fields_, response, *head_plan, max_http_header_bytes)) {
        return http2_submit_status::invalid_message;
    }
    apply_response_priority(*stream, response.headers());
    commit_connect_response_head(*stream, http2_remote_peer_half_closed(*stream));
    return http2_submit_status::accepted;
}

std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error> http2_connection::submit_websocket_handshake(std::uint32_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_websocket_handshake_submit_error::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) || http2_websocket_connect_remote_head_ended(*stream) || !http2_websocket_connect_content_acceptable(*stream) || stream->local_send().head_pending() == nullptr || !http2_is_pending_websocket_connect(*stream) || validation.accepted() == nullptr) {
        return http2_websocket_handshake_submit_error::invalid_state;
    }
    auto negotiation = make_websocket_server_negotiation(request, options);
    if (!append_http_binary_websocket_response_fields(fields_, negotiation, max_http_header_bytes)) {
        throw std::invalid_argument("WebSocket response headers exceed HTTP/2 header list limit");
    }
    commit_connect_response_head(*stream, http2_remote_peer_half_closed(*stream));
    const auto compression = negotiation.compression();
    return websocket_extended_connect_negotiation(std::move(negotiation).take_subprotocol(), compression);
}

http2_finish_status http2_connection::finish_request(std::uint32_t stream_id, std::span<const http_header_view> trailers) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_finish_status::closed;
    }
    if (role_ != http2_role::client || stream->local_send().request_content_open() == nullptr || stream->request_continue_pending() || stream->request_content_canceled()) {
        throw std::logic_error("HTTP/2 stream is not open for finishing request");
    }
    if (!stream->local_content().length_complete()) {
        return http2_finish_status::invalid_message;
    }
    if (trailers.empty()) {
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        binary_field_section_size size(max_http_header_bytes);
        for (const auto& field : trailers) {
            if (!http_binary_is_valid_outbound_regular_header(field.name(), field.value()) || is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
                return http2_finish_status::invalid_message;
            }
        }
        fields_.clear();
        fields_.reserve(trailers.size());
        fields_.add_lowercase_fields(trailers);
    }
    end_local_stream(*stream, trailers.empty());
    return http2_finish_status::accepted;
}

http2_finish_status http2_connection::finish_response(std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_finish_status::closed;
    }
    if (stream->local_send().response_content_open() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for finishing response");
    }
    if (!stream->local_content().length_complete()) {
        return http2_finish_status::invalid_message;
    }
    if (trailers.empty()) {
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        fields_.clear();
        fields_.reserve(trailers.fields().size());
        fields_.add_lowercase_fields(trailers.fields());
    }
    end_local_stream(*stream, trailers.empty());
    return http2_finish_status::accepted;
}

void http2_connection::end_local_stream(http2_stream_state& stream, bool empty_data) {
    const auto stream_id = stream.id();
    if (empty_data) {
        // Commit before nghttp2 sends the frame: an END_STREAM that completes the
        // stream closes it during the same drain.
        (void)stream.commit_local_end_stream();
        release_local_request_stream_if_closed(stream);
        send_data(stream_id, {}, http2_end_stream::end_stream);
        return;
    }
    submit_header_frames(stream, http2_end_stream::end_stream);
    (void)stream.commit_local_end_stream();
    release_local_request_stream_if_closed(stream);
    flush_session();
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    if (stream_id == 0 || stream_id > http2_max_stream_id) {
        return http2_submit_status::invalid_state;
    }
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        // nghttp2 keeps no closed-stream history (RFC 9113 §5.1 lets an endpoint
        // forget closed streams); a known identifier is simply closed.
        const bool local_id = ((stream_id & 1U) != 0) == (role_ == http2_role::client);
        const bool used = local_id ? stream_id < nghttp2_session_get_next_stream_id(session_.get()) : stream_id <= highest_peer_stream_id_;
        return used ? http2_submit_status::closed : http2_submit_status::invalid_state;
    }
    if (stream->is_aborted()) {
        return http2_submit_status::closed;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        // Owner release still retires the stream and its leases after failure;
        // only the wire reset is suppressed on the terminal connection.
        return close_stream_impl(stream_id, http2_stream_close_source::local, http2_error_code::no_error, close_notification_type::owner_already_knows) ? http2_submit_status::accepted : http2_submit_status::closed;
    }
    // A server owner does not own a peer stream until its initial header block
    // has decoded. A stream whose halves both ended is closed on the wire (RFC
    // 9113 §5.1): only a PROTOCOL_ERROR reset for a malformed message is
    // accepted, and it retires the stream locally without a frame.
    if ((role_ == http2_role::server && !http2_remote_final_head_decoded(*stream)) || (http2_stream_is_closed(*stream) && error != http2_error_code::protocol_error)) {
        return http2_submit_status::invalid_state;
    }
    if (!http2_stream_is_closed(*stream)) {
        submit_owner_reset(stream_id, error);
    }
    cancel_push_promise_on(stream_id);
    (void)close_stream_impl(stream_id, http2_stream_close_source::local, error, close_notification_type::owner_already_knows);
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_extension_frame(http2_frame_type type, std::uint32_t stream_id, http2_extension_payload& payload) {
    check_nghttp2(nghttp2_submit_extension(session_.get(), static_cast<std::uint8_t>(type), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), &payload));
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    if (!received_peer_settings_) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size = http_origin_payload_size(origins, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ =
            [](const http2_extension_payload& value, std::span<char> output) noexcept {
        write_http_origin_payload(output, value.origins_);
    },
        .origins_ = origins,
    };
    return submit_extension_frame(http2_frame_type::origin, 0, payload);
}

http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id, std::string_view origin, std::string_view field_value) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    const auto* stream = stream_id != 0 ? streams_.find(stream_id) : nullptr;
    if (!received_peer_settings_ || (stream_id != 0 && (stream == nullptr || http2_stream_is_closed(*stream) || stream->push_reservation() == http2_push_reservation::local || !http2_remote_final_head_decoded(*stream) || stream->tunnel().not_connect() == nullptr))) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size = http2_alternative_service_payload_size(stream_id, origin, field_value, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ =
            [](const http2_extension_payload& value, std::span<char> output) noexcept {
        http2_write_alternative_service_payload(output, value.origin_, value.field_value_);
    },
        .origin_ = origin,
        .field_value_ = field_value,
    };
    return submit_extension_frame(http2_frame_type::alternative_service, stream_id, payload);
}
}  // namespace ruvia::detail

namespace ruvia {

namespace {
using error_type = http_connection_advertisement_error;
constexpr std::size_t length_prefix_bytes = 2;
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) {
    return stream_id <= detail::http2_max_stream_id && (stream_id == 0) == has_origin;
}
std::vector<char> make_http2_frame(http2_frame_type type, std::uint32_t stream_id, std::size_t payload_size) {
    std::vector<char> bytes(http2_frame_header_bytes + payload_size);
    (void)encode_http2_frame_header(std::span(bytes).first(http2_frame_header_bytes), static_cast<std::uint32_t>(payload_size), type, 0, stream_id);
    return bytes;
}
}  // namespace
std::variant<http_origin_advertisement, error_type> decode_http_origin_advertisement(std::span<const char> payload_value) {
    http_origin_advertisement result;
    while (!payload_value.empty()) {
        if (payload_value.size() < length_prefix_bytes) {
            return error_type::malformed_payload;
        }
        const auto size = length(payload_value);
        payload_value = payload_value.subspan(length_prefix_bytes);
        if (size > payload_value.size()) {
            return error_type::malformed_payload;
        }
        const std::string_view origin(payload_value.data(), size);
        if (is_valid_http_serialized_origin(origin)) {
            detail::reserve_geometric(result.origins_, result.origins_.size() + 1);
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}
std::variant<std::vector<char>, error_type> encode_http2_origin_frame(std::span<const std::string_view> origins, std::uint32_t maximum) {
    const auto payload_size = detail::http_origin_payload_size(origins, std::min(maximum, http2_max_frame_size));
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_http2_frame(http2_frame_type::origin, 0, std::get<0>(payload_size));
    detail::write_http_origin_payload(std::span(bytes_value).subspan(http2_frame_header_bytes), origins);
    return bytes_value;
}
std::variant<http_alternative_service_advertisement, error_type> decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value) {
    if (payload_value.size() < length_prefix_bytes) {
        return error_type::malformed_payload;
    }
    const auto size = length(payload_value);
    payload_value = payload_value.subspan(length_prefix_bytes);
    if (size > payload_value.size()) {
        return error_type::malformed_payload;
    }
    if (!is_valid_alternative_service_stream(stream_id, size != 0)) {
        return error_type::invalid_stream;
    }
    const std::string_view origin(payload_value.data(), size);
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    const std::string_view value(payload_value.data() + size, payload_value.size() - size);
    if (!is_valid_http_header_value(value)) {
        return error_type::invalid_field;
    }
    http_alternative_service_advertisement result;
    result.stream_id_ = stream_id;
    result.origin_ = origin;
    result.field_value_ = value;
    return result;
}
std::variant<std::vector<char>, error_type> encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view value, std::uint32_t maximum) {
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, value, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_http2_frame(http2_frame_type::alternative_service, stream_id, std::get<0>(payload_size));
    detail::http2_write_alternative_service_payload(std::span(bytes_value).subspan(http2_frame_header_bytes), origin, value);
    return bytes_value;
}
namespace detail {
std::variant<std::size_t, error_type> http2_alternative_service_payload_size(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    if (!is_valid_alternative_service_stream(stream_id, !origin.empty())) {
        return error_type::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    if (!is_valid_http_header_value(field_value)) {
        return error_type::invalid_field;
    }
    std::size_t remaining = std::min(max_frame_size, http2_max_frame_size);
    if (!reserve_http_prefixed_origin(remaining, origin) || field_value.size() > remaining) {
        return error_type::limit;
    }
    return length_prefix_bytes + origin.size() + field_value.size();
}
void http2_write_alternative_service_payload(std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept {
    auto* out = write_http_prefixed_origin(payload.data(), origin);
    if (!field_value.empty()) {
        std::memcpy(out, field_value.data(), field_value.size());
    }
}
}  // namespace detail
}  // namespace ruvia

// Submitting a request head as the client: what an outbound :method / :path /
// :authority and its header section must satisfy before the connection will
// encode it, for a regular request, a CONNECT tunnel, and the extended CONNECT
// that carries a websocket handshake.

namespace ruvia::detail {

template <typename prepare_type>
std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_local_request_head(http2_end_stream end_stream, const prepare_type& prepare) {
    if (active_local_request_streams_ >= http2_local_settings::max_concurrent_streams) {
        return http2_request_head_submit_error::local_stream_capacity_reached;
    }
    // nghttp2 assigns this identifier to the next request HEADERS.
    const auto stream_id = static_cast<std::uint32_t>(nghttp2_session_get_next_stream_id(session_.get()));
    auto* stream = create_stream(stream_id);
    if (stream == nullptr) {
        return http2_request_head_submit_error::local_stream_capacity_reached;
    }
    std::int32_t submitted = 0;
    try {
        prepare(*stream);
        const auto fields = fields_.nv();
        submitted = nghttp2_submit_headers(session_.get(), static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE), -1, nullptr, fields.data(), fields.size(), nullptr);
        check_nghttp2(submitted);
    } catch (...) {
        fields_.clear();
        (void)streams_.remove(stream_id);
        throw;
    }
    fields_.release();
    if (static_cast<std::uint32_t>(submitted) != stream_id) {
        std::terminate();
    }
    if (stream->hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    flush_session();
    return stream_id;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_regular_request_head(std::string_view method, std::string_view scheme, std::optional<std::string_view> authority, std::string_view path, std::span<const http_header_view> headers, http_request_content_plan content, http_client_request_expectation expectation, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    const bool without_content = content.without_content() != nullptr;
    const auto* known_length_content = content.known_length_content();
    const bool streaming_content = content.streaming_content() != nullptr;
    const auto declared_length = content.declared_length();
    const auto expected_length = declared_length ? std::optional{declared_length->value()} : std::nullopt;
    const bool content_will_follow = !without_content && (!expected_length || *expected_length != 0);
    const auto content_indication = content_will_follow ? http_request_content_indication::will_follow : http_request_content_indication::no_content;
    auto end_stream = http2_end_stream::keep_open;
    // Only the formatted prefix is exposed; an omitted length keeps an empty view.
    std::array<char, http_max_unsigned_decimal_size> length_buffer;
    if (without_content) {
        end_stream = http2_end_stream::end_stream;
    } else if (known_length_content != nullptr) {
        end_stream = known_length_content->length().value() == 0 ? http2_end_stream::end_stream : http2_end_stream::keep_open;
    }
    const auto known_method = classify_http_method(method);
    const auto default_port = http_uri_scheme_default_port(scheme);
    const bool emit_length = expected_length && method != "TRACE";
    const auto content_length_value = emit_length ? http_format_unsigned(length_buffer, *expected_length) : std::string_view{};
    // Validate the entire semantic head and decoded-size budget before touching
    // the session, stream metadata, or lifecycle state.
    if (!http_binary_is_valid_outbound_regular_request_head(method, known_method, scheme, default_port, authority, path, headers, content_indication, content_length_value, expectation, source, max_http_header_bytes)) {
        return http2_request_head_submit_error::invalid_message;
    }
    return submit_local_request_head(end_stream, [&](http2_stream_state& stream) {
        stream.assign_request_method(method);
        stream.assign_request_scheme(scheme);
        fields_.clear();
        fields_.reserve(headers.size() + 6);
        fields_.add(":method", method);
        fields_.add(":scheme", scheme);
        if (authority.has_value()) {
            fields_.add(":authority", *authority);
        }
        fields_.add(":path", path);
        fields_.add_lowercase_fields(headers);
        if (expectation == http_client_request_expectation::continue_value) {
            fields_.add("expect", http_continue_expectation_token);
        }
        if (emit_length) {
            fields_.add_owned_value("content-length", content_length_value);
        }
        if (without_content) {
            stream.begin_local_content_forbidden();
        } else if (expected_length) {
            stream.begin_local_content_known_length(*expected_length);
        } else if (streaming_content) {
            stream.begin_local_content_unbounded();
        }
        if (http2_ends_stream(end_stream)) {
            (void)stream.commit_local_head_end_stream();
        } else {
            (void)stream.begin_local_request_content();
            if (expectation == http_client_request_expectation::continue_value) {
                stream.await_request_continue();
            }
        }
    });
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_connect_request_head(std::string_view authority, std::span<const http_header_view> headers, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }

    request_target_view target;
    binary_field_section_size section_size(max_http_header_bytes);
    request_field_state fields{request_field_framing::binary};
    if (!parse_request_target(http_known_method::connect, authority, target) || !section_size.add(":method", "CONNECT") || !section_size.add(":authority", authority) ||
        !http_binary_are_valid_outbound_request_headers(authority, 0, headers,
            /*allow_host=*/false,
            /*allow_trailers=*/false, section_size, fields, source)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head(http2_end_stream::keep_open, [&](http2_stream_state& stream) {
        (void)stream.begin_standard_connect();
        stream.assign_request_method("CONNECT");
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();
        fields_.clear();
        fields_.reserve(headers.size() + 2);
        fields_.add(":method", "CONNECT");
        fields_.add(":authority", authority);
        fields_.add_lowercase_fields(headers);
    });
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_extended_connect_request_head(std::string_view protocol, std::string_view scheme, std::string_view authority, std::string_view path, std::span<const http_header_view> headers, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    if (peer_setting(NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL) != 1) {
        return http2_request_head_submit_error::peer_capability_unavailable;
    }

    const bool websocket_value = http_ascii_equals_ignore_case(protocol, "websocket");
    const auto default_port = http_uri_scheme_default_port(scheme);
    const auto encoded_protocol = websocket_value ? std::string_view("websocket") : protocol;
    binary_field_section_size section_size(max_http_header_bytes);
    request_field_state fields{request_field_framing::binary};
    if (!is_valid_http_header_name(protocol) || !is_valid_uri_scheme(scheme) || (websocket_value && default_port == 0) || !is_valid_binary_request_authority(scheme, authority) || !http_binary_is_valid_extended_connect_path(scheme, path) || !section_size.add(":method", "CONNECT") || !section_size.add(":protocol", encoded_protocol) || !section_size.add(":scheme", scheme) || !section_size.add(":authority", authority) || !section_size.add(":path", path) ||
        !http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/!websocket_value,
            /*allow_trailers=*/false, section_size, fields, source,
            /*websocket_connect=*/websocket_value)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head(http2_end_stream::keep_open, [&](http2_stream_state& stream) {
        (void)stream.begin_extended_connect();
        stream.assign_request_method("CONNECT");
        stream.assign_request_scheme(scheme);
        stream.set_protocol(encoded_protocol);
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();
        fields_.clear();
        fields_.reserve(headers.size() + 5);
        fields_.add(":method", "CONNECT");
        // RFC 8441 registers and requires the lowercase `websocket` value. HTTP
        // protocol-name matching is case-insensitive, so accept caller spelling but
        // never put a non-canonical websocket token on the wire or in stream state.
        fields_.add(":protocol", encoded_protocol);
        fields_.add(":scheme", scheme);
        fields_.add(":authority", authority);
        fields_.add(":path", path);
        fields_.add_lowercase_fields(headers);
    });
}

std::variant<std::uint32_t, http2_push_submit_error> http2_connection::submit_push_promise(std::uint32_t associated_stream_id, http_push_request_view request) {
    const auto* parent_value = streams_.find(associated_stream_id);
    const auto promised_id = static_cast<std::uint32_t>(nghttp2_session_get_next_stream_id(session_.get()));
    if (role_ != http2_role::server || !received_peer_settings_ || local_connection_state_.open() == nullptr || peer_goaway_ || !parent_value || parent_value->is_aborted() || (associated_stream_id & 1U) == 0 || !http2_remote_final_head_decoded(*parent_value) || parent_value->tunnel().not_connect() == nullptr || parent_value->local_send().end_stream_committed() || promised_id > http2_max_stream_id) {
        return http2_push_submit_error::invalid_state;
    }
    if (peer_setting(NGHTTP2_SETTINGS_ENABLE_PUSH) == 0) {
        return http2_push_submit_error::push_disabled;
    }
    // RFC 9113 §8.4: the client's SETTINGS_MAX_CONCURRENT_STREAMS bounds pushed
    // responses. When no pushed stream could open now the promise is not made.
    // Reserved promises do not count (§5.1.2); their response head waits for a slot.
    if (active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_push_submit_error::stream_limit;
    }
    const auto known_method = request.method_ == "GET" ? http_known_method::get : (request.method_ == "HEAD" ? http_known_method::head : http_known_method::unknown);
    const auto default_port = http_uri_scheme_default_port(request.scheme_);
    if (known_method == http_known_method::unknown || !http_binary_is_valid_outbound_regular_request_head(request.method_.view(), known_method, request.scheme_.view(), default_port, request.authority_.view(), request.path_.view(), request.headers_, http_request_content_indication::no_content, {}, http_client_request_expectation::none, http_request_field_source::generated, max_http_header_bytes)) {
        return http2_push_submit_error::invalid_request;
    }
    auto* stream = create_stream(promised_id);
    if (stream == nullptr) {
        return http2_push_submit_error::stream_limit;
    }
    try {
        stream->assign_request_method(request.method_);
        stream->assign_request_scheme(request.scheme_);
        stream->assign_request_authority(request.authority_);
        stream->assign_request_path(request.path_);
        stream->mark_scheme(default_port);
        stream->mark_authority();
        stream->mark_path();
        std::array<char, 64> lower_name_stack;
        std::string lower_name_scratch;
        for (std::size_t i = 0; i < request.headers_.size(); ++i) {
            const auto& header = request.headers_[i];
            const auto kind = classify_http_field(header.name());
            if (kind == http_field_kind::host) {
                stream->mark_host(stream->remote_header_count());
            }
            if (kind == http_field_kind::cookie) {
                if (!http2_append_cookie_header_value(*stream, header.value())) {
                    throw std::length_error("HTTP/2 push request cookie is too large");
                }
            } else {
                const auto lower_name = http_lower_header_name(header.name(), [&](std::size_t size) {
                    if (size <= lower_name_stack.size()) {
                        return lower_name_stack.data();
                    }
                    lower_name_scratch.resize(size);
                    return lower_name_scratch.data();
                });
                if (!stream->append_remote_header(lower_name, header.value(), kind)) {
                    throw std::length_error("HTTP/2 push request has too many fields");
                }
            }
        }
        (void)stream->record_remote_head_end_stream();
        (void)stream->finalize_remote_content_head();
        stream->reserve_push(http2_push_reservation::local);
        fields_.clear();
        fields_.reserve(request.headers_.size() + 4);
        fields_.add(":method", request.method_.view());
        fields_.add(":scheme", request.scheme_.view());
        fields_.add(":authority", request.authority_.view());
        fields_.add(":path", request.path_.view());
        fields_.add_lowercase_fields(request.headers_);
        const auto fields = fields_.nv();
        const auto submitted = nghttp2_submit_push_promise(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(associated_stream_id), fields.data(), fields.size(), nullptr);
        check_nghttp2(submitted);
        if (static_cast<std::uint32_t>(submitted) != promised_id) {
            std::terminate();
        }
    } catch (...) {
        fields_.clear();
        streams_.remove(promised_id);
        throw;
    }
    fields_.release();
    flush_session();
    return promised_id;
}

}  // namespace ruvia::detail

// Inbound HEADERS, PUSH_PROMISE and DATA as nghttp2 delivers them: one field
// block per on_begin_headers/on_header/on_frame_recv sequence, DATA payload in
// chunks borrowed from the fed input. nghttp2 has already enforced framing,
// stream states and flow control; this layer applies HTTP message semantics.

namespace ruvia::detail {

namespace {

// CONNECT has no request content; DATA after its HEADERS is tunnel data (RFC
// 9113 §8.5). RFC 9110 §8.6 makes Content-Length on CONNECT only a sender SHOULD
// NOT, so a declared zero is harmless, while a non-zero length would claim
// content that the stream never frames and stays malformed (as in HTTP/1).
[[nodiscard]] bool http2_connect_content_length_acceptable(const http2_stream_state& stream) noexcept {
    const auto* known = stream.remote_content().allowed_known_length();
    return known == nullptr || known->declared_length() == 0;
}

// Decoded HTTP/2 field names are already required to be lowercase. TE is
// permitted only in requests (RFC 9113 Section 8.2.2); a response carrying TE
// is malformed even when its value is exactly "trailers".
[[nodiscard]] bool http2_is_valid_decoded_response_header(std::string_view name, std::string_view value) noexcept {
    return http2_is_valid_regular_header(name, value) && name != "te";
}

// ':status' is first and unique; interim metadata is retained separately
// without applying sender prohibitions.
[[nodiscard]] bool accept_response_header(http2_header_decode_context& context, std::vector<http_header>& informational_fields, std::optional<http_received_status>& status_value, http_response_content_semantics& content_semantics, bool& saw_regular, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context, name, value)) {
        return false;
    }
    auto& stream = context.stream_;
    if (name.empty()) {
        return false;
    }
    if (name.front() == ':') {
        if (name != ":status" || status_value || saw_regular) {
            return false;
        }
        // RFC 9110 §15: a received 000..099 or 600..999 is processed as a 5xx response.
        const auto status = http_received_status::try_parse(value);
        if (!status || status->status() == http_status::switching_protocols) {
            return false;
        }
        status_value = *status;
        content_semantics = classify_http_response_content_semantics(stream.request_known_method(), status->status());
        return true;
    }
    if (!status_value || !http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    saw_regular = true;
    if (status_value->status().is_informational()) {
        // These fields cannot frame a 1xx or carry over to the final response.
        informational_fields.emplace_back(name, value);
        return true;
    }
    const auto kind = classify_http_field(name);
    // RFC 9110 §9.3.6: successful CONNECT ignores Content-Length semantics,
    // including invalid numerals; the field is retained for extension validators.
    if (kind == http_field_kind::content_length && content_semantics != http_response_content_semantics::connect_tunnel) {
        http_content_length_state content_length;
        if (content_length.parse_field(value) != http_content_length_parse_status::ok || !stream.declare_remote_content_length(*content_length.value())) {
            return false;
        }
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] bool http2_on_decoded_response_trailer(http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }
    // Pseudo-header, connection-specific and TE fields are malformed (RFC 9113
    // §8.1, §8.2.2). A field the response trailer table does not permit binds
    // only the sender (RFC 9110 §6.5.1) and is discarded.
    if (!http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    return is_forbidden_response_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace

// --- HEADERS / PUSH_PROMISE ---------------------------------------------------

int http2_connection::on_begin_headers(const nghttp2_frame& frame) {
    header_block_.reset();
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    if (header_observer_ != nullptr) {
        observed_header_sequence_ = observed_frame_.observation_sequence_;
        observed_header_stream_ = static_cast<std::uint32_t>(frame.hd.stream_id);
        observed_field_index_ = 0;
        observe_header(http2_header_observation_phase::begin);
    }
    if (frame.hd.type == NGHTTP2_HEADERS && receive_frame_metadata_ && (frame.hd.flags & NGHTTP2_FLAG_PRIORITY) != 0) {
        // RFC 9113 §5.3.2 priority fields are advisory metadata, queued ahead
        // of every event the frame produces.
        publish_event(http2_event(http2_received_priority{.stream_id_ = static_cast<std::uint32_t>(frame.hd.stream_id), .stream_dependency_ = static_cast<std::uint32_t>(frame.headers.pri_spec.stream_id), .weight_ = static_cast<std::uint16_t>(frame.headers.pri_spec.weight), .exclusive_ = frame.headers.pri_spec.exclusive != 0}));
    }
    if (role_ == http2_role::server) {
        return begin_request_headers(frame);
    }
    return frame.hd.type == NGHTTP2_PUSH_PROMISE ? begin_push_promise(frame) : begin_response_headers(frame);
}

int http2_connection::begin_request_headers(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    if (frame.headers.cat == NGHTTP2_HCAT_REQUEST) {
        const auto* drain = local_connection_state_.graceful_drain();
        const bool beyond_round_trip = drain != nullptr && drain->awaiting_round_trip() && stream_id > drain->round_trip_stream_limit();
        auto* stream = beyond_round_trip || (drain != nullptr && stream_id > drain->last_stream_id()) ? nullptr : create_stream(stream_id);
        if (stream == nullptr) {
            // RFC 9113 §8.7: refused before any processing; the client may
            // retry. The refusal is queued ahead of a final GOAWAY because
            // sending that GOAWAY closes every stream above its boundary.
            check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, frame.hd.stream_id, NGHTTP2_REFUSED_STREAM));
            if (beyond_round_trip) {
                // The final boundary is the high-water mark before this stream.
                finish_drain_round_trip();
            }
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        last_peer_stream_id_ = stream_id;
        highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, stream_id);
        if (end_stream) {
            (void)stream->record_remote_head_end_stream();
        }
        header_block_.emplace(header_block_kind::request, *stream);
        return 0;
    }

    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        // A locally reset stream: nghttp2 still decodes the block for HPACK.
        return 0;
    }
    // CONNECT has no trailers, and a second initial head or a trailer section
    // without END_STREAM is malformed (RFC 9113 §8.1, §8.5).
    if (stream->tunnel().open() != nullptr || stream->tunnel().pending() != nullptr || stream->remote_receive().content_open() == nullptr || !end_stream) {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    header_block_.emplace(header_block_kind::request_trailers, *stream);
    return 0;
}

int http2_connection::begin_response_headers(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return 0;
    }
    if (!http2_remote_final_head_decoded(*stream)) {
        // nghttp2 already enforced SETTINGS_MAX_CONCURRENT_STREAMS for a pushed
        // response opening its stream (RFC 9113 §5.1.2).
        if (end_stream) {
            (void)stream->record_remote_head_end_stream();
        }
        header_block_.emplace(header_block_kind::response, *stream);
        return 0;
    }
    if (stream->tunnel().open() != nullptr || stream->remote_receive().content_open() == nullptr || !end_stream) {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    header_block_.emplace(header_block_kind::response_trailers, *stream);
    return 0;
}

int http2_connection::begin_push_promise(const nghttp2_frame& frame) {
    // RFC 9113 §8.4: push is only accepted when this client enabled it.
    if (!enable_push_) {
        fail_connection(http2_error_code::protocol_error);
        return 0;
    }
    const auto associated_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const auto promised_id = static_cast<std::uint32_t>(frame.push_promise.promised_stream_id);
    last_peer_stream_id_ = promised_id;
    highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, promised_id);
    const auto* parent = streams_.find(associated_id);
    // The promised request decodes into scratch storage; the pushed stream's own
    // receive state awaits the response.
    push_scratch_.emplace(promised_id);
    push_associated_stream_id_ = associated_id;
    // PUSH_PROMISE carries a complete request without content.
    (void)push_scratch_->record_remote_head_end_stream();
    auto& block = header_block_.emplace(header_block_kind::push_promise, *push_scratch_);
    block.reject_connected_tunnel_ = parent != nullptr && parent->tunnel().open() != nullptr;
    return 0;
}

int http2_connection::on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value) {
    if (!header_block_.has_value() || local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    observe_header(http2_header_observation_phase::field, name, value);
    if (accept_header_field(name, value)) {
        return 0;
    }
    // Malformed: reset now; nghttp2 decodes the rest of the block for HPACK
    // without further callbacks.
    const auto kind = header_block_->kind_;
    header_block_.reset();
    if (kind == header_block_kind::push_promise) {
        push_scratch_.reset();
        reset_stream_for_peer_error(static_cast<std::uint32_t>(frame.push_promise.promised_stream_id), http2_error_code::protocol_error);
    } else {
        reject_header_block(static_cast<std::uint32_t>(frame.hd.stream_id), kind == header_block_kind::request);
    }
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
}

bool http2_connection::accept_header_field(std::string_view name, std::string_view value) {
    auto& block = *header_block_;
    switch (block.kind_) {
        case header_block_kind::request:
        case header_block_kind::push_promise:
            return http2_on_decoded_initial_header(block.context_, name, value);
        case header_block_kind::request_trailers:
            return http2_on_decoded_request_trailer(block.context_, name, value);
        case header_block_kind::response:
            return accept_response_header(block.context_, block.informational_fields_, block.status_, block.content_semantics_, block.saw_regular_, name, value);
        case header_block_kind::response_trailers:
            return http2_on_decoded_response_trailer(block.context_, name, value);
    }
    return false;
}

void http2_connection::reject_header_block(std::uint32_t stream_id, bool malformed_request_head) {
    reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
    if (!malformed_request_head) {
        return;
    }
    // A peer can otherwise keep one connection alive while forcing the server to
    // decode and reset malformed request heads across almost the whole stream-id
    // space. Valid exchanges do not refill this budget.
    if (++malformed_request_streams_ >= max_malformed_request_streams) {
        fail_connection(http2_error_code::enhance_your_calm);
    }
}

void http2_connection::finish_header_block(const nghttp2_frame& frame) {
    observe_header(http2_header_observation_phase::end);
    if (!header_block_.has_value()) {
        return;
    }
    auto block = std::move(*header_block_);
    header_block_.reset();
    auto& stream = block.context_.stream_;
    switch (block.kind_) {
        case header_block_kind::request:
            if (!finish_request_head(stream)) {
                reject_header_block(stream.id(), true);
                return;
            }
            emit_request_headers(stream);
            return;
        case header_block_kind::response:
            if (!finish_response_head(stream, block)) {
                reject_header_block(stream.id(), false);
            }
            return;
        case header_block_kind::request_trailers:
        case header_block_kind::response_trailers:
            if (!finish_trailers(stream)) {
                reject_header_block(stream.id(), false);
            }
            return;
        case header_block_kind::push_promise:
            finish_push_promise(frame, block);
            return;
    }
}

bool http2_connection::finish_request_head(http2_stream_state& stream) {
    if (!stream.has_method()) {
        return false;
    }
    if (stream.has_protocol()) {
        // RFC 8441 §4: Extended CONNECT requires our SETTINGS_ENABLE_CONNECT_PROTOCOL.
        if (!received_peer_settings_ || stream.request_known_method() != http_known_method::connect || !stream.has_scheme() || !stream.has_path() || !stream.has_authority() || !is_valid_binary_request_authority(stream.request_scheme(), stream.request_authority()) || !http_binary_request_path_matches_method_and_scheme(stream.request_known_method(), stream.request_scheme(), stream.request_path()) || !http2_connect_content_length_acceptable(stream)) {
            return false;
        }
        // RFC 8441 defines websocket extended CONNECT only for HTTP(S) URI
        // schemes. Other extended protocols retain the full RFC 3986 space.
        if (!is_valid_extended_connect_scheme(stream.request_protocol(), stream.request_scheme())) {
            return false;
        }
        if (!stream.begin_extended_connect()) {
            return false;
        }
    } else if (stream.request_known_method() == http_known_method::connect) {
        request_target_view connect_target;
        if (!stream.has_authority() || stream.has_scheme() || stream.has_path() || !http2_connect_content_length_acceptable(stream) || !parse_request_target(http_known_method::connect, stream.request_authority(), connect_target)) {
            return false;
        }
        if (!stream.begin_standard_connect()) {
            return false;
        }
    } else if (!stream.has_scheme() || !stream.has_path() || (stream.has_authority() && !is_valid_binary_request_authority(stream.request_scheme(), stream.request_authority())) || !http_binary_request_path_matches_method_and_scheme(stream.request_known_method(), stream.request_scheme(), stream.request_path()) || !http2_has_required_request_authority(stream)) {
        return false;
    }
    const bool remote_head_finalized = stream.tunnel().pending() != nullptr ? stream.finalize_remote_connect_head() : stream.finalize_remote_content_head();
    if (!remote_head_finalized) {
        return false;
    }
    return !http2_remote_peer_half_closed(stream) || stream.remote_content().terminal_length_valid();
}

bool http2_connection::finish_response_head(http2_stream_state& stream, header_block_type& block) {
    if (!block.status_) {
        return false;
    }
    if (block.status_->status().is_informational()) {
        // A 1xx cannot carry END_STREAM. The remote state stays head-pending so
        // the next HEADERS decodes as another head.
        if (stream.remote_receive().head_end_stream_pending() != nullptr) {
            return false;
        }
        stream.mark_informational_response();
        stream.activate_push();
        auto head = http_client_response_head_access::make(*block.status_, http_protocol_version::http2);
        if (!block.informational_fields_.empty()) {
            http_client_response_head_access::headers(head) = std::move(block.informational_fields_);
        }
        std::optional<http_client_request_content_signal> signal;
        if (block.status_->status() == http_status::continue_value && stream.release_request_continue()) {
            signal = http_client_request_content_signal::continue_value;
        }
        publish_event(http2_event(http2_informational_head_event(stream.id(), std::move(head), signal)));
        return true;
    }
    (void)stream.cancel_pending_request_continue();
    if (!stream.set_response_status(*block.status_)) {
        return false;
    }
    // A 205 sender MUST NOT generate content (RFC 9110 §15.3.6), but DATA
    // framing stays unambiguous: Content-Length still binds the DATA sum and any
    // content is consumed and discarded by the DATA path.
    if (block.content_semantics_ == http_response_content_semantics::without_content && !stream.select_remote_content_metadata_only()) {
        return false;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (block.content_semantics_ == http_response_content_semantics::connect_tunnel) {
            if (!stream.accept_connect()) {
                return false;
            }
            stream.begin_local_content_unbounded();
            (void)stream.open_local_connect_tunnel();
        } else {
            if (!stream.reject_connect()) {
                return false;
            }
            if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
                return false;
            }
            // A rejected CONNECT ends the client's send half with an empty
            // DATA(END_STREAM); nghttp2 holds it until flow control admits it.
            nghttp2_data_provider2 provider{};
            provider.source.ptr = nullptr;
            provider.read_callback = &http2_connection::read_outbound_data;
            check_nghttp2(nghttp2_submit_data2(session_.get(), NGHTTP2_FLAG_END_STREAM, static_cast<std::int32_t>(stream.id()), &provider));
            if (can_send_empty_end_stream(stream.id())) {
                (void)stream.reject_local_connect();
            }
        }
    } else if (!stream.finalize_remote_content_head()) {
        return false;
    }
    if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
        return false;
    }
    stream.activate_push();
    emit_request_headers(stream);
    return true;
}

bool http2_connection::finish_trailers(http2_stream_state& stream) {
    if (!stream.remote_content().terminal_length_valid() || !stream.finish_remote_content()) {
        return false;
    }
    publish_event(http2_event(http2_message_end_record{stream.id()}));
    release_local_request_stream_if_closed(stream);
    return true;
}

void http2_connection::finish_push_promise(const nghttp2_frame& frame, header_block_type& block) {
    const auto promised_id = static_cast<std::uint32_t>(frame.push_promise.promised_stream_id);
    const auto associated_id = push_associated_stream_id_;
    struct scratch_guard final {
        std::optional<http2_stream_state>& scratch_;
        ~scratch_guard() {
            scratch_.reset();
        }
    } guard{push_scratch_};
    auto& scratch = *push_scratch_;
    std::optional<http2_error_code> refusal;
    if (block.reject_connected_tunnel_) {
        // RFC 9113 §8.5: on a connected stream only DATA, RST_STREAM,
        // WINDOW_UPDATE and PRIORITY are allowed. The promised stream is
        // reserved by the frame (§6.6), so it is cancelled explicitly.
        refusal = http2_error_code::cancel;
    } else if (!finish_request_head(scratch) || (scratch.request_method() != "GET" && scratch.request_method() != "HEAD")) {
        refusal = http2_error_code::protocol_error;
    }
    auto* stream = refusal ? nullptr : create_stream(promised_id);
    if (stream == nullptr) {
        if (block.reject_connected_tunnel_) {
            reset_stream_for_peer_error(associated_id, http2_error_code::protocol_error);
        }
        check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(promised_id), static_cast<std::uint32_t>(refusal.value_or(http2_error_code::enhance_your_calm))));
        return;
    }
    http_push_request request;
    request.method_ = scratch.request_method();
    request.scheme_ = scratch.request_scheme();
    request.authority_ = scratch.request_authority();
    request.path_ = scratch.request_path();
    request.headers_.reserve(scratch.remote_header_count() + static_cast<std::size_t>(scratch.has_cookie()));
    for (std::size_t i = 0; i < scratch.remote_header_count(); ++i) {
        const auto field = scratch.remote_header_at(i);
        request.headers_.emplace_back(field.name_, field.value_);
    }
    if (scratch.has_cookie()) {
        request.headers_.emplace_back("cookie", scratch.request_cookie());
    }
    pin_stream(promised_id);
    stream->take_request_target_from(scratch);
    stream->begin_local_content_forbidden();
    (void)stream->commit_local_head_end_stream();
    stream->reserve_push(http2_push_reservation::remote);
    publish_event(http2_event(http2_push_promise_event{associated_id, promised_id, std::move(request)}));
}

void http2_connection::cancel_push_promise_on(std::uint32_t associated_stream_id) {
    if (!header_block_.has_value() || header_block_->kind_ != header_block_kind::push_promise || push_associated_stream_id_ != associated_stream_id) {
        return;
    }
    const auto promised_id = push_scratch_->id();
    header_block_.reset();
    push_scratch_.reset();
    // RFC 9113 §8.4: a client may cancel a promise it will not use.
    submit_owner_reset(promised_id, http2_error_code::cancel);
}

void http2_connection::emit_request_headers(http2_stream_state& stream) {
    // A later frame in the same input batch can close this stream before the
    // public request event acquires its lease. Keep that event's storage alive.
    if (role_ == http2_role::server) {
        if (!priorities_.empty()) {
            const auto priority = std::ranges::find_if(priorities_, [&stream](const auto& entry_value) noexcept {
                return entry_value.stream_id_ == stream.id();
            });
            if (priority != priorities_.end()) {
                stream.set_priority_update(priority->fields_);
                priorities_.erase(priority);
            }
        }
        pin_stream(stream.id());
    }
    const auto request_content_signal = stream.request_content_canceled() ? std::optional<http_client_request_content_signal>(http_client_request_content_signal::exchange_complete) : std::nullopt;
    const bool body_open = !http2_remote_peer_half_closed(stream) && (role_ == http2_role::client || stream.tunnel().pending() == nullptr);
    publish_event(http2_event(http2_message_head_record{stream.id(), request_content_signal, body_open, http2_remote_peer_half_closed(stream)}));
    if (role_ == http2_role::server && stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content; the typed remote state owns any later
        // tunnel-end signal, so a generic message_end would be misleading.
        return;
    }
    if (role_ == http2_role::client && stream.tunnel().open() != nullptr) {
        if (http2_remote_peer_half_closed(stream)) {
            publish_event(http2_event(http2_tunnel_end_event(stream.id())));
        }
        release_local_request_stream_if_closed(stream);
        return;
    }
    if (http2_remote_peer_half_closed(stream)) {
        publish_event(http2_event(http2_message_end_record{stream.id()}));
    }
    release_local_request_stream_if_closed(stream);
}

// --- DATA ----------------------------------------------------------------------

int http2_connection::on_data_chunk_recv(std::uint8_t, std::uint32_t stream_id, std::string_view data) {
    auto* stream = streams_.find(stream_id);
    if (local_connection_state_.fatal_failure() != nullptr || stream == nullptr || stream->is_aborted()) {
        // DATA already in flight when this endpoint reset the stream still
        // consumed flow-control credit; return it at once.
        consume_received(stream_id, data.size());
        return 0;
    }
    const auto drop = [this, stream_id, &data](http2_error_code error) {
        reset_stream_for_peer_error(stream_id, error);
        consume_received(stream_id, data.size());
        return 0;
    };
    const auto& remote = stream->remote_receive();
    if (remote.head_pending() != nullptr || remote.head_end_stream_pending() != nullptr) {
        return drop(http2_error_code::protocol_error);
    }
    if (remote.connect_pending() != nullptr) {
        // RFC 9113 §8.5 makes every DATA after a CONNECT request tunnel data, and
        // RFC 9298 §5 (as updated by RFC 9931 §6.3) lets a client send it before
        // the 2xx. The owner holds the credit until it consumes the bytes after
        // accepting.
        (void)stream->mark_remote_connect_tunnel_data();
        stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
        publish_event(http2_event(http2_tunnel_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
        return 0;
    }
    if (remote.connect_rejected_awaiting_end_stream() != nullptr) {
        // A rejected CONNECT's further DATA is discarded and its credit returned.
        consume_received(stream_id, data.size());
        return 0;
    }
    if (remote.tunnel_open() != nullptr) {
        stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
        publish_event(http2_event(http2_tunnel_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
        return 0;
    }
    if (remote.content_open() == nullptr) {
        return drop(http2_error_code::stream_closed);
    }
    switch (stream->account_remote_content(data.size())) {
        case http2_remote_content_accounting_result::accepted:
            break;
        case http2_remote_content_accounting_result::counter_overflow:
            return drop(http2_error_code::cancel);
        default:
            return drop(http2_error_code::protocol_error);
    }
    // HEAD-like responses carry metadata only; a 205 response's content is
    // consumed and discarded because its sender MUST NOT generate any (RFC 9110
    // §15.3.6). Both keep flow control and Content-Length accounting intact.
    const auto* response_status = stream->response_status();
    const bool discarded = stream->remote_content().metadata_only_without_length() != nullptr || stream->remote_content().metadata_only_known_length() != nullptr || (response_status != nullptr && response_status->status() == http_status::reset_content);
    if (discarded) {
        consume_received(stream_id, data.size());
        return 0;
    }
    // New capacity is advertised only after the owner consumed or copied the
    // borrowed bytes, preserving backpressure.
    stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
    publish_event(http2_event(http2_message_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
    return 0;
}

void http2_connection::finish_data_frame(http2_stream_state& stream) {
    const auto stream_id = stream.id();
    const auto& remote = stream.remote_receive();
    if (remote.connect_pending() != nullptr) {
        (void)stream.finish_remote_pending_connect();
    } else if (remote.connect_rejected_awaiting_end_stream() != nullptr) {
        (void)stream.finish_remote_rejected_connect();
    } else if (remote.tunnel_open() != nullptr) {
        (void)stream.finish_remote_tunnel();
        publish_event(http2_event(http2_tunnel_end_event(stream_id)));
    } else if (remote.content_open() != nullptr) {
        if (!stream.remote_content().terminal_length_valid()) {
            // Content-Length mismatch (RFC 9113 §8.1.1): the delivered chunks
            // stand, the stream is malformed.
            reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
            return;
        }
        (void)stream.finish_remote_content();
        publish_event(http2_event(http2_message_end_record{stream_id}));
    } else {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return;
    }
    release_local_request_stream_if_closed(stream);
}

}  // namespace ruvia::detail

namespace ruvia {

http2_setting_entry http2_received_settings::at(std::size_t index) const {
    if (index >= size_) {
        throw std::out_of_range("HTTP/2 received SETTINGS index out of range");
    }
    return entries_[index];
}

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(const detail::http2_stream_state& stream) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value = detail::http_client_response_head_access::make(*status, http_protocol_version::http2);
    if (const auto* known = stream.remote_content().allowed_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, known->declared_length());
    } else if (const auto* metadata = stream.remote_content().metadata_only_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, metadata->declared_length());
    }
    const auto header_count = stream.remote_header_count();
    if (header_count != 0) {
        auto& headers = detail::http_client_response_head_access::headers(result_value);
        headers.reserve(header_count);
        for (std::size_t index = 0; index < header_count; ++index) {
            const auto field = stream.remote_header_at(index);
            headers.emplace_back(field.name_, field.value_);
        }
    }
    return result_value;
}

[[nodiscard]] http2_message_content_semantics message_content_semantics(const detail::http2_stream_state& stream) noexcept {
    const auto& content = stream.remote_content();
    return content.metadata_only_without_length() != nullptr || content.metadata_only_known_length() != nullptr ? http2_message_content_semantics::metadata_only : http2_message_content_semantics::content;
}

}  // namespace

// Intrusively reference-counted so escaped events or credits can still inspect
// or abandon stream state after the public http2_connection facade is destroyed.
namespace detail {

class http2_connection_owner_endpoint final {
public:
    http2_connection_owner_endpoint(http2_role role, const http2_connection_options& options) :
        connection_(role, options.enable_push_, options.receive_origin_advertisements_, options.receive_frame_metadata_, options.frame_observer_, options.header_observer_, options.observer_context_) {}
    http2_connection_owner_endpoint(const http2_connection_owner_endpoint&) = delete;
    http2_connection_owner_endpoint& operator=(const http2_connection_owner_endpoint&) = delete;

    void retain() noexcept {
        ++references_;
    }

    void release() noexcept {
        if (references_ == 0) {
            std::terminate();
        }
        if (--references_ == 0) {
            delete this;
        }
    }

    void abandon_request(std::uint32_t stream_id) noexcept {
        connection_.abandon_stream_lease(stream_id);
    }

    void return_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        connection_.return_received_data(stream_id, bytes_value);
    }
    [[nodiscard]] ::ruvia::http2_event materialize_message_head_event(const detail::http2_message_head_record& value) {
        auto* stream = connection_.stream(value.stream_id_);
        if (connection_.role() == http2_role::client) {
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 response event has no stream");
            }
            return ::ruvia::http2_event::response_head(value.stream_id_, response_head_from_stream(*stream), value.request_content_signal_, !value.body_open_);
        }
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 request event has no stream");
        }
        connection_.pin_stream(value.stream_id_);
        const auto content = stream->request_content_indication();
        const auto& remote = stream->remote_receive();
        const bool connect_pending = remote.connect_pending() != nullptr || remote.connect_pending_end_stream() != nullptr;
        std::optional<std::size_t> declared_content_length;
        if (const auto* known = stream->remote_content().allowed_known_length(); known != nullptr) {
            declared_content_length = known->declared_length();
        }
        const http2_server_request_snapshot snapshot{
            .content_ = content,
            .declared_content_length_ = declared_content_length,
            .priority_update_ = stream->priority_update(),
            .body_open_ = value.body_open_,
            .connect_pending_ = connect_pending,
            .initial_end_stream_ = value.initial_end_stream_,
        };
        return ::ruvia::http2_event::request_head(this, value.stream_id_, stream->request_expectations(), content, snapshot);
    }

    template <typename event_record_type>
    [[nodiscard]] ::ruvia::http2_event materialize_public_http2_event(event_record_type& value) {
        using value_type = std::decay_t<event_record_type>;
        const bool client = connection_.role() == http2_role::client;
        if constexpr (std::is_same_v<value_type, detail::http2_message_head_record>) {
            return materialize_message_head_event(value);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_data_record>) {
            return ::ruvia::http2_event::message_body_chunk(this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_tunnel_data_record>) {
            return ::ruvia::http2_event::tunnel_data(this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_end_record>) {
            auto* stream = connection_.stream(value.stream_id_);
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 message end event has no stream");
            }
            auto result = ::ruvia::http2_event::message_end(value.stream_id_, stream->take_remote_trailers(), message_content_semantics(*stream));
            if (client) {
                connection_.release_stream_lease_after_remote_end(value.stream_id_);
            }
            return result;
        } else {
            if constexpr (std::is_same_v<value_type, http2_tunnel_end_event>) {
                if (client) {
                    connection_.release_stream_lease_after_remote_end(value.stream_id());
                }
            } else if constexpr (std::is_same_v<value_type, http2_stream_closed_event> || std::is_same_v<value_type, http2_request_unprocessed_event>) {
                if (client) {
                    connection_.release_stream_lease(value.stream_id());
                }
            }
            return ::ruvia::http2_event(std::move(value));
        }
    }
    std::size_t references_{1};
    detail::http2_connection connection_;
};

}  // namespace detail

http2_received_data_credit::http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id, std::uint32_t bytes_value) noexcept :
    endpoint_(endpoint),
    stream_id_(stream_id),
    bytes_(bytes_value) {
    endpoint_->retain();
}

http2_received_data_credit::~http2_received_data_credit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->return_credit(stream_id_, bytes_);
    endpoint_->release();
}

http2_received_data_credit::http2_received_data_credit(http2_received_data_credit&& other) noexcept :
    endpoint_(std::exchange(other.endpoint_, nullptr)),
    stream_id_(std::exchange(other.stream_id_, 0)),
    bytes_(std::exchange(other.bytes_, 0)) {}

void http2_received_data_credit::merge(http2_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP/2 received data credit");
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP/2 received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP/2 received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id, http_request_expectations expectations, http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept :
    stream_id_(stream_id),
    snapshot_(snapshot),
    expectations_(expectations),
    content_(content),
    endpoint_(endpoint) {
    endpoint_->retain();
}

http2_request_head_event::~http2_request_head_event() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_request(stream_id_);
    endpoint_->release();
}

http2_request_head_event::http2_request_head_event(http2_request_head_event&& other) noexcept :
    stream_id_(std::exchange(other.stream_id_, 0)),
    snapshot_(other.snapshot_),
    expectations_(other.expectations_),
    content_(other.content_),
    endpoint_(std::exchange(other.endpoint_, nullptr)) {}

http2_server_request_view http2_request_head_event::request_view() const& noexcept {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        return {};
    }
    return endpoint_->connection_.server_request_view(stream_id_).value_or(http2_server_request_view{});
}

http_request http2_request_head_event::build_request(borrowed_text body) const& {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        throw std::logic_error("HTTP/2 request head event is not active");
    }
    auto* stream = endpoint_->connection_.stream(stream_id_);
    if (stream == nullptr) {
        throw std::logic_error("missing HTTP/2 request stream");
    }
    const auto* pending = stream->tunnel().pending();
    const detail::binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http2,
        .target_form_ = http_request_target_form::http2,
        .method_ = stream->request_method(),
        .scheme_ = stream->request_scheme(),
        .authority_ = stream->request_authority(),
        .host_ = detail::http2_request_host_field(*stream),
        .path_ = stream->request_path(),
        .protocol_ = stream->request_protocol(),
        .cookie_ = stream->request_cookie(),
        .body_ = body.view(),
        .regular_header_count_ = stream->remote_header_count(),
        .has_authority_ = stream->has_authority(),
        .has_host_ = stream->has_host(),
        .has_path_ = stream->has_path(),
        .has_cookie_ = stream->has_cookie(),
        .standard_connect_ = pending != nullptr && pending->form() == detail::http2_connect_form::standard,
        .failure_message_ = "validated HTTP/2 request cannot be materialized",
    };
    return detail::materialize_binary_http_request(spec, [stream](std::size_t index) noexcept {
        const auto header_value = stream->remote_header_at(index);
        return std::pair{http_header_view{header_value.name_, header_value.value_}, header_value.kind_};
    });
}
void http2_connection::impl_deleter_type::operator()(detail::http2_connection_owner_endpoint* value) const noexcept {
    if (value != nullptr) {
        value->release();
    }
}

http2_connection::http2_connection(http2_role role, const http2_connection_options& options) {
    impl_.reset(new detail::http2_connection_owner_endpoint(role, options));
    impl_->connection_.begin_connection();
}
http2_connection http2_connection::server(http2_connection_options options) {
    // Server push (RFC 9113 section 8.4) and ORIGIN receipt (RFC 8336 section 2)
    // are client roles; a server configured with them would silently ignore them.
    if (options.enable_push_ || options.receive_origin_advertisements_) {
        throw std::invalid_argument("HTTP/2 server connection does not accept client-only options");
    }
    return http2_connection(http2_role::server, options);
}
http2_connection http2_connection::client(http2_connection_options options) {
    return http2_connection(http2_role::client, options);
}
http2_connection::~http2_connection() = default;
http2_connection::http2_connection(http2_connection&&) noexcept = default;
http2_connection& http2_connection::operator=(http2_connection&&) noexcept = default;

websocket_handshake_validation_result validate_http2_websocket_handshake(http2_connection& connection, std::uint32_t stream_id, const http_request& request, websocket_handshake_validation_options options) noexcept {
    const auto* stream = connection.impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request, options);
}

std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error> http2_connection::submit_websocket_handshake(std::uint32_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_websocket_handshake(stream_id, request, validation, options);
}

http2_role http2_connection::role() const noexcept {
    return impl_->connection_.role();
}
bool http2_connection::header_block_in_progress() const noexcept {
    return impl_->connection_.header_block_in_progress();
}
bool http2_connection::received_peer_settings() const noexcept {
    return impl_->connection_.received_peer_settings();
}
http2_feed_result http2_connection::feed(std::string_view input) {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    const auto result_value = connection.feed(input);
    connection.retry_deferred_stream_leases();
    return result_value;
}

std::optional<http2_event> http2_connection::next_event() {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    auto* event = connection.peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    auto result_value = std::visit([this](auto& value) {
        return impl_->materialize_public_http2_event(value);
    }, event->value());
    connection.consume_event();
    return std::optional<http2_event>(std::move(result_value));
}

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes, std::string& output, http2_data_output_observer_type observer, void* observer_context) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.take_output_batch(max_bytes, output, observer, observer_context);
}
bool http2_connection::wants_write() const noexcept {
    return impl_->connection_.wants_write();
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_regular_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    std::optional<std::string_view> authority;
    if (request.authority_) {
        authority = request.authority_->view();
    }
    auto result_value = impl_->connection_.submit_regular_request_head(request.method_.view(), request.scheme_.view(), authority, request.target_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.content_, request.expectation_, request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_connect_request_head(request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_extended_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_extended_connect_request_head(request.protocol_.view(), request.scheme_.view(), request.authority_.view(), request.target_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

http2_data_submit_result http2_connection::submit_data(std::uint32_t stream_id, std::string_view bytes_value, http2_end_stream end_stream, std::size_t max_payload_bytes) {
    auto& connection = impl_->connection_;
    const auto result_value = connection.submit_data(stream_id, bytes_value, end_stream, max_payload_bytes);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return result_value;
}
http2_request_content_release_status http2_connection::release_request_content(std::uint32_t stream_id) noexcept {
    return impl_->connection_.release_request_content(stream_id);
}
http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_connect_response_head(stream_id, response);
}

http2_submit_status http2_connection::submit_interim_response_head(std::uint32_t stream_id, const http_interim_response_head& response) {
    return impl_->connection_.submit_interim_response_head(stream_id, response);
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error> http2_connection::submit_response_head(std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error> http2_connection::submit_streaming_response_head(std::uint32_t stream_id, http_response response, http_response_trailer_intent trailer_intent) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_streaming_response_head(stream_id, std::move(response), trailer_intent);
}

http2_finish_status http2_connection::finish_request(std::uint32_t stream_id, std::span<const http_header_view> trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_request(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

http2_finish_status http2_connection::finish_response(std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_response(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(std::uint32_t associated_stream_id, http_push_request_view request) {
    impl_->connection_.retry_deferred_stream_leases();
    const auto id = impl_->connection_.submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    impl_->connection_.pin_stream(std::get<0>(id));
    return http2_request_head_event(impl_.get(), std::get<0>(id), {}, http_request_content_indication::no_content, {});
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    return impl_->connection_.submit_origin_advertisement(origins);
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id, std::string_view origin, std::string_view field_value) {
    return impl_->connection_.submit_alternative_service_advertisement(stream_id, origin, field_value);
}
http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    return impl_->connection_.submit_priority_update(stream_id, fields_value);
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    auto& connection = impl_->connection_;
    const auto status = connection.submit_reset(stream_id, error);
    if (status == http2_submit_status::accepted && connection.role() == http2_role::client) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submit_request_head() here rather than waiting for next_event().
        connection.release_stream_lease(stream_id);
    }
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}
http2_submit_status http2_connection::submit_ping() {
    return impl_->connection_.submit_ping();
}
http2_submit_status http2_connection::submit_max_concurrent_streams(std::uint32_t limit) {
    return impl_->connection_.submit_max_concurrent_streams(limit);
}
std::size_t http2_connection::available_request_streams(std::size_t local_limit) const noexcept {
    return impl_->connection_.available_local_request_streams(local_limit);
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.pending_data_output_bytes(stream_id);
}
std::optional<http2_send_window_state> http2_connection::send_window_state(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.send_window_state(stream_id);
}
bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_aborted(stream_id);
}
http2_stream_receive_status http2_connection::stream_receive_status(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_receive_status(stream_id);
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 || request.endpoint_ != impl_.get()) {
        throw std::logic_error("invalid HTTP/2 server request lease");
    }
    impl_->connection_.retry_deferred_stream_leases();
    const bool live = impl_->connection_.stream(request.stream_id_) != nullptr;
    if (live) {
        impl_->connection_.release_stream_lease(request.stream_id_);
    }
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    endpoint->release();
    return live ? http2_server_request_release_status::released : http2_server_request_release_status::closed;
}
void http2_connection::begin_drain() {
    impl_->connection_.begin_drain();
}
void http2_connection::finish_drain() {
    impl_->connection_.finish_drain();
}

void http2_connection::terminate(http2_error_code error) {
    impl_->connection_.terminate(error);
}
bool http2_connection::draining() const noexcept {
    return impl_->connection_.draining();
}
bool http2_connection::drain_final() const noexcept {
    return impl_->connection_.drain_final();
}
std::optional<http2_error_code> http2_connection::connection_error() const noexcept {
    return impl_->connection_.connection_error();
}

std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme, std::string_view authority, std::string_view target, borrowed_http_headers headers, http_request_field_source source) {
    const auto staged_headers = detail::stage_websocket_extended_connect_fields(negotiation, headers.view());
    return connection.submit_request_head(http2_extended_connect_request_head_view{
        .protocol_ = "websocket",
        .scheme_ = scheme,
        .authority_ = authority,
        .target_ = target,
        .headers_ = staged_headers.view(),
        .field_source_ = source,
    });
}

}  // namespace ruvia

namespace ruvia {
namespace detail {

class hpack_decoder final {
public:
    hpack_decoder() :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_inflate_new2(&inflater_, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK inflater");
        }
    }
    ~hpack_decoder() {
        nghttp2_hd_inflate_del(inflater_);
    }
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;

    void set_max_dynamic_table_size(std::uint32_t bytes) {
        if (decoding_) {
            throw std::logic_error("HPACK table size cannot change during decoding");
        }
        const int result = nghttp2_hd_inflate_change_table_size(inflater_, bytes);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::invalid_argument("invalid HPACK dynamic table size");
        }
    }

    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, void* target, bool (*callback)(void*, std::string_view, std::string_view)) {
        if (decoding_) {
            throw std::logic_error("nested HPACK decode on the same decoder");
        }
        if (poisoned_) {
            return hpack_decode_error::compression_error;
        }
        struct decoding_guard final {
            bool& decoding_;
            explicit decoding_guard(bool& decoding) noexcept :
                decoding_(decoding) {
                decoding_ = true;
            }
            ~decoding_guard() {
                decoding_ = false;
            }
            decoding_guard(const decoding_guard&) = delete;
            decoding_guard& operator=(const decoding_guard&) = delete;
        } guard(decoding_);

        const auto* cursor = reinterpret_cast<const std::uint8_t*>(block.data());
        auto remaining = block.size();
        bool rejected = false;
        for (;;) {
            nghttp2_nv field{};
            int flags = 0;
            const auto consumed = nghttp2_hd_inflate_hd3(inflater_, &field, &flags, cursor, remaining, 1);
            if (consumed < 0) {
                // The table may already hold part of this block; it can no
                // longer be trusted to mirror the peer's encoder.
                poisoned_ = true;
                if (consumed == NGHTTP2_ERR_NOMEM) {
                    memory_.rethrow_allocation_failure();
                }
                return hpack_decode_error::compression_error;
            }
            cursor += consumed;
            remaining -= static_cast<std::size_t>(consumed);
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) != 0 && !rejected && !callback(target, std::string_view(reinterpret_cast<const char*>(field.name), field.namelen), std::string_view(reinterpret_cast<const char*>(field.value), field.valuelen))) {
                rejected = true;
            }
            if ((flags & NGHTTP2_HD_INFLATE_FINAL) != 0) {
                nghttp2_hd_inflate_end_headers(inflater_);
                break;
            }
        }
        return rejected ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_inflater* inflater_{nullptr};
    bool decoding_{false};
    bool poisoned_{false};
};

namespace {

// Uses a per-call deflater so encoding maintains no shared state.
class hpack_field_deflater final {
public:
    hpack_field_deflater() :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_deflate_new2(&deflater_, 0, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK deflater");
        }
        // A deflater whose table is smaller than RFC 7541's initial 4096 bytes
        // announces the reduction in its first block. Consume that update
        // here so every encoded field is self-contained.
        std::array<std::uint8_t, 8> announcement{};
        if (nghttp2_hd_deflate_hd2(deflater_, announcement.data(), announcement.size(), nullptr, 0) < 0) {
            nghttp2_hd_deflate_del(deflater_);
            throw std::runtime_error("nghttp2 failed to prime the HPACK deflater");
        }
    }
    ~hpack_field_deflater() {
        nghttp2_hd_deflate_del(deflater_);
    }
    hpack_field_deflater(const hpack_field_deflater&) = delete;
    hpack_field_deflater& operator=(const hpack_field_deflater&) = delete;

    void encode(std::string& output, std::string_view name, std::string_view value, bool sensitive) {
        nghttp2_nv field{
            .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
            .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
            .namelen = name.size(),
            .valuelen = value.size(),
            .flags = static_cast<std::uint8_t>(sensitive ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
        };
        const auto bound = nghttp2_hd_deflate_bound(deflater_, &field, 1);
        if (bound > output.max_size() - output.size()) {
            throw std::length_error("HPACK field exceeds the output size limit");
        }
        const auto previous_size = output.size();
        output.resize(previous_size + bound);
        const auto encoded = nghttp2_hd_deflate_hd2(deflater_, reinterpret_cast<std::uint8_t*>(output.data() + previous_size), bound, &field, 1);
        if (encoded < 0) {
            output.resize(previous_size);
            if (encoded == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::length_error("nghttp2 rejected the HPACK field");
        }
        output.resize(previous_size + static_cast<std::size_t>(encoded));
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_deflater* deflater_{nullptr};
};
}  // namespace
}  // namespace detail

hpack_decoder::hpack_decoder() :
    impl_(std::make_unique<detail::hpack_decoder>()) {}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(hpack_table_size bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value.value());
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decode(block, target, callback_value);
}

void hpack_encoder::encode_header(std::string& output, std::string_view name, std::string_view value) {
    // Resizing output may invalidate borrowed views; own them first.
    std::string owned_name;
    std::string owned_value;
    if (detail::http_text_aliases_string(name, output)) {
        owned_name.assign(name);
        name = owned_name;
    }
    if (detail::http_text_aliases_string(value, output)) {
        owned_value.assign(value);
        value = owned_value;
    }
    detail::hpack_field_deflater deflater;
    deflater.encode(output, name, value, detail::http_field_never_indexed(name));
}

void hpack_encoder::encode_status(std::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    encode_header(output, ":status", detail::http_status_code_token_view(token));
}

}  // namespace ruvia
