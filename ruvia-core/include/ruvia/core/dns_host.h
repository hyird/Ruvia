#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia {

[[nodiscard]] inline bool is_ascii_dns_alnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool is_ascii_dns_digit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// Validates an ASCII transport hostname with an optional root dot. This is
// intentionally narrower than a DNS owner name or URI reg-name; the
// four-numeric-label exclusion is lexical and does not parse IPv4 addresses.
[[nodiscard]] inline bool is_valid_dns_host(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    if (host.empty() || host.ends_with('.') || host.size() > 253) {
        return false;
    }

    std::size_t labels = 0;
    bool saw_dot = false;
    bool all_labels_numeric = true;
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const auto label_end = host.find('.', label_start);
        const auto end = label_end == std::string_view::npos ? host.size() : label_end;
        const auto label_length = end - label_start;
        if (label_length == 0 || label_length > 63) {
            return false;
        }
        const auto first = static_cast<unsigned char>(host[label_start]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!is_ascii_dns_alnum(first) || !is_ascii_dns_alnum(last)) {
            return false;
        }

        bool label_numeric = true;
        for (std::size_t i = label_start; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!is_ascii_dns_alnum(byte) && byte != '-') {
                return false;
            }
            label_numeric = label_numeric && is_ascii_dns_digit(byte);
        }
        all_labels_numeric = all_labels_numeric && label_numeric;
        ++labels;
        if (label_end == std::string_view::npos) {
            break;
        }
        saw_dot = true;
        label_start = end + 1;
    }
    return !(saw_dot && labels == 4 && all_labels_numeric);
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::is_ascii_dns_alnum;
using ::ruvia::is_ascii_dns_digit;
using ::ruvia::is_valid_dns_host;
}  // namespace ruvia::detail
