#include "ruvia/web/Context.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/detail/http/context/ContextRequestStorage.h"
#include "ruvia/web/detail/server/RequestDeadline.h"

namespace ruvia {

Context::~Context() = default;

bool Context::deadlineExceeded() const noexcept {
    return requestDeadline_ != nullptr && requestDeadline_->exceeded();
}

const Env& Context::env() const noexcept {
    static const Env empty;
    return env_ != nullptr ? *env_ : empty;
}

std::pmr::string& Context::decodedBody() const {
    auto& storage = requestStorage();
    if (!storage.decodedBody) {
        storage.decodedBody.emplace(resource());
    }
    return *storage.decodedBody;
}

detail::ContextRequestStorage& Context::requestStorage() const {
    return *requestStorage_;
}

detail::ContextRequestBodySource& Context::requestBodySource() noexcept {
    return requestStorage().requestBodySource;
}

const detail::ContextRequestBodySource& Context::requestBodySource() const noexcept {
    return requestStorage().requestBodySource;
}

detail::ContextResponseOutput& Context::responseOutput() noexcept {
    return requestStorage().responseOutput;
}

const detail::ContextResponseOutput& Context::responseOutput() const noexcept {
    return requestStorage().responseOutput;
}

detail::ContextResponseState& Context::responseState() noexcept {
    return requestStorage().responseState;
}

const detail::ContextResponseState& Context::responseState() const noexcept {
    return requestStorage().responseState;
}

detail::ContextSessionState& Context::sessionState() noexcept {
    return requestStorage().sessionState;
}

const detail::ContextSessionState& Context::sessionState() const noexcept {
    return requestStorage().sessionState;
}

detail::RequestBindings& Context::requestBindings() noexcept {
    return requestStorage().requestBindings;
}

const detail::RequestBindings& Context::requestBindings() const noexcept {
    return requestStorage().requestBindings;
}

HttpResponse& Context::responseStorage() {
    return responseState().materializeProvisional();
}

const HttpResponse* Context::response() const noexcept {
    const auto* final = responseState().final();
    return final == nullptr ? nullptr : &final->response();
}

bool Context::hasResponse() const noexcept {
    return responseState().final() != nullptr;
}

void Context::respond(HttpResponse&& response) {
    storeAssignedResponse(std::move(response));
}

HttpResponse Context::takeResponse() {
    return responseState().take();
}

}  // namespace ruvia
