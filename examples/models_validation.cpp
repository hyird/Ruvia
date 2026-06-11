#include <cstdint>
#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

RUVIA_MODEL(ProfileRequest,
    RUVIA_FIELD(displayName, ruvia::String),
    RUVIA_FIELD(email, ruvia::String),
    RUVIA_FIELD(age, ruvia::UInt32)
);

RUVIA_MODEL(RoleRequest,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(level, ruvia::UInt32)
);

RUVIA_MODEL(RegisterRequest,
    RUVIA_FIELD_NAME("user_name", username, ruvia::String, RUVIA_DEFAULT("guest")),
    RUVIA_FIELD(password, ruvia::String),
    RUVIA_FIELD(code, ruvia::String),
    RUVIA_FIELD(profile, ProfileRequest),
    RUVIA_FIELD(roles, ruvia::Array<RoleRequest>),
    RUVIA_FIELD(tags, ruvia::Array<ruvia::String>, RUVIA_OMIT_EMPTY),
    RUVIA_FIELD(newsletter, ruvia::Bool, RUVIA_EMIT_NULL)
);

RUVIA_MODEL(RegisterResponse,
    RUVIA_FIELD(username, ruvia::String),
    RUVIA_FIELD(roleCount, ruvia::UInt32),
    RUVIA_FIELD(tags, ruvia::Array<ruvia::String>)
);

RUVIA_MODEL(ContactForm,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(email, ruvia::String),
    RUVIA_FIELD(message, ruvia::String)
);

RUVIA_MODEL(Category,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(children, ruvia::List<Category>)
);

static bool hasRuviaCodePrefix(const ruvia::String& code) {
    return code.view().starts_with("CY-");
}

class ProfileValidator final : public ruvia::Middleware<ProfileValidator> {
public:
    RUVIA_VALIDATE_JSON(ProfileRequest,
        RUVIA_RULE(displayName,
            RUVIA_REQUIRED("display name is required"),
            RUVIA_MIN(2, "display name is too short"),
            RUVIA_MAX(64, "display name is too long")),
        RUVIA_RULE(email,
            RUVIA_REQUIRED("email is required"),
            RUVIA_EMAIL("email format is invalid")),
        RUVIA_RULE(age,
            RUVIA_MIN(0, "age is too small"),
            RUVIA_MAX(130, "age is too large")))
};

class RoleValidator final : public ruvia::Middleware<RoleValidator> {
public:
    RUVIA_VALIDATE_JSON(RoleRequest,
        RUVIA_RULE(name,
            RUVIA_REQUIRED("role is required"),
            RUVIA_ONE_OF("role is not allowed", "admin", "user", "editor")),
        RUVIA_RULE(level,
            RUVIA_MIN(1, "level is too small"),
            RUVIA_MAX(10, "level is too large")))
};

class RegisterValidator final : public ruvia::Middleware<RegisterValidator> {
public:
    RUVIA_VALIDATE_JSON(RegisterRequest,
        RUVIA_RULE_NAME("user_name", username,
            RUVIA_REQUIRED("username is required"),
            RUVIA_PATTERN("username format is invalid", "^[a-z][a-z0-9_]*$")),
        RUVIA_RULE(password,
            RUVIA_REQUIRED("password is required"),
            RUVIA_MIN(8, "password is too short")),
        RUVIA_RULE(code,
            RUVIA_CUSTOM("code must use CY- prefix", hasRuviaCodePrefix)),
        RUVIA_RULE(profile,
            RUVIA_REQUIRED("profile is required"),
            RUVIA_NESTED(ProfileValidator)),
        RUVIA_RULE(roles,
            RUVIA_REQUIRED("at least one role is required"),
            RUVIA_MIN(1, "too few roles"),
            RUVIA_MAX(5, "too many roles"),
            RUVIA_EACH(RoleValidator)))
};

class ContactFormValidator final : public ruvia::Middleware<ContactFormValidator> {
public:
    RUVIA_VALIDATE_FORM(ContactForm,
        RUVIA_RULE(name,
            RUVIA_REQUIRED("name is required"),
            RUVIA_MIN(2, "name is too short")),
        RUVIA_RULE(email,
            RUVIA_REQUIRED("email is required"),
            RUVIA_EMAIL("email format is invalid")),
        RUVIA_RULE(message,
            RUVIA_REQUIRED("message is required"),
            RUVIA_MIN(10, "message is too short")))
};

class ModelController final : public ruvia::Controller<ModelController> {
public:
    RUVIA_CONTROLLER_GROUP("/models")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/register", registerUser, RegisterValidator);
    RUVIA_POST("/contact", contact, ContactFormValidator);
    RUVIA_GET("/category", category);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> registerUser(ruvia::Context& c) {
        const auto& request = c.valid<RegisterRequest>();

        RegisterResponse response(c);
        response.username(request.username()->view());
        if (request.roles()) {
            response.roleCount(ruvia::UInt32{static_cast<std::uint32_t>(request.roles()->size())});
        }
        response.tags().emplace_back(ruvia::String("created", c.resource()));
        response.tags().emplace_back(ruvia::String("validated", c.resource()));
        co_return c.json(response, 201);
    }

    ruvia::Task<ruvia::HttpResponse> contact(ruvia::Context& c) {
        const auto& form = c.valid<ContactForm>(ruvia::Form);
        std::pmr::string body(c.allocator<char>());
        body.append("message from ");
        body.append(form.name()->view());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> category(ruvia::Context& c) {
        Category root(c);
        root.name("root");
        root.children().emplace().name("leaf");
        co_return c.json(root);
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8081)
        .setThreadNum(2)
        .run();
}
