#include "ruvia/http/http3_settings.h"

#include <array>
#include <memory_resource>
#include <unordered_set>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {
namespace {

constexpr std::uint64_t qpack_max_table_capacity = 0x1;
constexpr std::uint64_t max_field_section_size = 0x6;
constexpr std::uint64_t qpack_blocked_streams = 0x7;
constexpr std::uint64_t enable_connect_protocol = 0x8;
constexpr std::uint64_t h3_datagram = 0x33;

[[nodiscard]] constexpr bool is_forbidden_setting(std::uint64_t identifier) noexcept {
    return identifier == 0 || (identifier >= 0x2 && identifier <= 0x5);
}

}  // namespace

std::variant<http3_settings, http3_settings_error> decode_http3_settings(
    std::span<const char> payload_value, std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        resource = std::pmr::get_default_resource();
    }
    std::pmr::unordered_set<std::uint64_t> identifiers(resource);
    http3_settings settings;
    std::size_t offset = 0;
    while (offset < payload_value.size()) {
        const auto identifier = decode_http3_var_int(payload_value.subspan(offset));
        if ((identifier.index() != 0)) {
            return http3_settings_error::need_more_data;
        }
        offset += std::get<0>(identifier).encoded_bytes_;
        const auto value = decode_http3_var_int(payload_value.subspan(offset));
        if ((value.index() != 0)) {
            return http3_settings_error::need_more_data;
        }
        offset += std::get<0>(value).encoded_bytes_;

        if (!identifiers.insert(std::get<0>(identifier).value_).second) {
            return http3_settings_error::duplicate_identifier;
        }
        if (is_forbidden_setting(std::get<0>(identifier).value_)) {
            return http3_settings_error::forbidden_identifier;
        }
        switch (std::get<0>(identifier).value_) {
            case qpack_max_table_capacity:
                settings.qpack_max_table_capacity_ = std::get<0>(value).value_;
                break;
            case max_field_section_size:
                settings.max_field_section_size_ = std::get<0>(value).value_;
                break;
            case qpack_blocked_streams:
                settings.qpack_blocked_streams_ = std::get<0>(value).value_;
                break;
            case enable_connect_protocol:
                if (std::get<0>(value).value_ > 1) {
                    return http3_settings_error::value_out_of_range;
                }
                settings.enable_connect_protocol_ = std::get<0>(value).value_ == 1;
                break;
            case h3_datagram:
                if (std::get<0>(value).value_ > 1) {
                    return http3_settings_error::value_out_of_range;
                }
                settings.h3_datagram_ = std::get<0>(value).value_ == 1;
                break;
            default:
                break;
        }
    }
    return settings;
}

std::variant<std::size_t, http3_settings_error> encode_http3_settings(
    std::span<char> output, const http3_settings& settings) noexcept {
    constexpr std::array<std::uint64_t, 5> identifiers{
        qpack_max_table_capacity, max_field_section_size, qpack_blocked_streams, enable_connect_protocol, h3_datagram};
    const std::array<std::optional<std::uint64_t>, 5> values{settings.qpack_max_table_capacity_,
        settings.max_field_section_size_, settings.qpack_blocked_streams_,
        settings.enable_connect_protocol_ ? std::optional<std::uint64_t>{1} : std::nullopt,
        settings.h3_datagram_ ? std::optional<std::uint64_t>{1} : std::nullopt};
    std::size_t required = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        if (*values[i] > http3_var_int_max) {
            return http3_settings_error::value_out_of_range;
        }
        required += http3_var_int_encoded_size(identifiers[i]) + http3_var_int_encoded_size(*values[i]);
    }
    if (output.size() < required) {
        return http3_settings_error::output_too_small;
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < identifiers.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        for (const auto value : {identifiers[i], *values[i]}) {
            const auto written = encode_http3_var_int(output.subspan(offset), value);
            if ((written.index() != 0)) {
                return http3_settings_error::value_out_of_range;
            }
            offset += std::get<0>(written);
        }
    }
    return offset;
}

}  // namespace ruvia
