#pragma once

#include "ruvia/web/db/DbTypes.h"

#include <memory_resource>
#include <string>
#include <utility>

namespace ruvia::detail {

struct DbValueAccess final {
    [[nodiscard]] static DbValue ownedString(std::pmr::string value) {
        return DbValue(std::move(value));
    }
};

}  // namespace ruvia::detail
