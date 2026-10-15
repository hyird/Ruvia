#pragma once

#include <memory_resource>

#include "ruvia/http/quic_crypto_provider.h"

namespace ruvia::detail {

class openssl_quic_crypto_provider final {
public:
    explicit openssl_quic_crypto_provider(std::pmr::memory_resource* resource);
    ~openssl_quic_crypto_provider() noexcept;
    openssl_quic_crypto_provider(const openssl_quic_crypto_provider&) = delete;
    openssl_quic_crypto_provider& operator=(const openssl_quic_crypto_provider&) = delete;
    openssl_quic_crypto_provider(openssl_quic_crypto_provider&&) = delete;
    openssl_quic_crypto_provider& operator=(openssl_quic_crypto_provider&&) = delete;

    // The provider and its memory resource must outlive every key created from this view.
    [[nodiscard]] quic_crypto_provider_view view() noexcept;

private:
    struct impl;
    impl* impl_{};
};

}  // namespace ruvia::detail
