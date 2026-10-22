#pragma once

#include <span>
#include <string_view>

namespace ruvia::detail {

// Request-scoped typed access to the owning connection's advertisement lane.
class http_connection_advertisement_output final {
public:
    using origins_type = void (*)(void*, std::span<const std::string_view>);
    using alternative_service_type = void (*)(void*, std::string_view);
    http_connection_advertisement_output(
        void* target, origins_type origins, alternative_service_type service) noexcept
        : target_(target),
          origins_(origins),
          service_(service) {}
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

private:
    void* target_;
    origins_type origins_;
    alternative_service_type service_;
};

}  // namespace ruvia::detail
