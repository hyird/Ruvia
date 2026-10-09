#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/http/attributes.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"

namespace ruvia {

// Owns a decoded HTTP/3 request head and body for the lifetime of its borrowed
// http_request view. request_resource and body_pool must outlive this owner.
class http3_server_request final {
public:
    // Copies all head data before returning. The body is accumulated separately
    // and is not published through request() until finish_body().
    http3_server_request(const http3_message_head& callback_head,
        std::pmr::memory_resource* request_resource, std::pmr::memory_resource* body_pool);

    http3_server_request(const http3_server_request&) = delete;
    http3_server_request& operator=(const http3_server_request&) = delete;
    http3_server_request(http3_server_request&&) = delete;
    http3_server_request& operator=(http3_server_request&&) = delete;

    [[nodiscard]] const http_request& request() const& noexcept {
        return request_;
    }
    const http_request& request() const&& = delete;

    [[nodiscard]] std::string_view extended_connect_protocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_.protocol_;
    }
    std::string_view extended_connect_protocol() const&& = delete;

    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy policy) const noexcept {
        const bool content_remaining = !body_complete_ && !body_aborted_ && request_.known_method() != http_known_method::connect &&
                                       (!head_.content_length_ || *head_.content_length_ > body_.size());
        return expectations_.server_plan(content_remaining ? http_request_content_indication::will_follow : http_request_content_indication::no_content, policy);
    }

    [[nodiscard]] bool body_complete() const noexcept {
        return body_complete_;
    }
    [[nodiscard]] std::size_t body_bytes() const noexcept {
        return body_.size();
    }
    void append_body(std::span<const std::byte> bytes);
    void finish_body();
    void abort_body() noexcept;

private:
    void build_request();

    // request_ is destroyed before the storage its views borrow.
    http3_message_head head_;
    std::pmr::string cookies_;
    std::pmr::vector<std::byte> body_;
    http_request request_;
    http_request_expectations expectations_{};
    bool body_complete_{false};
    bool body_aborted_{false};
};

}  // namespace ruvia
