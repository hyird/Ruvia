#include "ruvia/web/Dotenv.h"

#include "ruvia/web/detail/app/EnvState.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/core/memory/PmrObject.h"

namespace ruvia {
namespace {

template <typename Variables>
[[nodiscard]] auto findVariableSlot(Variables& variables, std::string_view name) noexcept {
    return std::ranges::lower_bound(variables, name, std::ranges::less{}, [](const detail::EnvVariable& variable) noexcept { return std::string_view(variable.name); });
}

}  // namespace

Env::Env()
    : state_(detail::constructPmrObject<detail::EnvState>(detail::appResource())) {}

Env::~Env() = default;

void Env::StateDeleter::operator()(detail::EnvState* state) const noexcept {
    detail::destroyPmrObject(state, detail::appResource());
}

std::optional<std::string_view> Env::get(std::string_view name) const& noexcept {
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
    if (detail::httpAsciiEqualsIgnoreCase(value, "true") || detail::httpAsciiEqualsIgnoreCase(value, "yes") || detail::httpAsciiEqualsIgnoreCase(value, "on")) {
        return true;
    }
    if (detail::httpAsciiEqualsIgnoreCase(value, "false") || detail::httpAsciiEqualsIgnoreCase(value, "no") || detail::httpAsciiEqualsIgnoreCase(value, "off")) {
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
    if (std::ifstream probe(path); !probe) {
        if (options.required) {
            throw std::runtime_error("dotenv file not found: " + path.string());
        }
        return detail::DotenvResultAccess::make(false);
    }

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
