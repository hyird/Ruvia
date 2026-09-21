// Typed models and validation: rules live on model fields; routes select the
// source with JsonBody/FormBody/Query/Path/Header/Cookie.
// Handlers return HTTP responses using c.json(model).
// jsonIf/formIf still parse without field rules.

#include <charconv>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

static bool hasRuviaCodePrefix(const ruvia::String& code) {
    return code.view().starts_with("CY-");
}

RUVIA_MODEL(ProfileRequest,
    RUVIA_REQUIRED_FIELD(displayName, ruvia::String, RUVIA_MIN(2, "display name is too short"),
        RUVIA_MAX(64, "display name is too long")),
    RUVIA_REQUIRED_FIELD(email, ruvia::String, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32, RUVIA_MIN(0, "age is too small"),
        RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(RoleRequest,
    RUVIA_REQUIRED_FIELD(
        name, ruvia::String, RUVIA_ONE_OF("role is not allowed", "admin", "user", "editor")),
    RUVIA_OPTIONAL_FIELD(level, ruvia::UInt32, RUVIA_MIN(1, "level is too small"),
        RUVIA_MAX(10, "level is too large")));

RUVIA_MODEL(RegisterRequest,
    RUVIA_OPTIONAL_FIELD_NAME("user_name", username, ruvia::String, RUVIA_DEFAULT("guest"),
        RUVIA_PATTERN("username format is invalid", "^[a-z][a-z0-9_]*$")),
    RUVIA_REQUIRED_FIELD(password, ruvia::String, RUVIA_MIN(8, "password is too short")),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String,
        RUVIA_CUSTOM("code must use CY- prefix", hasRuviaCodePrefix)),
    RUVIA_REQUIRED_FIELD(profile, ProfileRequest),
    RUVIA_REQUIRED_FIELD(roles, ruvia::Array<RoleRequest>, RUVIA_MIN(1, "too few roles"),
        RUVIA_MAX(5, "too many roles")),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(newsletter, ruvia::Bool));

RUVIA_MODEL(RegisterResponse, RUVIA_OPTIONAL_FIELD(username, ruvia::String),
    RUVIA_OPTIONAL_FIELD(roleCount, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

RUVIA_MODEL(ProfileChanges,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE));

RUVIA_MODEL(ModelConfig,
    RUVIA_OPTIONAL_FIELD(retries, ruvia::UInt8, RUVIA_INITIAL(3)),
    RUVIA_OPTIONAL_FIELD(timeoutMs, ruvia::UInt16, RUVIA_INITIAL(250)),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::Bytes));

RUVIA_MODEL(ContactForm,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_MIN(2, "name is too short")),
    RUVIA_OPTIONAL_FIELD(email, ruvia::String, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String, RUVIA_MIN(10, "message is too short")));

RUVIA_MODEL(SearchQuery,
    RUVIA_REQUIRED_FIELD(q, ruvia::String, RUVIA_MIN(2, "query is too short")),
    RUVIA_OPTIONAL_FIELD(page, ruvia::UInt32, RUVIA_MIN(1, "page is too small")));

RUVIA_MODEL(
    CategoryParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_MIN(2, "category id is too short")));

RUVIA_MODEL(RequestHeaders,
    RUVIA_REQUIRED_FIELD_NAME(
        "x-request-id", requestId, ruvia::String, RUVIA_MIN(8, "request id is too short")));

RUVIA_MODEL(PreferencesCookie,
    RUVIA_REQUIRED_FIELD(
        theme, ruvia::String, RUVIA_ONE_OF("theme is not allowed", "light", "dark")));

RUVIA_MODEL(Category, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(children, ruvia::BoxedArray<Category>));

class ModelController final : public ruvia::Controller<ModelController> {
public:
    RUVIA_CONTROLLER_GROUP("/models")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/register", registerUser, ruvia::JsonBody<RegisterRequest>);
    RUVIA_PATCH("/profile", patchProfile, ruvia::JsonBody<ProfileChanges>);
    RUVIA_POST("/contact", contact, ruvia::FormBody<ContactForm>);
    RUVIA_GET("/search", search, ruvia::QueryModel<SearchQuery>);
    RUVIA_GET("/category", category);
    RUVIA_GET("/config", config);
    RUVIA_GET("/category/:id", categoryById, ruvia::PathModel<CategoryParams>);
    RUVIA_GET("/headers", headers, ruvia::HeaderModel<RequestHeaders>);
    RUVIA_GET("/cookies", cookies, ruvia::CookieModel<PreferencesCookie>);
    RUVIA_POST("/feedback", feedback);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> config(ruvia::Context& c) {
        // INITIAL applies to this construction, not to JSON request parsing.
        ModelConfig value({.resource = c.arena()});
        constexpr std::uint8_t bytes[] = {0, 1, 2, 255};
        value.set<"payload">(std::span<const std::uint8_t>(bytes));
        co_return c.json(value);  // payload is the base64 string "AAEC/w==".
    }

    // Echo the requested changes: omission means leave unchanged, null means
    // clear, and a concrete value means assign. No raw JSON inspection needed.
    ruvia::Task<ruvia::HttpResponse> patchProfile(ruvia::Context& c) {
        const auto& patch = c.req().validated<ProfileChanges>();
        co_return c.json(patch);
    }

    ruvia::Task<ruvia::HttpResponse> registerUser(ruvia::Context& c) {
        const auto& request = c.req().validated<RegisterRequest>();

        RegisterResponse response({.resource = c.arena()});
        const auto& username = request.get<"username">();
        if (username) {
            response.set<"username">(username->view());
        }
        const auto& roles = request.get<"roles">();
        response.set<"roleCount">(ruvia::UInt32{static_cast<std::uint32_t>(roles.size())});
        response.ensure<"tags">().emplace_back(ruvia::String("created", {.resource = c.arena()}));
        response.ensure<"tags">().emplace_back(
            ruvia::String("validated", {.resource = c.arena()}));
        c.status(ruvia::http_status::kCreated);
        co_return c.json(response);
    }

    // jsonIf/formIf still parse without field rules. A malformed selected body
    // is 400; only a different Content-Type yields nullopt.
    ruvia::Task<ruvia::HttpResponse> feedback(ruvia::Context& c) {
        if (const auto json = co_await c.req().jsonIf<ContactForm>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("json feedback from ");
            body.append(
                json->get<"name">().has_value() ? json->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        if (const auto form = co_await c.req().formIf<ContactForm>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("form feedback from ");
            body.append(
                form->get<"name">().has_value() ? form->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        co_return c.text("no feedback body\n");
    }

    ruvia::Task<ruvia::HttpResponse> contact(ruvia::Context& c) {
        const auto& form = c.req().validated<ContactForm>();
        std::pmr::string body(c.allocator<char>());
        body.append("message from ");
        body.append(form.get<"name">() ? form.get<"name">()->view() : "anonymous");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> search(ruvia::Context& c) {
        const auto& query = c.req().validated<SearchQuery>();
        const auto requestQuery = c.req().query("q");
        std::pmr::string body(c.allocator<char>());
        body.append("search=");
        const auto queryValue = query.get<"q">().view();
        body.append(queryValue);
        body.append("\nquery-shared=");
        body.append(requestQuery.has_value() && requestQuery->data() == queryValue.data() &&
                            requestQuery->size() == queryValue.size()
                        ? "true"
                        : "false");
        if (const auto page = query.get<"page">()) {
            body.append("\npage=");
            char buffer[16]{};
            const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), page->value);
            if (ec == std::errc{}) {
                body.append(buffer, static_cast<std::size_t>(ptr - buffer));
            }
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> category(ruvia::Context& c) {
        Category root({.resource = c.arena()});
        root.set<"name">("root");
        root.ensure<"children">().emplace().set<"name">("leaf");
        co_return c.json(root);
    }

    ruvia::Task<ruvia::HttpResponse> categoryById(ruvia::Context& c) {
        const auto& params = c.req().validated<CategoryParams>();
        std::pmr::string body(c.allocator<char>());
        body.append("category=");
        body.append(params.get<"id">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> headers(ruvia::Context& c) {
        const auto& headers = c.req().validated<RequestHeaders>();
        std::pmr::string body(c.allocator<char>());
        body.append("request-id=");
        body.append(headers.get<"requestId">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> cookies(ruvia::Context& c) {
        const auto& cookies = c.req().validated<PreferencesCookie>();
        std::pmr::string body(c.allocator<char>());
        body.append("theme=");
        body.append(cookies.get<"theme">().view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8081})
        .server({.workerCount = 2,
            .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall})
        .run();
}
