#include "ruvia/http/Context.h"

namespace ruvia {

std::pmr::string& Context::decodedBody() const {
    if (decodedBody_ == nullptr) {
        decodedBody_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *decodedBody_;
}

std::pmr::string& Context::sessionIdStorage() {
    if (sessionId_ == nullptr) {
        sessionId_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *sessionId_;
}

std::pmr::string& Context::sessionDataStorage() {
    if (sessionData_ == nullptr) {
        sessionData_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *sessionData_;
}

detail::ContextValueStore& Context::values() {
    if (values_ == nullptr) {
        values_ = &memory_.emplace<detail::ContextValueStore>(resource());
    }
    return *values_;
}

HttpResponse& Context::responseStorage() {
    if (response_ == nullptr) {
        response_ = &memory_.emplace<HttpResponse>(resource());
    }
    return *response_;
}

HttpResponse& Context::res() {
    return responseStorage();
}

Context& Context::res(HttpResponse&& response) {
    responseStorage() = std::move(response);
    return *this;
}

HttpResponse Context::takeResponse() {
    return std::move(responseStorage());
}

bool Context::has(std::string_view name) const noexcept {
    return values_ != nullptr && values_->contains(name);
}

}  // namespace ruvia
