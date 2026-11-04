#include "tls/tls_served_certificate.h"

#include <algorithm>
#include <string_view>

#include <openssl/x509v3.h>

#include "ruvia/http/http_ascii.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::string_view origin_key(const http_target_origin& origin) noexcept {
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        return origin.host();
    }
    const auto address = origin.address();
    return {reinterpret_cast<const char*>(address.data()), address.size()};
}

}  // namespace

bool tls_served_certificate::covers(const http_target_origin& origin) noexcept {
    if (origin.action() != http_target_origin_action::verify_certificate) {
        return false;
    }
    const auto key = origin_key(origin);
    const bool dns_name = origin.host_kind() == http_target_host_kind::dns_name;
    for (std::size_t i = 0; i < cached_; ++i) {
        const auto& entry = cache_[i];
        if (entry.kind_ != origin.host_kind() || entry.key_size_ != key.size()) {
            continue;
        }
        const std::string_view cached_key(key_storage_.data() + entry.key_offset_, entry.key_size_);
        if (dns_name ? http_ascii_equals_ignore_case(cached_key, key) : cached_key == key) {
            return entry.covered_;
        }
    }
    const bool covered = check(origin);
    if (key.size() <= cached_key_capacity) {
        if (cached_ == cache_size || key.size() > key_storage_capacity - key_used_) {
            cached_ = 0;
            key_used_ = 0;
        }
        auto& entry = cache_[cached_++];
        entry.key_offset_ = static_cast<std::uint8_t>(key_used_);
        entry.key_size_ = static_cast<std::uint8_t>(key.size());
        entry.kind_ = origin.host_kind();
        entry.covered_ = covered;
        std::copy_n(key.data(), key.size(), key_storage_.data() + key_used_);
        key_used_ = static_cast<std::uint16_t>(key_used_ + key.size());
    }
    return covered;
}

bool tls_served_certificate::check(const http_target_origin& origin) const noexcept {
    if (certificate_ == nullptr) {
        return false;
    }
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        const auto host = origin.host();
        // A NUL inside the name is refused by OpenSSL (-2), which fails closed.
        return X509_check_host(certificate_, host.data(), host.size(),
                   X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) == 1;
    }
    const auto address = origin.address();
    return X509_check_ip(certificate_, address.data(), address.size(), 0) == 1;
}

}  // namespace ruvia::detail
