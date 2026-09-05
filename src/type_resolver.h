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

#ifndef TYPE_RESOLVER_H_
#define TYPE_RESOLVER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "dwarf_metadata_fetcher.h"
#include "src/object_layout.pb.h"
#include "type_tree.h"

namespace devtools_crosstool_fdo_field_access {

enum class AllocationCategory : uint8_t {
  kSimple,
  kAllocationAware,
  kComplex,
};

absl::string_view AllocationCategoryToString(
    AllocationCategory allocation_category);

struct TypeResolutionResult {
  std::unique_ptr<TypeTree> type_tree;
  AllocationCategory allocation_category = AllocationCategory::kSimple;
  std::string resolution_strategy;
};

// The AbstractTypeResolver is expected to be able to build a TypeTree from a
// given type name. For now, the approach is to use DWARF debuginfo, but this
// can also be done by Clang frontend (or clang-tidy). Resolving a TypeTree
// from a type name is used for any allocation made outside of a container.
// Similarly, the AbstractTypeResolver is expected to be able to build a
// TypeTree given a function name. This is used for any type of allocation
// that is made within a container.
class AbstractTypeResolver {
 public:
  using CallStack = std::vector<DwarfMetadataFetcher::Frame>;

  virtual ~AbstractTypeResolver() = default;

  virtual absl::StatusOr<std::unique_ptr<TypeTree>> ResolveTypeFromTypeName(
      absl::string_view type_name) = 0;

  virtual absl::StatusOr<std::unique_ptr<TypeTree>> ResolveTypeFromFrame(
      const DwarfMetadataFetcher::Frame& frame) = 0;

  virtual absl::StatusOr<TypeResolutionResult> ResolveTypeFromCallstack(
      const CallStack& callstack, int64_t request_size,
      absl::Time deadline) = 0;
};

class DwarfTypeResolver final : public AbstractTypeResolver {
 public:
  // Context for building the tree. This holds information about both the
  // current node being built, and the parent. It combines split information
  // held in FieldData and TypeData.
  struct BuilderCtxt {
    // For node under construction:
    std::string type_name;
    std::string field_name;
    int64_t field_index;
    int64_t field_offset;
    int64_t multiplicity;

    // For already built parent_node, required for setting global offset into
    // tree.
    const TypeTree::Node* parent_node;
    // Resolved fields are only required to infer the size of unresolved types,
    // so that we can guess the size of a field even if Dwarf data is missing,
    // without breaking the tree invariants.
    const std::vector<const DwarfMetadataFetcher::FieldData*> resolved_fields;

    absl::flat_hash_set<const DwarfMetadataFetcher::TypeData*> active_types;
    int recursion_depth = 0;
  };

  // This struct holds the information needed to resolve a type from a function
  // name. The type resolution strategy can always be determined by the provided
  // callstack. For example, the STL containers need to be resolved from the
  // leaf node, which will give the "real" allocated type. Abseil containers on
  // the other hand erase the type at the leaf node, so we need to resolve the
  // type from further up in the callstack.
  struct ContainerResolutionStrategy {
    enum Type {
      kDefaultStrategy,
      kSpecialAllocatingFunction,
      kCharContainer,
      kAllocatorAllocate,
      kAbslAllocatorAllocate,
      kLeafContainerGWPStrategy,
      kAbseilContainerSwissMapNodeHash,
      kAbseilContainerSwissMapFlatHash,
      kAbseilContainerBtree,
      kAbseilContainerInserted,
      kADTContainer,
      kADTDenseContainer,
      kProtobufArena,
      kGrpcArena,
      kXcallocator,
      kGgcAlloc,
      kGgcAllocStat
    };

    static std::string TypeToString(Type type) {
      switch (type) {
        case kDefaultStrategy:
          return "kDefaultStrategy";
        case kSpecialAllocatingFunction:
          return "kSpecialAllocatingFunction";
        case kCharContainer:
          return "kCharContainer";
        case kAllocatorAllocate:
          return "kAllocatorAllocate";
        case kAbslAllocatorAllocate:
          return "kAbslAllocatorAllocate";
        case kLeafContainerGWPStrategy:
          return "kLeafContainerGWPStrategy";
        case kAbseilContainerSwissMapNodeHash:
          return "kAbseilContainerSwissMapNodeHash";
        case kAbseilContainerSwissMapFlatHash:
          return "kAbseilContainerSwissMapFlatHash";
        case kAbseilContainerBtree:
          return "kAbseilContainerBtree";
        case kAbseilContainerInserted:
          return "kAbseilContainerInserted";
        case kADTContainer:
          return "kADTContainer";
        case kADTDenseContainer:
          return "kADTDenseContainer";
        case kProtobufArena:
          return "protobuf-arena";
        case kGrpcArena:
          return "grpc-arena";
        case kXcallocator:
          return "xcallocator";
        case kGgcAlloc:
          return "ggc-alloc";
        case kGgcAllocStat:
          return "ggc-alloc-stat";
        default:
          return "kUnknownType";
      }
    }

    // Name of the container within which the allocation is made.
    std::string container_name;
    // Mangled function name of the critical element in the callstack.
    std::string func_name;
    // Name of the container class. Not always necessary, but some containers
    // rely on this to resolve the type, for example Abseil containers.
    std::string lookup_type;
    // Type of container resolution strategy. This determines what DWARF data
    // to look for.
    Type container_type;

    explicit ContainerResolutionStrategy(absl::string_view container_name,
                                         absl::string_view func_name,
                                         Type container_type,
                                         std::string lookup_type = "")
        : container_name(container_name.data(), container_name.size()),
          func_name(func_name.data(), func_name.size()),
          lookup_type(std::move(lookup_type)),
          container_type(container_type) {}

    explicit ContainerResolutionStrategy() = default;
  };

  // Expects the DWARF metadata fetcher to be initialized and to have already
  // fetched the data.
  explicit DwarfTypeResolver(
      std::unique_ptr<DwarfMetadataFetcher> metadata_fetcher,
      bool is_local = false)
      : metadata_fetcher_(std::move(metadata_fetcher)), is_local_(is_local) {}

  absl::StatusOr<std::unique_ptr<TypeTree>> CreateTreeFromDwarf(
      absl::string_view type_name, bool from_container = false,
      absl::string_view container_name = "");

  absl::StatusOr<std::unique_ptr<TypeTree>> ResolveTypeFromTypeName(
      absl::string_view type_name) override;

  // Resolves the type of the given function name within which the allocation is
  // made. This only works for allocations made within an allocation-aware
  // container.
  absl::StatusOr<std::unique_ptr<TypeTree>> ResolveTypeFromResolutionStrategy(
      const ContainerResolutionStrategy& resolution_strategy,
      const CallStack& callstack, int64_t request_size);

  // Resolve the type for an allocation made at a specific frame. Relies on
  // DW_TAG_GOOGLE_heapalloc DWARF tag, internal llvm-patch: cl/647366639.
  absl::StatusOr<std::unique_ptr<TypeTree>> ResolveTypeFromFrame(
      const DwarfMetadataFetcher::Frame& frame) override;

  // Resolve the type from a callstack. First tries the container strategy. If
  // that does not resolve, walk the frames and try DW_TAG_GOOGLE_heapalloc.
  absl::StatusOr<TypeResolutionResult> ResolveTypeFromCallstack(
      const CallStack& callstack, int64_t request_size,
      absl::Time deadline = absl::InfiniteFuture()) override;

  // Only public for testing.
  static std::string UnwrapAndCleanTypeName(absl::string_view type_name);
  static std::string MakeTypePrefixPattern(absl::string_view canonical);
  static std::optional<std::string> TypeStartsWith(absl::string_view s,
                                                   absl::string_view canonical);

 private:
  static bool IsIndirection(absl::string_view type_name);
  static int64_t GetArrayMultiplicity(absl::string_view type_name);
  static std::string GetArrayChildTypeName(absl::string_view type_name);
  static void DereferencePointer(std::string* type_name);
  static void CleanTypeName(std::string* type_name);

  std::unique_ptr<DwarfMetadataFetcher> metadata_fetcher_;

  std::unique_ptr<TypeTree::Node> BuildTreeRecursive(BuilderCtxt ctxt);

  absl::StatusOr<std::unique_ptr<TypeTree::Node>> BuildTree(
      absl::string_view type_name);

  absl::StatusOr<ContainerResolutionStrategy>
  GetCallStackContainerResolutionStrategy(const CallStack& callstack);

  // This resolves cases where two fields of an object have the same offset.
  // For now, use a size heuristic, inheritance heuristic, and then a name
  // prefix heuristic, in that order. If the conflict cannot be resolved after
  // those three, take the first type seen.
  absl::StatusOr<std::vector<const DwarfMetadataFetcher::FieldData*>>
  ResolveFieldConflicts(const DwarfMetadataFetcher::TypeData* type_data);

  absl::StatusOr<int64_t> GetAlignmentFromAbslAllocatorCall(
      absl::string_view function_name);

  bool IsLocalTypeResolver() const { return is_local_; }

  // Whether the type resolution is on profile information from GWP or from
  // MemProf instrumentation. There are some cases where the strategy is
  // different for GWP and MemProf, for example Abseil containers.
  bool is_local_;
};

}  // namespace devtools_crosstool_fdo_field_access

#endif  // TYPE_RESOLVER_H_
