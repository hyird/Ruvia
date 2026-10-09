#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/attributes.h"
#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/model_types.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/request_fields.h"
#include "ruvia/web/streaming.h"

namespace ruvia {

class context;
class context_request;
template <typename t_type>
class validated_json;

namespace detail {
template <typename t_type>
[[nodiscard]] request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model);
template <typename t_type>
[[nodiscard]] request_binding_handle<t_type> bind_validated_json_model(
    context& context_value, const t_type& model, std::string_view raw_json);
template <typename t_type>
    requires(!std::is_lvalue_reference_v<t_type>)
request_binding_handle<std::remove_cvref_t<t_type>> bind_validated_model(context&, t_type&&) = delete;

[[noreturn]] void throw_invalid_json_content_type();
[[noreturn]] void throw_invalid_json_body();
[[noreturn]] void throw_invalid_form_content_type();
[[noreturn]] void throw_invalid_form_body();
[[noreturn]] void throw_invalid_query();
[[noreturn]] void throw_invalid_param();
[[noreturn]] void throw_invalid_header();
[[noreturn]] void throw_invalid_cookie();
}  // namespace detail

struct signed_cookie_lookup_options final {
    borrowed_text name_{};
    borrowed_text secret_{};
};

// Borrow-only request facade. Returned fields and body views belong to context,
// not this by-value facade. Model validation belongs to route middleware.
class context_request final {
public:
    class request_blob_type final {
    public:
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] std::string_view text() const noexcept {
            return bytes_.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
        }
        [[nodiscard]] std::string_view content_type() const noexcept {
            return content_type_;
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_.size();
        }
        [[nodiscard]] bool empty() const noexcept {
            return bytes_.empty();
        }

    private:
        friend class context_request;
        constexpr request_blob_type(std::span<const std::byte> bytes_value, std::string_view content_type_value) noexcept
            : bytes_(bytes_value),
              content_type_(content_type_value) {}
        std::span<const std::byte> bytes_;
        std::string_view content_type_;
    };

    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] http_known_method known_method() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view scheme() const noexcept;
    [[nodiscard]] std::string_view authority() const noexcept;
    [[nodiscard]] std::string_view target() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] http_request_target_form target_form() const noexcept;
    [[nodiscard]] std::string_view route_path() const noexcept;
    // Initial Priority fields, replaced by the latest RFC 9218 update when a
    // live HTTP/2 or HTTP/3 peer reprioritizes this request.
    [[nodiscard]] http_priority priority() const noexcept;

    // Accept uses media ranges; language uses RFC 4647 basic filtering;
    // charset matches tokens or '*'; encoding also handles coding aliases and
    // the default acceptability of identity.
    enum class negotiable_type : std::uint8_t { media_type,
        language,
        encoding,
        charset };
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    // Read after body completion to observe the complete terminal section.
    // Trailers never replace initial headers or change route selection.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;
    [[nodiscard]] bool accepts(std::string_view media_type) const noexcept;
    // Client weights select a caller-owned supported value; server order breaks
    // ties. An absent field selects the first offer, no acceptable offer is nullopt.
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_type field, std::span<const std::string_view> supported) const noexcept;
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_type field, std::initializer_list<std::string_view> supported) const noexcept {
        return negotiate(field, std::span<const std::string_view>(supported.begin(), supported.size()));
    }
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

    // Materialized once in request order, preserving duplicates. Header names
    // retain their semantic spelling (HTTP/2 is lowercase); header get/count
    // and header_model binding compare names ASCII case-insensitively.
    [[nodiscard]] const request_name_value_list& header_fields() const;
    [[nodiscard]] const request_name_value_list& query_fields() const;
    [[nodiscard]] const request_name_value_list& cookie_fields() const;
    [[nodiscard]] const request_name_value_list& param_fields() const;
    [[nodiscard]] std::optional<std::string_view> signed_cookie(signed_cookie_lookup_options options) const;

    [[nodiscard]] scoped_operation<std::string_view> text() const;
    [[nodiscard]] scoped_operation<std::span<const std::byte>> bytes() const;
    [[nodiscard]] scoped_operation<request_blob_type> blob() const;
    scoped_operation<void> discard_body() const;

    // Typed optional parsing without field-rule validation. Only a media-type
    // mismatch yields nullopt; selected but malformed input throws HTTP 400.
    // Use json_body<T>/form_body<T> with validated<T>() to run schema rules.
    template <typename t_type>
    [[nodiscard]] scoped_operation<std::optional<t_type>> json_if() const;
    template <typename t_type>
    [[nodiscard]] scoped_operation<std::optional<t_type>> form_if() const;
    template <typename t_type>
    [[nodiscard]] const t_type& validated() const;
    template <typename t_type>
    [[nodiscard]] validated_json<t_type> validated_json() const;

    // Multipart remains a flat protocol representation, without dotted paths
    // or implicit array/group interpretation. Streaming is explicit-route only.
    [[nodiscard]] scoped_operation<std::pmr::vector<multipart_part>> multipart() const;
    [[nodiscard]] body_reader& get_body_reader() const;
    [[nodiscard]] multipart_reader get_multipart_reader() const;

private:
    friend class context;
    explicit constexpr context_request(const context& context_value RUVIA_LIFETIMEBOUND) noexcept
        : context_(&context_value) {}

    [[nodiscard]] bool content_type_matches(std::string_view expected) const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] const detail::request_bindings& request_bindings() const noexcept;
    [[nodiscard]] static task<std::span<const std::byte>> bytes_task(const context* context);
    [[nodiscard]] static task<request_blob_type> blob_task(const context* context);
    template <typename t_type>
    [[nodiscard]] static task<std::optional<t_type>> json_if_model_task(const context* context_value);
    template <typename t_type>
    [[nodiscard]] static task<std::optional<t_type>> form_if_model_task(const context* context_value);
    [[nodiscard]] static task<std::string_view> context_text_task(const context* context);
    [[nodiscard]] static bool context_content_type_matches(const context* context, std::string_view expected) noexcept;
    [[nodiscard]] static std::pmr::memory_resource* context_resource(const context* context) noexcept;
    [[nodiscard]] static ::ruvia::operation_scope& context_operation_scope(const context* context) noexcept;
    const context* context_{nullptr};
};

}  // namespace ruvia

#include "ruvia/web/detail/http/context/context_request_model.inl"
