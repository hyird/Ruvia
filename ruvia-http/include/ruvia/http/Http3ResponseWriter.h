#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/HttpResponseStream.h"

namespace ruvia {

enum class Http3ResponseHeadError : std::uint8_t {
    kInvalidField,
    kForbiddenField,
    kUnsupportedStatus,
    kFieldSectionError,
    // A connection-owned encoder refused the peer's decoded-size limit before
    // touching QPACK state; this is not an invalid application response.
    peer_field_section_limit,
};

struct Http3ResponseFieldSection final {
    // Owned by the supplied PMR resource; that resource must outlive this value.
    std::pmr::vector<char> fieldSection;

    // RFC 9114 field-list size from emitted names and values, not QPACK bytes.
    [[nodiscard]] std::size_t decodedFieldSectionSize() const noexcept {
        return decodedFieldSectionSize_;
    }

    Http3ResponseFieldSection(std::pmr::vector<char> bytes, std::size_t decodedSize)
        : fieldSection(std::move(bytes)),
          decodedFieldSectionSize_(decodedSize) {}
    Http3ResponseFieldSection(const Http3ResponseFieldSection&) = delete;
    Http3ResponseFieldSection& operator=(const Http3ResponseFieldSection&) = delete;
    Http3ResponseFieldSection(Http3ResponseFieldSection&& other) noexcept
        : fieldSection(std::move(other.fieldSection)),
          decodedFieldSectionSize_(std::exchange(other.decodedFieldSectionSize_, 0)) {
        other.fieldSection.clear();
    }
    Http3ResponseFieldSection& operator=(Http3ResponseFieldSection&& other) {
        if (this != &other) {
            if (fieldSection.get_allocator() == other.fieldSection.get_allocator()) {
                fieldSection = std::move(other.fieldSection);
            } else {
                std::pmr::vector<char> replacement(fieldSection.get_allocator().resource());
                replacement.assign(other.fieldSection.begin(), other.fieldSection.end());
                fieldSection.swap(replacement);
            }
            decodedFieldSectionSize_ = std::exchange(other.decodedFieldSectionSize_, 0);
            other.fieldSection.clear();
        }
        return *this;
    }

private:
    std::size_t decodedFieldSectionSize_;
};

struct Http3ResponseHead final {
    // The encoded section can be moved independently of the body metadata.
    Http3ResponseFieldSection field_section;
    HttpResponseBodyPlan bodyPlan;
    std::optional<std::uint64_t> declaredContentLength{};

    Http3ResponseHead(std::pmr::vector<char> bytes, HttpResponseBodyPlan plan,
        std::size_t decodedSize, std::optional<std::uint64_t> length = {})
        : field_section(std::move(bytes), decodedSize),
          bodyPlan(plan),
          declaredContentLength(length) {}
    Http3ResponseHead(const Http3ResponseHead&) = delete;
    Http3ResponseHead& operator=(const Http3ResponseHead&) = delete;
    Http3ResponseHead(Http3ResponseHead&&) = default;
    Http3ResponseHead& operator=(Http3ResponseHead&&) = default;
};

struct Http3StreamingResponseHead final {
    Http3ResponseHead head;
    http_response_stream_commit_plan commit_plan;
};

struct Http3ResponseHeadFailure final {
    Http3ResponseHeadError kind;
    Http3FieldSectionError fieldSectionError{Http3FieldSectionError::kInvalidPrefix};
};

// Validates and projects streaming metadata without inventing a buffered length.
[[nodiscard]] std::variant<Http3StreamingResponseHead, Http3ResponseHeadFailure>
encodeHttp3StreamingResponseHead(HttpResponse response, HttpKnownMethod method,
    http_response_stream_kind kind, http_response_trailer_intent trailers,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Projects an interim 1xx head, validating its bodyless message semantics.
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure>
encodeHttp3InterimResponseHead(const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes a response QPACK field section with the permanently empty dynamic
// table. Caller fields are borrowed only for this call. The returned bytes are
// the field section payload (not a complete HTTP/3 HEADERS frame), owned by
// resource, and contain :status followed by ordinary response fields.
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    HttpStatusCode status, HttpKnownMethod requestMethod,
    std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Projects a buffered response into a QPACK field section. The body remains
// external; the returned body plan describes whether and how DATA may be sent.
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    const HttpResponse& response, HttpBufferedResponseWritePlan writePlan,
    Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes an HTTP/3 trailing HEADERS QPACK field section using the permanently
// empty dynamic table. The returned bytes are payload only (no HEADERS frame or
// FIN); the caller owns transmission and must keep resource alive until the
// returned field section is destroyed. Its decoded size is the uncompressed
// field-list size, independent of QPACK representation size.
[[nodiscard]] std::variant<Http3ResponseFieldSection, Http3ResponseHeadFailure>
encodeHttp3ResponseTrailers(std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class Http3QpackEncoder;
[[nodiscard]] std::variant<Http3StreamingResponseHead, Http3ResponseHeadFailure>
encodeHttp3StreamingResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, HttpResponse response, HttpKnownMethod method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure>
encodeHttp3InterimResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
// Dynamic forms share one encoder for every stream in the connection. Returned
// storage belongs to resource; encoder-stream output is drained separately.
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, HttpStatusCode status, HttpKnownMethod method,
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan plan,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeHttp3ResponseTrailers(
    Http3QpackEncoder& encoder, std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
