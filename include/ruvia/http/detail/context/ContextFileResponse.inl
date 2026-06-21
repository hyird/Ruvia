#pragma once

namespace ruvia {

inline HttpResponse Context::file(
    const std::filesystem::path& path,
    std::string_view contentType) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw HttpError(404, "not_found", "file not found");
    }

    return fileWithMetadata(FileToken(path), path, static_cast<std::uint64_t>(size), modified, contentType);
}

inline HttpResponse Context::staticFile(
    const StaticRoot& root,
    std::string_view relativePath,
    std::string_view contentType) const {
    auto relative = detail::normalizeStaticRelativePath(relativePath, allocator<char>());

    if (relative.empty() && !root.hasDirectoryIndex()) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }

    const auto* entry = root.find(relative);
    if (entry == nullptr && root.isIndexedDirectory(relative)) {
        if (!relative.empty() && relative.back() != '/') {
            relative.push_back('/');
        }
        relative.append(root.indexFile().data(), root.indexFile().size());
        entry = root.find(relative);
    }
    if (entry == nullptr) {
        throw HttpError(404, "not_found", "file not found");
    }

    return fileWithMetadata(
        FileToken::borrow(entry->file),
        detail::fileTokenPath(entry->file),
        entry->size,
        entry->modified,
        contentType.empty() ? std::string_view(entry->contentType) : contentType,
        entry->cacheControl,
        entry->enableRanges,
        entry->enableValidators,
        entry->etag,
        entry->lastModified);
}

inline detail::FileConditionalHeaders Context::fileConditionalHeaders() const noexcept {
    return detail::FileConditionalHeaders{
        request_.header(HttpRequest::KnownHeader::kIfMatch),
        request_.header(HttpRequest::KnownHeader::kIfUnmodifiedSince),
        request_.header(HttpRequest::KnownHeader::kIfNoneMatch),
        request_.header(HttpRequest::KnownHeader::kIfModifiedSince),
        request_.header(HttpRequest::KnownHeader::kRange),
        request_.header(HttpRequest::KnownHeader::kIfRange)};
}

inline HttpResponse Context::fileWithMetadata(
    FileToken file,
    const std::filesystem::path& path,
    std::uint64_t size,
    std::filesystem::file_time_type modified,
    std::string_view contentType,
    std::string_view cacheControl,
    bool enableRanges,
    bool enableValidators,
    std::string_view precomputedEtag,
    std::string_view precomputedLastModified) const {
    std::pmr::string etagStorage(resource());
    std::pmr::string lastModifiedStorage(resource());
    std::string_view etag;
    std::string_view lastModified;
    std::time_t modifiedSeconds{};
    if (enableValidators) {
        modifiedSeconds = detail::httpFileTimeToTimeT(modified);
        if (precomputedEtag.empty() || precomputedLastModified.empty()) {
            etagStorage = detail::httpMakeFileEtag(resource(), size, modified);
            lastModifiedStorage = detail::httpFormatDate(resource(), modifiedSeconds);
            etag = etagStorage;
            lastModified = lastModifiedStorage;
        } else {
            etag = precomputedEtag;
            lastModified = precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        response.reserveHeaders(kFileResponseHeaderReserve);
        if (contentType.empty()) {
            response.setHeaderStableView("Content-Type", detail::httpGuessContentType(path));
        } else {
            response.setHeader("Content-Type", contentType);
        }
        if (!cacheControl.empty()) {
            response.setHeader("Cache-Control", cacheControl);
        }
        if (enableRanges) {
            response.setHeaderStableView("Accept-Ranges", "bytes");
        }
        if (enableValidators) {
            response.setHeader("ETag", etag);
            response.setHeader("Last-Modified", lastModified);
        }
    };

    if (request_.method() == HttpMethod::kGet || request_.method() == HttpMethod::kHead) {
        const auto conditional = fileConditionalHeaders();
        if (enableValidators && !detail::ifMatchAllows(conditional.ifMatch, etag)) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }
        if (enableValidators && !conditional.ifUnmodifiedSince.empty() &&
            !detail::httpDateUnmodified(conditional.ifUnmodifiedSince, modifiedSeconds)) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }

        if (enableValidators && detail::ifNoneMatchMatches(conditional.ifNoneMatch, etag)) {
            HttpResponse response(resource());
            addFileHeaders(response);
            applyResponseState(response, 304, {});
            return response;
        }

        if (enableValidators && conditional.ifNoneMatch.empty() &&
            !conditional.ifModifiedSince.empty() &&
            detail::httpDateNotModified(conditional.ifModifiedSince, modifiedSeconds)) {
            HttpResponse response(resource());
            addFileHeaders(response);
            applyResponseState(response, 304, {});
            return response;
        }

        if (enableRanges && !conditional.range.empty()) {
            if (detail::httpByteRangeSetHasMultiple(conditional.range)) {
                HttpResponse response(resource());
                addFileHeaders(response);
                response.setFileBody(std::move(file), size);
                applyResponseState(response, 0, {});
                return response;
            }

            if (enableValidators && !detail::ifRangeAllows(conditional.ifRange, etag, modifiedSeconds)) {
                HttpResponse response(resource());
                addFileHeaders(response);
                response.setFileBody(std::move(file), size);
                applyResponseState(response, 0, {});
                return response;
            }

            const auto parsedRange = detail::httpParseByteRange(conditional.range, size);
            if (!parsedRange) {
                HttpResponse response(resource());
                response.setContentRangeUnsatisfied(size);
                addFileHeaders(response);
                applyResponseState(response, 416, {});
                return response;
            }

            const auto [offset, length] = *parsedRange;
            HttpResponse response(resource());
            addFileHeaders(response);
            response.setContentRange(offset, length, size);
            response.setFileBody(std::move(file), size, offset, length);
            applyResponseState(response, 206, {});
            return response;
        }
    }

    HttpResponse response(resource());
    addFileHeaders(response);
    response.setFileBody(std::move(file), size);
    applyResponseState(response, 0, {});
    return response;
}

}  // namespace ruvia
