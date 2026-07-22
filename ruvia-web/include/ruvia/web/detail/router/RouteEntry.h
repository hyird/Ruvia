#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/router/RouteEndpoint.h"

// One registered route: its method and path, the endpoint it runs, its captured
// parameter names, and the slice of the table's middleware it inherits.

namespace ruvia::detail {

class RouteEntry final {
public:
    struct Init final {
        HttpKnownMethod method;
        std::string_view path;
        RouteEndpoint endpoint;
        bool dynamic{false};
        std::size_t middlewareOffset{0};
        std::size_t middlewareCount{0};
    };

    RouteEntry(std::pmr::memory_resource* resource, Init init);
    RouteEntry(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource, Init init);
    RouteEntry(const RouteEntry&) = delete;
    RouteEntry& operator=(const RouteEntry&) = delete;
    RouteEntry(RouteEntry&&) noexcept = default;
    RouteEntry& operator=(RouteEntry&&) = delete;

    [[nodiscard]] HttpKnownMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const RouteEndpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    [[nodiscard]] std::span<const std::string_view> paramNames() const noexcept {
        return paramNames_;
    }

    [[nodiscard]] std::size_t middlewareOffset() const noexcept {
        return middlewareOffset_;
    }

    [[nodiscard]] std::size_t middlewareCount() const noexcept {
        return middlewareCount_;
    }

    [[nodiscard]] bool hasMiddleware() const noexcept {
        return middlewareCount_ != 0;
    }

    void setMiddlewareRange(std::size_t offset, std::size_t count) noexcept {
        middlewareOffset_ = offset;
        middlewareCount_ = count;
    }

    void setParamNames(std::span<const std::string_view> names) noexcept {
        paramNames_ = names;
    }

private:
    HttpKnownMethod method_;
    std::pmr::string path_;
    RouteEndpoint endpoint_;
    bool dynamic_{false};
    std::span<const std::string_view> paramNames_{};
    std::size_t middlewareOffset_{0};
    std::size_t middlewareCount_{0};
};

}  // namespace ruvia::detail
