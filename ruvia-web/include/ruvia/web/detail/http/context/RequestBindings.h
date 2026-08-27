#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ruvia {
template <typename T>
class ValidatedJson;
}

namespace ruvia::detail {

template <typename T>
struct RequestBindingTypeKey final {
    inline static constexpr std::byte value{};
};

// One stable address per type, used as the binding's identity. Cheaper and more
// robust than typeid: no RTTI requirement and no cross-boundary name equality.
template <typename T>
[[nodiscard]] const void* requestBindingTypeKey() noexcept {
    return &RequestBindingTypeKey<std::remove_cvref_t<T>>::value;
}

// Two kinds share one intrusive list but never answer each other's lookups.
// c.req().validated<T>() means "a validator produced and checked this"; if a
// hand-bound request-state value of the same type could satisfy it, that
// guarantee would silently become a lie. Keeping the kinds disjoint preserves
// both contracts on one mechanism.
enum class RequestBindingKind : std::uint8_t {
    kValidatedModel,
    kRequestState,
};

class RequestBindings;

struct RequestBindingNode final {
    const void* typeKey;
    const void* value;
    // Validated-JSON bindings only: the exact original bytes, for JSONB
    // passthrough. Empty for every other binding.
    std::string_view rawJson;
    RequestBindingNode* previous;
    RequestBindingKind kind;
};

// The RAII handle a binder holds for as long as the value must stay visible.
// Neither copyable nor movable: the node it owns is linked into an intrusive
// stack by address, so it must not be relocated, and its scope IS the binding's
// lifetime. Held in the binding coroutine's frame across co_await next().
template <typename T>
class RequestBindingHandle final {
public:
    RequestBindingHandle(const RequestBindingHandle&) = delete;
    RequestBindingHandle& operator=(const RequestBindingHandle&) = delete;
    RequestBindingHandle(RequestBindingHandle&&) = delete;
    RequestBindingHandle& operator=(RequestBindingHandle&&) = delete;
    ~RequestBindingHandle() noexcept;

private:
    friend class RequestBindings;

    RequestBindingHandle(RequestBindings& bindings, const T& value, RequestBindingKind kind,
        std::string_view rawJson) noexcept;

    RequestBindings* bindings_;
    RequestBindingNode node_;
};

// Context owns only the intrusive head. Every binder owns its value and its
// binding node, so nested next() calls form a naturally scoped, allocation-free
// stack that unwinds in strict LIFO order on success or failure. A binding is a
// capability visible only to its binder's downstream dynamic next() scope;
// upstream middleware cannot retain or observe it later.
class RequestBindings final {
public:
    RequestBindings() noexcept = default;
    ~RequestBindings() noexcept {
        if (head_ != nullptr) {
            std::terminate();
        }
    }
    RequestBindings(const RequestBindings&) = delete;
    RequestBindings& operator=(const RequestBindings&) = delete;

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>* tryFind(RequestBindingKind kind) const noexcept {
        using ValueT = std::remove_cvref_t<T>;
        const auto* key = requestBindingTypeKey<ValueT>();
        for (auto* node = head_; node != nullptr; node = node->previous) {
            if (node->typeKey == key && node->kind == kind) {
                return static_cast<const ValueT*>(node->value);
            }
        }
        return nullptr;
    }

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>& getValidated() const {
        const auto* found = tryFind<T>(RequestBindingKind::kValidatedModel);
        if (found == nullptr) {
            throw std::logic_error("validated request model is not available");
        }
        return *found;
    }

    template <typename T>
    [[nodiscard]] ValidatedJson<std::remove_cvref_t<T>> getValidatedJson() const {
        using ModelT = std::remove_cvref_t<T>;
        const auto* key = requestBindingTypeKey<ModelT>();
        for (auto* node = head_; node != nullptr; node = node->previous) {
            if (node->typeKey == key && node->kind == RequestBindingKind::kValidatedModel &&
                !node->rawJson.empty()) {
                return ValidatedJson<ModelT>(
                    *static_cast<const ModelT*>(node->value), node->rawJson);
            }
        }
        throw std::logic_error("validated JSON request model is not available");
    }

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>* tryGetState() const noexcept {
        return tryFind<T>(RequestBindingKind::kRequestState);
    }

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>& getState() const {
        const auto* found = tryFind<T>(RequestBindingKind::kRequestState);
        if (found == nullptr) {
            throw std::logic_error("request state is not bound for this type");
        }
        return *found;
    }

    template <typename T>
    [[nodiscard]] RequestBindingHandle<T> bindValidated(
        const T& value, std::string_view rawJson = {}) {
        return RequestBindingHandle<T>(*this, value, RequestBindingKind::kValidatedModel, rawJson);
    }

    template <typename T>
        requires(!std::is_lvalue_reference_v<T>)
    [[nodiscard]] RequestBindingHandle<std::remove_cvref_t<T>> bindValidated(
        T&&, std::string_view = {}) = delete;

    template <typename T>
    [[nodiscard]] RequestBindingHandle<T> bindState(const T& value) {
        return RequestBindingHandle<T>(*this, value, RequestBindingKind::kRequestState, {});
    }

    // The node stores the value by address, so a temporary would leave the
    // binding dangling the moment the full expression ends.
    template <typename T>
        requires(!std::is_lvalue_reference_v<T>)
    [[nodiscard]] RequestBindingHandle<std::remove_cvref_t<T>> bindState(T&&) = delete;

private:
    template <typename T>
    friend class RequestBindingHandle;

    void push(RequestBindingNode& node) noexcept {
        node.previous = head_;
        head_ = &node;
    }

    void pop(RequestBindingNode& node) noexcept {
        if (head_ != &node) {
            std::terminate();
        }
        head_ = node.previous;
    }

    RequestBindingNode* head_{nullptr};
};

template <typename T>
RequestBindingHandle<T>::RequestBindingHandle(RequestBindings& bindings, const T& value,
    RequestBindingKind kind, std::string_view rawJson) noexcept
    : bindings_(&bindings),
      node_{requestBindingTypeKey<T>(), &value, rawJson, nullptr, kind} {
    bindings_->push(node_);
}

template <typename T>
RequestBindingHandle<T>::~RequestBindingHandle() noexcept {
    bindings_->pop(node_);
}

}  // namespace ruvia::detail

namespace ruvia {

// The handle a middleware holds to keep a request-scoped value visible to
// everything it calls through next(). Neither copyable nor movable; its scope is
// the binding's lifetime.
template <typename T>
using RequestStateBinding = detail::RequestBindingHandle<T>;

}  // namespace ruvia
