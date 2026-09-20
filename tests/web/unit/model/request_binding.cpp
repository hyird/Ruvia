#include <memory_resource>
#include <string_view>

#include "ruvia/web/Controller.h"
#include "ruvia/web/Testing.h"

#include "test_harness.h"

RUVIA_REQUEST_MODEL(RequestBindingParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_MIN(2, "id is too short")));

RUVIA_REQUEST_MODEL(RequestBindingBody,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "name is too short")));

RUVIA_RESPONSE_MODEL(RequestBindingOut, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(name, ruvia::String));

namespace {

ruvia::Task<RequestBindingOut> makeResponse(std::string_view id, std::string_view name,
    std::pmr::memory_resource* resource) {
    RequestBindingOut response({.resource = resource});
    response.set<"id">(id);
    response.set<"name">(name);
    co_return response;
}

}  // namespace

class RequestBindingController final : public ruvia::Controller<RequestBindingController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_PUT("/request-binding/:id", update, ruvia::PathModel<RequestBindingParams>,
        ruvia::JsonBody<RequestBindingBody>);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto& params = c.req().validated<RequestBindingParams>();
        const auto& body = c.req().validated<RequestBindingBody>();
        auto response = co_await makeResponse(params.get<"id">().view(), body.get<"name">().view(), c.arena());
        co_return c.json(response);
    }
};

RUVIA_TEST(request_binding_validates_inputs_and_serializes_service_model) {
    ruvia::TestApp app;
    const auto ok = app.request(
        ruvia::TestRequest::put("/request-binding/u1").json(R"({"name":"Al"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::kOk);
    RUVIA_CHECK(ok.body().find("\"id\":\"u1\"") != std::string_view::npos);
    RUVIA_CHECK(ok.body().find("\"name\":\"Al\"") != std::string_view::npos);

    const auto shortId = app.request(
        ruvia::TestRequest::put("/request-binding/x").json(R"({"name":"Al"})"));
    RUVIA_CHECK(shortId.status() == ruvia::http_status::kBadRequest);

    const auto shortName = app.request(
        ruvia::TestRequest::put("/request-binding/u1").json(R"({"name":"A"})"));
    RUVIA_CHECK(shortName.status() == ruvia::http_status::kBadRequest);
}
