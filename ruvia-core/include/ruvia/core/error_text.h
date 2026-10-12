#pragma once

#include <string>
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

}  // namespace ruvia
