#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_client.h"

namespace ruvia::detail {

class http_client_tunnel_state;
class http_client_upload_state;
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
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return method_.get_allocator().resource();
    }
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return tunnel_protocol_ == "connect-udp";
    }

    // Field views borrow this request's owned fields through `headers`.
    [[nodiscard]] http_client_request_view view(std::pmr::vector<http_header_view>& headers) const;

private:
    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<http_header> headers_;
    std::pmr::string body_;
    std::pmr::string tunnel_authority_;
    std::pmr::string tunnel_protocol_;
    bool is_tunnel_{};
    http_client_tunnel_state* tunnel_{};
    bool has_body_{false};
    http_client_upload_state* upload_{};
};

}  // namespace ruvia::detail
