#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_base64.h"

namespace ruvia {
class bytes;
}  // namespace ruvia

namespace ruvia::detail {

// Text semantics (ordinary string or JSON token) belong to the facade. This
// value owns the common borrow/clone/transfer and strong publication rules.
class model_text_storage final {
public:
    explicit model_text_storage(std::string_view borrowed = {}) noexcept
        : value_(std::in_place_type<std::string_view>, borrowed) {}

    model_text_storage(const model_text_storage&) = delete;
    model_text_storage& operator=(const model_text_storage&) = delete;
    model_text_storage(model_text_storage&& other) noexcept
        : value_(std::move(other.value_)) {
        other.assign_borrowed({});
    }
    model_text_storage& operator=(model_text_storage&&) = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&value_)) {
            return *borrowed;
        }
        return std::get<std::string>(value_);
    }
    std::string_view view() const&& = delete;

    void assign_borrowed(std::string_view value) noexcept {
        value_.emplace<std::string_view>(value);
    }

    void assign_owned(std::string_view value) {
        std::string owned(value);
        value_.emplace<std::string>(std::move(owned));
    }

    void assign_owned(std::string&& value) noexcept {
        value_.emplace<std::string>(std::move(value));
    }

    [[nodiscard]] model_text_storage rebind() const& {
        return model_text_storage(std::string(view()));
    }

    [[nodiscard]] model_text_storage rebind() && {
        if (auto* owned = std::get_if<std::string>(&value_)) {
            model_text_storage rebound(std::move(*owned));
            assign_borrowed({});
            return rebound;
        }
        auto rebound = static_cast<const model_text_storage&>(*this).rebind();
        assign_borrowed({});
        return rebound;
    }

    void assign_from(model_text_storage&& source_value) {
        if (this == &source_value) {
            return;
        }
        auto rebound = std::move(source_value).rebind();
        // Rebinding may throw; replacing with an already-owned variant cannot.
        std::destroy_at(&value_);
        std::construct_at(&value_, std::move(rebound.value_));
    }

private:
    explicit model_text_storage(std::string&& owned) noexcept
        : value_(std::in_place_type<std::string>, std::move(owned)) {}

    std::variant<std::string_view, std::string> value_;
};

template <typename bytes_type = ::ruvia::bytes>
inline void append_model_binary(std::string& output, const bytes_type& value) {
    const auto encoded_size = http_base64_encoded_size(value.size());
    const auto start = output.size();
    output.resize(start + encoded_size + 2);
    output[start] = '"';
    http_base64_encode(output.data() + start + 1, value.view());
    output[start + encoded_size + 1] = '"';
}

template <typename bytes_type = ::ruvia::bytes>
[[nodiscard]] inline std::optional<bytes_type> decode_model_binary(std::string_view encoded) {
    if (encoded.empty()) {
        return bytes_type();
    }
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> decoded(http_base64_decoded_size_bound(encoded.size()));
    const auto decoded_size = http_base64_decode(
        encoded, decoded, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded_size) {
        return std::nullopt;
    }
    decoded.resize(*decoded_size);
    bytes_type result_value;
    result_value.assign_owned(std::move(decoded));
    return result_value;
}

}  // namespace ruvia::detail
