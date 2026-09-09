#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool isAsciiDnsAlnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool isAsciiDnsDigit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// Validates an ASCII transport hostname with an optional root dot. This is
// intentionally narrower than a DNS owner name or URI reg-name; the
// four-numeric-label exclusion is lexical and does not parse IPv4 addresses.
[[nodiscard]] inline bool isValidDnsHost(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    if (host.empty() || host.ends_with('.') || host.size() > 253) {
        return false;
    }

    std::size_t labels = 0;
    bool sawDot = false;
    bool allLabelsNumeric = true;
    std::size_t labelStart = 0;
    while (labelStart < host.size()) {
        const auto labelEnd = host.find('.', labelStart);
        const auto end = labelEnd == std::string_view::npos ? host.size() : labelEnd;
        const auto labelLength = end - labelStart;
        if (labelLength == 0 || labelLength > 63) {
            return false;
        }
        const auto first = static_cast<unsigned char>(host[labelStart]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!isAsciiDnsAlnum(first) || !isAsciiDnsAlnum(last)) {
            return false;
        }

        bool labelNumeric = true;
        for (std::size_t i = labelStart; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!isAsciiDnsAlnum(byte) && byte != '-') {
                return false;
            }
            labelNumeric = labelNumeric && isAsciiDnsDigit(byte);
        }
        allLabelsNumeric = allLabelsNumeric && labelNumeric;
        ++labels;
        if (labelEnd == std::string_view::npos) {
            break;
        }
        sawDot = true;
        labelStart = end + 1;
    }
    return !(sawDot && labels == 4 && allLabelsNumeric);
}

}  // namespace ruvia::detail
