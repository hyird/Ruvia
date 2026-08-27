#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

// Walking SQL text well enough to tell code from data.
//
// Two callers need the same distinction and got it wrong in the same way when
// each scanned for a single byte: the MariaDB parameter binder looked for '?'
// and the migration validator looks for a statement separator. A quoted
// literal, a quoted identifier and a comment can all contain either byte, and
// treating those occurrences as syntax silently shifts every parameter after
// them onto the wrong slot.
//
// This is deliberately not a SQL parser. It knows exactly the constructs that
// can hide a byte from the scanner -- '...' and "..." literals, `...`
// identifiers, "--"/"#" line comments and block comments -- and steps over each
// one atomically. Anything else advances a single byte.
//
// Backslash escapes are honoured inside quoted runs. Under the server's
// NO_BACKSLASH_ESCAPES mode a trailing backslash is data instead, so a literal
// ending in one is over-consumed here; that direction only ever loses a
// placeholder, which the caller reports as a count mismatch. The opposite
// choice would end the literal early and bind a parameter into the middle of a
// string, which is the failure worth ruling out.

[[nodiscard]] inline constexpr bool isSqlWhitespace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

[[nodiscard]] inline constexpr bool isMariaDbDoubleDashComment(
    std::string_view sql, std::size_t index) noexcept {
    if (index + 2 >= sql.size() || sql[index] != '-' || sql[index + 1] != '-') {
        return false;
    }
    const auto byte = static_cast<unsigned char>(sql[index + 2]);
    return byte <= 0x20 || byte == 0x7f;
}

[[nodiscard]] inline constexpr bool hasSqlNonWhitespace(std::string_view sql) noexcept {
    for (const auto character : sql) {
        if (!isSqlWhitespace(character)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::size_t skipSqlQuotedRun(
    std::string_view sql, std::size_t index, char terminator, bool backslashEscapes) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    auto cursor = index + 1;
    while (cursor < size) {
        const auto character = sql[cursor];
        if (backslashEscapes && character == '\\') {
            cursor += 2;
            continue;
        }
        if (character == terminator) {
            // A doubled terminator is one escaped terminator byte, not the end
            // of the run: 'a''b', "a""b" and `a``b` are single tokens.
            if (cursor + 1 < size && sql[cursor + 1] == terminator) {
                cursor += 2;
                continue;
            }
            return cursor + 1;
        }
        ++cursor;
    }
    return size;
}

[[nodiscard]] constexpr std::size_t skipSqlLineComment(
    std::string_view sql, std::size_t start) noexcept {
    auto cursor = start;
    while (cursor < sql.size() && sql[cursor] != '\n') {
        ++cursor;
    }
    return cursor < sql.size() ? cursor + 1 : sql.size();
}

[[nodiscard]] constexpr std::size_t skipSqlBlockComment(
    std::string_view sql, std::size_t index) noexcept {
    auto cursor = index + 2;
    while (cursor + 1 < sql.size()) {
        if (sql[cursor] == '*' && sql[cursor + 1] == '/') {
            return cursor + 2;
        }
        ++cursor;
    }
    return sql.size();
}

// The index just past the construct starting at `index`. For an ordinary byte
// that is `index + 1`; for a literal, identifier or comment it is the first
// index after its terminator, or `sql.size()` when the construct is unclosed.
[[nodiscard]] constexpr std::size_t skipSqlAtom(std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    switch (sql[index]) {
        case '\'':
        case '"':
            return skipSqlQuotedRun(sql, index, sql[index], true);
        case '`':
            return skipSqlQuotedRun(sql, index, '`', false);
        case '#':
            // MariaDB's second line-comment introducer. PostgreSQL has no '#'
            // comment and uses skipPostgreSqlSqlAtom() instead.
            return skipSqlLineComment(sql, index + 1);
        case '-':
            // MySQL/MariaDB accept "--" as a line-comment introducer only when
            // the second dash is followed by whitespace or a control byte.
            // Otherwise expressions such as "balance--1" and placeholders such
            // as "--?" stay statement-level SQL.
            if (isMariaDbDoubleDashComment(sql, index)) {
                return skipSqlLineComment(sql, index + 2);
            }
            return index + 1;
        case '/':
            if (index + 1 < size && sql[index + 1] == '*') {
                return skipSqlBlockComment(sql, index);
            }
            return index + 1;
        default:
            return index + 1;
    }
}

[[nodiscard]] constexpr bool isPostgreSqlDollarTagStart(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

[[nodiscard]] constexpr bool isPostgreSqlDollarTagContinue(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return isPostgreSqlDollarTagStart(character) || (byte >= '0' && byte <= '9');
}

// PostgreSQL dollar-quoted strings are opaque SQL data just like ordinary
// string literals, but their delimiter is either $$ or $tag$. Keep this scan
// separate from skipSqlAtom(): MariaDB's '?' binder shares that generic helper
// and must not treat its ordinary '$' identifiers as quoted strings.
[[nodiscard]] constexpr std::size_t skipPostgreSqlDollarQuotedAtom(
    std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size || sql[index] != '$') {
        return index < size ? index + 1 : size;
    }

    auto delimiterEnd = index + 1;
    if (delimiterEnd >= size) {
        return size;
    }
    if (sql[delimiterEnd] != '$') {
        if (!isPostgreSqlDollarTagStart(sql[delimiterEnd])) {
            return index + 1;
        }
        ++delimiterEnd;
        while (delimiterEnd < size && isPostgreSqlDollarTagContinue(sql[delimiterEnd])) {
            ++delimiterEnd;
        }
        if (delimiterEnd >= size || sql[delimiterEnd] != '$') {
            return index + 1;
        }
    }

    const auto delimiter = sql.substr(index, delimiterEnd - index + 1);
    const auto closing = sql.find(delimiter, delimiterEnd + 1);
    return closing == std::string_view::npos ? size : closing + delimiter.size();
}

[[nodiscard]] constexpr bool isPostgreSqlIdentifierContinue(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return isPostgreSqlDollarTagContinue(character) || byte == '$';
}

[[nodiscard]] constexpr bool hasPostgreSqlEscapeStringPrefix(
    std::string_view sql, std::size_t index) noexcept {
    if (index + 1 >= sql.size() || (sql[index] != 'E' && sql[index] != 'e') ||
        sql[index + 1] != '\'') {
        return false;
    }
    return index == 0 || !isPostgreSqlIdentifierContinue(sql[index - 1]);
}

// PostgreSQL differs from MariaDB in the constructs that can hide a statement
// separator: ordinary strings and quoted identifiers use doubled delimiters,
// not backslash escapes, and '#' is an operator byte rather than a comment
// opener. Escape strings keep the explicit E'...' backslash rule.
[[nodiscard]] constexpr std::size_t skipPostgreSqlSqlAtom(
    std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    if (hasPostgreSqlEscapeStringPrefix(sql, index)) {
        return skipSqlQuotedRun(sql, index + 1, '\'', true);
    }
    if (sql[index] == '$') {
        const auto next = skipPostgreSqlDollarQuotedAtom(sql, index);
        if (next != index + 1) {
            return next;
        }
    }

    switch (sql[index]) {
        case '\'':
        case '"':
            return skipSqlQuotedRun(sql, index, sql[index], false);
        case '`':
            // PostgreSQL does not use backtick identifiers, but stepping over a
            // MariaDB-style quoted identifier keeps backend-specific migrations
            // from being rejected just because an otherwise opaque identifier
            // contains the wanted byte.
            return skipSqlQuotedRun(sql, index, '`', false);
        case '-':
            if (index + 1 < size && sql[index + 1] == '-') {
                return skipSqlLineComment(sql, index + 2);
            }
            return index + 1;
        case '/':
            if (index + 1 < size && sql[index + 1] == '*') {
                return skipSqlBlockComment(sql, index);
            }
            return index + 1;
        default:
            return index + 1;
    }
}

// The index of the next `wanted` byte at statement level at or after `from`, or
// npos. Bytes inside literals, quoted identifiers and comments are data and are
// skipped whole. A byte that opens one of those constructs is that construct
// and is never reported as syntax.
[[nodiscard]] constexpr std::size_t findSqlSyntaxByte(
    std::string_view sql, char wanted, std::size_t from = 0) noexcept {
    for (auto index = from; index < sql.size();) {
        const auto next = skipSqlAtom(sql, index);
        if (next == index + 1 && sql[index] == wanted) {
            return index;
        }
        index = next;
    }
    return std::string_view::npos;
}

// PostgreSQL adds dollar-quoted strings to the generic opaque constructs. This
// variant is used by migration statement validation; the MariaDB parameter
// binder intentionally continues to use findSqlSyntaxByte().
[[nodiscard]] constexpr std::size_t findPostgreSqlSyntaxByte(
    std::string_view sql, char wanted, std::size_t from = 0) noexcept {
    for (auto index = from; index < sql.size();) {
        const auto next = skipPostgreSqlSqlAtom(sql, index);
        if (next == index + 1 && sql[index] == wanted) {
            return index;
        }
        index = next;
    }
    return std::string_view::npos;
}

}  // namespace ruvia::detail
