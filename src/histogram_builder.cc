// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "histogram_builder.h"

#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "binary_file_retriever.h"
#include "dwarf_metadata_fetcher.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Threading.h"
#include "llvm/include/llvm/ADT/StringExtras.h"
#include "llvm/include/llvm/Demangle/Demangle.h"
#include "llvm/include/llvm/Object/Binary.h"
#include "llvm/include/llvm/Object/BuildID.h"
#include "llvm/include/llvm/Object/ObjectFile.h"
#include "llvm/include/llvm/ProfileData/MemProf.h"
#include "llvm/include/llvm/ProfileData/MemProfReader.h"
#include "llvm/include/llvm/Support/Error.h"
#include "llvm/include/llvm/Support/ErrorOr.h"
#include "llvm/include/llvm/Support/MemoryBuffer.h"
#include "src/object_layout.pb.h"
#include "status_macros.h"
#include "type_resolver.h"
#include "type_tree.h"

namespace devtools_crosstool_fdo_field_access {

using llvm::memprof::RawMemProfReader;

constexpr char kCacheDir[] = "/tmp/dwarf_metadata";

namespace {

constexpr uint64_t kCycleHighBit = uint64_t{1} << 63;
constexpr uint64_t kCycleValueMask = ~kCycleHighBit;

struct CompactFrameKey {
  uint64_t function = 0;
  uint32_t line_offset = 0;
  uint32_t column = 0;

  bool operator==(const CompactFrameKey& other) const {
    return function == other.function && line_offset == other.line_offset &&
           column == other.column;
  }

  template <typename H>
  friend H AbslHashValue(H h, const CompactFrameKey& frame) {
    return H::combine(std::move(h), frame.function, frame.line_offset,
                      frame.column);
  }
};

using CompactCallStackKey = absl::InlinedVector<CompactFrameKey, 8>;

CompactCallStackKey MakeCompactCallStackKey(
    absl::Span<const llvm::memprof::Frame> frames) {
  CompactCallStackKey result;
  result.reserve(frames.size());
  for (const llvm::memprof::Frame& frame : frames) {
    result.push_back({frame.hasSymbolName() ? frame.Function : 0,
                      frame.LineOffset, frame.Column});
  }
  return result;
}

uint64_t NormalizeCycleTimestamp(uint64_t timestamp) {
  return timestamp & kCycleValueMask;
}

double Percentify(uint64_t value, uint64_t total) {
  if (total == 0) return 0.0;
  return 100.0 * static_cast<double>(value) / static_cast<double>(total);
}

}  // namespace

void Statistics::Log() const {
  std::ostringstream output;

  const AllocationCategoryStatistics empty_category_stats;
  auto get_category_stats = [&](absl::string_view category)
      -> const AllocationCategoryStatistics& {
    auto it = allocation_category_statistics.find(
        std::string(category.data(), category.size()));
    return it == allocation_category_statistics.end() ? empty_category_stats
                                                       : it->second;
  };

  const AllocationCategoryStatistics& simple_stats =
      get_category_stats("simple");
  const AllocationCategoryStatistics& allocation_aware_stats =
      get_category_stats("allocation-aware");
  const AllocationCategoryStatistics& complex_stats =
      get_category_stats("complex");

  const uint64_t categorized_allocation_count =
      simple_stats.allocation_count + allocation_aware_stats.allocation_count +
      complex_stats.allocation_count;
  const uint64_t categorized_resolution_key_count =
      simple_stats.resolution_key_count +
      allocation_aware_stats.resolution_key_count +
      complex_stats.resolution_key_count;
  const uint64_t categorized_access_count =
      simple_stats.access_count + allocation_aware_stats.access_count +
      complex_stats.access_count;
  const uint64_t categorized_size_bytes =
      simple_stats.size_bytes + allocation_aware_stats.size_bytes +
      complex_stats.size_bytes;

  output
      << "- \n"
      << "====== Statistics ======\n"
      << "Total allocations count: " << total_allocations_count << " ("
      << Percentify(total_allocations_count, total_allocations_count) << "%)\n"
      << "Total found type: " << total_found_type << " ("
      << Percentify(total_found_type, total_allocations_count) << "%)\n"
      << "Total duplicate callstack: " << duplicate_callstack_count << " ("
      << Percentify(duplicate_callstack_count, total_allocations_count)
      << "%)\n"
      << "Total verified: " << total_verified << " ("
      << Percentify(total_verified, total_allocations_count) << "%)\n"
      << "Simple allocation count: " << simple_stats.allocation_count << " ("
      << Percentify(simple_stats.allocation_count,
                    categorized_allocation_count)
      << "%)\n"
      << "Allocation-aware allocation count: "
      << allocation_aware_stats.allocation_count << " ("
      << Percentify(allocation_aware_stats.allocation_count,
                    categorized_allocation_count)
      << "%)\n"
      << "Complex allocation count: " << complex_stats.allocation_count
      << " ("
      << Percentify(complex_stats.allocation_count,
                    categorized_allocation_count)
      << "%)\n"
      << "Total record count: " << total_record_count << " ("
      << Percentify(total_record_count, total_allocations_count) << "%)\n"
      << "Total after filtering: " << total_after_filtering << " ("
      << Percentify(total_after_filtering, total_allocations_count) << "%)\n"
      << "\n"
      << "Unique callstacks: " << unique_callstack_count << "\n"
      << "Resolved callstacks: " << resolved_callstack_count << " ("
      << Percentify(resolved_callstack_count, unique_callstack_count) << "%)\n"
      << "Fully unresolved callstacks: " << unresolved_callstack_count << " ("
      << Percentify(unresolved_callstack_count, unique_callstack_count)
      << "%)\n"
      << "Verified callstacks: " << verified_callstack_count << " ("
      << Percentify(verified_callstack_count, unique_callstack_count) << "%)\n"
      << "Record callstacks: " << record_callstack_count << " ("
      << Percentify(record_callstack_count, unique_callstack_count) << "%)\n"
      << "Simple allocation callstacks: " << simple_stats.callstack_count
      << " ("
      << Percentify(simple_stats.callstack_count, resolved_callstack_count)
      << "%)\n"
      << "Allocation-aware allocation callstacks: "
      << allocation_aware_stats.callstack_count << " ("
      << Percentify(allocation_aware_stats.callstack_count,
                    resolved_callstack_count)
      << "%)\n"
      << "Complex allocation callstacks: " << complex_stats.callstack_count
      << " ("
      << Percentify(complex_stats.callstack_count, resolved_callstack_count)
      << "%)\n"
      << "\n"
      << "Unique resolution keys: " << unique_resolution_key_count << "\n"
      << "Resolved resolution keys: " << resolved_resolution_key_count << " ("
      << Percentify(resolved_resolution_key_count, unique_resolution_key_count)
      << "%)\n"
      << "Unresolved resolution keys: " << unresolved_resolution_key_count
      << " ("
      << Percentify(unresolved_resolution_key_count,
                    unique_resolution_key_count)
      << "%)\n"
      << "Resolution keys after filtering: "
      << after_filtering_resolution_key_count << " ("
      << Percentify(after_filtering_resolution_key_count,
                    unique_resolution_key_count)
      << "%)\n"
      << "Resolution keys filtered by type: "
      << type_filtered_resolution_key_count << " ("
      << Percentify(type_filtered_resolution_key_count,
                    unique_resolution_key_count)
      << "%)\n"
      << "Resolution keys filtered by only-records: "
      << only_records_filtered_resolution_key_count << " ("
      << Percentify(only_records_filtered_resolution_key_count,
                    unique_resolution_key_count)
      << "%)\n"
      << "Verified resolution keys: " << verified_resolution_key_count << " ("
      << Percentify(verified_resolution_key_count, unique_resolution_key_count)
      << "%)\n"
      << "Record resolution keys: " << record_resolution_key_count << " ("
      << Percentify(record_resolution_key_count, unique_resolution_key_count)
      << "%)\n"
      << "Simple allocation resolution keys: "
      << simple_stats.resolution_key_count << " ("
      << Percentify(simple_stats.resolution_key_count,
                    categorized_resolution_key_count)
      << "%)\n"
      << "Allocation-aware allocation resolution keys: "
      << allocation_aware_stats.resolution_key_count << " ("
      << Percentify(allocation_aware_stats.resolution_key_count,
                    categorized_resolution_key_count)
      << "%)\n"
      << "Complex allocation resolution keys: "
      << complex_stats.resolution_key_count << " ("
      << Percentify(complex_stats.resolution_key_count,
                    categorized_resolution_key_count)
      << "%)\n"
      << "Duplicate resolutions avoided: " << duplicate_resolution_count << "\n"
      << "Resolved trees inserted: " << inserted_tree_count << "\n"
      << "Final TypeTreeStore size: " << final_type_tree_store_size << "\n"
      << "\n"
      << "Total raw accesses: " << total_raw_accesses << " ("
      << Percentify(total_raw_accesses, total_raw_accesses) << "%)\n"
      << "Raw accesses with resolved type: " << resolved_raw_accesses << " ("
      << Percentify(resolved_raw_accesses, total_raw_accesses) << "%)\n"
      << "Raw accesses with unresolved type: " << unresolved_raw_accesses
      << " (" << Percentify(unresolved_raw_accesses, total_raw_accesses)
      << "%)\n"
      << "Total accesses in retained type trees: " << total_accesses << " ("
      << Percentify(total_accesses, total_raw_accesses)
      << "% of raw accesses)\n"
      << "Total accesses on simple allocations: " << simple_stats.access_count
      << " (" << Percentify(simple_stats.access_count,
                              categorized_access_count)
      << "%)\n"
      << "Total accesses on allocation-aware allocations: "
      << allocation_aware_stats.access_count << " ("
      << Percentify(allocation_aware_stats.access_count,
                    categorized_access_count)
      << "%)\n"
      << "Total accesses on complex allocations: "
      << complex_stats.access_count << " ("
      << Percentify(complex_stats.access_count, categorized_access_count)
      << "%)\n"
      << "Total accesses on records: " << total_accesses_on_records << " ("
      << Percentify(total_accesses_on_records, total_accesses) << "%)\n"
      << "\n"
      << "Total allocated bytes: " << total_raw_size_bytes << " ("
      << Percentify(total_raw_size_bytes, total_raw_size_bytes) << "%)\n"
      << "Allocated bytes with resolved type: " << resolved_raw_size_bytes
      << " (" << Percentify(resolved_raw_size_bytes, total_raw_size_bytes)
      << "%)\n"
      << "Allocated bytes with unresolved type: " << unresolved_raw_size_bytes
      << " (" << Percentify(unresolved_raw_size_bytes, total_raw_size_bytes)
      << "%)\n"
      << "Allocated bytes in retained type trees: " << total_size_bytes << " ("
      << Percentify(total_size_bytes, total_raw_size_bytes)
      << "% of allocated bytes)\n"
      << "Allocated bytes on simple allocations: " << simple_stats.size_bytes
      << " (" << Percentify(simple_stats.size_bytes, categorized_size_bytes)
      << "%)\n"
      << "Allocated bytes on allocation-aware allocations: "
      << allocation_aware_stats.size_bytes << " ("
      << Percentify(allocation_aware_stats.size_bytes, categorized_size_bytes)
      << "%)\n"
      << "Allocated bytes on complex allocations: "
      << complex_stats.size_bytes << " ("
      << Percentify(complex_stats.size_bytes, categorized_size_bytes)
      << "%)\n"
      << "Allocated bytes on records: " << total_size_bytes_on_records << " ("
      << Percentify(total_size_bytes_on_records, total_size_bytes) << "%)\n"
      << "\n"
      << "Callstacks with root type char: " << char_root_callstack_count << " ("
      << Percentify(char_root_callstack_count, resolved_callstack_count)
      << "% of resolved)\n"
      << "Resolution keys with root type char: "
      << char_root_resolution_key_count << " ("
      << Percentify(char_root_resolution_key_count,
                    resolved_resolution_key_count)
      << "% of resolved)\n"
      << "Allocations with root type char: " << char_root_allocation_count
      << " (" << Percentify(char_root_allocation_count, total_found_type)
      << "% of resolved)\n"
      << "Accesses on root type char: " << char_root_access_count << " ("
      << Percentify(char_root_access_count, total_accesses)
      << "% of retained accesses)\n"
      << "Allocated bytes with root type char: " << char_root_size_bytes << " ("
      << Percentify(char_root_size_bytes, total_size_bytes)
      << "% of retained bytes)\n"
      << "\n"
      << "====== Type Distribution ======\n";

  for (const auto& [type_name, type_stats] : type_statistics) {
    output << "Type: " << type_name
           << ", callstacks: " << type_stats.callstack_count
           << ", resolution keys: " << type_stats.resolution_key_count
           << ", allocations: " << type_stats.allocation_count
           << ", accesses: " << type_stats.access_count
           << ", allocated bytes: " << type_stats.size_bytes
           << ", allocation percentage: "
           << Percentify(type_stats.allocation_count, total_found_type) << "%"
           << ", access percentage: "
           << Percentify(type_stats.access_count, total_accesses) << "%"
           << ", allocated-byte percentage: "
           << Percentify(type_stats.size_bytes, total_size_bytes) << "%\n";
  }

  output << "\n====== Resolution Policy Distribution ======\n";
  for (const auto& [policy, policy_stats] : resolution_policy_statistics) {
    output << "Policy: " << policy
           << ", callstacks: " << policy_stats.callstack_count
           << ", resolution keys: " << policy_stats.resolution_key_count
           << ", allocations: " << policy_stats.allocation_count
           << ", raw accesses: " << policy_stats.raw_access_count
           << ", raw allocated bytes: " << policy_stats.raw_size_bytes
           << ", allocation percentage: "
           << Percentify(policy_stats.allocation_count, total_found_type) << "%"
           << ", raw-access percentage: "
           << Percentify(policy_stats.raw_access_count, resolved_raw_accesses)
           << "%"
           << ", raw-allocated-byte percentage: "
           << Percentify(policy_stats.raw_size_bytes, resolved_raw_size_bytes)
           << "%\n";
  }

  output << "====== End Statistics ======";
  LOG(INFO) << output.str();
}

absl::StatusOr<std::string> GetBuildIdForLocalFile(
    absl::string_view memprof_profiled_binary) {
  llvm::Expected<llvm::object::OwningBinary<llvm::object::ObjectFile>> elfobj =
      llvm::object::ObjectFile::createObjectFile(memprof_profiled_binary);
  if (!elfobj) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Cannot create object file for ", memprof_profiled_binary));
  }
  return llvm::toHex(llvm::object::getBuildID(elfobj->getBinary()),
                     /*lowercase=*/true);
}

void LogCallStackAndTypeTree(const TypeTreeStore::CallStack& callstack,
                             const TypeTree* type_tree, bool verify_verbose) {
  if (!verify_verbose) return;
  std::stringstream error_string;
  if (type_tree) {
    type_tree->Dump(error_string);
    error_string << "\n";
  } else {
    error_string << "- \n";
  }
  TypeTreeStore::DumpCallStack(callstack, error_string);
  error_string.flush();
  LOG(WARNING) << error_string.str();
}

absl::StatusOr<std::unique_ptr<HistogramBuilderResults>>
LocalHistogramBuilder::BuildHistogram() {
  constexpr absl::Duration kTypeResolutionTimeout = absl::Seconds(10);
  constexpr size_t kFilteredCallStack = std::numeric_limits<size_t>::max();

  struct UniqueWorkItem {
    size_t callstack_group_index = 0;
    int64_t request_size = 0;
    uint64_t occurrence_count = 0;
    uint64_t total_size_bytes = 0;
    uint64_t total_access_count = 0;
    std::vector<uint64_t> access_histogram;
    std::vector<TypeTreeStore::AllocationRange> allocation_ranges;
  };

  struct CallStackGroup {
    TypeTreeStore::CallStack callstack;
    absl::InlinedVector<std::pair<int64_t, size_t>, 4> resolutions_by_size;
  };

  struct ResolutionResult {
    absl::Status status = absl::OkStatus();
    std::unique_ptr<TypeTree> type_tree;
    bool resolved = false;
    bool filtered_by_type = false;
    bool filtered_by_only_records = false;
    bool is_record = false;
    bool from_container = false;
    bool histogram_warning = false;
    bool verify_passed = false;
    AllocationCategory allocation_category = AllocationCategory::kSimple;
    std::string resolution_policy;
  };

  Statistics stats;
  LOG(INFO) << "Building Type Tree store..";
  auto type_tree_store = std::make_unique<TypeTreeStore>();

  uint64_t total_allocation_count = 0;
  for (const auto& [unused, record] : *memprof_reader_) {
    total_allocation_count += record.AllocSites.size();
  }
  LOG(INFO) << "Dedup will process " << total_allocation_count
            << " allocations";

  const absl::Time dedup_start = absl::Now();
  std::vector<UniqueWorkItem> work_items;
  std::vector<CallStackGroup> callstack_groups;
  absl::flat_hash_map<CompactCallStackKey, size_t> callstack_index;

  uint64_t record_count = 0;
  uint64_t allocation_count = 0;
  uint64_t filtered_callstack_count = 0;
  uint64_t duplicate_callstack_count = 0;
  uint64_t duplicate_resolution_count = 0;
  uint64_t total_compact_frames = 0;
  uint64_t total_histogram_bins = 0;

  for (const auto& [unused, record] : *memprof_reader_) {
    record_count++;
    for (const llvm::memprof::AllocationInfo& alloc_info : record.AllocSites) {
      allocation_count++;
      if (allocation_count % 250000 == 0) {
        LOG(INFO) << "Dedup progress: " << allocation_count
                  << " allocations processed, " << work_items.size()
                  << " unique resolution keys, " << duplicate_resolution_count
                  << " resolutions avoided";
      }

      QCHECK(!alloc_info.CallStack.empty()) << "Empty callstack for allocation";
      total_compact_frames += alloc_info.CallStack.size();
      CompactCallStackKey compact_callstack =
          MakeCompactCallStackKey(alloc_info.CallStack);
      auto [callstack_it, inserted] = callstack_index.try_emplace(
          std::move(compact_callstack), kFilteredCallStack);

      if (inserted) {
        TypeTreeStore::CallStack callstack =
            TypeTreeStore::ConvertCallStack(alloc_info.CallStack);
        if (FilterCallstack(callstack)) {
          filtered_callstack_count++;
          continue;
        }
        CallStackGroup group;
        group.callstack = std::move(callstack);
        callstack_it->second = callstack_groups.size();
        callstack_groups.push_back(std::move(group));
      } else {
        if (callstack_it->second == kFilteredCallStack) {
          filtered_callstack_count++;
          continue;
        }
        duplicate_callstack_count++;
      }

      const size_t callstack_group_index = callstack_it->second;
      CallStackGroup& callstack_group = callstack_groups[callstack_group_index];
      stats.total_allocations_count++;

      const uint64_t mib_alloc_count = alloc_info.Info.getAllocCount();
      QCHECK_EQ(mib_alloc_count, 1)
          << "Expected one allocation per MemInfoBlock";

      const uint64_t allocation_address = alloc_info.Info.getAllocAddress();
      const uint64_t allocation_size = alloc_info.Info.getTotalSize();
      const uint64_t raw_allocation_timestamp =
          alloc_info.Info.getAllocTimestamp();
      const uint64_t raw_deallocation_timestamp =
          alloc_info.Info.getDeallocTimestamp();
      const uint64_t allocation_timestamp =
          NormalizeCycleTimestamp(raw_allocation_timestamp);
      const uint64_t deallocation_timestamp =
          NormalizeCycleTimestamp(raw_deallocation_timestamp);

      VLOG(1) << "[cycle] Allocation timestamps"
              << " raw_alloc=" << raw_allocation_timestamp
              << " raw_dealloc=" << raw_deallocation_timestamp
              << " raw_alloc_hex=0x"
              << llvm::utohexstr(raw_allocation_timestamp, true)
              << " raw_dealloc_hex=0x"
              << llvm::utohexstr(raw_deallocation_timestamp, true)
              << " normalized_alloc=" << allocation_timestamp
              << " normalized_dealloc=" << deallocation_timestamp;

      if ((raw_allocation_timestamp & kCycleHighBit) !=
          (raw_deallocation_timestamp & kCycleHighBit)) {
        LOG(WARNING) << "Allocation and deallocation timestamps have "
                        "inconsistent high bits:"
                     << " raw_alloc=" << raw_allocation_timestamp
                     << " raw_dealloc=" << raw_deallocation_timestamp;
      }
      QCHECK_LE(allocation_timestamp, deallocation_timestamp)
          << "Allocation timestamp occurs after deallocation timestamp"
          << " raw_alloc=" << raw_allocation_timestamp
          << " raw_dealloc=" << raw_deallocation_timestamp
          << " normalized_alloc=" << allocation_timestamp
          << " normalized_dealloc=" << deallocation_timestamp;

      const uint64_t total_access_count = alloc_info.Info.getTotalAccessCount();
      TypeTreeStore::AllocationRange allocation_range;
      allocation_range.alloc_address = allocation_address;
      allocation_range.size = allocation_size;
      allocation_range.alloc_timestamp = allocation_timestamp;
      allocation_range.dealloc_timestamp = deallocation_timestamp;

      QCHECK_LE(allocation_size,
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
          << "Allocation size does not fit in int64_t";
      const int64_t request_size = static_cast<int64_t>(allocation_size);
      const uint64_t histogram_size = alloc_info.Info.getAccessHistogramSize();
      total_histogram_bins += histogram_size;

      auto size_it = std::find_if(
          callstack_group.resolutions_by_size.begin(),
          callstack_group.resolutions_by_size.end(),
          [request_size](const std::pair<int64_t, size_t>& entry) {
            return entry.first == request_size;
          });

      if (size_it != callstack_group.resolutions_by_size.end()) {
        UniqueWorkItem& work_item = work_items[size_it->second];
        work_item.occurrence_count++;
        work_item.total_size_bytes += allocation_size;
        work_item.total_access_count += total_access_count;
        work_item.allocation_ranges.push_back(allocation_range);
        duplicate_resolution_count++;
        if (histogram_size > 0) {
          const uint64_t* histogram = reinterpret_cast<const uint64_t*>(
              alloc_info.Info.getAccessHistogram());
          QCHECK(histogram != nullptr)
              << "Non-zero histogram size with null histogram";
          QCHECK_EQ(work_item.access_histogram.size(), histogram_size)
              << "Histogram size differs for identical resolution key";
          for (size_t i = 0; i < histogram_size; ++i) {
            work_item.access_histogram[i] += histogram[i];
          }
        }
        continue;
      }

      UniqueWorkItem work_item;
      work_item.callstack_group_index = callstack_group_index;
      work_item.request_size = request_size;
      work_item.occurrence_count = 1;
      work_item.total_size_bytes = allocation_size;
      work_item.total_access_count = total_access_count;
      work_item.allocation_ranges.push_back(allocation_range);
      if (histogram_size > 0) {
        const uint64_t* histogram = reinterpret_cast<const uint64_t*>(
            alloc_info.Info.getAccessHistogram());
        QCHECK(histogram != nullptr)
            << "Non-zero histogram size with null histogram";
        work_item.access_histogram.assign(histogram,
                                          histogram + histogram_size);
      }
      const size_t work_index = work_items.size();
      work_items.push_back(std::move(work_item));
      callstack_group.resolutions_by_size.push_back({request_size, work_index});
    }
  }

  stats.duplicate_callstack_count = duplicate_callstack_count;
  LOG(INFO) << "Finished deduplication in "
            << absl::FormatDuration(absl::Now() - dedup_start);
  LOG(INFO) << "MemProf records: " << record_count;
  LOG(INFO) << "Total allocation sites: " << allocation_count;
  LOG(INFO) << "Allocation sites after callstack filtering: "
            << stats.total_allocations_count;
  LOG(INFO) << "Allocation sites filtered by callstack: "
            << filtered_callstack_count;
  LOG(INFO) << "Unique callstacks: " << callstack_groups.size();
  LOG(INFO) << "Duplicate callstacks: " << duplicate_callstack_count;
  LOG(INFO) << "Unique resolution keys: " << work_items.size();
  LOG(INFO) << "Duplicate resolutions avoided: " << duplicate_resolution_count;
  LOG(INFO) << "Compact callstack frames processed: " << total_compact_frames;
  LOG(INFO) << "Histogram bins processed: " << total_histogram_bins;

  if (stats.total_allocations_count > 0) {
    LOG(INFO) << "Duplicate callstack rate: "
              << Percentify(duplicate_callstack_count,
                            stats.total_allocations_count)
              << "%";
    LOG(INFO) << "Resolution deduplication rate: "
              << Percentify(duplicate_resolution_count,
                            stats.total_allocations_count)
              << "%";
  }

  uint64_t repeated_resolution_keys = 0;
  uint64_t maximum_occurrence_count = 0;
  for (const UniqueWorkItem& work_item : work_items) {
    if (work_item.occurrence_count > 1) repeated_resolution_keys++;
    maximum_occurrence_count =
        std::max(maximum_occurrence_count, work_item.occurrence_count);
  }
  LOG(INFO) << "Resolution keys occurring more than once: "
            << repeated_resolution_keys;
  LOG(INFO) << "Maximum occurrences of one resolution key: "
            << maximum_occurrence_count;

  if (work_items.empty()) {
    LOG(INFO) << "No allocation sites require type resolution.";
    return std::make_unique<HistogramBuilderResults>(std::move(type_tree_store),
                                                     stats);
  }

  const size_t worker_count = std::min<size_t>(
      std::max<uint32_t>(1, type_resolution_thread_count_), work_items.size());
  LOG(INFO) << "Resolving " << work_items.size()
            << " unique resolution keys using " << worker_count << " thread(s)";

  std::vector<ResolutionResult> results(work_items.size());
  std::atomic<size_t> next_work_index{0};
  std::atomic<size_t> completed_work_count{0};
  const size_t progress_interval = std::max<size_t>(1, work_items.size() / 20);
  const absl::Time resolution_start = absl::Now();
  std::vector<std::thread> workers;
  workers.reserve(worker_count);

  for (size_t worker_id = 0; worker_id < worker_count; ++worker_id) {
    workers.emplace_back([&]() {
      while (true) {
        const size_t work_index =
            next_work_index.fetch_add(1, std::memory_order_relaxed);
        if (work_index >= work_items.size()) break;

        UniqueWorkItem& work_item = work_items[work_index];
        ResolutionResult& result = results[work_index];
        const TypeTreeStore::CallStack& callstack =
            callstack_groups[work_item.callstack_group_index].callstack;
        const absl::Time deadline = absl::Now() + kTypeResolutionTimeout;
        auto status_or_resolution =
            dwarf_type_resolver_->ResolveTypeFromCallstack(
                callstack, work_item.request_size, deadline);

        if (!status_or_resolution.ok()) {
          result.status = status_or_resolution.status();
          if (absl::IsDeadlineExceeded(result.status)) {
            LOG(WARNING) << "Type resolution timed out: work_index="
                         << work_index << ", timeout="
                         << absl::FormatDuration(kTypeResolutionTimeout)
                         << ", occurrences=" << work_item.occurrence_count
                         << ", callstack_depth=" << callstack.size();
          }
        } else {
          result.resolved = true;
          TypeResolutionResult resolution =
              std::move(status_or_resolution.value());
          std::unique_ptr<TypeTree> type_tree =
              std::move(resolution.type_tree);
          result.allocation_category = resolution.allocation_category;
          result.resolution_policy = std::move(resolution.resolution_strategy);
          QCHECK(type_tree != nullptr)
              << "Successful type resolution returned a null TypeTree";
          QCHECK(!result.resolution_policy.empty())
              << "Successful type resolution returned no strategy";

          if (FilterType(type_tree->Name())) {
            result.filtered_by_type = true;
          } else {
            result.is_record = type_tree->IsRecordType();
            result.from_container = type_tree->FromContainer();
            if (only_records_ && !result.is_record) {
              result.filtered_by_only_records = true;
            } else {
              if (!work_item.access_histogram.empty()) {
                absl::Status histogram_status =
                    type_tree->RecordAccessHistogram<
                        kMemprofHistogramGranularity,
                        TypeTree::AccessCounters::AccessType::kAccess>(
                        work_item.access_histogram);
                if (!histogram_status.ok()) result.histogram_warning = true;
              }
              result.verify_passed = type_tree->Verify(verify_verbose_);
              result.type_tree = std::move(type_tree);
            }
          }
        }

        const size_t completed =
            completed_work_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (completed % progress_interval == 0 ||
            completed == work_items.size()) {
          LOG(INFO) << "Type resolution progress: " << completed << "/"
                    << work_items.size() << " ("
                    << Percentify(completed, work_items.size()) << "%)";
        }
      }
    });
  }

  for (std::thread& worker : workers) worker.join();
  LOG(INFO) << "Parallel type resolution completed in "
            << absl::FormatDuration(absl::Now() - resolution_start);
  LOG(INFO) << "Processing resolution results...";

  const absl::Time merge_start = absl::Now();
  uint64_t unique_resolved_count = 0;
  uint64_t unique_unresolved_count = 0;
  uint64_t unique_after_filtering_count = 0;
  uint64_t unique_type_filtered_count = 0;
  uint64_t unique_only_records_filtered_count = 0;
  uint64_t unique_verified_count = 0;
  uint64_t unique_record_count = 0;
  uint64_t unique_container_count = 0;
  uint64_t unique_heap_alloc_count = 0;
  uint64_t inserted_tree_count = 0;
  uint64_t resolved_access_count = 0;
  uint64_t unresolved_access_count = 0;
  uint64_t resolved_size_bytes = 0;
  uint64_t unresolved_size_bytes = 0;
  uint64_t char_root_resolution_key_count = 0;
  uint64_t char_root_allocation_count = 0;
  uint64_t char_root_access_count = 0;
  uint64_t char_root_size_bytes = 0;

  absl::flat_hash_set<TypeTreeStore::CallStack> resolved_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> failed_resolution_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> verified_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> record_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> container_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> heap_alloc_callstacks;
  absl::flat_hash_set<TypeTreeStore::CallStack> char_root_callstacks;

  std::map<std::string, uint64_t> unique_type_counts;
  std::map<std::string, absl::flat_hash_set<TypeTreeStore::CallStack>>
      callstacks_by_type;
  std::map<std::string, uint64_t> allocation_weighted_type_counts;
  std::map<std::string, uint64_t> access_counts_by_type;
  std::map<std::string, uint64_t> size_bytes_by_type;
  std::map<std::string, uint64_t> resolution_keys_by_policy;
  std::map<std::string, absl::flat_hash_set<TypeTreeStore::CallStack>>
      callstacks_by_policy;
  std::map<std::string, uint64_t> allocations_by_policy;
  std::map<std::string, uint64_t> raw_accesses_by_policy;
  std::map<std::string, uint64_t> raw_size_bytes_by_policy;
  std::map<std::string, uint64_t> resolution_keys_by_category;
  std::map<std::string, absl::flat_hash_set<TypeTreeStore::CallStack>>
      callstacks_by_category;
  std::map<std::string, uint64_t> allocations_by_category;
  std::map<std::string, uint64_t> accesses_by_category;
  std::map<std::string, uint64_t> size_bytes_by_category;

  const size_t merge_progress_interval =
      std::max<size_t>(1, work_items.size() / 10);

  for (size_t work_index = 0; work_index < work_items.size(); ++work_index) {
    UniqueWorkItem& work_item = work_items[work_index];
    ResolutionResult& result = results[work_index];
    const TypeTreeStore::CallStack& callstack =
        callstack_groups[work_item.callstack_group_index].callstack;
    const uint64_t occurrence_count = work_item.occurrence_count;
    const uint64_t raw_access_count = work_item.total_access_count;
    const uint64_t raw_size_bytes = work_item.total_size_bytes;

    if (!result.resolved) {
      unique_unresolved_count++;
      unresolved_access_count += raw_access_count;
      unresolved_size_bytes += raw_size_bytes;
      failed_resolution_callstacks.insert(callstack);
      continue;
    }

    unique_resolved_count++;
    resolved_access_count += raw_access_count;
    resolved_size_bytes += raw_size_bytes;
    resolved_callstacks.insert(callstack);
    stats.total_found_type += occurrence_count;
    QCHECK(!result.resolution_policy.empty())
        << "Resolved TypeTree has no resolution policy";
    resolution_keys_by_policy[result.resolution_policy]++;
    callstacks_by_policy[result.resolution_policy].insert(callstack);
    allocations_by_policy[result.resolution_policy] += occurrence_count;
    raw_accesses_by_policy[result.resolution_policy] += raw_access_count;
    raw_size_bytes_by_policy[result.resolution_policy] += raw_size_bytes;

    if (result.filtered_by_type) {
      unique_type_filtered_count++;
      continue;
    }
    unique_after_filtering_count++;
    stats.total_after_filtering += occurrence_count;

    if (result.is_record) {
      unique_record_count++;
      record_callstacks.insert(callstack);
      stats.total_record_count += occurrence_count;
    }
    if (result.filtered_by_only_records) {
      unique_only_records_filtered_count++;
      continue;
    }

    QCHECK(result.type_tree != nullptr) << "Resolved work item has no TypeTree";
    TypeTree* type_tree = result.type_tree.get();
    const absl::string_view type_name_view = type_tree->Name();
    const std::string type_name(type_name_view.data(), type_name_view.size());
    unique_type_counts[type_name]++;
    callstacks_by_type[type_name].insert(callstack);
    allocation_weighted_type_counts[type_name] += occurrence_count;

    if (result.histogram_warning && verify_verbose_) {
      LOG(WARNING) << "Collapsing histogram does not precisely align "
                      "with type size, counters may be distorted for:\n";
      LogCallStackAndTypeTree(callstack, type_tree, verify_verbose_);
    }
    if (result.verify_passed) {
      unique_verified_count++;
      verified_callstacks.insert(callstack);
      stats.total_verified += occurrence_count;
    } else {
      LogCallStackAndTypeTree(callstack, type_tree, verify_verbose_);
    }

    const uint64_t total_access_count = raw_access_count;
    stats.total_accesses += total_access_count;
    stats.total_size_bytes += raw_size_bytes;
    access_counts_by_type[type_name] += total_access_count;
    size_bytes_by_type[type_name] += raw_size_bytes;

    const absl::string_view allocation_category_view =
        AllocationCategoryToString(result.allocation_category);
    const std::string allocation_category(allocation_category_view.data(),
                                          allocation_category_view.size());
    resolution_keys_by_category[allocation_category]++;
    callstacks_by_category[allocation_category].insert(callstack);
    allocations_by_category[allocation_category] += occurrence_count;
    accesses_by_category[allocation_category] += total_access_count;
    size_bytes_by_category[allocation_category] += raw_size_bytes;

    if (type_name == "char") {
      char_root_resolution_key_count++;
      char_root_allocation_count += occurrence_count;
      char_root_access_count += total_access_count;
      char_root_size_bytes += raw_size_bytes;
      char_root_callstacks.insert(callstack);
    }

    if (result.from_container) {
      unique_container_count++;
      container_callstacks.insert(callstack);
      stats.container_alloc_count += occurrence_count;
      stats.total_accesses_on_containers += total_access_count;
      stats.total_size_bytes_on_containers += raw_size_bytes;
    } else {
      unique_heap_alloc_count++;
      heap_alloc_callstacks.insert(callstack);
      stats.heap_alloc_count += occurrence_count;
      stats.total_accesses_on_heapallocs += total_access_count;
      stats.total_size_bytes_on_heapallocs += raw_size_bytes;
    }
    if (result.is_record) {
      stats.total_accesses_on_records += total_access_count;
      stats.total_size_bytes_on_records += raw_size_bytes;
    }

    RETURN_IF_ERROR(type_tree_store->Insert(
        callstack, std::move(result.type_tree),
        std::move(work_item.allocation_ranges)));
    inserted_tree_count++;

    if ((work_index + 1) % merge_progress_interval == 0 ||
        work_index + 1 == work_items.size()) {
      LOG(INFO) << "TypeTreeStore merge progress: " << work_index + 1 << "/"
                << work_items.size() << ", current store size: "
                << type_tree_store->callstack_to_type_tree_.size();
    }
  }

  absl::flat_hash_set<TypeTreeStore::CallStack> fully_unresolved_callstacks;
  for (const TypeTreeStore::CallStack& callstack :
       failed_resolution_callstacks) {
    if (!resolved_callstacks.contains(callstack)) {
      fully_unresolved_callstacks.insert(callstack);
    }
  }
  if (verify_verbose_) {
    for (const TypeTreeStore::CallStack& callstack :
         fully_unresolved_callstacks) {
      LogCallStackAndTypeTree(callstack, nullptr, true);
    }
  }
  if (dump_unresolved_callstacks_) {
    for (const TypeTreeStore::CallStack& callstack :
         fully_unresolved_callstacks) {
      TypeTreeStore::DumpCallStack(callstack, std::cout, 0, true);
    }
  }

  LOG(INFO) << "Finished TypeTreeStore merge in "
            << absl::FormatDuration(absl::Now() - merge_start);
  stats.unique_callstack_count = callstack_groups.size();
  stats.resolved_callstack_count = resolved_callstacks.size();
  stats.unresolved_callstack_count = fully_unresolved_callstacks.size();
  stats.verified_callstack_count = verified_callstacks.size();
  stats.record_callstack_count = record_callstacks.size();
  stats.container_callstack_count = container_callstacks.size();
  stats.heap_alloc_callstack_count = heap_alloc_callstacks.size();
  stats.unique_resolution_key_count = work_items.size();
  stats.resolved_resolution_key_count = unique_resolved_count;
  stats.unresolved_resolution_key_count = unique_unresolved_count;
  stats.after_filtering_resolution_key_count = unique_after_filtering_count;
  stats.type_filtered_resolution_key_count = unique_type_filtered_count;
  stats.only_records_filtered_resolution_key_count =
      unique_only_records_filtered_count;
  stats.verified_resolution_key_count = unique_verified_count;
  stats.record_resolution_key_count = unique_record_count;
  stats.container_resolution_key_count = unique_container_count;
  stats.heap_alloc_resolution_key_count = unique_heap_alloc_count;
  stats.duplicate_resolution_count = duplicate_resolution_count;
  stats.inserted_tree_count = inserted_tree_count;
  stats.final_type_tree_store_size =
      type_tree_store->callstack_to_type_tree_.size();
  stats.resolved_raw_accesses = resolved_access_count;
  stats.unresolved_raw_accesses = unresolved_access_count;
  stats.total_raw_accesses = resolved_access_count + unresolved_access_count;
  stats.resolved_raw_size_bytes = resolved_size_bytes;
  stats.unresolved_raw_size_bytes = unresolved_size_bytes;
  stats.total_raw_size_bytes = resolved_size_bytes + unresolved_size_bytes;
  stats.char_root_callstack_count = char_root_callstacks.size();
  stats.char_root_resolution_key_count = char_root_resolution_key_count;
  stats.char_root_allocation_count = char_root_allocation_count;
  stats.char_root_access_count = char_root_access_count;
  stats.char_root_size_bytes = char_root_size_bytes;

  for (const auto& [type_name, resolution_key_count] : unique_type_counts) {
    Statistics::TypeStatistics& type_stats = stats.type_statistics[type_name];
    type_stats.callstack_count = callstacks_by_type.at(type_name).size();
    type_stats.resolution_key_count = resolution_key_count;
    type_stats.allocation_count = allocation_weighted_type_counts.at(type_name);
    type_stats.access_count = access_counts_by_type.at(type_name);
    type_stats.size_bytes = size_bytes_by_type.at(type_name);
  }
  for (const auto& [policy, resolution_key_count] : resolution_keys_by_policy) {
    Statistics::ResolutionPolicyStatistics& policy_stats =
        stats.resolution_policy_statistics[policy];
    policy_stats.callstack_count = callstacks_by_policy.at(policy).size();
    policy_stats.resolution_key_count = resolution_key_count;
    policy_stats.allocation_count = allocations_by_policy.at(policy);
    policy_stats.raw_access_count = raw_accesses_by_policy.at(policy);
    policy_stats.raw_size_bytes = raw_size_bytes_by_policy.at(policy);
  }
  for (AllocationCategory category :
       {AllocationCategory::kSimple, AllocationCategory::kAllocationAware,
        AllocationCategory::kComplex}) {
    const absl::string_view category_view =
        AllocationCategoryToString(category);
    const std::string category_name(category_view.data(), category_view.size());
    Statistics::AllocationCategoryStatistics& category_stats =
        stats.allocation_category_statistics[category_name];
    category_stats.callstack_count =
        callstacks_by_category[category_name].size();
    category_stats.resolution_key_count =
        resolution_keys_by_category[category_name];
    category_stats.allocation_count = allocations_by_category[category_name];
    category_stats.access_count = accesses_by_category[category_name];
    category_stats.size_bytes = size_bytes_by_category[category_name];
  }

  stats.Log();
  return std::make_unique<HistogramBuilderResults>(std::move(type_tree_store),
                                                   stats);
}

void TypeTreeStore::Dump(std::ostream& os, int64_t limit) const {
  int64_t n = limit < 0 ? callstack_to_type_tree_.size() : limit;
  int64_t i = 0;
  for (const auto& [callstack, type_tree] : callstack_to_type_tree_) {
    if (i >= n) return;
    os << "- Entry: \n";
    os << "    type_tree: \n";
    type_tree->Dump(os, 3);
    auto ranges_it = callstack_to_allocation_ranges_.find(callstack);
    if (ranges_it == callstack_to_allocation_ranges_.end() ||
        ranges_it->second.empty()) {
      os << "    allocation_ranges: []\n";
    } else {
      os << "    allocation_ranges:\n";
      for (const AllocationRange& range : ranges_it->second) {
        QCHECK_LE(range.size,
                  std::numeric_limits<uint64_t>::max() - range.alloc_address)
            << "Allocation address range overflows uint64_t";
        const uint64_t address_end = range.alloc_address + range.size;
        os << "      - address_start: 0x"
           << llvm::utohexstr(range.alloc_address, true) << "\n"
           << "        address_end: 0x" << llvm::utohexstr(address_end, true)
           << "\n"
           << "        cycle_start: 0x"
           << llvm::utohexstr(range.alloc_timestamp, true) << "\n"
           << "        cycle_end: 0x"
           << llvm::utohexstr(range.dealloc_timestamp, true) << "\n";
      }
    }
    os << "    callstack: \n";
    DumpCallStack(callstack, os, 3);
    i++;
  }
}

void TypeTreeStore::DumpFlamegraph(std::ostream& os, int64_t limit) const {
  int64_t n = limit < 0 ? callstack_to_type_tree_.size() : limit;
  int64_t i = 0;
  for (const auto& [unused, type_tree] : callstack_to_type_tree_) {
    if (i >= n) return;
    type_tree->DumpFlameGraph(os, i + 1);
    i++;
  }
}

TypeTreeStore::CallStack TypeTreeStore::ConvertCallStack(
    absl::Span<const llvm::memprof::Frame> callstack) {
  std::vector<DwarfMetadataFetcher::Frame> dwarf_callstack;
  dwarf_callstack.reserve(callstack.size());
  for (const auto& frame : callstack) {
    dwarf_callstack.push_back(DwarfMetadataFetcher::Frame(
        frame.hasSymbolName() ? frame.getSymbolName().str() : "<none>",
        frame.LineOffset, frame.Column));
  }
  return dwarf_callstack;
}

absl::StatusOr<const TypeTree*> TypeTreeStore::InsertAndGet(
    const CallStack& callstack, std::unique_ptr<TypeTree> type_tree) {
  RETURN_IF_ERROR(Insert(callstack, std::move(type_tree)));
  return callstack_to_type_tree_[callstack].get();
}

absl::Status TypeTreeStore::Insert(
    const CallStack& callstack, std::unique_ptr<TypeTree> type_tree,
    std::vector<AllocationRange> allocation_ranges) {
  if (!type_tree) return absl::InvalidArgumentError("TypeTree is null.");
  auto it = callstack_to_type_tree_.find(callstack);
  if (it != callstack_to_type_tree_.end()) {
    const TypeTree* current_type_tree = it->second.get();
    if (current_type_tree->Name() != type_tree->Name()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Trying to insert different type trees for the same callstack: ",
          current_type_tree->Name(), " vs ", type_tree->Name()));
    }
    if (current_type_tree->ResolutionPolicy() !=
        type_tree->ResolutionPolicy()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Trying to insert type trees resolved by different policies for "
          "the same callstack: ",
          current_type_tree->ResolutionPolicy(), " vs ",
          type_tree->ResolutionPolicy()));
    }
    RETURN_IF_ERROR(type_tree->MergeCounts(current_type_tree));
  }
  callstack_to_type_tree_[callstack] = std::move(type_tree);
  std::vector<AllocationRange>& stored_ranges =
      callstack_to_allocation_ranges_[callstack];
  stored_ranges.insert(stored_ranges.end(), allocation_ranges.begin(),
                       allocation_ranges.end());
  return absl::OkStatus();
}

std::vector<TypeTreeStore::CallStack> TypeTreeStore::GetCallStacksForTypeName(
    std::string root_type_name) const {
  std::vector<CallStack> callstacks;
  for (const auto& [callstack, type_tree] : callstack_to_type_tree_) {
    if (type_tree->Name() == root_type_name) callstacks.push_back(callstack);
  }
  return callstacks;
}

absl::StatusOr<std::shared_ptr<TypeTree>> TypeTreeStore::GetTypeTree(
    const std::vector<DwarfMetadataFetcher::Frame>& callstack) const {
  auto it = callstack_to_type_tree_.find(callstack);
  if (it != callstack_to_type_tree_.end()) return it->second;
  return absl::NotFoundError("TypeTree not found for callstack.");
}

bool LocalHistogramBuilder::FilterType(absl::string_view type_name) const {
  if (type_prefix_filter_.empty()) return false;
  for (const auto& type : type_prefix_filter_) {
    if (absl::StartsWith(type_name, type)) return false;
  }
  return true;
}

bool LocalHistogramBuilder::FilterCallstack(
    const TypeTreeStore::CallStack& callstack) const {
  if (callstack_filter_.empty()) return false;
  for (const auto& frame : callstack) {
    for (const auto& callstack_filter : callstack_filter_) {
      if (frame.function_name == callstack_filter) return false;
    }
  }
  return true;
}

absl::StatusOr<std::unique_ptr<AbstractHistogramBuilder>>
LocalHistogramBuilder::Create(
    std::string memprof_profile, std::string memprof_profiled_binary,
    std::string memprof_profiled_binary_dwarf,
    const std::vector<std::string>& type_prefix_filter,
    const std::vector<std::string>& callstack_filter, bool only_records,
    bool verify_verbose, bool dump_unresolved_callstacks,
    uint32_t parse_thread_count) {
  std::string build_id;
  auto status_or = GetBuildIdForLocalFile(memprof_profiled_binary);
  if (status_or.ok()) {
    build_id = status_or.value();
  } else {
    LOG(WARNING) << "Failed to get build id for local file: "
                 << status_or.status() << " continuing with empty build id.";
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer_or_error =
      llvm::MemoryBuffer::getFile(memprof_profile);
  if (auto ec = buffer_or_error.getError()) {
    return absl::InternalError(absl::StrFormat(
        "Error opening profile file `%s`:%s", memprof_profile, ec.message()));
  }

  LOG(INFO) << "Creating RawMemprof reader for " << memprof_profiled_binary
            << " and " << memprof_profile << "...";
  llvm::parallel::strategy = llvm::hardware_concurrency(parse_thread_count);
  llvm::Expected<std::unique_ptr<RawMemProfReader>> rawmemprof_reader =
      llvm::memprof::RawMemProfReader::create(std::move(buffer_or_error.get()),
                                              memprof_profiled_binary, true);
  if (llvm::Error error = rawmemprof_reader.takeError()) {
    return absl::InternalError(absl::StrFormat(
        "Could not create reader: %s", llvm::toString(std::move(error))));
  }

  LOG(INFO) << "Creating Binary file retriever...";
  ASSIGN_OR_RETURN(std::unique_ptr<BinaryFileRetriever> binary_file_retriever,
                   BinaryFileRetriever::CreateBinaryFileRetriever());
  auto dwarf_metadata_fetcher = std::make_unique<DwarfMetadataFetcher>(
      std::move(binary_file_retriever), kCacheDir, true, false,
      parse_thread_count);
  LOG(INFO) << "Fetching DWP with path: " << memprof_profiled_binary_dwarf
            << " for build id: " << build_id << "\n";
  RETURN_IF_ERROR(dwarf_metadata_fetcher->FetchDWPWithPath(
      {{.build_id = build_id, .path = memprof_profiled_binary_dwarf}}, true));

  auto type_resolver = std::make_unique<DwarfTypeResolver>(
      std::move(dwarf_metadata_fetcher), true);
  return std::make_unique<LocalHistogramBuilder>(
      std::move(rawmemprof_reader.get()), std::move(type_resolver),
      type_prefix_filter, callstack_filter, only_records, verify_verbose,
      dump_unresolved_callstacks, parse_thread_count);
}

void TypeTreeStore::DumpCallStack(const CallStack& callstack, std::ostream& os,
                                  int level, bool as_entry) {
  if (as_entry) {
    os << "- entry: \n";
    level += 2;
  }
  std::string indent;
  for (size_t i = 0; i < static_cast<size_t>(level); ++i) indent += "  ";
  for (const auto& frame : callstack) {
    os << indent << "- function_name: " << frame.function_name << "\n"
       << indent << "  line_offset: " << frame.line_offset << "\n"
       << indent << "  column: " << frame.column << "\n";
  }
}

}  // namespace devtools_crosstool_fdo_field_access
