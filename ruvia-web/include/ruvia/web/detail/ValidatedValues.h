#pragma once

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <type_traits>

namespace ruvia::detail {

template <typename T>
struct ValidatedValueTypeKey final {
    inline static constexpr std::byte value{};
};

template <typename T>
[[nodiscard]] const void* validatedValueTypeKey() noexcept {
    return &ValidatedValueTypeKey<std::remove_cvref_t<T>>::value;
}

class ValidatedModelBindings;

struct ValidatedModelBindingNode final {
    const void* typeKey;
    const void* value;
    ValidatedModelBindingNode* previous;
};

template <typename T>
class ValidatedModelBinding final {
public:
    ValidatedModelBinding(const ValidatedModelBinding&) = delete;
    ValidatedModelBinding& operator=(const ValidatedModelBinding&) = delete;
    ValidatedModelBinding(ValidatedModelBinding&&) = delete;
    ValidatedModelBinding& operator=(ValidatedModelBinding&&) = delete;
    ~ValidatedModelBinding() noexcept;

private:
    friend class ValidatedModelBindings;

    ValidatedModelBinding(ValidatedModelBindings& bindings, const T& value) noexcept;

    ValidatedModelBindings* bindings_;
    ValidatedModelBindingNode node_;
};

// Context owns only the intrusive head. Every typed validator coroutine owns
// its model and binding node, so nested next() calls form a naturally scoped,
// allocation-free stack and unwind in strict LIFO order on success or failure.
// A validated model is a capability visible only to that validator's downstream
// dynamic next() scope; upstream middleware cannot retain or observe it later.
class ValidatedModelBindings final {
public:
    ValidatedModelBindings() noexcept = default;
    ~ValidatedModelBindings() noexcept {
        if (head_ != nullptr) {
            std::terminate();
        }
    }
    ValidatedModelBindings(const ValidatedModelBindings&) = delete;
    ValidatedModelBindings& operator=(const ValidatedModelBindings&) = delete;

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>& get() const {
        using ModelT = std::remove_cvref_t<T>;
        const auto* key = validatedValueTypeKey<ModelT>();
        for (auto* node = head_; node != nullptr; node = node->previous) {
            if (node->typeKey == key) {
                return *static_cast<const ModelT*>(node->value);
            }
        }
        throw std::logic_error("validated request model is not available");
    }

    template <typename T>
    [[nodiscard]] ValidatedModelBinding<T> bind(const T& value) {
        return ValidatedModelBinding<T>(*this, value);
    }

    template <typename T>
        requires (!std::is_lvalue_reference_v<T>)
    [[nodiscard]] ValidatedModelBinding<std::remove_cvref_t<T>> bind(T&&) = delete;

private:
    template <typename T>
    friend class ValidatedModelBinding;

    void push(ValidatedModelBindingNode& node) noexcept {
        node.previous = head_;
        head_ = &node;
    }

    void pop(ValidatedModelBindingNode& node) noexcept {
        if (head_ != &node) {
            std::terminate();
        }
        head_ = node.previous;
    }

    ValidatedModelBindingNode* head_{nullptr};
};

template <typename T>
ValidatedModelBinding<T>::ValidatedModelBinding(
    ValidatedModelBindings& bindings,
    const T& value) noexcept
    : bindings_(&bindings),
      node_{validatedValueTypeKey<T>(), &value, nullptr} {
    bindings_->push(node_);
}

template <typename T>
ValidatedModelBinding<T>::~ValidatedModelBinding() noexcept {
    bindings_->pop(node_);
}

}  // namespace ruvia::detail
