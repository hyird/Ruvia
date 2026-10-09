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

#include "ruvia/web/model.h"
#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/model_object.h"
#include "ruvia/web/validation.h"

RUVIA_MODEL(import_record,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(labels, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(metadata, ruvia::json_object));

int main() {
    try {
        // Every model/result using this allocator must die before the pool.
        // In a handler, use c.arena() for request data and c.pool() for reusable
        // temporary storage instead of creating another allocator per request.
        std::pmr::unsynchronized_pool_resource memory;
        std::string input = R"({"name":"sample","count":7,"ratio":1.25,"enabled":true,"labels":["a","b"],"metadata":{"revision":2}})";
        auto owned = ruvia::from_json<import_record>(input, {.resource_ = &memory});
        if (!owned) {
            throw std::runtime_error("invalid record JSON");
        }
        {
            // parse() borrows input even when a PMR resource is supplied.
            // Keep input unchanged and keep nested views inside this scope.
            auto object = ruvia::json_object::parse(input, {.resource_ = &memory});
            if (!object || !object->for_each_field([](std::string_view name, const ruvia::json_value& value) {
                    std::cout << name << "=" << value.view() << '\n';
                    return true;  // false intentionally stops traversal.
                })) {
                throw std::runtime_error("object traversal did not complete");
            }
            auto labels = object->get<ruvia::json_value>("labels");
            if (!labels || !labels->for_each_element([](const ruvia::json_value& element) {
                    const auto label = element.get<ruvia::string>();
                    if (!label) {
                        return false;
                    }
                    std::cout << "label=" << label->view() << '\n';
                    return true;
                })) {
                throw std::runtime_error("labels must be an array of strings");
            }
        }
        // from_json() owns strings and dynamic tokens, unlike parse() above.
        input.assign("the original buffer is now reused");
        ruvia::validator validation({.resource_ = &memory});
        // Validator accepts optional values, just like query/header lookups.
        const std::optional<std::string_view> name{owned->get<"name">().view()};
        const std::optional<std::int64_t> count{static_cast<std::int64_t>(owned->get<"count">())};
        validation.required(name, "name").min_length(name, "name", 2).range(count, "count", 1, 100);
        validation.throw_if_invalid();
        // Parsing, field/business validation, and serialization are separate
        // operations. to_json() never silently validates or applies defaults.
        std::cout << ruvia::to_json(*owned, {.resource_ = &memory}) << '\n';

        auto form = ruvia::from_form<import_record>("name=form+record&count=8", {.resource_ = &memory});
        if (!form) {
            throw std::runtime_error("invalid form record");
        }
        std::cout << ruvia::to_json(*form, {.resource_ = &memory}) << '\n';

        // Scalar and array codecs do not require a wrapper model. Integer
        // conversion is exact: no fraction, overflow, or trailing junk.
        auto numbers = ruvia::from_json<ruvia::array<ruvia::uint64>>("[1,2,3]", {.resource_ = &memory});
        if (!numbers || ruvia::from_json<ruvia::uint8>("256", {.resource_ = &memory})) {
            throw std::runtime_error("integer bounds were not enforced");
        }
        std::cout << ruvia::to_json(*numbers, {.resource_ = &memory}) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
