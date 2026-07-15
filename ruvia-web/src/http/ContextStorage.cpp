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
    return responseState_.materializeProvisional();
}

const HttpResponse* Context::response() const noexcept {
    const auto* final = responseState_.final();
    return final == nullptr ? nullptr : &final->response();
}

void Context::respond(HttpResponse&& response) {
    storeAssignedResponse(std::move(response));
}

HttpResponse Context::takeResponse() {
    return responseState_.take();
}

}  // namespace ruvia
