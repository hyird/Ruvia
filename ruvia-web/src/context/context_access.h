#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/http/context/request_bindings.h"

#include "context/context_services.h"
#include "context/context_session_state.h"
#include "context/http_interim_response_output.h"
#include "http/request_query_values.h"
#include "http/static_file_variant.h"
#include "server/inbound_buffer_budget.h"

namespace ruvia::detail {

// Draft and selected response have disjoint ownership. Building a response
// consumes the draft; selecting or replacing a response never merges metadata.
class context_response_state final {
public:
    context_response_state() noexcept = default;

    [[nodiscard]] bool has_draft() const noexcept {
        return draft_.has_value();
    }
    [[nodiscard]] http_status_code draft_status() const noexcept {
        return draft_ ? draft_->status() : http_status::ok;
    }
    void reset_draft() noexcept {
        draft_.reset();
    }
    [[nodiscard]] http_response take_draft_or_new() noexcept {
        if (!draft_) {
            return http_response{};
        }
        auto response = std::move(*draft_);
        draft_.reset();
        return response;
    }
    [[nodiscard]] http_response& draft() {
        if (!draft_) {
            draft_.emplace();
        }
        return *draft_;
    }
    [[nodiscard]] http_response* final() noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] const http_response* final() const noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] http_response& policy() {
        if (!policy_) {
            policy_.emplace();
        }
        return *policy_;
    }
    [[nodiscard]] http_response& defaults() {
        if (!defaults_) {
            defaults_.emplace();
        }
        return *defaults_;
    }
    void policy_status(http_status_code status) {
        policy_status_ = status;
    }
    void apply_policy(http_response& response) const {
        if (defaults_ && !defaults_->headers().empty()) {
            response.transfer_headers_from(*defaults_, http_response_header_transfer::merge);
        }
        if (policy_ && !policy_->headers().empty()) {
            response.transfer_headers_from(*policy_, http_response_header_transfer::apply);
        }
        if (policy_status_) {
            response.status(*policy_status_);
        }
    }

    void finalize(http_response&& response) {
        final_.emplace(std::move(response));
    }

    [[nodiscard]] http_response&& release_final() & {
        assert(final_);
        apply_policy(*final_);
        return std::move(*final_);
    }
    http_response&& release_final() && = delete;

private:
    std::optional<http_response> draft_;
    std::optional<http_response> policy_;
    std::optional<http_response> defaults_;
    std::optional<http_status_code> policy_status_;
    std::optional<http_response> final_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct aligned_storage_deleter final {
    std::size_t alignment_{alignof(std::max_align_t)};
    void operator()(std::byte* storage) const noexcept {
        ::operator delete[](storage, std::align_val_t{alignment_});
    }
};

// Owns every request-local context value that is not a borrowed pointer: lazy
// caches, response/session machines, typed bindings, and middleware next() hop
// storage. Synchronous construction borrows the input view, then retains one
// services value here before returning; no reference to the input survives.
class context_request_storage final {
public:
    explicit context_request_storage(const context_services& service_view) noexcept
        : services_(service_view),
          decoded_body_reservation_(services_.inbound_buffers()) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    [[nodiscard]] std::byte* allocate_raw(std::size_t size, std::size_t alignment) {
        auto* storage = static_cast<std::byte*>(::operator new[](size, std::align_val_t{alignment}));
        try {
            owned_blocks_.emplace_back(storage, aligned_storage_deleter{alignment});
        } catch (...) {
            ::operator delete[](storage, std::align_val_t{alignment});
            throw;
        }
        return storage;
    }

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::string> decoded_body_;
    inbound_buffer_reservation decoded_body_reservation_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::string decoded_route_params_;
    // Views borrow route storage, the request path, or decoded_route_params_.
    std::optional<request_name_value_list> route_params_;
    std::vector<std::unique_ptr<std::byte[], aligned_storage_deleter>> owned_blocks_;
    bool route_params_invalid_{false};
    bool route_params_unencoded_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    // An interrupted output operation cannot withdraw what it wrote, so it
    // resets the request: the HTTP/2 or HTTP/3 stream, or the HTTP/1 connection.
    static void reset_interrupted_request(void* raw_context) noexcept {
        auto& request_services = static_cast<context*>(raw_context)->services();
        try {
            request_services.reset_request({});
        } catch (...) {
            request_services.abort_connection();
        }
    }

    // A request-stream router borrows the already-bound transport context.
    // Restore every borrowed route view before its resolution frame retires;
    // retain the output state, since a stream may have committed in that scope.
    class route_binding final {
    public:
        route_binding(context& value, std::string_view path, std::span<const std::string_view> names,
            std::span<const std::string_view> values, std::uintptr_t scope)
            : context_(value),
              services_(value.services()),
              path_(value.route_path_),
              names_(value.param_names_),
              values_(value.param_values_),
              count_(value.param_count_),
              scope_(value.route_rate_limit_scope_),
              worker_states_(value.capabilities_.worker_states()) {
            value.route_path_ = path;
            value.param_names_ = names.data();
            value.param_values_ = values.data();
            value.param_count_ = values.size();
            value.route_rate_limit_scope_ = scope;
        }
        route_binding(const route_binding&) = delete;
        route_binding& operator=(const route_binding&) = delete;
        ~route_binding() {
            const auto output = context_.services().response_output();
            context_.services() = services_;
            context_.services().response_output() = output;
            context_.route_path_ = path_;
            context_.param_names_ = names_;
            context_.param_values_ = values_;
            context_.param_count_ = count_;
            context_.route_rate_limit_scope_ = scope_;
            context_.capabilities_.bind_worker_states(worker_states_);
        }

        // Points the context's worker-state lookups at the registry its
        // services now name; the destructor restores the previous one.
        void bind_worker_states() noexcept {
            context_.capabilities_.bind_worker_states(context_.services().worker_states());
        }

    private:
        context& context_;
        context_services services_;
        std::string_view path_;
        const std::string_view* names_;
        const std::string_view* values_;
        std::size_t count_;
        std::uintptr_t scope_;
        const worker_state_registry* worker_states_;
    };

    [[nodiscard]] static context make(
        const http_request& request, const context_services& services) {
        return context(request, services);
    }

    [[nodiscard]] static context make(const http_request& request,
        std::string_view route_path, const std::string_view* param_names,
        const std::string_view* param_values, std::size_t param_count,
        std::uintptr_t route_rate_limit_scope, const context_services& services) {
        return context(request, route_path, param_names, param_values, param_count,
            route_rate_limit_scope, services);
    }

    [[nodiscard]] static std::byte* allocate_request_storage(
        context& context_value, std::size_t size, std::size_t alignment) {
        return context_value.request_storage().allocate_raw(size, alignment);
    }

    // The response for an indexed entry already resolved below `root`. The
    // representation is selected and the content type copied before the task
    // is returned, so `relative` and `content_type_value` are read only by
    // this call; `root` and `base_entry` (a view into it) must outlive the
    // task, which opens a file-backed entry on the blocking pool.
    [[nodiscard]] static task<http_response> static_file_resolved(
        context& context_value, const static_root& root, std::string_view relative,
        std::string_view content_type_value, static_root_entry_view base_entry,
        static_file_selection_mode mode);

    [[nodiscard]] static http_response make_static_json_response(
        context& context_value, std::string_view json_body) {
        auto response = context_value.response_state().take_draft_or_new();
        if (!response.header("Content-Type")) {
            response.content_type(http_content_type::application_json);
        }
        response.static_body(json_body);
        return response;
    }

    [[nodiscard]] static http_response make_owned_json_response(
        context& context_value, std::string& json_body) {
        return context_value.json_serialized(json_body);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static const context_request_body_source& request_body_source(
        const context& context_value) noexcept {
        return context_value.request_body_source();
    }

    [[nodiscard]] static context_services& services(context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static const context_services& services(const context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static std::optional<std::string_view> decoded_body(
        const context& context_value) noexcept {
        if (!context_value.body_decoded_) {
            return std::nullopt;
        }
        return std::string_view(*context_value.request_storage_->decoded_body_);
    }

    [[nodiscard]] static bool body_identity(const context& context_value) noexcept {
        return context_value.body_identity_;
    }

    static void mark_body_identity(const context& context_value) noexcept {
        context_value.body_identity_ = true;
    }

    [[nodiscard]] static std::string_view store_decoded_body(
        const context& context_value, std::string&& decoded_content) {
        auto& storage = context_value.request_storage();
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded_content));
        auto& decoded = context_value.decoded_body();
        decoded = std::move(decoded_content);
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded));
        context_value.body_decoded_ = true;
        return std::string_view(decoded);
    }

    [[nodiscard]] static detail::rate_limiter* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static const std::optional<std::chrono::steady_clock::time_point>& rate_limit_arrival(
        context& context_value) noexcept {
        return context_value.services().pinned_arrival();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static const http_response_headers& handshake_headers(context& context_value) {
        auto& state = context_value.response_state();
        state.apply_policy(state.draft());
        return state.draft().headers();
    }
    [[nodiscard]] static http_response& response_policy(context& context_value) {
        return context_value.response_state().policy();
    }
    static void discard_draft(context& context_value) {
        context_value.response_state().reset_draft();
    }
    static void apply_response_policy(context& context_value, http_response& response) {
        context_value.response_state().apply_policy(response);
    }
    static void security_header(context& context_value, std::string_view name, std::string_view value, bool replace) {
        auto& state = context_value.response_state();
        auto& response = replace ? state.policy() : state.defaults();
        response.header_stable_view(name, value);
    }

    // The status the handler set; a file response sends it for the full body.
    [[nodiscard]] static http_status_code response_status(const context& context_value) noexcept {
        return context_value.response_state().draft_status();
    }

    static void apply_response_state(context& context_value, http_response& response,
        std::optional<http_status_code> status_code) {
        context_value.apply_response_state(response, status_code);
    }

    // Called by a driver right before it hands the upgrade/CONNECT handshake
    // head to the transport: from here on no HTTP response can follow, and no
    // 1xx head either -- the stream now carries the upgraded protocol. Throws,
    // before anything is written, while an interim head is still being written.
    static void commit_upgrade(context& context_value) {
        if (auto* interim = context_value.services().interim_output()) {
            interim->commit_final();
        }
        context_value.response_output() = context_response_output::upgraded();
    }

    [[nodiscard]] static bool output_committed(const context& context_value) noexcept {
        return context_value.response_output().committed();
    }

    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    // The final response, for the dispatcher to return; see
    // context_response_state::release_final.
    [[nodiscard]] static http_response&& release_response(context& context_value) {
        return context_value.response_state().release_final();
    }

    [[nodiscard]] static http_response streaming_head(context& context_value) {
        return context_value.streaming_head();
    }

    // Commits the bound session, then builds the head a response stream sends before its first byte.
    [[nodiscard]] static task<http_response> commit_streaming_head(context& context_value);

private:
    friend class context_websocket_binding;
    friend class context_tunnel_binding;
    [[nodiscard]] static context_response_output bind_tunnel(context& context_value, http_tunnel& tunnel) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static context_response_output bind_websocket(
        context& context_value, websocket& websocket_value) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::websocket_value(websocket_value);
        return previous;
    }

    static void restore_response_output(context& context_value, context_response_output output) noexcept {
        context_value.response_output() = output;
    }
};

class context_tunnel_binding final {
public:
    context_tunnel_binding(context& context_value, http_tunnel& tunnel) noexcept
        : context_(context_value),
          previous_(context_access::bind_tunnel(context_value, tunnel)) {}
    context_tunnel_binding(const context_tunnel_binding&) = delete;
    context_tunnel_binding& operator=(const context_tunnel_binding&) = delete;
    ~context_tunnel_binding() {
        context_access::restore_response_output(context_, previous_);
    }

private:
    context& context_;
    context_response_output previous_;
};

// The facade borrowed by context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class context_websocket_binding final {
public:
    context_websocket_binding(context& context_value, websocket& websocket_value) noexcept
        : context_(&context_value),
          previous_(context_access::bind_websocket(context_value, websocket_value)) {}

    context_websocket_binding(const context_websocket_binding&) = delete;
    context_websocket_binding& operator=(const context_websocket_binding&) = delete;
    context_websocket_binding(context_websocket_binding&&) = delete;
    context_websocket_binding& operator=(context_websocket_binding&&) = delete;

    ~context_websocket_binding() {
        context_access::restore_response_output(*context_, previous_);
    }

private:
    context* context_;
    context_response_output previous_;
};

}  // namespace ruvia::detail
