#include "ruvia/app/Dotenv.h"

#include "DotenvInternal.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ruvia {

std::pmr::vector<Env::Variable>::const_iterator Env::findVariable(std::string_view name) const noexcept {
    const auto it = std::lower_bound(
        variables_.begin(),
        variables_.end(),
        name,
        [](const Variable& variable, std::string_view key) {
            return std::string_view(variable.name).compare(key) < 0;
        });

    if (it == variables_.end() || std::string_view(it->name) != name) {
        return variables_.end();
    }
    return it;
}

std::pmr::vector<Env::Variable>::iterator Env::findInsertPosition(std::string_view name) noexcept {
    return std::lower_bound(
        variables_.begin(),
        variables_.end(),
        name,
        [](const Variable& variable, std::string_view key) {
            return std::string_view(variable.name).compare(key) < 0;
        });
}

std::optional<std::string_view> Env::get(std::string_view name) const noexcept {
    const auto it = findVariable(name);
    if (it == variables_.end()) {
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
    return loaded_;
}

std::size_t Env::size() const noexcept {
    return variables_.size();
}

DotenvResult Env::loadFromExecutableDirectory(DotenvOptions options) {
    return loadFromFile(detail::dotenvExecutableDirectory() / ".env", options);
}

DotenvResult Env::loadFromFile(const std::filesystem::path& path, DotenvOptions options) {
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

    for (const auto& entry : entries) {
        auto it = findInsertPosition(entry.name);
        if (it != variables_.end() && std::string_view(it->name) == std::string_view(entry.name)) {
            if (!options.overrideExisting) {
                ++result.variablesSkipped;
                continue;
            }

            it->value = entry.value;
            ++result.variablesSet;
            continue;
        }

        Variable variable;
        variable.name.assign(entry.name.data(), entry.name.size());
        variable.value.assign(entry.value.data(), entry.value.size());
        variables_.insert(it, std::move(variable));
        ++result.variablesSet;
    }

    loaded_ = true;
    return result;
}

}  // namespace ruvia
