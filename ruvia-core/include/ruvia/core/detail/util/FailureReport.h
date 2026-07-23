#pragma once

#include <exception>
#include <string_view>

namespace ruvia::detail {

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
// `context` names the failing unit ("edge session", "event loop stop callback").
// Writing is best effort -- stderr may be closed, full, or redirected -- but
// nothing beyond this point could report the failure either way.
void reportUnhandledFailure(
    std::string_view context,
    std::exception_ptr exception) noexcept;

}  // namespace ruvia::detail
