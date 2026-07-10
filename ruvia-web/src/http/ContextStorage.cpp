#include "ruvia/web/Context.h"

#include "ruvia/web/App.h"

namespace ruvia {

const Env& Context::env() const noexcept {
    return app().env();
}

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
        applyResponseState(*response_, 0, {});
    }
    return *response_;
}

HttpResponse& Context::res() {
    return responseStorage();
}

void Context::res(HttpResponse&& response) {
    storeAssignedResponse(std::move(response));
}

HttpResponse Context::takeResponse() {
    if (response_ == nullptr) {
        return HttpResponse(resource());
    }

    auto response = std::move(*response_);
    response_ = nullptr;
    responseFinalized_ = false;
    return response;
}

}  // namespace ruvia
