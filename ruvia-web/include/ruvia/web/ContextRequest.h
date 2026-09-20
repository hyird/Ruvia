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

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/Attributes.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/RequestFields.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"

namespace ruvia {

class Context;
class ContextRequest;
template <typename T>
class ValidatedJson;

namespace detail {
template <typename T>
[[nodiscard]] RequestBindingHandle<T> bindValidatedModel(Context& context, const T& model);
template <typename T>
[[nodiscard]] RequestBindingHandle<T> bindValidatedJsonModel(
    Context& context, const T& model, std::string_view rawJson);
template <typename T>
    requires(!std::is_lvalue_reference_v<T>)
RequestBindingHandle<std::remove_cvref_t<T>> bindValidatedModel(Context&, T&&) = delete;

[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();
[[noreturn]] void throwInvalidQuery();
[[noreturn]] void throwInvalidParam();
[[noreturn]] void throwInvalidHeader();
[[noreturn]] void throwInvalidCookie();
}  // namespace detail

struct SignedCookieLookupOptions final {
    BorrowedText name{};
    BorrowedText secret{};
};

// Borrow-only request facade. Returned fields and body views belong to Context,
// not this by-value facade. Model validation belongs to route middleware.
class ContextRequest final {
public:
    class RequestBlob final {
    public:
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] std::string_view text() const noexcept {
            return bytes_.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
        }
        [[nodiscard]] std::string_view contentType() const noexcept {
            return contentType_;
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_.size();
        }
        [[nodiscard]] bool empty() const noexcept {
            return bytes_.empty();
        }

    private:
        friend class ContextRequest;
        constexpr RequestBlob(std::span<const std::byte> bytes, std::string_view contentType) noexcept
            : bytes_(bytes),
              contentType_(contentType) {}
        std::span<const std::byte> bytes_;
        std::string_view contentType_;
    };

    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view routePath() const noexcept;

    // Accept uses media ranges; language uses RFC 4647 basic filtering;
    // encoding and charset match tokens or '*'.
    enum class Negotiable : std::uint8_t { kMediaType,
        kLanguage,
        kEncoding,
        kCharset };
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;
    // Client weights select a caller-owned supported value; server order breaks
    // ties. An absent field selects the first offer, no acceptable offer is nullopt.
    [[nodiscard]] std::optional<std::string_view> negotiate(
        Negotiable field, std::span<const std::string_view> supported) const noexcept;
    [[nodiscard]] std::optional<std::string_view> negotiate(
        Negotiable field, std::initializer_list<std::string_view> supported) const noexcept {
        return negotiate(field, std::span<const std::string_view>(supported.begin(), supported.size()));
    }
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

    // Materialized once in request order, preserving duplicates. Header names
    // retain their semantic spelling (HTTP/2 is lowercase); header get/count
    // and HeaderModel binding compare names ASCII case-insensitively.
    [[nodiscard]] const RequestNameValueList& headerFields() const;
    [[nodiscard]] const RequestNameValueList& queryFields() const;
    [[nodiscard]] const RequestNameValueList& cookieFields() const;
    [[nodiscard]] const RequestNameValueList& paramFields() const;
    [[nodiscard]] std::optional<std::string_view> signedCookie(SignedCookieLookupOptions options) const;

    [[nodiscard]] ScopedOperation<std::string_view> text() const;
    [[nodiscard]] ScopedOperation<std::span<const std::byte>> bytes() const;
    [[nodiscard]] ScopedOperation<RequestBlob> blob() const;
    ScopedOperation<void> discardBody() const;

    // Typed optional parsing without field-rule validation. Only a media-type
    // mismatch yields nullopt; selected but malformed input throws HTTP 400.
    // Use JsonBody<T>/FormBody<T> with validated<T>() to run schema rules.
    template <typename T>
    [[nodiscard]] ScopedOperation<std::optional<T>> jsonIf() const;
    template <typename T>
    [[nodiscard]] ScopedOperation<std::optional<T>> formIf() const;
    template <typename T>
    [[nodiscard]] const T& validated() const;
    template <typename T>
    [[nodiscard]] ValidatedJson<T> validatedJson() const;

    // Multipart remains a flat protocol representation, without dotted paths
    // or implicit array/group interpretation. Streaming is explicit-route only.
    [[nodiscard]] ScopedOperation<std::pmr::vector<MultipartPart>> multipart() const;
    [[nodiscard]] BodyReader& bodyReader() const;
    [[nodiscard]] MultipartReader multipartReader() const;

private:
    friend class Context;
    explicit constexpr ContextRequest(const Context& context RUVIA_LIFETIMEBOUND) noexcept
        : context_(&context) {}

    [[nodiscard]] bool contentTypeMatches(std::string_view expected) const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] const detail::RequestBindings& requestBindings() const noexcept;
    [[nodiscard]] static Task<std::span<const std::byte>> bytesTask(const Context* context);
    [[nodiscard]] static Task<RequestBlob> blobTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<std::optional<T>> jsonIfModelTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<std::optional<T>> formIfModelTask(const Context* context);
    [[nodiscard]] static Task<std::string_view> contextTextTask(const Context* context);
    [[nodiscard]] static bool contextContentTypeMatches(const Context* context, std::string_view expected) noexcept;
    [[nodiscard]] static std::pmr::memory_resource* contextResource(const Context* context) noexcept;
    [[nodiscard]] static detail::ScopedOperationScope& contextOperationScope(const Context* context) noexcept;
    const Context* context_{nullptr};
};

}  // namespace ruvia

#include "ruvia/web/detail/http/context/ContextRequestModel.inl"
