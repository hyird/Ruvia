// Typed models and validation: rules live on model fields; routes select the
// source with json_body/form_body/Query/Path/Header/Cookie.
// Handlers return HTTP responses using c.json(model).
// json_if/form_if still parse without field rules.
// Run ruvia_example_models_validation on port 8081.
// curl "http://127.0.0.1:8081/models/search?q=hello&page=2"
// POST JSON to /models/register and form data to /models/contact.
// Missing required values and invalid fields become structured HTTP errors
// before a handler runs. Keep parsed request-backed models inside the request.

#include <charconv>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

static bool has_ruvia_code_prefix(const ruvia::string& code) {
    return code.view().starts_with("CY-");
}

RUVIA_MODEL(profile_request,
    RUVIA_REQUIRED_FIELD_NAME("displayName", display_name, ruvia::string, RUVIA_MIN(2, "display name is too short"),
        RUVIA_MAX(64, "display name is too long")),
    RUVIA_REQUIRED_FIELD(email, ruvia::string, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(age, ruvia::uint32, RUVIA_MIN(0, "age is too small"),
        RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(role_request,
    RUVIA_REQUIRED_FIELD(
        name, ruvia::string, RUVIA_ONE_OF("role is not allowed", "admin", "user", "editor")),
    RUVIA_OPTIONAL_FIELD(level, ruvia::uint32, RUVIA_MIN(1, "level is too small"),
        RUVIA_MAX(10, "level is too large")));

RUVIA_MODEL(register_request,
    RUVIA_OPTIONAL_FIELD_NAME("user_name", username, ruvia::string, RUVIA_DEFAULT("guest"),
        RUVIA_PATTERN("username format is invalid", "^[a-z][a-z0-9_]*$")),
    RUVIA_REQUIRED_FIELD(password, ruvia::string, RUVIA_MIN(8, "password is too short")),
    RUVIA_OPTIONAL_FIELD(code, ruvia::string,
        RUVIA_CUSTOM("code must use CY- prefix", has_ruvia_code_prefix)),
    RUVIA_REQUIRED_FIELD(profile, profile_request),
    RUVIA_REQUIRED_FIELD(roles, ruvia::array<role_request>, RUVIA_MIN(1, "too few roles"),
        RUVIA_MAX(5, "too many roles")),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(newsletter, ruvia::bool_value));

RUVIA_MODEL(register_response, RUVIA_OPTIONAL_FIELD(username, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("roleCount", role_count, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

RUVIA_MODEL(profile_changes,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));

RUVIA_MODEL(model_config,
    RUVIA_OPTIONAL_FIELD(retries, ruvia::uint8, RUVIA_INITIAL(3)),
    RUVIA_OPTIONAL_FIELD_NAME("timeoutMs", timeout_ms, ruvia::uint16, RUVIA_INITIAL(250)),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::bytes));

RUVIA_MODEL(contact_form,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_MIN(2, "name is too short")),
    RUVIA_OPTIONAL_FIELD(email, ruvia::string, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(message, ruvia::string, RUVIA_MIN(10, "message is too short")));

RUVIA_MODEL(search_query,
    RUVIA_REQUIRED_FIELD(q, ruvia::string, RUVIA_MIN(2, "query is too short")),
    RUVIA_OPTIONAL_FIELD(page, ruvia::uint32, RUVIA_MIN(1, "page is too small")));

RUVIA_MODEL(
    category_params, RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(2, "category id is too short")));

RUVIA_MODEL(request_headers,
    RUVIA_REQUIRED_FIELD_NAME(
        "x-request-id", request_id, ruvia::string, RUVIA_MIN(8, "request id is too short")));

RUVIA_MODEL(preferences_cookie,
    RUVIA_REQUIRED_FIELD(
        theme, ruvia::string, RUVIA_ONE_OF("theme is not allowed", "light", "dark")));

RUVIA_MODEL(category, RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<category>));

class model_controller final : public ruvia::controller<model_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/models")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/register", register_user, ruvia::json_body<register_request>);
    RUVIA_PATCH("/profile", patch_profile, ruvia::json_body<profile_changes>);
    RUVIA_POST("/contact", contact, ruvia::form_body<contact_form>);
    RUVIA_GET("/search", search, ruvia::query_model<search_query>);
    RUVIA_GET("/category", category);
    RUVIA_GET("/config", config);
    RUVIA_GET("/category/:id", category_by_id, ruvia::path_model<category_params>);
    RUVIA_GET("/headers", headers, ruvia::header_model<request_headers>);
    RUVIA_GET("/cookies", cookies, ruvia::cookie_model<preferences_cookie>);
    RUVIA_POST("/feedback", feedback);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> config(ruvia::context& c) {
        // INITIAL applies to this construction, not to JSON request parsing.
        model_config value({.resource_ = c.arena()});
        constexpr std::uint8_t bytes_value[] = {0, 1, 2, 255};
        value.set<"payload">(std::span<const std::uint8_t>(bytes_value));
        co_return c.json(value);  // payload is the base64 string "AAEC/w==".
    }

    // Echo the requested changes: omission means leave unchanged, null means
    // clear, and a concrete value means assign. No raw JSON inspection needed.
    ruvia::task<ruvia::http_response> patch_profile(ruvia::context& c) {
        const auto& patch = c.req().validated<profile_changes>();
        co_return c.json(patch);
    }

    ruvia::task<ruvia::http_response> register_user(ruvia::context& c) {
        const auto& request = c.req().validated<register_request>();

        register_response response({.resource_ = c.arena()});
        const auto& username = request.get<"username">();
        if (username) {
            response.set<"username">(username->view());
        }
        const auto& roles = request.get<"roles">();
        response.set<"role_count">(ruvia::uint32{static_cast<std::uint32_t>(roles.size())});
        response.ensure<"tags">().emplace_back(ruvia::string("created", {.resource_ = c.arena()}));
        response.ensure<"tags">().emplace_back(
            ruvia::string("validated", {.resource_ = c.arena()}));
        c.status(ruvia::http_status::created);
        co_return c.json(response);
    }

    // json_if/form_if still parse without field rules. A malformed selected body
    // is 400; only a different Content-Type yields nullopt.
    ruvia::task<ruvia::http_response> feedback(ruvia::context& c) {
        if (const auto json = co_await c.req().json_if<contact_form>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("json feedback from ");
            body.append(
                json->get<"name">().has_value() ? json->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        if (const auto form = co_await c.req().form_if<contact_form>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("form feedback from ");
            body.append(
                form->get<"name">().has_value() ? form->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        co_return c.text("no feedback body\n");
    }

    ruvia::task<ruvia::http_response> contact(ruvia::context& c) {
        const auto& form = c.req().validated<contact_form>();
        std::pmr::string body(c.allocator<char>());
        body.append("message from ");
        body.append(form.get<"name">() ? form.get<"name">()->view() : "anonymous");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> search(ruvia::context& c) {
        const auto& query = c.req().validated<search_query>();
        const auto request_query = c.req().query("q");
        std::pmr::string body(c.allocator<char>());
        body.append("search=");
        const auto query_value = query.get<"q">().view();
        body.append(query_value);
        body.append("\nquery-shared=");
        body.append(request_query.has_value() && request_query->data() == query_value.data() &&
                            request_query->size() == query_value.size()
                        ? "true"
                        : "false");
        if (const auto page = query.get<"page">()) {
            body.append("\npage=");
            char buffer[16]{};
            const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), page->value_);
            if (ec == std::errc{}) {
                body.append(buffer, static_cast<std::size_t>(ptr - buffer));
            }
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> category(ruvia::context& c) {
        ::category root({.resource_ = c.arena()});
        root.set<"name">("root");
        root.ensure<"children">().emplace().set<"name">("leaf");
        co_return c.json(root);
    }

    ruvia::task<ruvia::http_response> category_by_id(ruvia::context& c) {
        const auto& params = c.req().validated<category_params>();
        std::pmr::string body(c.allocator<char>());
        body.append("category=");
        body.append(params.get<"id">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> headers(ruvia::context& c) {
        const auto& headers = c.req().validated<request_headers>();
        std::pmr::string body(c.allocator<char>());
        body.append("request-id=");
        body.append(headers.get<"request_id">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> cookies(ruvia::context& c) {
        const auto& cookies = c.req().validated<preferences_cookie>();
        std::pmr::string body(c.allocator<char>());
        body.append("theme=");
        body.append(cookies.get<"theme">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8081})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .run();
}
