# Style

Mechanical rules for C++ in this repository. Architecture, layering, and
performance contracts live in `AGENTS.md`; do not repeat them here.

Format with clang-format 21 using the root `.clang-format` (`Standard: Latest`,
which is the C++23 language dialect this formatter accepts). Do not re-wrap to a
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

## Errors

Use one shape per layer. Do not mix them on one API family.

| Layer | Shape | Example |
| --- | --- | --- |
| Protocol hot path | `enum class *Status` / `*Result` | `Http2SubmitStatus`, `Http2FeedResult` |
| Pure function that can fail | `std::expected<T, E>` | `parseMultipartBody()`, `encodeHttpContent()` |
| Application / configuration | exception | `std::invalid_argument` at startup, `DbError` |
| Contract violation | `std::terminate()` | destroying a started `Task` |

A status enum that includes `kQueued` or `kBackpressured` is not a failure and
stays an enum. `std::expected` is for operations that either produce a value or
fail.

## Names

- Types: `PascalCase`.
- Enumerators: `k` + `PascalCase` (`kAccepted`, `kNeedInput`).
- Data members: trailing underscore (`worker_`).
- Files describe the unit, not the directory (`hpack.cpp`, not `unit_hpack.cpp`).

Do not hand-wrap enumerators to a column limit. `ColumnLimit` is 0, so
clang-format owns whether a short enum stays on one line.
