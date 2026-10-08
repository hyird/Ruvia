#include "ruvia/web/Context.h"
#include "ruvia/web/Dotenv.h"

#include "context/ContextRequestStorage.h"
#include "context/ContextServices.h"
#include "router/RouteLimits.h"
#include "server/RequestDeadline.h"

namespace ruvia {

Context::Context(
    RequestMemory& memory, const HttpRequest& request, detail::ContextServices services)
    : Context(memory, request, {}, nullptr, nullptr, 0, 0, services) {}

Context::Context(RequestMemory& memory, const HttpRequest& request,
    std::string_view routePath, const std::string_view* paramNames,
    const std::string_view* paramValues, std::size_t paramCount, std::uintptr_t routeRateLimitScope,
    detail::ContextServices services)
    : memory_(memory),
      request_(request),
      early_data_info_(services.early_data_info().received_from_early_data(),
          request.header("early-data").has_value()),
      connInfo_(services.resolveConnInfo(request)),
      capabilities_(services.worker(), services.stopToken(), services.workerStates(), services.blockingPool()),
      routePath_(routePath),
      paramNames_(paramNames),
      paramValues_(paramValues),
      paramCount_(paramCount < detail::kMaxRouteParams ? paramCount : detail::kMaxRouteParams),
      routeRateLimitScope_(routeRateLimitScope),
      requestStorage_(detail::makePmrObject<detail::ContextRequestStorage>(memory.resource(),
          services, memory.resource())) {
    if (!services.automaticAltSvc().empty()) {
        setStableResponseHeader("Alt-Svc", services.automaticAltSvc());
    }
}

Context::~Context() = default;

bool Context::isSubrequest() const noexcept {
    return services().dispatchDepth() != 0;
}

detail::ContextServices& Context::services() noexcept {
    return requestStorage().services;
}

const detail::ContextServices& Context::services() const noexcept {
    return requestStorage().services;
}

bool Context::deadlineExceeded() const noexcept {
    return services().requestDeadline() != nullptr && services().requestDeadline()->exceeded();
}

const Env& Context::env() const noexcept {
    static const Env empty;
    return services().env() != nullptr ? *services().env() : empty;
}

std::pmr::string& Context::decodedBody() const {
    auto& storage = requestStorage();
    if (!storage.decodedBody) {
        storage.decodedBody.emplace(services().inbound_buffer_pool() != nullptr ? services().inbound_buffer_pool() : pool());
    }
    return *storage.decodedBody;
}

detail::ContextRequestStorage& Context::requestStorage() const {
    return *requestStorage_;
}

detail::ContextRequestBodySource& Context::requestBodySource() noexcept {
    return services().requestBodySource();
}

const detail::ContextRequestBodySource& Context::requestBodySource() const noexcept {
    return services().requestBodySource();
}

detail::ContextResponseOutput& Context::responseOutput() noexcept {
    return services().responseOutput();
}

const detail::ContextResponseOutput& Context::responseOutput() const noexcept {
    return services().responseOutput();
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
