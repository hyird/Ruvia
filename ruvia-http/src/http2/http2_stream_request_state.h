#pragma once

#include <cstdint>
#include <optional>

#include "ruvia/http/http_status.h"

namespace ruvia::detail {

class http2_stream_request_state final {
public:
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

    [[nodiscard]] bool mark_singleton_header(std::uint32_t bit) noexcept {
        if ((singleton_header_bits_ & bit) != 0) {
            return false;
        }
        singleton_header_bits_ |= bit;
        return true;
    }

    [[nodiscard]] bool has_singleton_header(std::uint32_t bit) const noexcept {
        return (singleton_header_bits_ & bit) != 0;
    }

    // Client role: nullptr until the final response :status is committed once for
    // a stream this endpoint opened. The owner bounds preceding 1xx heads.
    [[nodiscard]] const http_status_code* response_status() const& noexcept {
        return response_status_ ? &*response_status_ : nullptr;
    }
    [[nodiscard]] const http_status_code* response_status() const&& = delete;

    [[nodiscard]] bool set_response_status(http_status_code status) noexcept {
        if (response_status_) {
            return false;
        }
        response_status_ = status;
        return true;
    }

    [[nodiscard]] std::uint8_t interim_response_count() const noexcept {
        return interim_responses_;
    }

    void count_interim_response() noexcept {
        ++interim_responses_;
    }

private:
    bool has_protocol_ : 1 {false};
    bool has_scheme_ : 1 {false};
    bool has_authority_ : 1 {false};
    bool has_path_ : 1 {false};
    bool has_host_ : 1 {false};
    bool has_cookie_ : 1 {false};
    bool regular_header_seen_ : 1 {false};
    std::uint32_t singleton_header_bits_{0};
    std::uint16_t scheme_default_port_{0};
    std::optional<http_status_code> response_status_;
    std::uint8_t interim_responses_{0};
};

}  // namespace ruvia::detail
