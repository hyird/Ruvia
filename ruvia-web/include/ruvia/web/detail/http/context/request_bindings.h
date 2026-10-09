#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ruvia {
template <typename t_type>
class validated_json;
}

namespace ruvia::detail {

template <typename t_type>
struct request_binding_type_key final {
    inline static constexpr std::byte value{};
};

// One stable address per type, used as the binding's identity. Cheaper and more
// robust than typeid: no RTTI requirement and no cross-boundary name equality.
template <typename t_type>
[[nodiscard]] const void* request_binding_key() noexcept {
    return &request_binding_type_key<std::remove_cvref_t<t_type>>::value;
}

// Two kinds share one intrusive list but never answer each other's lookups.
// c.req().validated<T>() means "a validator produced and checked this"; if a
// hand-bound request-state value of the same type could satisfy it, that
// guarantee would silently become a lie. Keeping the kinds disjoint preserves
// both contracts on one mechanism.
enum class request_binding_kind : std::uint8_t {
    validated_model,
    request_state,
};

class request_bindings;

struct request_binding_node final {
    const void* type_key_;
    const void* value_;
    // Validated-JSON bindings only: the exact original bytes, for JSONB
    // passthrough. Empty for every other binding.
    std::string_view raw_json_;
    request_binding_node* previous_;
    request_binding_kind kind_;
};

// The RAII handle a binder holds for as long as the value must stay visible.
// Neither copyable nor movable: the node it owns is linked into an intrusive
// stack by address, so it must not be relocated, and its scope IS the binding's
// lifetime. Held in the binding coroutine's frame across co_await next().
template <typename t_type>
class request_binding_handle final {
public:
    request_binding_handle(const request_binding_handle&) = delete;
    request_binding_handle& operator=(const request_binding_handle&) = delete;
    request_binding_handle(request_binding_handle&&) = delete;
    request_binding_handle& operator=(request_binding_handle&&) = delete;
    ~request_binding_handle() noexcept;

private:
    friend class request_bindings;

    request_binding_handle(request_bindings& bindings, const t_type& value, request_binding_kind kind,
        std::string_view raw_json) noexcept;

    request_bindings* bindings_;
    request_binding_node node_;
};

// context owns only the intrusive head. Every binder owns its value and its
// binding node, so nested next() calls form a naturally scoped, allocation-free
// stack that unwinds in strict LIFO order on success or failure. A binding is a
// capability visible only to its binder's downstream dynamic next() scope;
// upstream middleware cannot retain or observe it later.
class request_bindings final {
public:
    request_bindings() noexcept = default;
    ~request_bindings() noexcept {
        if (head_ != nullptr) {
            std::terminate();
        }
    }
    request_bindings(const request_bindings&) = delete;
    request_bindings& operator=(const request_bindings&) = delete;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_find(request_binding_kind kind) const noexcept {
        using value_t_type = std::remove_cvref_t<t_type>;
        const auto* key = request_binding_key<value_t_type>();
        for (auto* node_value = head_; node_value != nullptr; node_value = node_value->previous_) {
            if (node_value->type_key_ == key && node_value->kind_ == kind) {
                return static_cast<const value_t_type*>(node_value->value_);
            }
        }
        return nullptr;
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_validated() const {
        const auto* found = try_find<t_type>(request_binding_kind::validated_model);
        if (found == nullptr) {
            throw std::logic_error("validated request model is not available");
        }
        return *found;
    }

    template <typename t_type>
    [[nodiscard]] validated_json<std::remove_cvref_t<t_type>> get_validated_json() const {
        using model_t_type = std::remove_cvref_t<t_type>;
        const auto* key = request_binding_key<model_t_type>();
        for (auto* node_value = head_; node_value != nullptr; node_value = node_value->previous_) {
            if (node_value->type_key_ == key && node_value->kind_ == request_binding_kind::validated_model &&
                !node_value->raw_json_.empty()) {
                return validated_json<model_t_type>(
                    *static_cast<const model_t_type*>(node_value->value_), node_value->raw_json_);
            }
        }
        throw std::logic_error("validated JSON request model is not available");
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_get_state() const noexcept {
        return try_find<t_type>(request_binding_kind::request_state);
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_state() const {
        const auto* found = try_find<t_type>(request_binding_kind::request_state);
        if (found == nullptr) {
            throw std::logic_error("request state is not bound for this type");
        }
        return *found;
    }

    template <typename t_type>
    [[nodiscard]] request_binding_handle<t_type> bind_validated(
        const t_type& value, std::string_view raw_json = {}) {
        return request_binding_handle<t_type>(*this, value, request_binding_kind::validated_model, raw_json);
    }

    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    [[nodiscard]] request_binding_handle<std::remove_cvref_t<t_type>> bind_validated(
        t_type&&, std::string_view = {}) = delete;

    template <typename t_type>
    [[nodiscard]] request_binding_handle<t_type> bind_state(const t_type& value) {
        return request_binding_handle<t_type>(*this, value, request_binding_kind::request_state, {});
    }

    // The node stores the value by address, so a temporary would leave the
    // binding dangling the moment the full expression ends.
    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    [[nodiscard]] request_binding_handle<std::remove_cvref_t<t_type>> bind_state(t_type&&) = delete;

private:
    template <typename t_type>
    friend class request_binding_handle;

    void push(request_binding_node& node_value) noexcept {
        node_value.previous_ = head_;
        head_ = &node_value;
    }

    void pop(request_binding_node& node_value) noexcept {
        if (head_ != &node_value) {
            std::terminate();
        }
        head_ = node_value.previous_;
    }

    request_binding_node* head_{nullptr};
};

template <typename t_type>
request_binding_handle<t_type>::request_binding_handle(request_bindings& bindings, const t_type& value,
    request_binding_kind kind, std::string_view raw_json) noexcept
    : bindings_(&bindings),
      node_{request_binding_key<t_type>(), &value, raw_json, nullptr, kind} {
    bindings_->push(node_);
}

template <typename t_type>
request_binding_handle<t_type>::~request_binding_handle() noexcept {
    bindings_->pop(node_);
}

}  // namespace ruvia::detail
