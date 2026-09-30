#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/Hpack.h"

#include "test_harness.h"

namespace {

class CountingAllocationResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t outstandingBytes() const noexcept {
        return outstandingBytes_;
    }

    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

    [[nodiscard]] std::size_t deallocations() const noexcept {
        return deallocations_;
    }

    [[nodiscard]] std::pmr::vector<std::size_t> liveAllocationIds() const {
        std::pmr::vector<std::size_t> ids(std::pmr::new_delete_resource());
        for (const auto& [pointer, allocation] : liveAllocationIds_) {
            ids.push_back(allocation.id);
        }
        return ids;
    }

    [[nodiscard]] bool retains(std::span<const std::size_t> ids) const noexcept {
        return std::ranges::all_of(ids, [this](std::size_t id) {
            return std::ranges::any_of(liveAllocationIds_, [id](const auto& allocation) {
                return allocation.second.id == id;
            });
        });
    }

    [[nodiscard]] std::size_t liveAllocationsOfSize(std::size_t bytes) const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            liveAllocationIds_, [bytes](const auto& allocation) {
                return allocation.second.bytes == bytes;
            }));
    }

    [[nodiscard]] bool onlyAlignment(std::size_t bytes, std::size_t alignment) const noexcept {
        return std::ranges::all_of(liveAllocationIds_, [=](const auto& allocation) {
            return allocation.second.bytes != bytes || allocation.second.alignment == alignment;
        });
    }

    bool reject{false};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        try {
            liveAllocationIds_.emplace(pointer, Allocation{allocations_ + 1, bytes, alignment});
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
            throw;
        }
        outstandingBytes_ += bytes;
        ++allocations_;
        return pointer;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        const auto found = liveAllocationIds_.find(pointer);
        if (found == liveAllocationIds_.end() || found->second.bytes != bytes ||
            found->second.alignment != alignment) {
            std::terminate();
        }
        liveAllocationIds_.erase(found);
        outstandingBytes_ -= bytes;
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    struct Allocation final {
        std::size_t id;
        std::size_t bytes;
        std::size_t alignment;
    };

    std::size_t outstandingBytes_{0};
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::pmr::unordered_map<void*, Allocation> liveAllocationIds_{std::pmr::new_delete_resource()};
};

class ToggleAllocationResource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

}  // namespace

RUVIA_TEST(hpack_public_dynamic_entries_inline_through_48_and_pack_long_fields_once) {
    CountingAllocationResource resource;
    {
        ruvia::HpackDecoder decoder({.resource = &resource});
        std::string warmEntries;
        for (char name = 'a'; name < 'i'; name += 2) {
            warmEntries.push_back(static_cast<char>(0x40));
            warmEntries.push_back('\x01');
            warmEntries.push_back(name);
            warmEntries.push_back('\x01');
            warmEntries.push_back(static_cast<char>(name + 1));
        }
        RUVIA_CHECK(decoder.decode(warmEntries, [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\x20", 1), [](auto, auto) { return true; }).decoded());
        std::pmr::string restoreLimit(std::pmr::new_delete_resource());
        ruvia::HpackEncoder::encodeDynamicTableSizeUpdate(restoreLimit, 4096);
        RUVIA_CHECK(decoder.decode(std::string_view(restoreLimit), [](auto, auto) { return true; }).decoded());

        const auto allocationsBeforeInline = resource.allocations();
        resource.reject = true;
        {
            std::string inlineName(24, 'n');
            std::string inlineValue(24, 'v');
            std::pmr::string inlineBlock(std::pmr::new_delete_resource());
            ruvia::HpackEncoder::encodeHeader(inlineBlock, inlineName, inlineValue);
            inlineBlock[0] = static_cast<char>(0x40);
            RUVIA_CHECK(
                decoder.decode(std::string_view(inlineBlock), [](auto, auto) { return true; }).decoded());
            std::fill(inlineBlock.begin(), inlineBlock.end(), '#');
            std::fill(inlineName.begin(), inlineName.end(), '#');
            std::fill(inlineValue.begin(), inlineValue.end(), '#');
        }
        RUVIA_CHECK_EQ(resource.allocations(), allocationsBeforeInline);
        resource.reject = false;
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto name, auto value) {
                               return name == std::string(24, 'n') && value == std::string(24, 'v');
                           })
                .decoded());

        {
            std::string heapName(24, 'a');
            std::string heapValue(25, 'b');
            std::pmr::string heapBlock(std::pmr::new_delete_resource());
            ruvia::HpackEncoder::encodeHeader(heapBlock, heapName, heapValue);
            heapBlock[0] = static_cast<char>(0x40);
            RUVIA_CHECK(
                decoder.decode(std::string_view(heapBlock), [](auto, auto) { return true; }).decoded());
            // Vector capacity was warmed above, so this exact-size allocation is
            // the sole retained block for the 49 field bytes.
            RUVIA_CHECK_EQ(resource.liveAllocationsOfSize(49), std::size_t{1});
            RUVIA_CHECK(resource.onlyAlignment(49, alignof(char)));
            std::fill(heapBlock.begin(), heapBlock.end(), '#');
            std::fill(heapName.begin(), heapName.end(), '#');
            std::fill(heapValue.begin(), heapValue.end(), '#');
        }
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto name, auto value) {
                               return name == std::string(24, 'a') && value == std::string(25, 'b');
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbf", 1), [](auto name, auto value) {
                               return name == std::string(24, 'n') && value == std::string(24, 'v');
                           })
                .decoded());
    }
    RUVIA_CHECK_EQ(resource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(hpack_public_empty_and_one_sided_fields_are_owned_and_indexable) {
    CountingAllocationResource resource;
    {
        ruvia::HpackDecoder decoder({.resource = &resource});
        const std::string longBytes(49, 'x');
        std::pmr::string block(std::pmr::new_delete_resource());
        ruvia::HpackEncoder::encodeHeader(block, "", "");
        block[0] = static_cast<char>(0x40);
        const auto secondField = block.size();
        ruvia::HpackEncoder::encodeHeader(block, "", longBytes);
        block[secondField] = static_cast<char>(0x40);
        const auto thirdField = block.size();
        ruvia::HpackEncoder::encodeHeader(block, longBytes, "");
        block[thirdField] = static_cast<char>(0x40);
        RUVIA_CHECK(decoder.decode(std::string_view(block), [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK_EQ(resource.liveAllocationsOfSize(49), std::size_t{2});
        std::fill(block.begin(), block.end(), '#');

        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [&](auto name, auto value) {
                               return name == longBytes && value.empty();
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbf", 1), [&](auto name, auto value) {
                               return name.empty() && value == longBytes;
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xc0", 1), [](auto name, auto value) {
                               return name.empty() && value.empty();
                           })
                .decoded());
    }
    RUVIA_CHECK_EQ(resource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(hpack_public_dynamic_table_storage_stays_bounded_across_evictions) {
    CountingAllocationResource resource;
    {
        ruvia::HpackDecoder decoder({.resource = &resource});
        const std::string sizeUpdate{static_cast<char>(0x3f), static_cast<char>(0x59)};
        RUVIA_CHECK(decoder.decode(sizeUpdate, [](auto, auto) { return true; }).decoded());

        const std::string name(40, 'n');
        const std::string value(40, 'v');
        std::string insertion(1, static_cast<char>(0x40));
        insertion.push_back(static_cast<char>(name.size()));
        insertion += name;
        insertion.push_back(static_cast<char>(value.size()));
        insertion += value;

        std::size_t peakBytes = resource.outstandingBytes();
        for (std::size_t i = 0; i < 200; ++i) {
            RUVIA_CHECK(decoder.decode(insertion, [](auto, auto) { return true; }).decoded());
            peakBytes = std::max(peakBytes, resource.outstandingBytes());
            RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                                   [&](std::string_view actualName, std::string_view actualValue) {
                                       return actualName == name && actualValue == value;
                                   })
                    .decoded());
        }
        RUVIA_CHECK(peakBytes < 4096);
    }
    RUVIA_CHECK_EQ(resource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(hpack_public_commit_releases_evicted_heap_strings_immediately) {
    CountingAllocationResource resource;
    ruvia::HpackDecoder decoder({.resource = &resource});
    const std::string name(1500, 'n');
    const std::string value(1500, 'v');
    std::pmr::string insertion(std::pmr::get_default_resource());
    ruvia::HpackEncoder::encodeHeader(insertion, name, value);
    insertion[0] = static_cast<char>(0x40);
    RUVIA_CHECK(decoder.decode(std::string_view(insertion), [](auto, auto) { return true; }).decoded());

    const auto bytesWithEntry = resource.outstandingBytes();
    const auto deallocationsWithEntry = resource.deallocations();
    RUVIA_CHECK_EQ(resource.liveAllocationsOfSize(name.size() + value.size()), std::size_t{1});
    std::pmr::string evict(std::pmr::get_default_resource());
    ruvia::HpackEncoder::encodeDynamicTableSizeUpdate(evict, 0);
    RUVIA_CHECK(decoder.decode(std::string_view(evict), [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(resource.outstandingBytes() <= bytesWithEntry - name.size() - value.size());
    RUVIA_CHECK_EQ(resource.liveAllocationsOfSize(name.size() + value.size()), std::size_t{0});
    RUVIA_CHECK(resource.deallocations() >= deallocationsWithEntry + 1);

    // Preserve the actual allocation identities, including retained vector
    // capacity; releasing the packed field block must not be mistaken for a
    // vector reallocation.
    const auto retainedBytes = resource.outstandingBytes();
    const auto retainedAllocations = resource.liveAllocationIds();
    std::pmr::string small(std::pmr::get_default_resource());
    ruvia::HpackEncoder::encodeDynamicTableSizeUpdate(small, 4096);
    small.append(std::string_view("\x40\x01\x61\x01\x62", 5));
    RUVIA_CHECK(decoder.decode(std::string_view(small), [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(resource.retains(retainedAllocations));
    RUVIA_CHECK(resource.outstandingBytes() >= retainedBytes);
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto nameView, auto valueView) {
                           return nameView == "a" && valueView == "b";
                       })
            .decoded());
}

RUVIA_TEST(hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::HpackDecoder decoder;
    const std::string insertion{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
    const auto rejected = decoder.decode(insertion, [](auto, auto) { return false; });
    RUVIA_CHECK(!rejected.decoded());
    RUVIA_CHECK(rejected.error() == ruvia::HpackDecodeError::kCallbackRejected);

    bool indexedHeaderFound = false;
    const char indexedByte = static_cast<char>(0xbe);
    const auto indexed = decoder.decode(std::string_view(&indexedByte, 1),
        [&indexedHeaderFound](std::string_view name, std::string_view value) {
            indexedHeaderFound = name == "a" && value == "b";
            return true;
        });
    RUVIA_CHECK(indexed.decoded());
    RUVIA_CHECK(indexedHeaderFound);
}

RUVIA_TEST(hpack_public_failed_decode_rolls_back_dynamic_table) {
    ruvia::HpackDecoder decoder;
    const std::string first{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
    RUVIA_CHECK(decoder.decode(first, [](auto, auto) { return true; }).decoded());

    const std::string failing{static_cast<char>(0x40), 0x01, 'c', 0x01, 'd',
        static_cast<char>(0x80)};
    RUVIA_CHECK(!decoder.decode(failing, [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                           [](std::string_view name, std::string_view value) {
                               return name == "a" && value == "b";
                           })
            .decoded());
    const auto absent = decoder.decode(std::string_view("\xbf", 1), [](auto, auto) { return true; });
    RUVIA_CHECK(!absent.decoded());
}

RUVIA_TEST(hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::HpackDecoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    std::string insertion(1, static_cast<char>(0x40));
    const auto appendLength = [&insertion](std::size_t length) {
        insertion.push_back(static_cast<char>(0x7f));
        length -= 127;
        while (length >= 128) {
            insertion.push_back(static_cast<char>((length & 0x7f) | 0x80));
            length >>= 7;
        }
        insertion.push_back(static_cast<char>(length));
    };
    appendLength(name.size());
    insertion += name;
    appendLength(value.size());
    insertion += value;
    RUVIA_CHECK(decoder.decode(insertion, [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                           [&](std::string_view actualName, std::string_view actualValue) {
                               return actualName == name && actualValue == value;
                           })
            .decoded());
}

RUVIA_TEST(hpack_public_commit_compaction_preserves_indices_and_rollback_without_allocation) {
    CountingAllocationResource resource;
    {
        ruvia::HpackDecoder decoder({.resource = &resource});
        // 20 alternating inline/heap entries fit. Inserting 36 in one
        // transaction leaves a 16-entry dead prefix and a 20-entry live suffix,
        // so compaction also exercises overlapping source/destination ranges.
        const std::string longName(48, 'n');
        std::pmr::string initial;
        ruvia::HpackEncoder::encodeDynamicTableSizeUpdate(initial, 1150);
        for (int i = 0; i < 36; ++i) {
            std::pmr::string field;
            const char value = static_cast<char>('A' + i);
            ruvia::HpackEncoder::encodeHeader(field,
                i % 2 == 0 ? std::string_view("a") : std::string_view(longName),
                std::string_view(&value, 1));
            field[0] = static_cast<char>(0x40);
            initial += field;
        }
        // A final indexed field performs no table insertion. Reject every new
        // allocation from its callback onward, including the commit's compaction.
        initial.push_back(static_cast<char>(0x82));
        const auto deallocationsBeforeBlock = resource.deallocations();
        RUVIA_CHECK(decoder.decode(initial, [&](std::string_view name, std::string_view value) {
                               if (name == ":method" && value == "GET") {
                                   resource.reject = true;
                               }
                               return true;
                           })
                .decoded());
        // The transaction commit compacts its overlapping 16-entry dead prefix
        // immediately, while retaining the vector's backing allocation.
        RUVIA_CHECK(resource.deallocations() > deallocationsBeforeBlock);
        std::pmr::string indices;
        for (std::uint32_t index = 62; index < 82; ++index) {
            ruvia::HpackEncoder::encodeIndexed(indices, index);
        }
        const auto verifyIndices = [&] {
            int expected = 35;
            const auto result = decoder.decode(indices, [&](std::string_view name, std::string_view value) {
                const auto expectedName = expected % 2 == 0 ? std::string_view("a") : std::string_view(longName);
                const bool matches = name == expectedName && value.size() == 1 && value[0] == 'A' + expected;
                --expected;
                return matches;
            });
            RUVIA_CHECK(result.decoded());
            RUVIA_CHECK_EQ(expected, 15);
        };
        const auto compactedBytes = resource.outstandingBytes();
        const auto allocationsBeforeIndexCheck = resource.allocations();
        resource.reject = true;
        verifyIndices();
        RUVIA_CHECK_EQ(resource.allocations(), allocationsBeforeIndexCheck);
        RUVIA_CHECK_EQ(resource.outstandingBytes(), compactedBytes);
        resource.reject = false;
        // The newest entry has a heap-backed name. Refer to it from an indexed
        // literal that evicts an old entry, then fail the block and roll back.
        const std::string failed{static_cast<char>(0x7e), 0x01, 'z', static_cast<char>(0x80)};
        RUVIA_CHECK(!decoder.decode(failed, [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK_EQ(resource.outstandingBytes(), compactedBytes);
        resource.reject = true;
        verifyIndices();
        resource.reject = false;
        RUVIA_CHECK(decoder.decode(std::string_view(failed).substr(0, 3), [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [&](std::string_view name, std::string_view value) {
                               return name == longName && value == "z";
                           })
                .decoded());
    }
    RUVIA_CHECK_EQ(resource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(hpack_public_decoder_move_releases_original_pmr_resources) {
    CountingAllocationResource firstResource;
    CountingAllocationResource secondResource;
    {
        ruvia::HpackDecoder source({.resource = &firstResource});
        const std::string insertion{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
        RUVIA_CHECK(source.decode(insertion, [](auto, auto) { return true; }).decoded());
        const auto allocationsBeforeMove = firstResource.allocations();
        const auto bytesBeforeMove = firstResource.outstandingBytes();
        ruvia::HpackDecoder moved(std::move(source));
        ruvia::HpackDecoder destination({.resource = &secondResource});
        RUVIA_CHECK(secondResource.outstandingBytes() > 0);
        destination = std::move(moved);
        RUVIA_CHECK_EQ(secondResource.outstandingBytes(), std::size_t{0});
        RUVIA_CHECK_EQ(firstResource.outstandingBytes(), bytesBeforeMove);
        RUVIA_CHECK_EQ(firstResource.allocations(), allocationsBeforeMove);
        RUVIA_CHECK(destination.decode(std::string_view("\xbe", 1), [](std::string_view name, std::string_view value) {
                                   return name == "a" && value == "b";
                               })
                .decoded());
    }
    RUVIA_CHECK_EQ(firstResource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(secondResource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(firstResource.allocations(), firstResource.deallocations());
    RUVIA_CHECK_EQ(secondResource.allocations(), secondResource.deallocations());

    CountingAllocationResource unusedResource;
    {
        ruvia::HpackDecoder unused({.resource = &unusedResource});
        RUVIA_CHECK(unusedResource.outstandingBytes() > 0);
    }
    RUVIA_CHECK_EQ(unusedResource.outstandingBytes(), std::size_t{0});
    RUVIA_CHECK_EQ(unusedResource.allocations(), unusedResource.deallocations());
    unusedResource.reject = true;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        ruvia::HpackDecoder rejected({.resource = &unusedResource});
    }));
    RUVIA_CHECK_EQ(unusedResource.outstandingBytes(), std::size_t{0});
}

#if !defined(_MSC_VER)
RUVIA_TEST(hpack_public_callback_exception_precedes_continuation_allocation_failure) {
    ToggleAllocationResource resource;
    ruvia::HpackDecoder decoder({.resource = &resource});

    std::string block;
    block.push_back(static_cast<char>(0x82));  // indexed :method: GET
    block.push_back(static_cast<char>(0x40));  // incremental literal, new name
    block.push_back(static_cast<char>(100));
    block.append(100, 'x');
    block.push_back(1);
    block.push_back('y');

    bool sawOriginal = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            resource.reject();
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        sawOriginal = std::string_view(error.what()) == "original callback failure";
    }
    RUVIA_CHECK(sawOriginal);
}
#endif
