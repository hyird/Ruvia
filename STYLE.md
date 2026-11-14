# Style

Mechanical rules for C++ in this repository. Architecture, layering, and
performance contracts live in `AGENTS.md`; do not repeat them here.

Format with `clang-format` (managed by `mise`) using the root `.clang-format` (`Standard: c++20`). Do not re-wrap to a
column limit: `ColumnLimit` stays 0.

```bash
git ls-files '*.h' '*.hpp' '*.inl' '*.cpp' | xargs -n 80 clang-format -i
clang-format --dry-run --Werror path/to/file.cpp
```

## Includes

Project headers use quotes. The C++ standard library and third-party headers
use angle brackets:

```cpp
#include "task.h"

#include <algorithm>
#include <string_view>

#include <asio/post.hpp>
```

In a `.cpp` file, order groups as:

1. The header that declares this translation unit, if it has one.
2. C++ standard headers, then C system headers.
3. Third-party headers (`asio/`, `openssl/`, …).
4. Other project headers.

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

`AGENTS.md` §3 decides which outcomes are values and which throw; this section
fixes the shapes. Use one shape per API family:

| outcome | Shape | example |
| --- | --- | --- |
| Normal protocol progress | status enum / progress result | need input, queued, backpressured, completed |
| Data, format, or quota error from untrusted input | returned value: `std::optional<t_type>`, status enum, or result variant | malformed peer field, body over its limit |
| Program or configuration error, allocation failure | exception | invalid local argument or option, `std::bad_alloc` |
| Lifecycle contract violation | `std::terminate()` | destroying a started `task` |

Checked conversions and parsers at a trust boundary are the only operations
with two entries: the constructor throws (`explicit` unless implicit
conversion is the documented contract), and a `noexcept` static
`try_from_value(...)` (`try_parse(...)` for wire text) returns
`std::optional<t_type>`. No other operation gets a second throwing/non-throwing
variant, and no named throwing factory duplicates the constructor.

Queuing and backpressure states are not failures and stay in a status enum.
Result variants place the value (or `std::monostate`) first and the error
second; returning one does not promise `noexcept` or translate allocation
failures into its error type.

Throw a purpose-designed exception class, or a standard class such as
`std::invalid_argument` when it fully expresses the error; throw by value and
catch by `const&`. Do not throw integers or string literals, silently swallow
failures, or use exceptions for normal flow control.

Use RAII for cleanup during exception propagation. Destructors, deallocation,
and `swap` must not throw; mark operations `noexcept` when throwing is
impossible or unacceptable. Catch at a boundary that can handle or translate
the failure; objects it retains must still satisfy their invariants. A
lifecycle violation terminates because unwinding cannot safely retire a
suspended operation.

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
- Standard-library customization points, third-party APIs, and protocol tokens
  retain their required spelling; these are not project-owned identifiers.
- Files describe the unit, not the directory (`http_multipart.cpp`, not `unit_http_multipart.cpp`).

Apply this style to new identifiers and whenever existing identifiers are
renamed. Migrate an existing API family together with its definitions, callers,
tests, and documentation; do not add compatibility aliases or a second spelling
of the same operation. References to concrete APIs retain their actual spelling
until that API is migrated.

Do not hand-wrap enumerators to a column limit. `ColumnLimit` is 0, so
clang-format owns whether a short enum stays on one line.
