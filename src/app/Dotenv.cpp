#include "ruvia/app/Dotenv.h"

#include "DotenvInternal.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/memory/PmrObject.h"

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
    : state_(detail::constructPmrObject<detail::EnvState>(detail::appResource())) {}

Env::~Env() = default;

void detail::EnvStateDeleter::operator()(EnvState* state) const noexcept {
    destroyPmrObject(state, detail::appResource());
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

    const auto equalsIgnoreCase = [](std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto l = static_cast<unsigned char>(left[index]);
            const auto r = static_cast<unsigned char>(right[index]);
            if (std::tolower(l) != std::tolower(r)) {
                return false;
            }
        }

        return true;
    };

    if (equalsIgnoreCase(value, "true") ||
        equalsIgnoreCase(value, "yes") ||
        equalsIgnoreCase(value, "on")) {
        return true;
    }
    if (equalsIgnoreCase(value, "false") ||
        equalsIgnoreCase(value, "no") ||
        equalsIgnoreCase(value, "off")) {
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
        return {};
    }
    probe.close();

    const auto entries = detail::readDotenvEntries(path);
    DotenvResult result{.loaded = true};
    auto& state = detail::EnvAccess::state(env);

    for (const auto& entry : entries) {
        auto& variables = state.variables;
        auto it = findVariableSlot(variables, entry.name);
        if (it != variables.end() && std::string_view(it->name) == std::string_view(entry.name)) {
            if (!options.overrideExisting) {
                ++result.variablesSkipped;
                continue;
            }

            it->value = entry.value;
            ++result.variablesSet;
            continue;
        }

        detail::EnvVariable variable;
        variable.name.assign(entry.name.data(), entry.name.size());
        variable.value.assign(entry.value.data(), entry.value.size());
        variables.insert(it, std::move(variable));
        ++result.variablesSet;
    }

    state.loaded = true;
    return result;
}

}  // namespace ruvia
