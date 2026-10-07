#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct HttpClientRequestStorageAccess;
class HttpClientUploadState;
class HttpClientTunnelState;
class http_client_output_queue;

class HttpClientRequestStorage final {
public:
    // Internal owning boundary: callers must transfer the completed request before
    // asynchronous transport work can outlive the source operation.
    HttpClientRequestStorage(
        std::string_view method, std::string_view target, std::pmr::memory_resource* resource);

    HttpClientRequestStorage(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage& operator=(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage(HttpClientRequestStorage&& other) noexcept;
    HttpClientRequestStorage& operator=(HttpClientRequestStorage&&) noexcept = delete;

    [[nodiscard]] HttpClientRequestStorage intoResource(
        std::pmr::memory_resource* resource) &&;

    HttpClientRequestStorage& appendHeader(std::string_view name, std::string_view value);
    HttpClientRequestStorage& setBody(std::string_view body);
    void set_replay_safe(bool replay_safe) noexcept {
        replay_safe_ = replay_safe;
    }
    [[nodiscard]] bool replay_safe() const noexcept {
        return replay_safe_;
    }

    void setTunnel(std::string_view authority, std::string_view protocol);
    void bindTunnel(HttpClientTunnelState& tunnel) noexcept {
        tunnel_ = &tunnel;
    }
    [[nodiscard]] HttpClientTunnelState* tunnel() const noexcept {
        return tunnel_;
    }
    [[nodiscard]] http_client_output_queue* output() const noexcept;
    [[nodiscard]] bool isTunnel() const noexcept {
        return isTunnel_;
    }
    [[nodiscard]] std::string_view tunnelAuthority() const& noexcept {
        return tunnelAuthority_;
    }
    std::string_view tunnelAuthority() const&& = delete;
    [[nodiscard]] std::string_view tunnelProtocol() const& noexcept {
        return tunnelProtocol_;
    }
    std::string_view tunnelProtocol() const&& = delete;
    void bindUpload(HttpClientUploadState& upload) noexcept {
        upload_ = &upload;
    }
    [[nodiscard]] HttpClientUploadState* upload() const noexcept {
        return upload_;
    }
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
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return method_.get_allocator().resource();
    }

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
    std::pmr::string tunnelAuthority_;
    std::pmr::string tunnelProtocol_;
    bool isTunnel_{};
    HttpClientTunnelState* tunnel_{};
    bool hasBody_{false};
    bool replay_safe_{};
    HttpClientUploadState* upload_{};
};

struct HttpClientRequestStorageAccess final {
    [[nodiscard]] static HttpClientRequestView view(
        const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers);
    [[nodiscard]] static const auto& headers(const HttpClientRequestStorage& request) noexcept {
        return request.headers_;
    }
    [[nodiscard]] static auto& headers(HttpClientRequestStorage& request) noexcept {
        return request.headers_;
    }
    [[nodiscard]] static bool hasBody(const HttpClientRequestStorage& request) noexcept {
        return request.hasBody_;
    }
};

}  // namespace ruvia::detail
