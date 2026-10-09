#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

#include "test_harness.h"

RUVIA_MODEL(request_binding_params,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(2, "id is too short")));

RUVIA_MODEL(request_binding_body,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(2, "name is too short")));

RUVIA_MODEL(patch_binding_body,
    RUVIA_REQUIRED_FIELD(token, ruvia::string, RUVIA_DEFAULT("not-a-required-input")),
    RUVIA_OPTIONAL_FIELD(retries, ruvia::uint32, RUVIA_DEFAULT(0), RUVIA_MIN(1, "must be positive")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value));

RUVIA_MODEL(request_binding_out, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string));

namespace {

ruvia::task<request_binding_out> make_response(std::string_view id, std::string_view name,
    std::pmr::memory_resource* resource) {
    request_binding_out response({.resource_ = resource});
    response.set<"id">(id);
    response.set<"name">(name);
    co_return response;
}

}  // namespace

class request_binding_controller final : public ruvia::controller<request_binding_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_PUT("/request-binding/:id", update, ruvia::path_model<request_binding_params>,
        ruvia::json_body<request_binding_body>);
    RUVIA_PATCH("/request-binding-patch", patch, ruvia::json_body<patch_binding_body>);
    RUVIA_POST("/request-binding-echo", echo, ruvia::json_body<request_binding_body>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        co_return c.json(c.req().validated<request_binding_body>());
    }

    ruvia::task<ruvia::http_response> patch(ruvia::context& c) {
        const auto& body = c.req().validated<patch_binding_body>();
        if (!body.is_present<"remark">()) {
            co_return c.text("unchanged");
        }
        if (body.is_null<"remark">()) {
            co_return c.text("clear");
        }
        co_return c.text(body.get<"remark">()->view());
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto& params = c.req().validated<request_binding_params>();
        const auto& body = c.req().validated<request_binding_body>();
        auto response = co_await make_response(params.get<"id">().view(), body.get<"name">().view(), c.arena());
        co_return c.json(response);
    }
};

RUVIA_TEST(request_binding_enforces_defaults_and_exposes_patch_states) {
    ruvia::test_app app;
    for (const auto& [body, expected] : {
             std::pair{R"({"token":"ok","retries":1})", "unchanged"},
             std::pair{R"({"token":"ok","retries":1,"remark":null})", "clear"},
             std::pair{R"({"token":"ok","retries":1,"remark":"changed"})", "changed"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        RUVIA_CHECK(response.status() == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.body(), std::string_view(expected));
    }
    for (const auto& [body, code] : {
             std::pair{R"({"token":"ok"})", "too_small"},
             std::pair{R"({"retries":1})", "required"},
             std::pair{R"({"token":"ok","retries":1,"enabled":null})", "invalid_type"},
             std::pair{R"({"token":"ok","retries":1,"remark":""})", "too_small"},
             std::pair{R"({"token":"ok","retries":1,"remark":null,"remark":"changed"})", "duplicate"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        RUVIA_CHECK(response.status() == ruvia::http_status::bad_request);
        RUVIA_CHECK(response.body().find(code) != std::string_view::npos);
    }
}

RUVIA_TEST(request_binding_serializes_the_validated_model_without_copying_fields) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::post("/request-binding-echo").json(R"({"name":"A\u006c"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(ok.body(), std::string_view(R"({"name":"Al"})"));
    for (const auto body : {R"({"name":"A"})", R"({"name":null})", R"({})", R"({"name":"Al","name":"Bob"})"}) {
        const auto rejected = app.request(ruvia::test_request::post("/request-binding-echo").json(body));
        RUVIA_CHECK(rejected.status() == ruvia::http_status::bad_request);
    }
}

RUVIA_TEST(request_binding_validates_inputs_and_serializes_service_model) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"Al"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::ok);
    RUVIA_CHECK(ok.body().find("\"id\":\"u1\"") != std::string_view::npos);
    RUVIA_CHECK(ok.body().find("\"name\":\"Al\"") != std::string_view::npos);

    const auto short_id = app.request(
        ruvia::test_request::put("/request-binding/x").json(R"({"name":"Al"})"));
    RUVIA_CHECK(short_id.status() == ruvia::http_status::bad_request);

    const auto short_name = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"A"})"));
    RUVIA_CHECK(short_name.status() == ruvia::http_status::bad_request);
}
