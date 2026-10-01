#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpHeader.h"

namespace ruvia {

struct HttpPushRequestView final {
    std::string_view method{"GET"};
    std::string_view scheme{"https"};
    std::string_view authority{};
    std::string_view path{"/"};
    std::span<const HttpHeaderView> headers{};
};

// Detached promise metadata. Its resource must outlive this value, including
// when it survives the pushed stream or connection. Accepting a promise does
// not establish transport authority: the driver validates the URI origin using
// its authenticated connection before consuming the pushed response.
struct HttpPushRequest final {
    explicit HttpPushRequest(std::pmr::memory_resource* resource)
        : method(resource),
          scheme(resource),
          authority(resource),
          path(resource),
          headers(resource) {}
    HttpPushRequest(const HttpPushRequest&) = delete;
    HttpPushRequest& operator=(const HttpPushRequest&) = delete;
    HttpPushRequest(HttpPushRequest&&) noexcept = default;
    HttpPushRequest& operator=(HttpPushRequest&&) = delete;
    std::pmr::string method;
    std::pmr::string scheme;
    std::pmr::string authority;
    std::pmr::string path;
    std::pmr::vector<HttpHeader> headers;
};

struct Http2PushPromiseEvent final {
    std::uint32_t associatedStreamId;
    std::uint32_t promisedStreamId;
    HttpPushRequest request;
};

enum class Http2PushSubmitError : std::uint8_t {
    kInvalidState,
    kPushDisabled,
    kStreamLimit,
    kInvalidRequest,
};

}  // namespace ruvia
