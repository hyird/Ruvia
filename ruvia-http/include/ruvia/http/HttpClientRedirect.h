#pragma once

// Outbound HTTP redirect protocol helpers.
//
// These values contain no transport policy: redirect limits, cross-origin
// authorization, connection selection, and retries remain owned by the external
// I/O runtime. The helpers only classify HTTP response fields, apply the RFC
// method/content rewrite rules, and resolve one same-origin URI-reference.

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpClient.h"

namespace ruvia {

[[nodiscard]] bool isValidHttpClientOriginTarget(
    std::string_view target) noexcept;

[[nodiscard]] bool isHttpClientRedirectStatus(std::uint16_t status) noexcept;

class HttpClientResponseHeaderLookupResult;

class HttpClientResponseHeaderAbsent final {
private:
    friend class HttpClientResponseHeaderLookupResult;
    constexpr HttpClientResponseHeaderAbsent() noexcept = default;
};

class HttpClientResponseHeaderFound final {
public:
    // Borrowed from the owning HttpClientResponse passed to the lookup.
    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class HttpClientResponseHeaderLookupResult;

    explicit constexpr HttpClientResponseHeaderFound(
        std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

class HttpClientResponseHeaderRepeated final {
private:
    friend class HttpClientResponseHeaderLookupResult;
    constexpr HttpClientResponseHeaderRepeated() noexcept = default;
};

// A non-list response field has three mutually exclusive lookup outcomes. Only
// the found alternative exposes a value, so an absent/repeated field cannot be
// mistaken for a present field whose value is empty.
class HttpClientResponseHeaderLookupResult final {
public:
    [[nodiscard]] constexpr const HttpClientResponseHeaderAbsent*
    absent() const & noexcept {
        return std::get_if<HttpClientResponseHeaderAbsent>(&value_);
    }
    const HttpClientResponseHeaderAbsent* absent() const && = delete;

    [[nodiscard]] constexpr const HttpClientResponseHeaderFound*
    found() const & noexcept {
        return std::get_if<HttpClientResponseHeaderFound>(&value_);
    }
    const HttpClientResponseHeaderFound* found() const && = delete;

    [[nodiscard]] constexpr const HttpClientResponseHeaderRepeated*
    repeated() const & noexcept {
        return std::get_if<HttpClientResponseHeaderRepeated>(&value_);
    }
    const HttpClientResponseHeaderRepeated* repeated() const && = delete;

private:
    friend HttpClientResponseHeaderLookupResult
    lookupUniqueHttpClientResponseHeader(
        const HttpClientResponse&,
        std::string_view) noexcept;

    using Value = std::variant<
        HttpClientResponseHeaderAbsent,
        HttpClientResponseHeaderFound,
        HttpClientResponseHeaderRepeated>;

    template <typename Result>
    explicit constexpr HttpClientResponseHeaderLookupResult(
        Result result) noexcept
        : value_(result) {}

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult
    makeAbsent() noexcept {
        return HttpClientResponseHeaderLookupResult(
            HttpClientResponseHeaderAbsent());
    }

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult
    makeFound(std::string_view value) noexcept {
        return HttpClientResponseHeaderLookupResult(
            HttpClientResponseHeaderFound(value));
    }

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult
    makeRepeated() noexcept {
        return HttpClientResponseHeaderLookupResult(
            HttpClientResponseHeaderRepeated());
    }

    Value value_;
};

[[nodiscard]] HttpClientResponseHeaderLookupResult
lookupUniqueHttpClientResponseHeader(
    const HttpClientResponse& response,
    std::string_view name) noexcept;
[[nodiscard]] HttpClientResponseHeaderLookupResult
lookupUniqueHttpClientResponseHeader(
    const HttpClientResponse&& response,
    std::string_view name) = delete;

enum class HttpClientRedirectContentDisposition : std::uint8_t {
    kPreserve,
    // The I/O owner must omit both the representation and content-specific
    // fields when constructing the redirected request (RFC 9110 Section 15.4).
    kDrop,
};

class HttpClientRedirectRequestPlan final {
public:
    [[nodiscard]] constexpr std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] constexpr HttpClientRedirectContentDisposition
    contentDisposition() const noexcept {
        return contentDisposition_;
    }

private:
    friend HttpClientRedirectRequestPlan planHttpClientRedirectRequest(
        const HttpClientRequest&,
        std::uint16_t) noexcept;

    constexpr HttpClientRedirectRequestPlan(
        std::string_view method,
        HttpClientRedirectContentDisposition contentDisposition) noexcept
        : method_(method), contentDisposition_(contentDisposition) {}

    std::string_view method_;
    HttpClientRedirectContentDisposition contentDisposition_;
};

[[nodiscard]] HttpClientRedirectRequestPlan planHttpClientRedirectRequest(
    const HttpClientRequest& request,
    std::uint16_t status) noexcept;

// This classification has no alternative-specific payload, so an enum is the
// complete result rather than a status coupled to unrelated fields.
enum class HttpClientOriginAuthorityStatus : std::uint8_t {
    kSameOrigin,
    kDifferentOrigin,
    kInvalidAuthority,
};

[[nodiscard]] HttpClientOriginAuthorityStatus
classifyHttpClientOriginAuthority(
    const HttpOrigin& origin,
    std::string_view authority) noexcept;

enum class HttpClientRedirectTargetError : std::uint8_t {
    kInvalidCurrentTarget,
    kInvalidLocation,
    kNotSameOrigin,
};

class HttpClientRedirectTargetResult;

class HttpClientRedirectTarget final {
public:
    HttpClientRedirectTarget(const HttpClientRedirectTarget&) = delete;
    HttpClientRedirectTarget& operator=(const HttpClientRedirectTarget&) = delete;
    HttpClientRedirectTarget(HttpClientRedirectTarget&&) noexcept = default;
    HttpClientRedirectTarget& operator=(HttpClientRedirectTarget&&) = delete;

    [[nodiscard]] std::string_view value() const & noexcept {
        return std::string_view(value_.data(), value_.size());
    }
    [[nodiscard]] std::string_view value() const && = delete;

private:
    friend class HttpClientRedirectTargetResult;

    explicit HttpClientRedirectTarget(std::pmr::string value) noexcept
        : value_(std::move(value)) {}

    std::pmr::string value_;
};

class HttpClientRedirectTargetFailure final {
public:
    [[nodiscard]] constexpr HttpClientRedirectTargetError error() const noexcept {
        return error_;
    }

private:
    friend class HttpClientRedirectTargetResult;

    explicit constexpr HttpClientRedirectTargetFailure(
        HttpClientRedirectTargetError error) noexcept
        : error_(error) {}

    HttpClientRedirectTargetError error_;
};

// Resolution either owns one PMR target or reports one typed failure. There is
// no output parameter whose previous contents can accidentally survive a failed
// resolution.
class HttpClientRedirectTargetResult final {
public:
    HttpClientRedirectTargetResult(const HttpClientRedirectTargetResult&) = delete;
    HttpClientRedirectTargetResult& operator=(
        const HttpClientRedirectTargetResult&) = delete;
    HttpClientRedirectTargetResult(HttpClientRedirectTargetResult&&) noexcept = default;
    HttpClientRedirectTargetResult& operator=(
        HttpClientRedirectTargetResult&&) = delete;

    [[nodiscard]] const HttpClientRedirectTarget* target() const & noexcept {
        return std::get_if<HttpClientRedirectTarget>(&value_);
    }
    const HttpClientRedirectTarget* target() const && = delete;

    [[nodiscard]] constexpr const HttpClientRedirectTargetFailure*
    failure() const & noexcept {
        return std::get_if<HttpClientRedirectTargetFailure>(&value_);
    }
    const HttpClientRedirectTargetFailure* failure() const && = delete;

private:
    friend HttpClientRedirectTargetResult
    resolveHttpClientSameOriginRedirectTarget(
        const HttpOrigin&,
        std::string_view,
        std::string_view,
        std::pmr::memory_resource*);

    using Value = std::variant<
        HttpClientRedirectTarget,
        HttpClientRedirectTargetFailure>;

    explicit HttpClientRedirectTargetResult(
        HttpClientRedirectTarget target) noexcept
        : value_(std::move(target)) {}

    explicit constexpr HttpClientRedirectTargetResult(
        HttpClientRedirectTargetFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static HttpClientRedirectTargetResult makeTarget(
        std::pmr::string target) noexcept {
        return HttpClientRedirectTargetResult(
            HttpClientRedirectTarget(std::move(target)));
    }

    [[nodiscard]] static constexpr HttpClientRedirectTargetResult makeFailure(
        HttpClientRedirectTargetError error) noexcept {
        return HttpClientRedirectTargetResult(
            HttpClientRedirectTargetFailure(error));
    }

    Value value_;
};

[[nodiscard]] HttpClientRedirectTargetResult
resolveHttpClientSameOriginRedirectTarget(
    const HttpOrigin& origin,
    std::string_view currentTarget,
    std::string_view location,
    std::pmr::memory_resource* resource = nullptr);

}  // namespace ruvia
