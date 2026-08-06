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

// The index just past the construct starting at `index`. For an ordinary byte
// that is `index + 1`; for a literal, identifier or comment it is the first
// index after its terminator, or `sql.size()` when the construct is unclosed.
[[nodiscard]] constexpr std::size_t skipSqlAtom(std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    const auto quoted = [&](char terminator) noexcept {
        auto cursor = index + 1;
        while (cursor < size) {
            const auto character = sql[cursor];
            if (character == '\\' && terminator != '`') {
                cursor += 2;
                continue;
            }
            if (character == terminator) {
                // A doubled terminator is one escaped terminator byte, not the
                // end of the run: 'a''b' and `a``b` are single tokens.
                if (cursor + 1 < size && sql[cursor + 1] == terminator) {
                    cursor += 2;
                    continue;
                }
                return cursor + 1;
            }
            ++cursor;
        }
        return size;
    };

    const auto lineComment = [&](std::size_t start) noexcept {
        auto cursor = start;
        while (cursor < size && sql[cursor] != '\n') {
            ++cursor;
        }
        return cursor < size ? cursor + 1 : size;
    };

    switch (sql[index]) {
        case '\'':
        case '"':
        case '`':
            return quoted(sql[index]);
        case '#':
            // MariaDB's second line-comment introducer. PostgreSQL has no '#'
            // comment, but there it can only appear inside an operator or a
            // literal, and treating the rest of the line as opaque still never
            // reports syntax that is not there.
            return lineComment(index + 1);
        case '-':
            // "--" starts a comment only when the run really is two dashes; the
            // decrement operator does not exist in SQL, so no further lookahead
            // is needed.
            if (index + 1 < size && sql[index + 1] == '-') {
                return lineComment(index + 2);
            }
            return index + 1;
        case '/':
            if (index + 1 < size && sql[index + 1] == '*') {
                auto cursor = index + 2;
                while (cursor + 1 < size) {
                    if (sql[cursor] == '*' && sql[cursor + 1] == '/') {
                        return cursor + 2;
                    }
                    ++cursor;
                }
                return size;
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
[[nodiscard]] constexpr std::size_t skipPostgreSqlDollarQuotedAtom(std::string_view sql, std::size_t index) noexcept {
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

// The index of the next `wanted` byte at statement level at or after `from`, or
// npos. Bytes inside literals, quoted identifiers and comments are data and are
// skipped whole. A byte that opens one of those constructs is that construct
// and is never reported as syntax.
[[nodiscard]] constexpr std::size_t findSqlSyntaxByte(std::string_view sql, char wanted, std::size_t from = 0) noexcept {
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
[[nodiscard]] constexpr std::size_t findPostgreSqlSyntaxByte(std::string_view sql, char wanted, std::size_t from = 0) noexcept {
    for (auto index = from; index < sql.size();) {
        if (sql[index] == '$') {
            const auto next = skipPostgreSqlDollarQuotedAtom(sql, index);
            if (next != index + 1) {
                index = next;
                continue;
            }
        }
        const auto next = skipSqlAtom(sql, index);
        if (next == index + 1 && sql[index] == wanted) {
            return index;
        }
        index = next;
    }
    return std::string_view::npos;
}

}  // namespace ruvia::detail
