#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/Streaming.h"
#include "../../http/RequestBodyLoader.h"

#include <optional>
#include <string_view>

namespace ruvia::detail {

template <typename Reader>
[[nodiscard]] Task<std::optional<std::string_view>> bodyReaderReadThunk(void* target) {
    return static_cast<Reader*>(target)->read();
}

template <typename Reader>
void emplaceBodyReaderFacade(std::optional<BodyReader>& storage, Reader& reader) {
    StreamingAccess::emplaceBodyReader(storage, &reader, &bodyReaderReadThunk<Reader>);
}

template <typename Loader>
[[nodiscard]] Task<std::string_view> requestBodyLoaderReadAllThunk(void* target) {
    return static_cast<Loader*>(target)->readAll();
}

template <typename Loader>
Task<void> requestBodyLoaderDiscardThunk(void* target) {
    return static_cast<Loader*>(target)->discard();
}

template <typename Loader>
void emplaceRequestBodyLoaderFacade(std::optional<RequestBodyLoader>& storage, Loader& loader) {
    storage.emplace(
        &loader,
        &requestBodyLoaderReadAllThunk<Loader>,
        &requestBodyLoaderDiscardThunk<Loader>);
}

}  // namespace ruvia::detail
