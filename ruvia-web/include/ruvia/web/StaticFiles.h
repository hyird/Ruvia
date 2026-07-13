#pragma once

#include <filesystem>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

namespace ruvia {

class StaticRoot;

struct StaticMimeType final {
    std::pmr::string extension;
    std::pmr::string contentType;
};

struct StaticRootOptions final {
    std::pmr::string cacheControl;
    std::pmr::string indexFile;
    std::pmr::string defaultContentType{"application/octet-stream"};
    std::pmr::vector<StaticMimeType> mimeTypes;
    std::pmr::vector<std::pmr::string> fileTypes;
    bool allowAll{false};
    bool enableRanges{true};
    bool enableValidators{true};
};

namespace detail {

class StaticRootAccess;
struct StaticRootState;

}  // namespace detail

class StaticRoot final {
public:
    explicit StaticRoot(const std::filesystem::path& root, StaticRootOptions options = {});
    ~StaticRoot();

    StaticRoot(const StaticRoot&) = delete;
    StaticRoot& operator=(const StaticRoot&) = delete;
    StaticRoot(StaticRoot&&) = delete;
    StaticRoot& operator=(StaticRoot&&) = delete;

    [[nodiscard]] std::filesystem::path path() const;

private:
    struct StateDeleter final {
        void operator()(detail::StaticRootState* state) const noexcept;
    };

    friend class detail::StaticRootAccess;

    std::unique_ptr<detail::StaticRootState, StateDeleter> state_;
};

}  // namespace ruvia
