#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

namespace ruvia {

// UTF-8 text describing an error code, for every Ruvia message that embeds one.
// On Windows, message() of std::system_category() and asio::system_category()
// is FormatMessageA output in the process ANSI code page (for example GBK), which
// would corrupt UTF-8 responses, JSON and logs; codes of those categories are
// therefore formatted with FormatMessageW and converted to UTF-8, or described
// as "<category>:<value>" when the system has no message. Every other category,
// and every category on POSIX, uses message() unchanged.
[[nodiscard]] std::string error_code_text(const std::error_code& error);

// what() of a std::system_error with its error-code text produced by
// error_code_text. The supported standard libraries build what() as
// "<what_arg>: <code().message()>"; when what() does not end with that
// message, only error_code_text(code()) is returned.
[[nodiscard]] std::string system_error_text(const std::system_error& error);
// The last resort for a failure that has nowhere else to go: a detached task or
// a callback threw, no application sink was configured to receive it, and no
// caller will ever rethrow it. Writing one line to stderr is the only remaining
// way to keep it observable.
//
// Discarding it instead would make the fault undiagnosable, and terminating
// would turn one local failure into a whole-process one. Every layer routes its
// unowned failures here so a Ruvia process never fails silently, and so the
// diagnostic looks the same wherever it came from:
//
//     ruvia: <context> failed: <what>
//
// `context` names the failing unit ("web connection", "event loop stop callback").
// Writing is best effort -- stderr may be closed, full, or redirected -- but
// nothing beyond this point could report the failure either way.
//
// A lifecycle owner that rethrows one failure among several (application::run)
// reports only the ones it does not rethrow; the caller owns the rethrown one.
//
// Rate limited: a fault that fails every connection at once would otherwise
// bury the first, informative failure under its consequences, and a blocked
// stderr would slow the recovery it is describing. Excess reports are counted
// and the count travels with the next line that gets through, so the volume is
// bounded without the flood becoming invisible. An application that wants every
// failure should install its layer's sink instead of relying on this.
//
// `occurrences` > 1 reports that many equivalent failures (see
// equivalent_failures) as one line, so one root cause shared by several units
// is not repeated once per unit.
//
// A std::system_error is described by system_error_text,
// so the line stays UTF-8 on Windows.
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences = 1) noexcept;

// True for the same exception object, or for two std::exception objects of the
// same dynamic type with the same what(): the same fault seen by several units.
[[nodiscard]] bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept;

}  // namespace ruvia
