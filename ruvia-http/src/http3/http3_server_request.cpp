#include "ruvia/http/http3_server_request.h"

#include <stdexcept>
#include <string_view>

#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia {
namespace {

std::pmr::memory_resource* normalized_resource(std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void copy_head(http3_message_head& to, const http3_message_head& from) {
    to.method_ = from.method_;
    to.protocol_ = from.protocol_;
    to.scheme_ = from.scheme_;
    to.authority_ = from.authority_;
    to.path_ = from.path_;
    to.status_ = from.status_;
    to.content_length_ = from.content_length_;
    to.headers_.reserve(from.headers_.size());
    for (const auto& field : from.headers_) {
        to.headers_.emplace_back(field.name_, field.value_, to.headers_.get_allocator().resource());
    }
}

}  // namespace

http3_server_request::http3_server_request(const http3_message_head& callback_head,
    std::pmr::memory_resource* request_resource, std::pmr::memory_resource* body_pool)
    : head_(normalized_resource(request_resource)),
      cookies_(normalized_resource(request_resource)),
      body_(normalized_resource(body_pool)) {
    copy_head(head_, callback_head);
    for (const auto& field : head_.headers_) {
        if (field.name_ == "expect") {
            expectations_.parse_field(field.value_);
        }
    }
    build_request();
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
        if (field.name_ == "host") {
            has_host = true;
        }
        if (field.name_ == "cookie") {
            if (cookie_count != 0) {
                cookies_.append("; ");
            }
            cookies_.append(field.value_);
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

    detail::http_request_access::set_resource(request_, normalized_resource(head_.method_.get_allocator().resource()));
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
        if (field.name_ == "cookie") {
            if (!emitted_cookie) {
                detail::http_request_access::add_header(request_, http_header_view("cookie", cookies_));
                emitted_cookie = true;
            }
            continue;
        }
        detail::http_request_access::add_header(request_, http_header_view(field.name_, field.value_));
    }
    if (synthesize_host) {
        detail::http_request_access::add_header(request_, http_header_view("host", head_.authority_));
    }
}

void http3_server_request::append_body(std::span<const std::byte> bytes_value) {
    if (body_complete_ || body_aborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    body_.insert(body_.end(), bytes_value.begin(), bytes_value.end());
}

void http3_server_request::finish_body() {
    if (body_complete_ || body_aborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    detail::http_request_access::set_body(request_, std::span<const std::byte>(body_));
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
