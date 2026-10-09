# Style

Mechanical rules for C++ in this repository. Architecture, layering, and
performance contracts live in `AGENTS.md`; do not repeat them here.

Format with clang-format 21 using the root `.clang-format` (`Standard: c++20`). Do not re-wrap to a
column limit: `ColumnLimit` stays 0.

```bash
git ls-files '*.h' '*.hpp' '*.inl' '*.cpp' | xargs -n 80 clang-format -i
clang-format --dry-run --Werror path/to/file.cpp
```

## Includes

Project headers use quotes. The C++ standard library and third-party headers
use angle brackets:

```cpp
#include "ruvia/core/Task.h"

#include <algorithm>
#include <string_view>

#include <asio/post.hpp>
```

In a `.cpp` file, order groups as:

1. The header that declares this translation unit, if it has one.
2. C++ standard headers, then C system headers.
3. Third-party headers (`asio/`, `openssl/`, …).
4. Other `ruvia/` headers.

clang-format regroups these blocks and sorts within each group. Do not insert
blank lines by hand inside a group.

Public headers include only what the header itself needs. Forward-declare
pointer or reference members whose definitions are not part of the application
API; keep those includes in the `.cpp` or in a `detail` access header.

`.inl` implementation fragments must include the headers they use. Do not rely
on sibling `.inl` include order to make a helper visible.

Shared test fixture headers define free helpers as `inline`, with attributes
before the declaration specifiers (`[[nodiscard]] inline`). Coroutine fixture
types belong in a named test namespace so GCC-generated coroutine frames do not
refer to anonymous-namespace types with translation-unit-local linkage.

## Errors

Use exceptions by default when a new operation cannot complete its assigned
task (E.2). Use a purpose-designed exception class, throw by value, and catch
by `const&`. Standard exception classes such as `std::invalid_argument` are
appropriate when they fully express the error; use a domain exception when
callers need domain-specific information. Do not throw integers or string
literals, silently swallow failures, or use exceptions for normal flow control.

Choose the mechanism by the meaning of the outcome, not by the layer. Protocol,
sans-I/O, parser, and pure-function APIs can throw for an operation failure.
Recoverability or untrusted input alone is not a reason to require a returned
error. Use one error-handling shape per API family:

| Outcome | Shape | Example |
| --- | --- | --- |
| Normal protocol progress | status enum / progress result | need input, queued, backpressured, completed |
| Operation cannot complete its assigned task | exception | invalid complete message, encoding failure, invalid configuration |
| Explicit requirement to return failure as data | `std::variant<T, E>` / typed error result | a boundary that must not unwind, or an API designed to inspect failure values |
| Unrecoverable lifecycle contract violation | `std::terminate()` | destroying a started `Task` |

Queuing and backpressure states are not failures and stay in a status enum.
An operation may return these normal states while throwing for a genuine
failure. Result variants place the value (or `std::monostate`) first and the error second.
Returning a result variant does not itself promise `noexcept` or require
allocation failures to be translated into its error type.

Returned-error contracts must state the concrete requirement that makes them
preferable to exceptions. Do not exempt an entire protocol or hot path from
exception handling solely because of its category, or make unsupported claims
about exception cost. Existing result-returning APIs retain their implemented
contract until an API family is migrated with its callers and tests; their
current shape is not a requirement for new APIs. Do not add a second
throwing/non-throwing variant of the same operation.

Use RAII for cleanup during exception propagation. Destructors, deallocation,
and `swap` must not throw; mark operations `noexcept` when throwing is impossible
or unacceptable. Catch at a boundary that can handle or translate the failure,
rather than adding `try` / `catch` to every function. A lifecycle contract
violation such as destroying a started `Task` remains terminal because unwinding
cannot safely retire its suspended operations. For a recoverable failure,
objects retained by the catching boundary must still satisfy their invariants.

## Names

Use lowercase `underscore_style` consistently (NL.8 / NL.10):

- Namespaces, types, concepts, and template parameters: `http_client`,
  `http_origin_view`, `serializable`, `value_type`.
- Functions, methods, variables, and parameters: `parse_multipart_body`,
  `worker_count`, `request`.
- Constants and scoped enumerators: `max_header_size`, `accepted`, `need_input`.
  Do not add a `k` prefix or use `ALL_CAPS`.
- Data members: trailing underscore (`worker_`, `request_count_`).
- Macros: `ALL_CAPS` (`RUVIA_MODEL`, `RUVIA_ROUTES_BEGIN`). Do not encode type
  information in names.
- Files describe the unit, not the directory (`hpack.cpp`, not `unit_hpack.cpp`).

Apply this style to new identifiers and whenever existing identifiers are
renamed. Migrate an existing API family together with its definitions, callers,
tests, and documentation; do not add compatibility aliases or a second spelling
of the same operation. References to concrete APIs retain their actual spelling
until that API is migrated.

Do not hand-wrap enumerators to a column limit. `ColumnLimit` is 0, so
clang-format owns whether a short enum stays on one line.
