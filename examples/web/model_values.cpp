// Model values outside a request: owning codecs, borrowed JSON traversal,
// typed scalars/arrays, form decoding, and explicit business validation.
// Run ruvia_example_model_values; it prints traversed fields and serialized JSON
// and exits. No server, database, or network connection is required.

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"
RUVIA_MODEL(import_record,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(labels, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(metadata, ruvia::json_object));

int main() {
    try {
        std::string input(R"({"name":"sample","count":7,"ratio":1.25,"enabled":true,"labels":["a","b"],"metadata":{"revision":2}})");
        auto owned = ruvia::from_json<import_record>(input);
        {
            // parse() borrows input. Keep input unchanged and keep nested
            // views inside this scope.
            auto object = ruvia::json_object::parse(input);
            if (!object.for_each_field([](std::string_view name, const ruvia::json_value& value) {
                    std::cout << name << "=" << value.view() << '\n';
                    return true;  // false intentionally stops traversal.
                })) {
                throw std::runtime_error("object traversal did not complete");
            }
            // get(name) reports absent / found / repeated; a repeated name is
            // never resolved to one of its occurrences.
            const auto labels = object.get<ruvia::json_value>("labels");
            if (labels.status() != ruvia::unique_lookup_status::found) {
                throw std::runtime_error("labels must appear exactly once");
            }
            if (!labels.value() || !labels.value()->for_each_element([](const ruvia::json_value& element) {
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
        ruvia::validator validation;
        // Validator accepts optional values, just like query/header lookups.
        const std::optional<std::string_view> name{owned.get<"name">().view()};
        const std::optional<std::int64_t> count{static_cast<std::int64_t>(owned.get<"count">())};
        validation.required(name, "name").min_length(name, "name", 2).range(count, "count", 1, 100);
        validation.throw_if_invalid();
        // Field validation runs in from_json/from_form; business rules can also
        // use validator directly. to_json() never silently validates or applies defaults.
        std::cout << ruvia::to_json(owned) << '\n';

        auto form = ruvia::from_form<import_record>("name=form+record&count=8");
        std::cout << ruvia::to_json(form) << '\n';

        // Scalar and array codecs do not require a wrapper model. Integer
        // conversion is exact: no fraction, overflow, or trailing junk.
        auto numbers = ruvia::from_json<ruvia::array<ruvia::uint64>>("[1,2,3]");
        bool overflow_rejected = false;
        try {
            (void)ruvia::from_json<ruvia::uint8>("256");
        } catch (const std::invalid_argument&) {
            overflow_rejected = true;
        }
        if (!overflow_rejected) {
            throw std::runtime_error("integer bounds were not enforced");
        }
        std::cout << ruvia::to_json(numbers) << '\n';
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
