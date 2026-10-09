#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_client.h"

namespace ruvia::detail {

struct http_client_request_storage_access;
class http_client_upload_state;
class http_client_tunnel_state;
class http_client_output_queue;

class http_client_request_storage final {
public:
    // Internal owning boundary: callers must transfer the completed request before
    // asynchronous transport work can outlive the source operation.
    http_client_request_storage(
        std::string_view method, std::string_view target, std::pmr::memory_resource* resource);

    http_client_request_storage(const http_client_request_storage&) = delete;
    http_client_request_storage& operator=(const http_client_request_storage&) = delete;
    http_client_request_storage(http_client_request_storage&& other) noexcept;
    http_client_request_storage& operator=(http_client_request_storage&&) noexcept = delete;

    [[nodiscard]] http_client_request_storage into_resource(
        std::pmr::memory_resource* resource) &&;

    http_client_request_storage& append_header(std::string_view name, std::string_view value);
    http_client_request_storage& set_body(std::string_view body);
    void set_replay_safe(bool replay_safe) noexcept {
        replay_safe_ = replay_safe;
    }
    [[nodiscard]] bool replay_safe() const noexcept {
        return replay_safe_;
    }

    void set_tunnel(std::string_view authority, std::string_view protocol);
    void bind_tunnel(http_client_tunnel_state& tunnel) noexcept {
        tunnel_ = &tunnel;
    }
    [[nodiscard]] http_client_tunnel_state* tunnel() const noexcept {
        return tunnel_;
    }
    [[nodiscard]] http_client_output_queue* output() const noexcept;
    [[nodiscard]] bool is_tunnel() const noexcept {
        return is_tunnel_;
    }
    [[nodiscard]] std::string_view tunnel_authority() const& noexcept {
        return tunnel_authority_;
    }
    std::string_view tunnel_authority() const&& = delete;
    [[nodiscard]] std::string_view tunnel_protocol() const& noexcept {
        return tunnel_protocol_;
    }
    std::string_view tunnel_protocol() const&& = delete;
    void bind_upload(http_client_upload_state& upload) noexcept {
        upload_ = &upload;
    }
    [[nodiscard]] http_client_upload_state* upload() const noexcept {
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
    friend struct http_client_request_storage_access;

    struct header_type final {
        header_type(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name_(name, resource),
              value_(value, resource) {}
        std::pmr::string name_;
        std::pmr::string value_;
    };

    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<header_type> headers_;
    std::pmr::string body_;
    std::pmr::string tunnel_authority_;
    std::pmr::string tunnel_protocol_;
    bool is_tunnel_{};
    http_client_tunnel_state* tunnel_{};
    bool has_body_{false};
    bool replay_safe_{};
    http_client_upload_state* upload_{};
};

struct http_client_request_storage_access final {
    [[nodiscard]] static http_client_request_view view(
        const http_client_request_storage& request, std::pmr::vector<http_header_view>& headers);
    [[nodiscard]] static const auto& headers(const http_client_request_storage& request) noexcept {
        return request.headers_;
    }
    [[nodiscard]] static auto& headers(http_client_request_storage& request) noexcept {
        return request.headers_;
    }
    [[nodiscard]] static bool has_body(const http_client_request_storage& request) noexcept {
        return request.has_body_;
    }
};

}  // namespace ruvia::detail
