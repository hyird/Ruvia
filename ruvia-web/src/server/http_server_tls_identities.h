#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/tls_config.h"

#include "tls/tls_credential.h"

namespace ruvia::detail {

// Fixed identities are immutable; a live library publishes its own snapshots.
struct http_server_tls_identities final {
    // Certificate and staple form one immutable selection.
    struct identity final {
        identity(tls_certificate certificate, std::span<const std::byte> ocsp_response, std::pmr::memory_resource* resource)
            : certificate_(std::move(certificate)),
              ocsp_response_(ocsp_response.begin(), ocsp_response.end(), pmr_resource_or_default(resource)) {}
        tls_certificate certificate_;
        std::pmr::vector<std::byte> ocsp_response_;
    };
    struct named_identity final {
        named_identity(std::string_view name, identity value, std::pmr::memory_resource* resource)
            : name_(name, pmr_resource_or_default(resource)),
              value_(std::move(value)) {}
        std::pmr::string name_;
        identity value_;
    };
    struct addressed_identity final {
        asio::ip::address address_;
        identity value_;
    };

    http_server_tls_identities(identity primary, std::pmr::memory_resource* resource)
        : primary_(std::move(primary)),
          exact_names_(pmr_resource_or_default(resource)),
          wildcard_names_(pmr_resource_or_default(resource)),
          local_addresses_(pmr_resource_or_default(resource)) {}
    explicit http_server_tls_identities(std::pmr::memory_resource* resource)
        : exact_names_(pmr_resource_or_default(resource)),
          wildcard_names_(pmr_resource_or_default(resource)),
          local_addresses_(pmr_resource_or_default(resource)) {}

    [[nodiscard]] const identity& select(
        std::string_view server_name, const asio::ip::address* local_address) const noexcept;

    std::optional<identity> primary_;
    std::pmr::vector<named_identity> exact_names_;
    std::pmr::vector<named_identity> wildcard_names_;
    std::pmr::vector<addressed_identity> local_addresses_;
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

}  // namespace ruvia::detail
