// Typed request/response models and validation: JSON models, form bodies, nested
// models, arrays, recursive lists, defaults, validation
// middleware and rules, and jsonIf/formIf probes that fall back only on a
// media-type mismatch; malformed selected bodies remain 400 responses.

#include <cstdint>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

RUVIA_REQUEST_MODEL(ProfileRequest, RUVIA_OPTIONAL_FIELD(displayName, ruvia::String), RUVIA_OPTIONAL_FIELD(email, ruvia::String), RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32));

RUVIA_REQUEST_MODEL(RoleRequest, RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(level, ruvia::UInt32));

RUVIA_REQUEST_MODEL(RegisterRequest, RUVIA_OPTIONAL_FIELD_NAME("user_name", username, ruvia::String, RUVIA_DEFAULT("guest")), RUVIA_OPTIONAL_FIELD(password, ruvia::String), RUVIA_OPTIONAL_FIELD(code, ruvia::String), RUVIA_OPTIONAL_FIELD(profile, ProfileRequest), RUVIA_OPTIONAL_FIELD(roles, ruvia::Array<RoleRequest>), RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>), RUVIA_OPTIONAL_FIELD(newsletter, ruvia::Bool));

RUVIA_RESPONSE_MODEL(RegisterResponse, RUVIA_OPTIONAL_FIELD(username, ruvia::String), RUVIA_OPTIONAL_FIELD(roleCount, ruvia::UInt32), RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(ContactForm, RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(email, ruvia::String), RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_REQUEST_MODEL(SearchQuery, RUVIA_OPTIONAL_FIELD(q, ruvia::String), RUVIA_OPTIONAL_FIELD(page, ruvia::UInt32));

RUVIA_REQUEST_MODEL(CategoryParams, RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(RequestHeaders, RUVIA_OPTIONAL_FIELD_NAME("x-request-id", requestId, ruvia::String));

RUVIA_REQUEST_MODEL(PreferencesCookie, RUVIA_OPTIONAL_FIELD(theme, ruvia::String));

RUVIA_RESPONSE_MODEL(Category, RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(children, ruvia::BoxedArray<Category>));

static bool hasRuviaCodePrefix(const ruvia::String& code) {
    return code.view().starts_with("CY-");
}

class ProfileValidator final : public ruvia::Middleware<ProfileValidator> {
public:
    RUVIA_VALIDATE_JSON(ProfileRequest, RUVIA_RULE(displayName, RUVIA_REQUIRED("display name is required"), RUVIA_MIN(2, "display name is too short"), RUVIA_MAX(64, "display name is too long")), RUVIA_RULE(email, RUVIA_REQUIRED("email is required"), RUVIA_EMAIL("email format is invalid")), RUVIA_RULE(age, RUVIA_MIN(0, "age is too small"), RUVIA_MAX(130, "age is too large")))
};

class RoleValidator final : public ruvia::Middleware<RoleValidator> {
public:
    RUVIA_VALIDATE_JSON(RoleRequest, RUVIA_RULE(name, RUVIA_REQUIRED("role is required"), RUVIA_ONE_OF("role is not allowed", "admin", "user", "editor")), RUVIA_RULE(level, RUVIA_MIN(1, "level is too small"), RUVIA_MAX(10, "level is too large")))
};

class RegisterValidator final : public ruvia::Middleware<RegisterValidator> {
public:
    RUVIA_VALIDATE_JSON(RegisterRequest, RUVIA_RULE_NAME("user_name", username, RUVIA_REQUIRED("username is required"), RUVIA_PATTERN("username format is invalid", "^[a-z][a-z0-9_]*$")), RUVIA_RULE(password, RUVIA_REQUIRED("password is required"), RUVIA_MIN(8, "password is too short")), RUVIA_RULE(code, RUVIA_CUSTOM("code must use CY- prefix", hasRuviaCodePrefix)), RUVIA_RULE(profile, RUVIA_REQUIRED("profile is required"), RUVIA_NESTED(ProfileValidator)), RUVIA_RULE(roles, RUVIA_REQUIRED("at least one role is required"), RUVIA_MIN(1, "too few roles"), RUVIA_MAX(5, "too many roles"), RUVIA_EACH(RoleValidator)))
};

class ContactFormValidator final : public ruvia::Middleware<ContactFormValidator> {
public:
    RUVIA_VALIDATE_FORM(ContactForm, RUVIA_RULE(name, RUVIA_REQUIRED("name is required"), RUVIA_MIN(2, "name is too short")), RUVIA_RULE(email, RUVIA_REQUIRED("email is required"), RUVIA_EMAIL("email format is invalid")), RUVIA_RULE(message, RUVIA_REQUIRED("message is required"), RUVIA_MIN(10, "message is too short")))
};

class SearchQueryValidator final : public ruvia::Middleware<SearchQueryValidator> {
public:
    RUVIA_VALIDATE_QUERY(SearchQuery, RUVIA_RULE(q, RUVIA_REQUIRED("query is required"), RUVIA_MIN(2, "query is too short")), RUVIA_RULE(page, RUVIA_MIN(1, "page is too small")))
};

class CategoryParamValidator final : public ruvia::Middleware<CategoryParamValidator> {
public:
    RUVIA_VALIDATE_PARAM(CategoryParams, RUVIA_RULE(id, RUVIA_REQUIRED("category id is required"), RUVIA_MIN(2, "category id is too short")))
};

class RequestHeaderValidator final : public ruvia::Middleware<RequestHeaderValidator> {
public:
    RUVIA_VALIDATE_HEADER(RequestHeaders, RUVIA_RULE_NAME("x-request-id", requestId, RUVIA_REQUIRED("request id is required"), RUVIA_MIN(8, "request id is too short")))
};

class PreferencesCookieValidator final : public ruvia::Middleware<PreferencesCookieValidator> {
public:
    RUVIA_VALIDATE_COOKIE(PreferencesCookie, RUVIA_RULE(theme, RUVIA_REQUIRED("theme cookie is required"), RUVIA_ONE_OF("theme is not allowed", "light", "dark")))
};

class ModelController final : public ruvia::Controller<ModelController> {
public:
    RUVIA_CONTROLLER_GROUP("/models")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/register", registerUser, RegisterValidator);
    RUVIA_POST("/contact", contact, ContactFormValidator);
    RUVIA_GET("/search", search, SearchQueryValidator);
    RUVIA_GET("/category", category);
    RUVIA_GET("/category/:id", categoryById, CategoryParamValidator);
    RUVIA_GET("/headers", headers, RequestHeaderValidator);
    RUVIA_GET("/cookies", cookies, PreferencesCookieValidator);
    RUVIA_POST("/feedback", feedback);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> registerUser(ruvia::Context& c) {
        const auto& request = c.req().validated<RegisterRequest>();

        RegisterResponse response(c);
        const auto& username = request.get<"username">();
        response.set<"username">(username->view());
        const auto& roles = request.get<"roles">();
        if (roles) {
            response.set<"roleCount">(ruvia::UInt32{static_cast<std::uint32_t>(roles->size())});
        }
        response.ensure<"tags">().emplace_back(ruvia::String("created", {.resource = c.resource()}));
        response.ensure<"tags">().emplace_back(ruvia::String("validated", {.resource = c.resource()}));
        c.status(ruvia::http_status::kCreated);
        co_return c.json(response);
    }

    // json<T>()/form<T>() answer a wrong Content-Type with 415 and a
    // malformed body of the right type with 400. The *If variants only return
    // nullopt for a different Content-Type; a malformed body still receives
    // the 400 so content negotiation cannot silently accept broken JSON/form.
    ruvia::Task<ruvia::HttpResponse> feedback(ruvia::Context& c) {
        if (const auto json = co_await c.req().jsonIf<ContactForm>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("json feedback from ");
            body.append(json->get<"name">().has_value() ? json->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        if (const auto form = co_await c.req().formIf<ContactForm>()) {
            std::pmr::string body(c.allocator<char>());
            body.append("form feedback from ");
            body.append(form->get<"name">().has_value() ? form->get<"name">()->view() : "anonymous");
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        co_return c.text("no feedback body\n");
    }

    ruvia::Task<ruvia::HttpResponse> contact(ruvia::Context& c) {
        const auto& form = c.req().validated<ContactForm>();
        std::pmr::string body(c.allocator<char>());
        body.append("message from ");
        const auto& name = form.get<"name">();
        body.append(name->view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> search(ruvia::Context& c) {
        const auto& query = c.req().validated<SearchQuery>();
        const auto requestQuery = c.req().query("q");
        std::pmr::string body(c.allocator<char>());
        body.append("search=");
        const auto& q = query.get<"q">();
        body.append(q->view());
        body.append("\nquery-shared=");
        const auto queryValue = q->view();
        body.append(requestQuery.has_value() && requestQuery->data() == queryValue.data() && requestQuery->size() == queryValue.size() ? "true" : "false");
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
        Category root(c);
        root.set<"name">("root");
        root.ensure<"children">().emplace().set<"name">("leaf");
        co_return c.json(root);
    }

    ruvia::Task<ruvia::HttpResponse> categoryById(ruvia::Context& c) {
        const auto& params = c.req().validated<CategoryParams>();
        std::pmr::string body(c.allocator<char>());
        body.append("category=");
        const auto& id = params.get<"id">();
        body.append(id->view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> headers(ruvia::Context& c) {
        const auto& headers = c.req().validated<RequestHeaders>();
        std::pmr::string body(c.allocator<char>());
        body.append("request-id=");
        const auto& requestId = headers.get<"requestId">();
        body.append(requestId->view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> cookies(ruvia::Context& c) {
        const auto& cookies = c.req().validated<PreferencesCookie>();
        std::pmr::string body(c.allocator<char>());
        body.append("theme=");
        const auto& theme = cookies.get<"theme">();
        body.append(theme->view());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app().listen({.address = "0.0.0.0", .http = 8081}).server({.workerCount = 2, .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall}).run();
}
