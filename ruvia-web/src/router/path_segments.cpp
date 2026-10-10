#include "router/path_segments.h"

namespace ruvia::detail {

bool split_path_segment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept {
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
        if (path.empty()) {
            segment = {};
            rest = {};
            return false;
        }
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash);  // keep the leading '/' so empty segments survive
    return true;
}

}  // namespace ruvia::detail
