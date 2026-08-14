#include "ruvia/web/detail/http/request/RequestFormBodyParse.h"

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/parser/MultipartDelimiter.h"
#include "ruvia/web/detail/http/request/RequestFormAccess.h"
#include "ruvia/web/detail/http/request/RequestFieldParsing.h"
#include "ruvia/web/detail/http/request/RequestFieldsAccess.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool fieldNameIsArray(std::string_view name) noexcept {
    return name.ends_with("[]");
}

[[nodiscard]] bool fieldNameHasProtoObject(std::string_view name) noexcept {
    std::size_t offset = 0;
    for (;;) {
        const auto dot = name.find('.', offset);
        const auto segment = dot == std::string_view::npos ? name.substr(offset) : name.substr(offset, dot - offset);
        if (segment == "__proto__") {
            return true;
        }
        if (dot == std::string_view::npos) {
            return false;
        }
        offset = dot + 1;
    }
}

void assignDotPath(ContextRequest::RequestFormField& field, std::pmr::memory_resource* resource) {
    auto& path = detail::RequestFormFieldAccess::path(field);
    path.clear();
    const auto name = field.name();
    if (name.empty()) {
        return;
    }

    std::size_t offset = 0;
    for (;;) {
        const auto dot = name.find('.', offset);
        const auto segment = dot == std::string_view::npos ? name.substr(offset) : name.substr(offset, dot - offset);
        path.emplace_back(std::pmr::string(segment.data(), segment.size(), resource));
        if (dot == std::string_view::npos) {
            break;
        }
        offset = dot + 1;
    }
}

[[nodiscard]] std::pmr::vector<std::size_t> sortedFormFieldOrder(const std::pmr::vector<ContextRequest::RequestFormField>& fields, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        order.push_back(i);
    }
    std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
        const auto leftName = fields[left].name();
        const auto rightName = fields[right].name();
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });
    return order;
}

void appendParsedBodyField(std::pmr::vector<ContextRequest::RequestFormField>& fields, ContextRequest::RequestFormField&& field, ContextRequest::ParseBodyOptions options) {
    if (options.dottedNames == ContextRequest::DottedNamePolicy::kExpandPath) {
        if (fieldNameHasProtoObject(field.name())) {
            return;
        }
        assignDotPath(field, fields.get_allocator().resource());
    }

    // Reject before the field vector (and the sorts over it) can grow without
    // bound from an attacker-supplied body of many tiny fields.
    if (fields.size() >= options.maxFields) {
        detail::throwTooManyFormFields();
    }
    fields.emplace_back(std::move(field));
}

void compactParsedBodyFields(std::pmr::vector<ContextRequest::RequestFormField>& fields, ContextRequest::ParseBodyOptions options) {
    if (options.repeatedScalars == ContextRequest::RepeatedScalarPolicy::kRetainAll || fields.size() < 2) {
        return;
    }

    auto* const resource = fields.get_allocator().resource();
    const auto order = sortedFormFieldOrder(fields, resource);
    std::pmr::vector<unsigned char> keep(resource);
    keep.resize(fields.size(), 0);

    for (std::size_t offset = 0; offset < order.size();) {
        const auto name = fields[order[offset]].name();
        std::optional<std::size_t> lastScalar;
        do {
            const auto index = order[offset];
            // Retain every array ("name[]") field, and every file part: a
            // standard <input type=file multiple> emits several parts under one
            // non-"[]" name, and collapsing them as repeated scalars would
            // silently drop all but the last upload. Only true repeated scalars
            // (text fields) collapse to their last value.
            if (fields[index].isArray() || fields[index].isFile()) {
                keep[index] = 1;
            } else {
                lastScalar = index;
            }
            ++offset;
        } while (offset < order.size() && fields[order[offset]].name() == name);
        if (lastScalar.has_value()) {
            keep[*lastScalar] = 1;
        }
    }

    std::size_t write = 0;
    for (std::size_t read = 0; read < fields.size(); ++read) {
        if (keep[read] == 0) {
            continue;
        }
        if (write != read) {
            std::destroy_at(&fields[write]);
            std::construct_at(&fields[write], std::move(fields[read]));
        }
        ++write;
    }
    while (fields.size() > write) {
        fields.pop_back();
    }
}

[[nodiscard]] ContextRequest::RequestFormData parseUrlEncodedFormBody(std::string_view requestBody, std::pmr::memory_resource* resource, ContextRequest::ParseBodyOptions options) {
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(boundedFieldReserve(delimitedFieldCount(requestBody, '&')));
    bool valid = true;
    const bool ok = detail::visitUrlEncodedPairs(requestBody, [resource, &fields, &valid, options](std::string_view key, std::string_view value) {
        auto decodedName = detail::decodeUrlComponent(key, detail::UrlDecodeMode::kForm, resource);
        auto decodedValue = detail::decodeUrlComponent(value, detail::UrlDecodeMode::kForm, resource);
        if (!decodedName || !decodedValue) {
            valid = false;
            return false;
        }

        const bool array = fieldNameIsArray(std::string_view(decodedName->data(), decodedName->size()));
        appendParsedBodyField(fields, detail::RequestFormFieldAccess::make(resource, std::move(*decodedName), std::move(*decodedValue), std::pmr::string(resource), std::pmr::string(resource), false, array), options);
        return true;
    });
    if (!ok || !valid) {
        throwInvalidFormBody();
    }
    compactParsedBodyFields(fields, options);
    return detail::RequestFormDataAccess::fromFields(std::move(fields));
}

void countMultipartPart(std::size_t& parts, std::size_t maxFields) {
    ++parts;
    if (parts > maxFields) {
        detail::throwTooManyFormFields();
    }
}

void enforceMultipartFieldCap(std::string_view requestBody, const MultipartBoundary& boundary, std::size_t maxFields) {
    const auto initial = detail::httpFindInitialMultipartDelimiter(requestBody, boundary, true);
    const auto* firstPart = initial.part();
    if (firstPart == nullptr) {
        return;
    }

    std::size_t parts = 0;
    countMultipartPart(parts, maxFields);
    std::size_t cursor = firstPart->offset() + firstPart->lineBytes();
    for (;;) {
        const auto next = detail::httpFindMultipartBodyDelimiter(requestBody.substr(cursor), boundary, true);
        if (const auto* part = next.part()) {
            countMultipartPart(parts, maxFields);
            cursor += part->offset() + part->lineBytes();
            continue;
        }
        return;
    }
}

[[nodiscard]] ContextRequest::RequestFormData parseMultipartFormBody(std::string_view requestBody, MultipartBoundary boundary, std::pmr::memory_resource* resource, ContextRequest::ParseBodyOptions options) {
    enforceMultipartFieldCap(requestBody, boundary, options.maxFields);

    auto parts = parseCompleteMultipartBody(requestBody, std::move(boundary), resource);
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(boundedFieldReserve(parts.size()));
    for (const auto& part : parts) {
        const auto partName = part.name();
        const auto partBody = part.body();
        const auto partFilename = part.filename();
        const auto partContentType = part.contentType();
        std::pmr::string name(partName.data(), partName.size(), resource);
        const bool array = fieldNameIsArray(std::string_view(name));
        // RFC 7578 section 4.4: a part without a Content-Type defaults to
        // text/plain. Surface that effective type to the form consumer rather
        // than an empty string (the raw multipart parts API stays faithful).
        std::pmr::string contentType = partContentType.empty() ? std::pmr::string("text/plain", resource) : std::pmr::string(partContentType.data(), partContentType.size(), resource);
        appendParsedBodyField(fields, detail::RequestFormFieldAccess::make(resource, std::move(name), std::pmr::string(partBody.data(), partBody.size(), resource), std::pmr::string(partFilename.data(), partFilename.size(), resource), std::move(contentType), part.hasFilename(), array), options);
    }
    compactParsedBodyFields(fields, options);
    return detail::RequestFormDataAccess::fromFields(std::move(fields));
}

}  // namespace

[[nodiscard]] std::pmr::vector<MultipartPart> parseCompleteMultipartBody(std::string_view requestBody, MultipartBoundary boundary, std::pmr::memory_resource* resource) {
    auto parsed = parseMultipartBody(requestBody, std::move(boundary), resource);
    if (const auto* failure = parsed.failure()) {
        throw failure->protocolError();
    }
    auto* body = parsed.body();
    if (body == nullptr) {
        throw std::logic_error("unexpected multipart body parse result");
    }
    return std::move(*body).takeParts();
}

[[nodiscard]] ContextRequest::RequestFormData parseFormBodyFromView(std::string_view contentType, std::string_view requestBody, std::pmr::memory_resource* resource, ContextRequest::ParseBodyOptions options) {
    if (detail::contentTypeMatches(contentType, "application/x-www-form-urlencoded")) {
        return parseUrlEncodedFormBody(requestBody, resource, options);
    }

    const auto boundary = parseMultipartBoundary(contentType);
    if (const auto* parsed = boundary.boundary()) {
        return parseMultipartFormBody(requestBody, *parsed, resource, options);
    }
    if (boundary.notApplicable() != nullptr) {
        return detail::RequestFormDataAccess::empty(resource);
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocolError();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace ruvia::detail
