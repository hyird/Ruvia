#include "ruvia/app/Dotenv.h"

#include "DotenvInternal.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/HttpAsciiCase.h"
#include "detail/HttpPmrObject.h"

namespace ruvia {
namespace {

template <typename Variables>
[[nodiscard]] auto findVariableSlot(Variables& variables, std::string_view name) noexcept {
    return std::lower_bound(
        variables.begin(),
        variables.end(),
        name,
        [](const detail::EnvVariable& variable, std::string_view key) {
            return std::string_view(variable.name).compare(key) < 0;
        });
}

}  // namespace

Env::Env()
    : state_(detail::constructHttpPmrObject<detail::EnvState>(detail::appResource())) {}

Env::~Env() = default;

void detail::EnvStateDeleter::operator()(EnvState* state) const noexcept {
    destroyHttpPmrObject(state, detail::appResource());
}

std::optional<std::string_view> Env::get(std::string_view name) const noexcept {
    const auto& variables = state_->variables;
    const auto it = findVariableSlot(variables, name);
    if (it == variables.end() || std::string_view(it->name) != name) {
        return std::nullopt;
    }

    return std::string_view(it->value);
}

std::optional<bool> Env::parseBoolValue(std::string_view value) noexcept {
    if (value == "1") {
        return true;
    }
    if (value == "0") {
        return false;
    }

    // ASCII-only case fold via the shared owner: the boolean tokens are ASCII, and
    // std::tolower is locale-dependent (a non-"C" LC_CTYPE set by the host app could
    // fold bytes unexpectedly).
    if (detail::httpAsciiEqualsIgnoreCase(value, "true") ||
        detail::httpAsciiEqualsIgnoreCase(value, "yes") ||
        detail::httpAsciiEqualsIgnoreCase(value, "on")) {
        return true;
    }
    if (detail::httpAsciiEqualsIgnoreCase(value, "false") ||
        detail::httpAsciiEqualsIgnoreCase(value, "no") ||
        detail::httpAsciiEqualsIgnoreCase(value, "off")) {
        return false;
    }

    return std::nullopt;
}

bool Env::loaded() const noexcept {
    return state_->loaded;
}

std::size_t Env::size() const noexcept {
    return state_->variables.size();
}

DotenvResult detail::loadEnvFromExecutableDirectory(Env& env, DotenvOptions options) {
    return detail::loadEnvFromFile(env, detail::dotenvExecutableDirectory() / ".env", options);
}

DotenvResult detail::loadEnvFromFile(Env& env, const std::filesystem::path& path, DotenvOptions options) {
    std::ifstream probe(path);
    if (!probe) {
        if (options.required) {
            throw std::runtime_error("dotenv file not found: " + path.string());
        }
        return detail::DotenvResultAccess::make(false);
    }
    probe.close();

    const auto entries = detail::readDotenvEntries(path);
    auto result = detail::DotenvResultAccess::make(true);
    auto& state = detail::EnvAccess::state(env);

    for (const auto& entry : entries) {
        auto& variables = state.variables;
        auto it = findVariableSlot(variables, entry.name);
        if (it != variables.end() && std::string_view(it->name) == std::string_view(entry.name)) {
            if (!options.overrideExisting) {
                detail::DotenvResultAccess::incrementVariablesSkipped(result);
                continue;
            }

            it->value = entry.value;
            detail::DotenvResultAccess::incrementVariablesSet(result);
            continue;
        }

        detail::EnvVariable variable;
        variable.name.assign(entry.name.data(), entry.name.size());
        variable.value.assign(entry.value.data(), entry.value.size());
        variables.insert(it, std::move(variable));
        detail::DotenvResultAccess::incrementVariablesSet(result);
    }

    state.loaded = true;
    return result;
}

}  // namespace ruvia
