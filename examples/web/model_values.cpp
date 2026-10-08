// Model values outside a request: owning codecs, borrowed JSON traversal,
// typed scalars/arrays, form decoding, and explicit business validation.
// Run ruvia_example_model_values; it prints traversed fields and serialized JSON
// and exits. No server, database, or network connection is required.

#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelForm.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/Validation.h"

RUVIA_MODEL(import_record,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_REQUIRED_FIELD(count, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(labels, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(metadata, ruvia::JsonObject));

int main() {
    try {
        // Every model/result using this allocator must die before the pool.
        // In a handler, use c.arena() for request data and c.pool() for reusable
        // temporary storage instead of creating another allocator per request.
        std::pmr::unsynchronized_pool_resource memory;
        std::string input = R"({"name":"sample","count":7,"ratio":1.25,"enabled":true,"labels":["a","b"],"metadata":{"revision":2}})";
        auto owned = ruvia::fromJson<import_record>(input, {.resource = &memory});
        if (!owned) {
            throw std::runtime_error("invalid record JSON");
        }
        {
            // parse() borrows input even when a PMR resource is supplied.
            // Keep input unchanged and keep nested views inside this scope.
            auto object = ruvia::JsonObject::parse(input, {.resource = &memory});
            if (!object || !object->forEachField([](std::string_view name, const ruvia::JsonValue& value) {
                    std::cout << name << "=" << value.view() << '\n';
                    return true;  // false intentionally stops traversal.
                })) {
                throw std::runtime_error("object traversal did not complete");
            }
            auto labels = object->get<ruvia::JsonValue>("labels");
            if (!labels || !labels->forEachElement([](const ruvia::JsonValue& element) {
                    const auto label = element.get<ruvia::String>();
                    if (!label) {
                        return false;
                    }
                    std::cout << "label=" << label->view() << '\n';
                    return true;
                })) {
                throw std::runtime_error("labels must be an array of strings");
            }
        }
        // fromJson() owns strings and dynamic tokens, unlike parse() above.
        input.assign("the original buffer is now reused");
        ruvia::Validator validation({.resource = &memory});
        // Validator accepts optional values, just like query/header lookups.
        const std::optional<std::string_view> name{owned->get<"name">().view()};
        const std::optional<std::int64_t> count{static_cast<std::int64_t>(owned->get<"count">())};
        validation.required(name, "name").minLength(name, "name", 2).range(count, "count", 1, 100);
        validation.throwIfInvalid();
        // Parsing, field/business validation, and serialization are separate
        // operations. toJson() never silently validates or applies defaults.
        std::cout << ruvia::toJson(*owned, {.resource = &memory}) << '\n';

        auto form = ruvia::fromForm<import_record>("name=form+record&count=8", {.resource = &memory});
        if (!form) {
            throw std::runtime_error("invalid form record");
        }
        std::cout << ruvia::toJson(*form, {.resource = &memory}) << '\n';

        // Scalar and array codecs do not require a wrapper model. Integer
        // conversion is exact: no fraction, overflow, or trailing junk.
        auto numbers = ruvia::fromJson<ruvia::Array<ruvia::UInt64>>("[1,2,3]", {.resource = &memory});
        if (!numbers || ruvia::fromJson<ruvia::UInt8>("256", {.resource = &memory})) {
            throw std::runtime_error("integer bounds were not enforced");
        }
        std::cout << ruvia::toJson(*numbers, {.resource = &memory}) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
