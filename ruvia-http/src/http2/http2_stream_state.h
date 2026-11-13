#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_status.h"

#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"
#include "memory/geometric_reserve.h"
#include "response/http_local_content_state.h"

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
        const auto& field = index < inline_header_fields
                                ? inline_fields_[index]
                                : overflow_fields_[index - inline_header_fields];
        return http2_stored_header_view{.name_ = view(field.name_offset_, field.name_size_),
            .value_ = view(field.value_offset_, field.value_size_),
            .kind_ = field.kind_};
    }
    [[nodiscard]] http2_stored_header_view at(std::size_t) const&& = delete;

    [[nodiscard]] bool append(
        std::string_view name, std::string_view value, http_field_kind kind) {
        if (full() || name.size() > max_stored_header_view_size ||
            value.size() > max_stored_header_view_size ||
            storage_size_ > max_stored_header_view_size - name.size() ||
            storage_size_ + name.size() > max_stored_header_view_size - value.size()) {
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

        const auto field = header_field_type{.name_offset_ = name_offset,
            .name_size_ = static_cast<std::uint32_t>(name.size()),
            .value_offset_ = value_offset,
            .value_size_ = static_cast<std::uint32_t>(value.size()),
            .kind_ = kind};

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
    static constexpr std::size_t max_stored_header_view_size =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

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

    explicit constexpr http2_stream_aborted(http2_stream_close_source source_value) noexcept
        : source_(source_value) {}

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

    explicit constexpr http2_remote_content_allowed_without_length(
        std::size_t received_bytes = 0) noexcept
        : received_bytes_(received_bytes) {}

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

    explicit constexpr http2_remote_content_allowed_known_length(
        std::size_t declared_length, std::size_t received_bytes = 0) noexcept
        : declared_length_(declared_length),
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

    explicit constexpr http2_remote_content_metadata_only_known_length(
        std::size_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::size_t declared_length_;
};

enum class http2_remote_content_accounting_result : std::uint8_t {
    accepted,
    counter_overflow,
    declared_length_exceeded,
    content_forbidden
};

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
        if (auto* allowed = std::get_if<http2_remote_content_allowed_without_length>(&state_);
            allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        if (auto* allowed = std::get_if<http2_remote_content_allowed_known_length>(&state_);
            allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            if (allowed->received_bytes_ > allowed->declared_length_ ||
                bytes_value > allowed->declared_length_ - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::declared_length_exceeded;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        return bytes_value == 0 ? http2_remote_content_accounting_result::accepted
                                : http2_remote_content_accounting_result::content_forbidden;
    }

    [[nodiscard]] constexpr const http2_remote_content_allowed_without_length* allowed_without_length()
        const& noexcept {
        return std::get_if<http2_remote_content_allowed_without_length>(&state_);
    }
    const http2_remote_content_allowed_without_length* allowed_without_length() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_allowed_known_length* allowed_known_length()
        const& noexcept {
        return std::get_if<http2_remote_content_allowed_known_length>(&state_);
    }
    const http2_remote_content_allowed_known_length* allowed_known_length() const&& = delete;

    [[nodiscard]] constexpr const void* metadata_only_without_length() const& noexcept {
        return std::holds_alternative<std::monostate>(state_) ? this : nullptr;
    }
    const void* metadata_only_without_length() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_metadata_only_known_length*
    metadata_only_known_length() const& noexcept {
        return std::get_if<http2_remote_content_metadata_only_known_length>(&state_);
    }
    const http2_remote_content_metadata_only_known_length* metadata_only_known_length() const&& = delete;

    [[nodiscard]] bool terminal_length_valid() const noexcept {
        const auto* known = allowed_known_length();
        return known == nullptr || known->received_bytes() == known->declared_length();
    }

private:
    std::variant<http2_remote_content_allowed_without_length, http2_remote_content_allowed_known_length,
        std::monostate, http2_remote_content_metadata_only_known_length>
        state_{http2_remote_content_allowed_without_length()};
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

    explicit constexpr http2_remote_connect_pending_end_stream(bool ended_without_tunnel_data) noexcept
        : ended_without_tunnel_data_(ended_without_tunnel_data) {}

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
        return head_pending() != nullptr || head_end_stream_pending() != nullptr ||
               connect_pending() != nullptr || connect_pending_end_stream() != nullptr;
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

    [[nodiscard]] constexpr const http2_remote_connect_pending_end_stream* connect_pending_end_stream()
        const& noexcept {
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

    std::variant<phase_type, http2_remote_connect_pending, http2_remote_connect_pending_end_stream>
        state_{phase_type::head_pending};
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

    explicit constexpr http2_connect_pending(http2_connect_form form) noexcept
        : form_(form) {}

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
        if ((form != http2_connect_form::standard && form != http2_connect_form::extended) ||
            not_connect() == nullptr) {
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
    explicit http2_stream_state(std::uint32_t stream_id) noexcept
        : id_(stream_id) {}
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

    [[nodiscard]] http2_remote_content_accounting_result account_remote_content(
        std::size_t bytes_value) noexcept {
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
        const bool content_can_follow =
            remote_receive_.content_open() != nullptr &&
            (remote_content_.allowed_without_length() != nullptr ||
                (known_length != nullptr &&
                    known_length->received_bytes() < known_length->declared_length()));
        return content_can_follow ? http_request_content_indication::will_follow
                                  : http_request_content_indication::no_content;
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
        return known_method_ != http_known_method::unknown
                   ? known_http_method_token(known_method_)
                   : std::string_view(method_);
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

    [[nodiscard]] bool append_request_cookie_header_value(
        std::string_view value, bool has_existing_cookie) {
        return append_binary_cookie_crumb(cookie_, value, has_existing_cookie);
    }

    [[nodiscard]] std::size_t remote_header_count() const noexcept {
        return headers_.size();
    }

    [[nodiscard]] http2_stored_header_view remote_header_at(std::size_t index) const& noexcept {
        return headers_.at(index);
    }
    http2_stored_header_view remote_header_at(std::size_t) const&& = delete;

    [[nodiscard]] bool append_remote_header(
        std::string_view name, std::string_view value, http_field_kind kind) {
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
    return remote.head_end_stream_pending() != nullptr ||
           remote.connect_pending_end_stream() != nullptr || remote.end_stream() != nullptr ||
           remote.aborted() != nullptr;
}

// Protocol closure is independent of whether request-view storage remains pinned.
// A reset closes both halves immediately; otherwise both peer END_STREAM and a
// committed local END_STREAM are required.
[[nodiscard]] inline bool http2_stream_is_closed(const http2_stream_state& stream) noexcept {
    return stream.is_aborted() || (http2_remote_peer_half_closed(stream) &&
                                      stream.local_send().end_stream_committed() != nullptr);
}

}  // namespace ruvia::detail
