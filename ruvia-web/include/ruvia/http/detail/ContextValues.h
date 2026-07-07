#pragma once

#include "ruvia/memory/PmrObject.h"

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

template <typename T>
class ContextKey final {
public:
    explicit constexpr ContextKey(std::string_view name) noexcept
        : name_(name) {}

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

private:
    std::string_view name_;
};

namespace detail {

template <typename T>
[[nodiscard]] const void* contextValueTypeKey() noexcept {
    static constexpr std::byte key{};
    return &key;
}

class ContextValueStore final {
public:
    explicit ContextValueStore(std::pmr::memory_resource* resource)
        : resource_(pmrResourceOrDefault(resource)),
          entries_(resource_) {}

    ContextValueStore(const ContextValueStore&) = delete;
    ContextValueStore& operator=(const ContextValueStore&) = delete;

    ~ContextValueStore() {
        clear();
    }

    template <typename T, typename... Args>
    T& setAs(std::string_view name, Args&&... args) {
        using StoredT = std::remove_cvref_t<T>;
        static_assert(!std::is_reference_v<StoredT>, "Context values must be stored by value");

        auto* stored = constructPmrObject<StoredT>(resource_, std::forward<Args>(args)...);
        try {
            Entry next(resource_);
            next.name.assign(name.data(), name.size());
            next.typeKey = contextValueTypeKey<StoredT>();
            next.value = stored;
            next.destroy = &destroy<StoredT>;

            if (auto* entry = find(name)) {
                clearValue(*entry);
                *entry = std::move(next);
                return *stored;
            }

            entries_.emplace_back(std::move(next));
        } catch (...) {
            destroy<StoredT>(stored, resource_);
            throw;
        }
        return *stored;
    }

    template <typename T>
    std::decay_t<T>& set(std::string_view name, T&& value) {
        return setAs<std::decay_t<T>>(name, std::forward<T>(value));
    }

    template <typename T>
    [[nodiscard]] T* getIf(std::string_view name) noexcept {
        const auto* key = contextValueTypeKey<std::remove_cvref_t<T>>();
        auto* entry = find(name);
        if (entry == nullptr || entry->typeKey != key) {
            return nullptr;
        }
        return static_cast<std::remove_cvref_t<T>*>(entry->value);
    }

    template <typename T>
    [[nodiscard]] const T* getIf(std::string_view name) const noexcept {
        const auto* key = contextValueTypeKey<std::remove_cvref_t<T>>();
        const auto* entry = find(name);
        if (entry == nullptr || entry->typeKey != key) {
            return nullptr;
        }
        return static_cast<const std::remove_cvref_t<T>*>(entry->value);
    }

    template <typename T>
    [[nodiscard]] T& get(std::string_view name) {
        if (auto* value = getIf<T>(name)) {
            return *value;
        }
        throw std::logic_error("context value is not available");
    }

    template <typename T>
    [[nodiscard]] const T& get(std::string_view name) const {
        if (const auto* value = getIf<T>(name)) {
            return *value;
        }
        throw std::logic_error("context value is not available");
    }

    void clear() noexcept {
        for (auto& entry : entries_) {
            clearValue(entry);
        }
        entries_.clear();
    }

private:
    struct Entry final {
        explicit Entry(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
            : name(resource) {}

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;

        std::pmr::string name;
        const void* typeKey{nullptr};
        void* value{nullptr};
        void (*destroy)(void*, std::pmr::memory_resource*) noexcept{nullptr};
    };

    [[nodiscard]] Entry* find(std::string_view name) noexcept {
        for (auto& entry : entries_) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry* find(std::string_view name) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    template <typename T>
    static void destroy(void* value, std::pmr::memory_resource* resource) noexcept {
        destroyPmrObject(static_cast<T*>(value), resource);
    }

    void clearValue(Entry& entry) noexcept {
        if (entry.value != nullptr && entry.destroy != nullptr) {
            entry.destroy(entry.value, resource_);
        }
        entry.value = nullptr;
        entry.typeKey = nullptr;
        entry.destroy = nullptr;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> entries_;
};

}  // namespace detail
}  // namespace ruvia
