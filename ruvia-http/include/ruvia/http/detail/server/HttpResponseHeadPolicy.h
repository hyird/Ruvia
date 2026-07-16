#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/HttpResponseHeaderBits.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

class ResponseWritePolicy;

class ResponseNormalWrite final {
private:
    friend class ResponseWritePolicy;
    constexpr ResponseNormalWrite() noexcept = default;
};

class ResponseBodyForbiddenWrite final {
private:
    friend class ResponseWritePolicy;
    constexpr ResponseBodyForbiddenWrite() noexcept = default;
};

class ResponseZeroLengthWrite final {
private:
    friend class ResponseWritePolicy;
    constexpr ResponseZeroLengthWrite() noexcept = default;
};

class ResponseNotModifiedWrite final {
private:
    friend class ResponseWritePolicy;
    constexpr ResponseNotModifiedWrite() noexcept = default;
};

// Status-owned response writing semantics. Exactly one RFC state is active;
// body/framing capabilities are derived observations rather than four stored
// booleans that could describe contradictory products.
class ResponseWritePolicy final {
public:
    [[nodiscard]] constexpr const ResponseNormalWrite*
    normal() const & noexcept {
        return std::get_if<ResponseNormalWrite>(&state_);
    }
    const ResponseNormalWrite* normal() const && = delete;

    [[nodiscard]] constexpr const ResponseBodyForbiddenWrite*
    bodyForbidden() const & noexcept {
        return std::get_if<ResponseBodyForbiddenWrite>(&state_);
    }
    const ResponseBodyForbiddenWrite* bodyForbidden() const && = delete;

    [[nodiscard]] constexpr const ResponseZeroLengthWrite*
    zeroLength() const & noexcept {
        return std::get_if<ResponseZeroLengthWrite>(&state_);
    }
    const ResponseZeroLengthWrite* zeroLength() const && = delete;

    [[nodiscard]] constexpr const ResponseNotModifiedWrite*
    notModified() const & noexcept {
        return std::get_if<ResponseNotModifiedWrite>(&state_);
    }
    const ResponseNotModifiedWrite* notModified() const && = delete;

    [[nodiscard]] constexpr bool bodyAllowed() const noexcept {
        return normal() != nullptr;
    }

    [[nodiscard]] constexpr bool autoContentLengthAllowed() const noexcept {
        return normal() != nullptr || zeroLength() != nullptr;
    }

    [[nodiscard]] constexpr bool explicitContentLengthAllowed() const noexcept {
        return normal() != nullptr || notModified() != nullptr;
    }

    [[nodiscard]] constexpr bool transferEncodingAllowed() const noexcept {
        return normal() != nullptr;
    }

private:
    friend ResponseWritePolicy responseWritePolicy(HttpStatusCode) noexcept;

    using State = std::variant<
        ResponseNormalWrite,
        ResponseBodyForbiddenWrite,
        ResponseZeroLengthWrite,
        ResponseNotModifiedWrite>;

    template <typename Alternative>
    explicit constexpr ResponseWritePolicy(Alternative alternative) noexcept
        : state_(alternative) {}

    [[nodiscard]] static constexpr ResponseWritePolicy makeNormal() noexcept {
        return ResponseWritePolicy(ResponseNormalWrite());
    }

    [[nodiscard]] static constexpr ResponseWritePolicy
    makeBodyForbidden() noexcept {
        return ResponseWritePolicy(ResponseBodyForbiddenWrite());
    }

    [[nodiscard]] static constexpr ResponseWritePolicy
    makeZeroLength() noexcept {
        return ResponseWritePolicy(ResponseZeroLengthWrite());
    }

    [[nodiscard]] static constexpr ResponseWritePolicy
    makeNotModified() noexcept {
        return ResponseWritePolicy(ResponseNotModifiedWrite());
    }

    State state_;
};

static_assert(std::is_trivially_copyable_v<ResponseWritePolicy>);
static_assert(sizeof(ResponseWritePolicy) <= 2);

[[nodiscard]] inline ResponseWritePolicy responseWritePolicy(
    HttpStatusCode statusCode) noexcept {
    if (statusCode.isInformational()) {
        return ResponseWritePolicy::makeBodyForbidden();
    }
    if (statusCode == http_status::kNoContent) {
        return ResponseWritePolicy::makeBodyForbidden();
    }
    if (statusCode == http_status::kResetContent) {
        // RFC 9110 15.3.6 forbids content in a 205 response. Unlike 1xx/204/304,
        // HTTP/1.1 message framing does not make 205 self-delimiting from the
        // status alone, so the writer owns a canonical Content-Length: 0. A
        // caller-provided length and Transfer-Encoding are filtered instead of
        // creating a second, potentially contradictory framing declaration.
        return ResponseWritePolicy::makeZeroLength();
    }
    if (statusCode == http_status::kNotModified) {
        return ResponseWritePolicy::makeNotModified();
    }
    return ResponseWritePolicy::makeNormal();
}

}  // namespace ruvia::detail
