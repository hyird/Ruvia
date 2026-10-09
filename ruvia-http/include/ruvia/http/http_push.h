#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_header.h"

namespace ruvia {

struct http_push_request_view final {
    std::string_view method_{"GET"};
    std::string_view scheme_{"https"};
    std::string_view authority_{};
    std::string_view path_{"/"};
    std::span<const http_header_view> headers_{};
};

// Detached promise metadata. Its resource must outlive this value, including
// when it survives the pushed stream or connection. Accepting a promise does
// not establish transport authority: the driver validates the URI origin using
// its authenticated connection before consuming the pushed response.
struct http_push_request final {
    explicit http_push_request(std::pmr::memory_resource* resource)
        : method_(resource),
          scheme_(resource),
          authority_(resource),
          path_(resource),
          headers_(resource) {}
    http_push_request(const http_push_request&) = delete;
    http_push_request& operator=(const http_push_request&) = delete;
    http_push_request(http_push_request&&) noexcept = default;
    http_push_request& operator=(http_push_request&&) = delete;
    std::pmr::string method_;
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::string path_;
    std::pmr::vector<http_header> headers_;
};

struct http2_push_promise_event final {
    std::uint32_t associated_stream_id_;
    std::uint32_t promised_stream_id_;
    http_push_request request_;
};

enum class http2_push_submit_error : std::uint8_t {
    invalid_state,
    push_disabled,
    stream_limit,
    invalid_request,
};

}  // namespace ruvia
