#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia::test {

class scoped_temp_directory final {
public:
    explicit scoped_temp_directory(std::string_view prefix = "ruvia-test") {
        const std::filesystem::path root(RUVIA_TEST_TEMP_ROOT);
        std::filesystem::create_directories(root);
        std::random_device random;
        for (int attempt = 0; attempt != 100; ++attempt) {
            auto candidate = root / (std::string(prefix) + "-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create test directory");
    }

    scoped_temp_directory(const scoped_temp_directory&) = delete;
    scoped_temp_directory& operator=(const scoped_temp_directory&) = delete;

    ~scoped_temp_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

}  // namespace ruvia::test
