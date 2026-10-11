#include "ruvia/http/http3_server_request.h"

#include <stdexcept>
#include <string_view>

#include "http3/http3_request_content.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia {

http3_server_request::http3_server_request(http3_message_head&& head, std::pmr::memory_resource* body_pool)
    : head_(std::move(head)),
      cookies_(head_.method_.get_allocator().resource()),
      body_(body_pool != nullptr ? body_pool : std::pmr::get_default_resource()),
      body_state_(head_.content_length_, detail::http3_request_payload_allowed(head_)) {
    if (!detail::http3_request_content_metadata_valid(head_)) {
        throw std::invalid_argument("HTTP/3 request method does not permit its declared content");
    }
    build_request();
}

http_server_expectation_plan http3_server_request::expectation_plan(http_unsupported_expectation_policy policy) const noexcept {
    const bool content_remaining = !body_complete_ && !body_aborted_ && detail::http3_request_payload_allowed(head_) &&
                                   (!head_.content_length_ || *head_.content_length_ > body_state_.received_length());
    return head_.expectations_.server_plan(content_remaining ? http_request_content_indication::will_follow : http_request_content_indication::no_content, policy);
}

void http3_server_request::build_request() {
    const bool standard_connect = head_.method_ == "CONNECT" && head_.protocol_.empty();
    const std::string_view target = standard_connect ? std::string_view(head_.authority_)
                                                     : std::string_view(head_.path_);
    const auto query_at = standard_connect ? std::string_view::npos : target.find('?');
    const auto path = standard_connect ? std::string_view{} : target.substr(0, query_at);
    const auto query = query_at == std::string_view::npos ? std::string_view{}
                                                          : target.substr(query_at + 1);

    bool has_host = false;
    std::size_t cookie_count = 0;
    for (const auto& field : head_.headers_) {
        if (field.name() == "host") {
            has_host = true;
        }
        if (field.name() == "cookie") {
            if (cookie_count != 0) {
                cookies_.append("; ");
            }
            cookies_.append(field.value());
            ++cookie_count;
        }
    }
    const bool synthesize_host = !has_host && !head_.authority_.empty() &&
                                 detail::is_valid_host_header(head_.authority_);
    const std::size_t output_count = head_.headers_.size() - cookie_count +
                                     static_cast<std::size_t>(cookie_count != 0) +
                                     static_cast<std::size_t>(synthesize_host);
    if (output_count > max_http_header_fields) {
        throw std::length_error("too many HTTP/3 request headers");
    }

    detail::http_request_access::set_resource(request_, head_.method_.get_allocator().resource());
    detail::http_request_access::set_method(request_, head_.method_);
    detail::http_request_access::set_protocol_version(request_, http_protocol_version::http3);
    detail::http_request_access::set_target(request_, target);
    detail::http_request_access::set_scheme(request_, head_.scheme_);
    detail::http_request_access::set_authority(request_, head_.authority_);
    detail::http_request_access::set_target_form(request_, http_request_target_form::http3);
    detail::http_request_access::set_path(request_, path);
    detail::http_request_access::set_query_string(request_, query);
    detail::http_request_access::reserve_headers(request_, output_count);

    bool emitted_cookie = false;
    for (const auto& field : head_.headers_) {
        if (field.name() == "cookie") {
            if (!emitted_cookie) {
                detail::http_request_access::add_header(request_, http_header_view("cookie", cookies_));
                emitted_cookie = true;
            }
            continue;
        }
        detail::http_request_access::add_header(request_, http_header_view(field.name(), field.value()));
    }
    if (synthesize_host) {
        detail::http_request_access::add_header(request_, http_header_view("host", head_.authority_));
    }
}

void http3_server_request::append_body(std::span<const std::byte> bytes_value) {
    if (body_complete_ || body_aborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    auto next_state = body_state_;
    if (next_state.feed(bytes_value.size(), false) != http3_message_body_result::accepted) {
        throw std::invalid_argument("HTTP/3 request content violates its method or declared length");
    }
    body_.insert(body_.end(), bytes_value.begin(), bytes_value.end());
    body_state_ = next_state;
}

void http3_server_request::finish_body() {
    if (body_complete_ || body_aborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    auto next_state = body_state_;
    if (next_state.feed(0, true) != http3_message_body_result::complete) {
        throw std::logic_error("HTTP/3 request body does not satisfy Content-Length");
    }
    detail::http_request_access::set_body(request_, std::span<const std::byte>(body_));
    body_state_ = next_state;
    body_complete_ = true;
}

void http3_server_request::abort_body() noexcept {
    if (body_complete_ || body_aborted_) {
        return;
    }
    std::pmr::vector<std::byte> empty(body_.get_allocator().resource());
    body_.swap(empty);
    body_aborted_ = true;
}

}  // namespace ruvia
