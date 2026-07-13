#include "ruvia/web/detail/app/DotenvInternal.h"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>

namespace ruvia::detail {
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

    // Explicit ASCII checks rather than std::isalpha/isalnum: an environment
    // variable name is ASCII by definition, whereas the <cctype> predicates are
    // locale-dependent (a non-"C" LC_CTYPE set by the host app could admit high
    // bytes into a key). This also matches how the rest of the codebase validates
    // identifiers (isValidSessionId, isValidConfigHost, ...).
    const auto isAsciiAlpha = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!(isAsciiAlpha(key.front()) || key.front() == '_')) {
        return false;
    }

    return std::ranges::all_of(key.substr(1), [&isAsciiAlpha](char value) {
        return isAsciiAlpha(value) || (value >= '0' && value <= '9') || value == '_';
    });
}

[[nodiscard]] std::pmr::string locationMessage(
    const std::filesystem::path& path,
    std::size_t lineNumber,
    std::string_view message) {
    std::pmr::string result("invalid dotenv entry in ", appResource());
    result += path.string();
    result += ':';
    result += std::to_string(lineNumber);
    result += ": ";
    result += message;
    return result;
}

[[nodiscard]] std::pmr::string parseDoubleQuotedValue(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t lineNumber,
    std::size_t& consumed) {
    std::pmr::string result(appResource());
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
    return std::pmr::string(value.substr(1, close - 1), appResource());
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

    return std::pmr::string(trim(value.substr(0, end)), appResource());
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

}  // namespace

std::pmr::vector<DotenvEntry> readDotenvEntries(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::pmr::vector<DotenvEntry> entries(appResource());
    std::pmr::string line(appResource());
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

}  // namespace ruvia::detail
