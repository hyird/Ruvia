#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"

namespace ruvia {

class bytes;
class string;

namespace detail {
struct outbound_bytes_access;
}  // namespace detail

// Owns all outbound octets upon construction: borrowed inputs are copied once,
// while exact owning rvalues (std::string&&, std::vector<std::byte>&&,
// ruvia::bytes&&, owned ruvia::string&&) are adopted in O(1) without copying.
// Empty inputs allocate no heap storage.
class outbound_bytes final {
public:
    outbound_bytes() noexcept = default;

    template <std::same_as<std::string> string_type>
    outbound_bytes(string_type&& owned) noexcept :
        storage_(std::in_place_type<std::string>, std::move(owned)) {}

    template <std::same_as<std::vector<std::byte>> vector_type>
    outbound_bytes(vector_type&& owned) noexcept :
        storage_(std::in_place_type<std::vector<std::byte>>, std::move(owned)) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> &&
            !std::same_as<source_type, std::string> &&
            !std::same_as<std::remove_cvref_t<source_type>, string> &&
            std::convertible_to<source_type&&, std::string_view>)
    outbound_bytes(source_type&& text) :
        storage_(std::in_place_type<std::string>, static_cast<std::string_view>(std::forward<source_type>(text))) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> &&
            !std::same_as<source_type, std::vector<std::byte>> &&
            !std::same_as<std::remove_cvref_t<source_type>, ruvia::bytes> &&
            !std::convertible_to<source_type&&, std::string_view> &&
            std::convertible_to<source_type&&, byte_view>)
    outbound_bytes(source_type&& raw_bytes) :
        storage_(std::in_place_type<std::string>, as_chars(byte_view(std::forward<source_type>(raw_bytes)))) {}

    outbound_bytes(const ruvia::bytes& value);
    outbound_bytes(ruvia::bytes&& value) noexcept;
    outbound_bytes(const string& value);
    outbound_bytes(string&& value);

    outbound_bytes(const outbound_bytes&) = delete;
    outbound_bytes(outbound_bytes&&) noexcept = default;
    outbound_bytes& operator=(const outbound_bytes&) = delete;
    outbound_bytes& operator=(outbound_bytes&&) noexcept = default;
    ~outbound_bytes() = default;

    [[nodiscard]] std::size_t size() const noexcept {
        if (const auto* text = std::get_if<std::string>(&storage_)) {
            return text->size();
        }
        return std::get<std::vector<std::byte>>(storage_).size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* text = std::get_if<std::string>(&storage_)) {
            return *text;
        }
        return as_chars(byte_view(std::get<std::vector<std::byte>>(storage_)));
    }
    std::string_view view() const&& = delete;

    [[nodiscard]] byte_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* text = std::get_if<std::string>(&storage_)) {
            return std::as_bytes(std::span<const char>(*text));
        }
        return byte_view(std::get<std::vector<std::byte>>(storage_));
    }
    byte_view bytes() const&& = delete;

    [[nodiscard]] std::string take_string() && {
        if (auto* text = std::get_if<std::string>(&storage_)) {
            return std::move(*text);
        }
        auto raw = std::move(std::get<std::vector<std::byte>>(storage_));
        storage_.emplace<std::string>();
        if (raw.empty()) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
    std::string take_string() & = delete;

    [[nodiscard]] std::vector<std::byte> take_vector() && {
        if (auto* raw = std::get_if<std::vector<std::byte>>(&storage_)) {
            auto moved = std::move(*raw);
            storage_.emplace<std::string>();
            return moved;
        }
        auto text = std::move(std::get<std::string>(storage_));
        if (text.empty()) {
            return {};
        }
        std::vector<std::byte> copied(text.size());
        std::memcpy(copied.data(), text.data(), text.size());
        return copied;
    }
    std::vector<std::byte> take_vector() & = delete;

private:
    friend struct detail::outbound_bytes_access;
    std::variant<std::string, std::vector<std::byte>> storage_;
};

}  // namespace ruvia
