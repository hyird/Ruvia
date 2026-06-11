#include "ruvia/app/Dotenv.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace ruvia {
namespace {

[[nodiscard]] bool isSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r';
}

[[nodiscard]] std::string_view trimLeft(std::string_view value) noexcept {
    while (!value.empty() && isSpace(value.front())) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trimRight(std::string_view value) noexcept {
    while (!value.empty() && isSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    return trimRight(trimLeft(value));
}

[[nodiscard]] bool isValidKey(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(key.front());
    if (!(std::isalpha(first) != 0 || key.front() == '_')) {
        return false;
    }

    return std::ranges::all_of(key.substr(1), [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return std::isalnum(c) != 0 || value == '_';
    });
}

[[nodiscard]] std::pmr::string locationMessage(
    const std::filesystem::path& path,
    std::size_t lineNumber,
    std::string_view message) {
    std::pmr::string result("invalid dotenv entry in ", ProcessMemory::instance().upstreamResource());
    result += path.string();
    result += ':';
    result += std::to_string(lineNumber);
    result += ": ";
    result += message;
    return result;
}

struct DotenvEntry final {
    std::pmr::string name{ProcessMemory::instance().upstreamResource()};
    std::pmr::string value{ProcessMemory::instance().upstreamResource()};
};

[[nodiscard]] std::pmr::string parseDoubleQuotedValue(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t lineNumber,
    std::size_t& consumed) {
    std::pmr::string result(ProcessMemory::instance().upstreamResource());
    result.reserve(value.size());

    for (std::size_t index = 1; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '"') {
            consumed = index + 1;
            return result;
        }

        if (current != '\\') {
            result.push_back(current);
            continue;
        }

        if (index + 1 == value.size()) {
            throw std::invalid_argument(locationMessage(path, lineNumber, "unfinished escape sequence").c_str());
        }

        const char escaped = value[++index];
        switch (escaped) {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            default:
                result.push_back(escaped);
                break;
        }
    }

    throw std::invalid_argument(locationMessage(path, lineNumber, "unterminated double-quoted value").c_str());
}

[[nodiscard]] std::pmr::string parseSingleQuotedValue(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t lineNumber,
    std::size_t& consumed) {
    const auto close = value.find('\'', 1);
    if (close == std::string_view::npos) {
        throw std::invalid_argument(locationMessage(path, lineNumber, "unterminated single-quoted value").c_str());
    }

    consumed = close + 1;
    return std::pmr::string(value.substr(1, close - 1), ProcessMemory::instance().upstreamResource());
}

void validateQuotedRemainder(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t lineNumber) {
    value = trimLeft(value);
    if (!value.empty() && value.front() != '#') {
        throw std::invalid_argument(locationMessage(path, lineNumber, "unexpected characters after quoted value").c_str());
    }
}

[[nodiscard]] std::pmr::string parseUnquotedValue(std::string_view value) {
    std::size_t end = value.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '#' && (index == 0 || isSpace(value[index - 1]))) {
            end = index;
            break;
        }
    }

    return std::pmr::string(trim(value.substr(0, end)), ProcessMemory::instance().upstreamResource());
}

[[nodiscard]] std::pmr::string parseValue(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t lineNumber) {
    value = trimLeft(value);
    if (value.empty()) {
        return {};
    }

    if (value.front() == '"') {
        std::size_t consumed = 0;
        auto parsed = parseDoubleQuotedValue(value, path, lineNumber, consumed);
        validateQuotedRemainder(value.substr(consumed), path, lineNumber);
        return parsed;
    }

    if (value.front() == '\'') {
        std::size_t consumed = 0;
        auto parsed = parseSingleQuotedValue(value, path, lineNumber, consumed);
        validateQuotedRemainder(value.substr(consumed), path, lineNumber);
        return parsed;
    }

    return parseUnquotedValue(value);
}

void stripUtf8Bom(std::string_view& line) noexcept {
    constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == bom[0] &&
        static_cast<unsigned char>(line[1]) == bom[1] &&
        static_cast<unsigned char>(line[2]) == bom[2]) {
        line.remove_prefix(3);
    }
}

[[nodiscard]] DotenvEntry parseEntry(
    std::string_view line,
    const std::filesystem::path& path,
    std::size_t lineNumber) {
    line = trim(line);
    if (line.starts_with("export") && line.size() > 6 && isSpace(line[6])) {
        line = trim(line.substr(6));
    }

    const auto separator = line.find('=');
    if (separator == std::string_view::npos) {
        throw std::invalid_argument(locationMessage(path, lineNumber, "missing '='").c_str());
    }

    const auto key = trim(line.substr(0, separator));
    if (!isValidKey(key)) {
        throw std::invalid_argument(locationMessage(path, lineNumber, "invalid variable name").c_str());
    }

    DotenvEntry entry;
    entry.name.assign(key.data(), key.size());
    entry.value = parseValue(line.substr(separator + 1), path, lineNumber);
    return entry;
}

[[nodiscard]] std::pmr::vector<DotenvEntry> readDotenvEntries(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::pmr::vector<DotenvEntry> entries(ProcessMemory::instance().upstreamResource());
    std::pmr::string line(ProcessMemory::instance().upstreamResource());
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;

        std::string_view view(line);
        if (lineNumber == 1) {
            stripUtf8Bom(view);
        }

        view = trim(view);
        if (view.empty() || view.front() == '#') {
            continue;
        }

        entries.push_back(parseEntry(view, path, lineNumber));
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read dotenv file: " + path.string());
    }

    return entries;
}

[[nodiscard]] std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(260, L'\0');
    for (;;) {
        const auto length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    std::pmr::vector<char> buffer(size, ProcessMemory::instance().upstreamResource());
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("failed to resolve executable path");
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.data())).parent_path();
#else
    std::pmr::vector<char> buffer(1024, ProcessMemory::instance().upstreamResource());
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

}  // namespace

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
    return loadFromFile(executableDirectory() / ".env", options);
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

    const auto entries = readDotenvEntries(path);
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
