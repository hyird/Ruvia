#pragma once

#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"

#include "http2/http2_header_list.h"
#include "http_header_access.h"

namespace ruvia::detail {

class http2_stream_request_data final {
public:
    struct header_checkpoint_type final {
        http2_header_list::checkpoint_type headers_;
        std::size_t trailer_count_{0};
    };

    explicit http2_stream_request_data(std::pmr::memory_resource* resource = nullptr)
        : http2_stream_request_data(http_resolved_pmr_resource_tag{}, http_pmr_resource_or_default(resource)) {
    }

    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    [[nodiscard]] std::string_view method() const&& = delete;

    [[nodiscard]] http_known_method known_method() const noexcept {
        return known_method_;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return method_.get_allocator().resource();
    }

    void swap(http2_stream_request_data& other) noexcept {
        if (resource() != other.resource()) {
            std::terminate();
        }
        method_.swap(other.method_);
        using std::swap;
        swap(known_method_, other.known_method_);
        scheme_.swap(other.scheme_);
        authority_.swap(other.authority_);
        path_.swap(other.path_);
        protocol_.swap(other.protocol_);
        cookie_.swap(other.cookie_);
        headers_.swap(other.headers_);
        trailers_.swap(other.trailers_);
    }

    [[nodiscard]] header_checkpoint_type header_checkpoint() const noexcept {
        return header_checkpoint_type{.headers_ = headers_.checkpoint(),
            .trailer_count_ = trailers_.size()};
    }

    void rollback_headers(header_checkpoint_type checkpoint) noexcept {
        headers_.rollback(checkpoint.headers_);
        if (checkpoint.trailer_count_ > trailers_.size()) {
            std::terminate();
        }
        while (trailers_.size() > checkpoint.trailer_count_) {
            trailers_.pop_back();
        }
    }

    void assign_method(std::string_view method) {
        method_.assign(method.data(), method.size());
        known_method_ = classify_http_method(method);
    }

    [[nodiscard]] std::string_view scheme() const& noexcept {
        return scheme_;
    }
    [[nodiscard]] std::string_view scheme() const&& = delete;

    void assign_scheme(std::string_view value) {
        scheme_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view authority() const& noexcept {
        return authority_;
    }
    [[nodiscard]] std::string_view authority() const&& = delete;

    void assign_authority(std::string_view value) {
        authority_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view path() const& noexcept {
        return path_;
    }
    [[nodiscard]] std::string_view path() const&& = delete;

    void assign_path(std::string_view value) {
        path_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view protocol() const& noexcept {
        return protocol_;
    }
    [[nodiscard]] std::string_view protocol() const&& = delete;

    void assign_protocol(std::string_view value) {
        protocol_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view cookie() const& noexcept {
        return cookie_;
    }
    [[nodiscard]] std::string_view cookie() const&& = delete;

    [[nodiscard]] bool append_cookie_header_value(std::string_view value, bool has_existing_cookie) {
        constexpr std::string_view cookie_separator = "; ";
        const auto separator_bytes = has_existing_cookie ? cookie_separator.size() : 0;
        if (value.size() > max_http_header_bytes ||
            cookie_.size() > max_http_header_bytes - separator_bytes ||
            cookie_.size() + separator_bytes > max_http_header_bytes - value.size()) {
            return false;
        }

        if (has_existing_cookie) {
            cookie_.append(cookie_separator.data(), cookie_separator.size());
        }
        if (!value.empty()) {
            cookie_.append(value.data(), value.size());
        }
        return true;
    }

    [[nodiscard]] bool headers_full() const noexcept {
        return headers_.full();
    }

    [[nodiscard]] std::size_t header_count() const noexcept {
        return headers_.size();
    }

    [[nodiscard]] http2_stored_header_view header_at(std::size_t index) const& noexcept {
        return headers_.at(index);
    }
    [[nodiscard]] http2_stored_header_view header_at(std::size_t) const&& = delete;

    [[nodiscard]] bool append_header(
        std::string_view name, std::string_view value, request_header_kind kind) {
        return headers_.append(name, value, kind);
    }

    [[nodiscard]] bool append_trailer(std::string_view name, std::string_view value) {
        if (trailers_.size() == max_http_header_fields) {
            return false;
        }
        trailers_.push_back(http_header_access::make(name, value, resource()));
        return true;
    }

    [[nodiscard]] std::span<const http_header> trailers() const& noexcept {
        return trailers_;
    }
    [[nodiscard]] std::pmr::vector<http_header> take_trailers() & noexcept {
        return std::move(trailers_);
    }

private:
    http2_stream_request_data(http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : method_(resource),
          scheme_(resource),
          authority_(resource),
          path_(resource),
          protocol_(resource),
          cookie_(resource),
          headers_(resource),
          trailers_(resource) {}

    std::pmr::string method_;
    http_known_method known_method_{http_known_method::unknown};
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::string path_;
    std::pmr::string protocol_;
    std::pmr::string cookie_;
    http2_header_list headers_;
    std::pmr::vector<http_header> trailers_;
};

}  // namespace ruvia::detail
