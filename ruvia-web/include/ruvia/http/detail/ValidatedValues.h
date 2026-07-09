#pragma once

#include "ruvia/http/ValidationTypes.h"
#include "detail/HttpPmrObject.h"

#include <array>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

class ValidatedValueStore final {
public:
    ValidatedValueStore() = default;
    ValidatedValueStore(const ValidatedValueStore&) = delete;
    ValidatedValueStore& operator=(const ValidatedValueStore&) = delete;

    ~ValidatedValueStore() {
        clear();
    }

    template <typename T>
    [[nodiscard]] const T& get(ValidationTarget target) const {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validationTypeKey<BodyT>();
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& value = values_[i];
            if (value.target == target && value.typeKey == key) {
                return *static_cast<const BodyT*>(value.value);
            }
        }
        throw std::logic_error("validated request body is not available");
    }

    template <typename T>
    void set(ValidationTarget target, T&& body, std::pmr::memory_resource* resource) {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validationTypeKey<BodyT>();
        std::size_t slot = count_;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& value = values_[i];
            if (value.target == target && value.typeKey == key) {
                slot = i;
                break;
            }
        }

        if (slot == count_ && count_ == values_.size()) {
            throw std::logic_error("too many validated request bodies");
        }

        auto* stored = allocate<BodyT>(std::forward<T>(body), resource);
        Entry next{
            target,
            key,
            stored,
            resource,
            &destroy<BodyT>};

        if (slot == count_) {
            values_[count_++] = next;
            return;
        }

        clearValue(values_[slot]);
        values_[slot] = next;
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            clearValue(values_[i]);
        }
        count_ = 0;
    }

private:
    struct Entry final {
        ValidationTarget target{ValidationTarget::kJson};
        const void* typeKey{nullptr};
        void* value{nullptr};
        std::pmr::memory_resource* resource{nullptr};
        void (*destroy)(void*, std::pmr::memory_resource*) noexcept{nullptr};
    };

    template <typename T>
    [[nodiscard]] static const void* validationTypeKey() noexcept {
        static constexpr std::byte key{};
        return &key;
    }

    template <typename BodyT, typename ArgT>
    [[nodiscard]] static BodyT* allocate(ArgT&& body, std::pmr::memory_resource* resource) {
        return constructHttpPmrObject<BodyT>(resource, std::forward<ArgT>(body));
    }

    template <typename T>
    static void destroy(void* value, std::pmr::memory_resource* resource) noexcept {
        destroyHttpPmrObject(static_cast<T*>(value), resource);
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
