#pragma once

#include "ruvia/core/memory/PmrObject.h"

#include <array>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

template <typename T>
struct ValidatedValueTypeKey final {
    inline static constexpr std::byte value{};
};

template <typename T>
[[nodiscard]] const void* validatedValueTypeKey() noexcept {
    return &ValidatedValueTypeKey<std::remove_cvref_t<T>>::value;
}

class ValidatedValueStore final {
public:
    ValidatedValueStore() = default;
    ValidatedValueStore(const ValidatedValueStore&) = delete;
    ValidatedValueStore& operator=(const ValidatedValueStore&) = delete;

    ~ValidatedValueStore() {
        clear();
    }

    template <typename T>
    [[nodiscard]] const T& get() const {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validatedValueTypeKey<BodyT>();
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& value = values_[i];
            if (value.typeKey == key) {
                return *static_cast<const BodyT*>(value.value);
            }
        }
        throw std::logic_error("validated request model is not available");
    }

    template <typename T>
    void set(T&& body, std::pmr::memory_resource* resource) {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validatedValueTypeKey<BodyT>();
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& value = values_[i];
            if (value.typeKey == key) {
                throw std::logic_error("validated request model type is already available");
            }
        }

        if (count_ == values_.size()) {
            throw std::logic_error("too many validated request models");
        }

        auto* stored = allocate<BodyT>(std::forward<T>(body), resource);
        Entry next{
            key,
            stored,
            resource,
            &destroy<BodyT>};
        values_[count_++] = next;
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            clearValue(values_[i]);
        }
        count_ = 0;
    }

private:
    struct Entry final {
        const void* typeKey{nullptr};
        void* value{nullptr};
        std::pmr::memory_resource* resource{nullptr};
        void (*destroy)(void*, std::pmr::memory_resource*) noexcept{nullptr};
    };

    template <typename BodyT, typename ArgT>
    [[nodiscard]] static BodyT* allocate(ArgT&& body, std::pmr::memory_resource* resource) {
        return constructPmrObject<BodyT>(resource, std::forward<ArgT>(body));
    }

    template <typename T>
    static void destroy(void* value, std::pmr::memory_resource* resource) noexcept {
        destroyPmrObject(static_cast<T*>(value), resource);
    }

    static void clearValue(Entry& value) noexcept {
        if (value.value != nullptr) {
            if (value.destroy != nullptr) {
                value.destroy(value.value, value.resource);
            }
        }
        value = {};
    }

    std::array<Entry, 8> values_{};
    std::size_t count_{0};
};

}  // namespace ruvia::detail
