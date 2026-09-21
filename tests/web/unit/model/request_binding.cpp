#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/Controller.h"
#include "ruvia/web/Testing.h"

#include "test_harness.h"

RUVIA_MODEL(RequestBindingParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_MIN(2, "id is too short")));

RUVIA_MODEL(RequestBindingBody,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "name is too short")));

RUVIA_MODEL(PatchBindingBody,
    RUVIA_REQUIRED_FIELD(token, ruvia::String, RUVIA_DEFAULT("not-a-required-input")),
    RUVIA_OPTIONAL_FIELD(retries, ruvia::UInt32, RUVIA_DEFAULT(0), RUVIA_MIN(1, "must be positive")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool));

RUVIA_MODEL(RequestBindingOut, RUVIA_REQUIRED_FIELD(id, ruvia::String),
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
    RUVIA_PATCH("/request-binding-patch", patch, ruvia::JsonBody<PatchBindingBody>);
    RUVIA_POST("/request-binding-echo", echo, ruvia::JsonBody<RequestBindingBody>);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        co_return c.json(c.req().validated<RequestBindingBody>());
    }

    ruvia::Task<ruvia::HttpResponse> patch(ruvia::Context& c) {
        const auto& body = c.req().validated<PatchBindingBody>();
        if (!body.isPresent<"remark">()) {
            co_return c.text("unchanged");
        }
        if (body.isNull<"remark">()) {
            co_return c.text("clear");
        }
        co_return c.text(body.get<"remark">()->view());
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto& params = c.req().validated<RequestBindingParams>();
        const auto& body = c.req().validated<RequestBindingBody>();
        auto response = co_await makeResponse(params.get<"id">().view(), body.get<"name">().view(), c.arena());
        co_return c.json(response);
    }
};

RUVIA_TEST(request_binding_enforces_defaults_and_exposes_patch_states) {
    ruvia::TestApp app;
    for (const auto& [body, expected] : {
             std::pair{R"({"token":"ok","retries":1})", "unchanged"},
             std::pair{R"({"token":"ok","retries":1,"remark":null})", "clear"},
             std::pair{R"({"token":"ok","retries":1,"remark":"changed"})", "changed"}}) {
        const auto response = app.request(ruvia::TestRequest::patch("/request-binding-patch").json(body));
        RUVIA_CHECK(response.status() == ruvia::http_status::kOk);
        RUVIA_CHECK_EQ(response.body(), std::string_view(expected));
    }
    for (const auto& [body, code] : {
             std::pair{R"({"token":"ok"})", "too_small"},
             std::pair{R"({"retries":1})", "required"},
             std::pair{R"({"token":"ok","retries":1,"enabled":null})", "invalid_type"},
             std::pair{R"({"token":"ok","retries":1,"remark":""})", "too_small"},
             std::pair{R"({"token":"ok","retries":1,"remark":null,"remark":"changed"})", "duplicate"}}) {
        const auto response = app.request(ruvia::TestRequest::patch("/request-binding-patch").json(body));
        RUVIA_CHECK(response.status() == ruvia::http_status::kBadRequest);
        RUVIA_CHECK(response.body().find(code) != std::string_view::npos);
    }
}

RUVIA_TEST(request_binding_serializes_the_validated_model_without_copying_fields) {
    ruvia::TestApp app;
    const auto ok = app.request(
        ruvia::TestRequest::post("/request-binding-echo").json(R"({"name":"A\u006c"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(ok.body(), std::string_view(R"({"name":"Al"})"));
    for (const auto body : {R"({"name":"A"})", R"({"name":null})", R"({})", R"({"name":"Al","name":"Bob"})"}) {
        const auto rejected = app.request(ruvia::TestRequest::post("/request-binding-echo").json(body));
        RUVIA_CHECK(rejected.status() == ruvia::http_status::kBadRequest);
    }
}

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
