#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>

#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_status.h"
namespace ruvia::detail {

class http2_stream_request_state final {
public:
    void record_pseudo_header(http2_request_pseudo_header header) noexcept {
        // 解码入口已拒绝重复及未知伪首部，至多记录五个不同字段。
        assert(pseudo_header_count_ < pseudo_header_order_.size());
        pseudo_header_order_[pseudo_header_count_++] = header;
    }

    [[nodiscard]] std::span<const http2_request_pseudo_header> pseudo_header_order() const& noexcept {
        return {pseudo_header_order_.data(), pseudo_header_count_};
    }
    [[nodiscard]] std::span<const http2_request_pseudo_header> pseudo_header_order() const&& = delete;

    [[nodiscard]] bool has_protocol() const noexcept {
        return has_protocol_;
    }

    void mark_protocol() noexcept {
        has_protocol_ = true;
    }

    [[nodiscard]] bool has_scheme() const noexcept {
        return has_scheme_;
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

    void mark_host() noexcept {
        has_host_ = true;
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

    // Client role: nullptr until the final response :status is committed once for
    // a stream this endpoint opened. Informational heads do not commit that status.
    [[nodiscard]] const http_received_status* response_status() const& noexcept {
        return response_status_ ? &*response_status_ : nullptr;
    }
    [[nodiscard]] const http_received_status* response_status() const&& = delete;

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

private:
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
    std::optional<http_received_status> response_status_;
    std::optional<http_priority_fields> priority_update_;
};

}  // namespace ruvia::detail
