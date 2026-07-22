#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/http/request/RequestBodyLoader.h"

#include <optional>
#include <string_view>
#include <utility>

namespace ruvia::detail {

template <typename Reader>
[[nodiscard]] Task<std::optional<std::string_view>> bodyReaderReadThunk(void* target) {
    return static_cast<Reader*>(target)->read();
}

template <typename Reader>
void emplaceBodyReaderFacade(std::optional<BodyReader>& storage, Reader& reader) {
    StreamingAccess::emplaceBodyReader(storage, &reader, &bodyReaderReadThunk<Reader>);
}

template <typename Reader>
[[nodiscard]] BodyReader makeBodyReaderFacade(Reader& reader) noexcept {
    return StreamingAccess::makeBodyReader(
        &reader,
        &bodyReaderReadThunk<Reader>);
}

template <typename Reader>
class BodyReaderBinding final {
public:
    template <typename... Args>
    explicit BodyReaderBinding(Args&&... args)
        : reader_(std::forward<Args>(args)...),
          facade_(makeBodyReaderFacade(reader_)) {}

    BodyReaderBinding(const BodyReaderBinding&) = delete;
    BodyReaderBinding& operator=(const BodyReaderBinding&) = delete;
    BodyReaderBinding(BodyReaderBinding&&) = delete;
    BodyReaderBinding& operator=(BodyReaderBinding&&) = delete;

    [[nodiscard]] Reader& reader() noexcept { return reader_; }
    [[nodiscard]] const Reader& reader() const noexcept { return reader_; }
    [[nodiscard]] BodyReader& facade() noexcept { return facade_; }

private:
    Reader reader_;
    BodyReader facade_;
};

template <typename Loader>
[[nodiscard]] Task<std::string_view> requestBodyLoaderReadAllThunk(void* target) {
    return static_cast<Loader*>(target)->readAll();
}

template <typename Loader>
Task<void> requestBodyLoaderDiscardThunk(void* target) {
    return static_cast<Loader*>(target)->discard();
}

template <typename Loader>
[[nodiscard]] RequestBodyLoader makeRequestBodyLoaderFacade(
    Loader& loader) noexcept {
    return RequestBodyLoader(
        &loader,
        &requestBodyLoaderReadAllThunk<Loader>,
        &requestBodyLoaderDiscardThunk<Loader>);
}

template <typename Loader>
class RequestBodyLoaderBinding final {
public:
    template <typename... Args>
    explicit RequestBodyLoaderBinding(Args&&... args)
        : loader_(std::forward<Args>(args)...),
          facade_(makeRequestBodyLoaderFacade(loader_)) {}

    RequestBodyLoaderBinding(const RequestBodyLoaderBinding&) = delete;
    RequestBodyLoaderBinding& operator=(const RequestBodyLoaderBinding&) = delete;
    RequestBodyLoaderBinding(RequestBodyLoaderBinding&&) = delete;
    RequestBodyLoaderBinding& operator=(RequestBodyLoaderBinding&&) = delete;

    [[nodiscard]] Loader& loader() noexcept { return loader_; }
    [[nodiscard]] const Loader& loader() const noexcept { return loader_; }
    [[nodiscard]] RequestBodyLoader& facade() noexcept { return facade_; }

private:
    Loader loader_;
    RequestBodyLoader facade_;
};

}  // namespace ruvia::detail
