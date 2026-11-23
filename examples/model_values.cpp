// Model values outside a request: owning codecs, borrowed JSON traversal,
// typed scalars/arrays, form decoding, and explicit business validation.
// Run ruvia_example_model_values; it prints traversed fields and serialized JSON
// and exits. No server, database, or network connection is required.

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_error.h"
#include "json_codec.h"
#include "model.h"
RUVIA_MODEL(import_record, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(count, std::int64_t), RUVIA_OPTIONAL_FIELD(ratio, double), RUVIA_OPTIONAL_FIELD(enabled, bool), RUVIA_OPTIONAL_FIELD(labels, ruvia::array<ruvia::string>), RUVIA_OPTIONAL_FIELD(metadata, ruvia::json_object));

namespace {
void print_issues(const std::vector<ruvia::validation_issue>& issues) {
    for (const auto& issue : issues) {
        std::cerr << issue.field() << ": " << issue.code() << ": " << issue.message() << '\n';
    }
}

void print_parse_error(const ruvia::model_parse_error& error) {
    std::cerr << error.message() << '\n';
    print_issues(error.issues());
}
}  // namespace

int main() {
    try {
        std::string input(R"({"name":"sample","count":7,"ratio":1.25,"enabled":true,"labels":["a","b"],"metadata":{"revision":2}})");
        auto parsed = ruvia::parse_json<import_record>(input);
        if (!parsed.has_value()) {
            print_parse_error(parsed.error());
            return 1;
        }
        auto owned = std::move(parsed).take_value();
        {
            // parse() borrows input. Keep input unchanged and keep nested
            // views inside this scope.
            auto parsed_object = ruvia::json_object::parse(input);
            if (!parsed_object.has_value()) {
                print_parse_error(parsed_object.error());
                return 1;
            }
            auto object = std::move(parsed_object).take_value();
            if (!object.for_each_field([](std::string_view name, const ruvia::json_value& value) {
                std::cout << name << "=" << value.view() << '\n';
                return true;  // false intentionally stops traversal.
            })) {
                std::cerr << "object traversal did not complete\n";
                return 1;
            }
            // get(name) reports absent / found / repeated; a repeated name is
            // never resolved to one of its occurrences.
            const auto labels = object.get<ruvia::json_value>("labels");
            if (labels.status() != ruvia::unique_lookup_status::found) {
                std::cerr << "labels must appear exactly once\n";
                return 1;
            }
            if (!labels.value() || !labels.value()->for_each_element([](const ruvia::json_value& element) {
                const auto label = element.get<ruvia::string>();
                if (!label) {
                    return false;
                }
                std::cout << "label=" << label->view() << '\n';
                return true;
            })) {
                std::cerr << "labels must be an array of strings\n";
                return 1;
            }
        }
        // parse_json<T>() owns strings and dynamic tokens, unlike parse() above.
        input.assign("the original buffer is now reused");
        ruvia::validator validation;
        // Validator accepts optional values, just like query/header lookups.
        const std::optional<std::string_view> name{owned.get<"name">().view()};
        const std::optional<std::int64_t> count{owned.get<"count">()};
        validation.required(name, "name").min_length(name, "name", 2).range(count, "count", 1, 100);
        if (!validation.ok()) {
            print_issues(validation.issues());
            return 1;
        }
        // Field validation runs in parse_json/parse_form; business rules can also
        // use validator directly. to_json() never silently validates or applies defaults.
        std::cout << ruvia::to_json(owned) << '\n';

        auto parsed_form = ruvia::parse_form<import_record>("name=form+record&count=8");
        if (!parsed_form.has_value()) {
            print_parse_error(parsed_form.error());
            return 1;
        }
        auto form = std::move(parsed_form).take_value();
        std::cout << ruvia::to_json(form) << '\n';

        // Scalar and array codecs do not require a wrapper model. Integer
        // conversion is exact: no fraction, overflow, or trailing junk.
        auto parsed_numbers = ruvia::parse_json<ruvia::array<std::uint64_t>>("[1,2,3]");
        if (!parsed_numbers.has_value()) {
            print_parse_error(parsed_numbers.error());
            return 1;
        }
        auto numbers = std::move(parsed_numbers).take_value();
        const auto overflow = ruvia::parse_json<std::uint8_t>("256");
        if (overflow.has_value()) {
            std::cerr << "integer bounds were not enforced\n";
            return 1;
        }
        print_parse_error(overflow.error());
        std::cout << ruvia::to_json(numbers) << '\n';
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
