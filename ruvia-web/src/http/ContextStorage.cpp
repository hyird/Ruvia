#include "ruvia/web/Context.h"

#include "ruvia/web/Dotenv.h"

namespace ruvia {

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
    if (!requestStorage_) {
        requestStorage_ = detail::makePmrObject<detail::ContextRequestStorage>(
            resource());
    }
    return *requestStorage_;
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
