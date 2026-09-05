/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HISTOGRAM_BUILDER_H_
#define HISTOGRAM_BUILDER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
// #include "allocation_data.h"
#include "dwarf_metadata_fetcher.h"
#include "llvm/include/llvm/ProfileData/MemProf.h"
#include "llvm/include/llvm/ProfileData/MemProfReader.h"
#include "status_macros.h"
#include "type_resolver.h"
#include "type_tree.h"

namespace devtools_crosstool_fdo_field_access {

// This class is used to store the callstack and the corresponding type tree
// for a given allocation. It is used to store the histogram data for the
// memprof profile, with the resolved field access counts.

// For now, the underlying data structure is a map from callstack to type
// tree. In the future, we may consider using a CallStackTrie, which would be
// more efficient in some cases. However, we do not expect to have a large
// number of allocation call stacks and type trees in the profile, so this
// should not be a problem.

// This data structure is designed to support the following operations:
// 1. For a given callstack, return the corresponding type tree.
// 2. For a given type name, return all call stacks that have that type name
// as the root of the type tree.
// 3. Iterate over all call stacks and type tree pairs.
class TypeTreeStore {
 public:
  using CallStack = std::vector<DwarfMetadataFetcher::Frame>;

  struct AllocationRange {
    uint64_t alloc_address = 0;
    uint64_t size = 0;
    uint64_t alloc_timestamp = 0;
    uint64_t dealloc_timestamp = 0;
  };

  struct TypeTreeStoreEntry {
    std::shared_ptr<TypeTree> type_tree;
    std::vector<AllocationRange> allocation_ranges;
  };

  TypeTreeStore() = default;
  ~TypeTreeStore() = default;

  static void DumpCallStack(const CallStack& callstack, std::ostream& os,
                            int level = 0, bool as_entry = false);

  // Converts a callstack of memprof Frames to a callstack of
  // DwarfMetadataFetcher Frames.
  static CallStack ConvertCallStack(
      absl::Span<const llvm::memprof::Frame> callstack);

  // Inserts the given callstack, type tree, and allocation ranges into the
  // type tree store. If the callstack already exists in the store, the access
  // counts of the existing type tree are merged with the new type tree and the
  // new allocation ranges are appended. If the types do not match, an error is
  // returned.
  absl::Status Insert(
      const std::vector<DwarfMetadataFetcher::Frame>& callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree> type_tree,
      std::vector<AllocationRange> allocation_ranges = {});

  // Same as above, but takes a callstack of memprof Frames.
  absl::Status Insert(
      absl::Span<const llvm::memprof::Frame> callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree> type_tree,
      std::vector<AllocationRange> allocation_ranges = {}) {
    return Insert(ConvertCallStack(callstack), std::move(type_tree),
                  std::move(allocation_ranges));
  };

  // Same as Insert, but returns the type tree for the given callstack.
  absl::StatusOr<const TypeTree*> InsertAndGet(
      const CallStack& callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree> type_tree);

  // Same as above, but takes a callstack of memprof Frames.
  absl::StatusOr<const TypeTree*> InsertAndGet(
      const std::vector<llvm::memprof::Frame>& callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree>
          type_tree) {
    return InsertAndGet(ConvertCallStack(callstack), std::move(type_tree));
  }

  // Returns the type tree for the given callstack.
  absl::StatusOr<std::shared_ptr<TypeTree>> GetTypeTree(
      absl::Span<const llvm::memprof::Frame> callstack) const {
    return GetTypeTree(ConvertCallStack(callstack));
  }

  // Same as above, but takes a callstack of memprof Frames.
  absl::StatusOr<std::shared_ptr<TypeTree>> GetTypeTree(
      const std::vector<DwarfMetadataFetcher::Frame>& callstack) const;

  // Returns all call stacks that have the given type name as the root of the
  // type tree.
  std::vector<CallStack> GetCallStacksForTypeName(
      std::string root_type_name) const;

  void Dump(std::ostream& os, int64_t limit) const;

  void DumpFlamegraph(std::ostream& os, int64_t limit) const;

  absl::flat_hash_map<CallStack, std::shared_ptr<TypeTree>>
      callstack_to_type_tree_;

  absl::flat_hash_map<CallStack, std::vector<AllocationRange>>
      callstack_to_allocation_ranges_;
};

class TypeTreeStoreList : public TypeTreeStore {
 public:
  TypeTreeStoreList() = default;
  ~TypeTreeStoreList() = default;

  void Dump(std::ostream& os, int64_t limit) const;

  void DumpFlamegraph(std::ostream& os, int64_t limit) const;

  absl::Status Insert(
      const std::vector<DwarfMetadataFetcher::Frame>& callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree>
          type_tree) {
    type_tree_stores_.push_back(std::move(type_tree));
    callstacks_.push_back(callstack);
    return absl::OkStatus();
  }

  // Same as Insert, but returns the type tree for the given callstack.
  absl::StatusOr<const TypeTree*> InsertAndGet(
      const CallStack& callstack,
      std::unique_ptr<devtools_crosstool_fdo_field_access::TypeTree>
          type_tree) {
    RETURN_IF_ERROR(Insert(callstack, std::move(type_tree)));
    return type_tree_stores_.back().get();
  }

  absl::StatusOr<std::shared_ptr<TypeTree>> GetTypeTree(
      const std::vector<DwarfMetadataFetcher::Frame>& callstack) const {
    for (int i = 0; i < type_tree_stores_.size(); ++i) {
      if (callstacks_[i] == callstack) {
        return type_tree_stores_[i];
      }
    }
    return absl::NotFoundError("Type tree not found");
  }

  std::vector<std::shared_ptr<TypeTree>> type_tree_stores_;
  std::vector<CallStack> callstacks_;
};

struct Statistics {
  struct TypeStatistics {
    uint64_t callstack_count = 0;
    uint64_t resolution_key_count = 0;
    uint64_t allocation_count = 0;
    uint64_t access_count = 0;
    uint64_t size_bytes = 0;
  };

  struct ResolutionPolicyStatistics {
    uint64_t callstack_count = 0;
    uint64_t resolution_key_count = 0;
    uint64_t allocation_count = 0;
    uint64_t raw_access_count = 0;
    uint64_t raw_size_bytes = 0;
  };

  struct AllocationCategoryStatistics {
    uint64_t callstack_count = 0;
    uint64_t resolution_key_count = 0;
    uint64_t allocation_count = 0;
    uint64_t access_count = 0;
    uint64_t size_bytes = 0;
  };

  // Allocation tracking.
  uint64_t total_allocations_count = 0;
  uint64_t total_found_type = 0;
  uint64_t total_verified = 0;
  uint64_t heap_alloc_count = 0;
  uint64_t container_alloc_count = 0;
  uint64_t total_record_count = 0;
  uint64_t total_after_filtering = 0;
  uint64_t duplicate_callstack_count = 0;

  // Callstack tracking.
  uint64_t unique_callstack_count = 0;
  uint64_t resolved_callstack_count = 0;
  uint64_t unresolved_callstack_count = 0;
  uint64_t verified_callstack_count = 0;
  uint64_t record_callstack_count = 0;
  uint64_t container_callstack_count = 0;
  uint64_t heap_alloc_callstack_count = 0;

  // Resolution-key tracking.
  uint64_t unique_resolution_key_count = 0;
  uint64_t resolved_resolution_key_count = 0;
  uint64_t unresolved_resolution_key_count = 0;
  uint64_t after_filtering_resolution_key_count = 0;
  uint64_t type_filtered_resolution_key_count = 0;
  uint64_t only_records_filtered_resolution_key_count = 0;
  uint64_t verified_resolution_key_count = 0;
  uint64_t record_resolution_key_count = 0;
  uint64_t container_resolution_key_count = 0;
  uint64_t heap_alloc_resolution_key_count = 0;
  uint64_t duplicate_resolution_count = 0;
  uint64_t inserted_tree_count = 0;
  uint64_t final_type_tree_store_size = 0;

  // Access tracking.
  uint64_t total_raw_accesses = 0;
  uint64_t resolved_raw_accesses = 0;
  uint64_t unresolved_raw_accesses = 0;
  uint64_t total_accesses = 0;
  uint64_t total_accesses_on_heapallocs = 0;
  uint64_t total_accesses_on_containers = 0;
  uint64_t total_accesses_on_records = 0;

  // Allocation-size tracking.
  uint64_t total_raw_size_bytes = 0;
  uint64_t resolved_raw_size_bytes = 0;
  uint64_t unresolved_raw_size_bytes = 0;
  uint64_t total_size_bytes = 0;
  uint64_t total_size_bytes_on_heapallocs = 0;
  uint64_t total_size_bytes_on_containers = 0;
  uint64_t total_size_bytes_on_records = 0;

  // Root-char tracking.
  uint64_t char_root_callstack_count = 0;
  uint64_t char_root_resolution_key_count = 0;
  uint64_t char_root_allocation_count = 0;
  uint64_t char_root_access_count = 0;
  uint64_t char_root_size_bytes = 0;

  // Per-type tracking.
  std::map<std::string, TypeStatistics> type_statistics;

  std::map<std::string, ResolutionPolicyStatistics>
      resolution_policy_statistics;

  std::map<std::string, AllocationCategoryStatistics>
      allocation_category_statistics;

  void Log() const;
};

struct HistogramBuilderResults {
  std::unique_ptr<TypeTreeStore> type_tree_store;
  Statistics stats;
  explicit HistogramBuilderResults(
      std::unique_ptr<TypeTreeStore> type_tree_store, Statistics stats)
      : type_tree_store(std::move(type_tree_store)), stats(stats) {}
};

// Abstract class for receiving a field access histogram consisting of a set of
// type trees with field access counts indexed by their allocation callstacks.
class AbstractHistogramBuilder {
 public:
  virtual ~AbstractHistogramBuilder() = default;

  virtual absl::StatusOr<std::unique_ptr<HistogramBuilderResults>>
  BuildHistogram() = 0;
};

// This class is used to build a histogram for a local memprof profile. It
// uses the memprof reader to read the profile and the dwarf type resolver to
// resolve the type tree for each allocation.
class LocalHistogramBuilder : public AbstractHistogramBuilder {
 public:
  constexpr static uint32_t kMemprofHistogramGranularity = 8UL;

  static absl::StatusOr<std::unique_ptr<AbstractHistogramBuilder>> Create(
      std::string memprof_profile, std::string memprof_profiled_binary,
      std::string memprof_profiled_binary_dwarf,
      const std::vector<std::string>& type_prefix_filter,
      const std::vector<std::string>& callstack_filter, bool only_records,
      bool verify_verbose, bool dump_unresolved_callstacks,
      uint32_t parse_thread_count = 1);

  explicit LocalHistogramBuilder(
      std::unique_ptr<llvm::memprof::RawMemProfReader> memprof_reader,
      std::unique_ptr<DwarfTypeResolver> dwarf_type_resolver,
      std::vector<std::string> type_prefix_filter,
      std::vector<std::string> callstack_filter, bool only_records,
      bool verify_verbose, bool dump_unresolved_callstacks,
      uint32_t type_resolution_thread_count)
      : memprof_reader_(std::move(memprof_reader)),
        dwarf_type_resolver_(std::move(dwarf_type_resolver)),
        type_prefix_filter_(type_prefix_filter),
        callstack_filter_(callstack_filter),
        only_records_(only_records),
        verify_verbose_(verify_verbose),
        dump_unresolved_callstacks_(dump_unresolved_callstacks),
        type_resolution_thread_count_(type_resolution_thread_count) {}
  ~LocalHistogramBuilder() override = default;

  absl::StatusOr<std::unique_ptr<HistogramBuilderResults>> BuildHistogram()
      override;

 private:
  bool FilterType(absl::string_view type_name) const;
  bool FilterCallstack(const TypeTreeStore::CallStack& callstack) const;

  // The reader for the memprof profile.
  std::unique_ptr<llvm::memprof::RawMemProfReader> memprof_reader_;
  // The type resolver for resolving the type tree for a given type name.
  std::unique_ptr<DwarfTypeResolver> dwarf_type_resolver_;
  // The type prefix filter for filtering out types to include in the histogram.
  // Any type that has a matching prefix held in the prefix filter will be
  // included.
  std::vector<std::string> type_prefix_filter_;
  // The callstack filter for filtering out callstacks to include in the
  // histogram. Any callstack that has a matching function name held in the
  // callstack filter will be included.
  std::vector<std::string> callstack_filter_;
  // If true, only include records in the histogram. That means we discard all
  // basic types such as int or double, and only keep objects.
  bool only_records_;
  // If true, print out verbose information and dump the type tree when
  // verifying the type tree.
  bool verify_verbose_;
  // If true, print out callstacks of types that are not resolved.
  bool dump_unresolved_callstacks_;
  uint32_t type_resolution_thread_count_;
};

}  // namespace devtools_crosstool_fdo_field_access

#endif  // HISTOGRAM_BUILDER_H_
