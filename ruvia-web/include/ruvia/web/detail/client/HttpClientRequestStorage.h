#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

struct HttpClientRequestStorageAccess;

class HttpClientRequestStorage final {
public:
    // Internal owning boundary: callers must transfer the completed request before
    // asynchronous transport work can outlive the source operation.
    HttpClientRequestStorage(
        std::string_view method, std::string_view target, std::pmr::memory_resource* resource);

    HttpClientRequestStorage(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage& operator=(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage(HttpClientRequestStorage&&) noexcept = default;
    HttpClientRequestStorage& operator=(HttpClientRequestStorage&&) noexcept = delete;

    [[nodiscard]] HttpClientRequestStorage intoResource(
        std::pmr::memory_resource* resource) &&;

    HttpClientRequestStorage& appendHeader(std::string_view name, std::string_view value);
    HttpClientRequestStorage& setBody(std::string_view body);

    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view target() const& noexcept {
        return target_;
    }
    [[nodiscard]] std::string_view target() const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend struct HttpClientRequestStorageAccess;

    struct Header final {
        Header(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource),
              value(value, resource) {}
        std::pmr::string name;
        std::pmr::string value;
    };

    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<Header> headers_;
    std::pmr::string body_;
    bool hasBody_{false};
};

}  // namespace ruvia::detail
