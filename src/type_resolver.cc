// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "type_resolver.h"

#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "dwarf_metadata_fetcher.h"
#include "llvm/include/llvm/Demangle/Demangle.h"
#include "re2/re2.h"
#include "status_macros.h"
#include "type_tree.h"
#include "type_tree_container_blueprints.h"

namespace devtools_crosstool_fdo_field_access {

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

absl::string_view AllocationCategoryToString(
    AllocationCategory allocation_category) {
  switch (allocation_category) {
    case AllocationCategory::kSimple:
      return "simple";
    case AllocationCategory::kAllocationAware:
      return "allocation-aware";
    case AllocationCategory::kComplex:
      return "complex";
  }

  return "unknown";
}

constexpr absl::string_view kSTLContainerTypes[] = {
    "std::_Vector_base",
    "std::_Vector_base",
    "std::_Deque_base",
    "std::_Deque_base",
    "std::_Rb_tree",
    "std::_Rb_tree",
    "std::__tree",
    "std::__tree",
    "std::__detail::_Hashtable_alloc",
    "std::__detail::_Hashtable_alloc",
    "std::_Fwd_list_base",
    "std::_Fwd_list_base",
    "std::_List_base",
    "std::list",
    "absl::FixedArray"};

constexpr absl::string_view kSTLContainerLeafCheckTypes[] = {
    "std::vector",
    "std::deque",
    "std::set",
    "std::forward_list",
    "std::list",
    "std::list",
    "std::stack",
    "std::queue",
    "std::priority_queue",
    "std::map",
    "std::multimap",
    "std::multiset",
    "std::flat_multiset",
    "std::flat_multimap",
    "std::unordered_set",
    "std::unordered_map",
    "std::unordered_multiset",
    "std::unordered_multimap",
};

constexpr absl::string_view kSmartPointersTypes[] = {
    "_ZSt11make_unique", "_ZSt11make_shared", "_ZNS15allocate_shared",
    "_ZNS11make_unique"};

constexpr absl::string_view kADTContainerTypes[] = {
    "llvm::SmallVectorTemplateBase<",
    "llvm::PagedVector<",
    "llvm::SmallPtrSetImpl<",
    "llvm::StringMap<",
    "llvm::ImutAVLFactory<",
    "absl::inlined_vector_internal::Storage<",
    // GCC's hand-rolled growable array (vec.h). va_heap::reserve<T> and
    // va_gc::reserve<T, A> take the vec<T, ...> as an explicit formal
    // parameter (a reference to a pointer, since realloc may move the
    // buffer), which is exactly the shape this strategy already handles
    // for llvm::SmallVectorTemplateBase<T>.
    "vec<"};

constexpr absl::string_view kADTDenseContainerTypes[] = {"llvm::DenseMapBase"};

constexpr absl::string_view kCharContainerTypesLeafFrame[] = {
    "std::basic_string", "std::basic_string", "absl::cord_internal::",
    "std::basic_string", "absl::Cord::",      "std::basic_istream<",
    "operator>>",
    // GCC's GC allocator for memory that contains no GC-managed pointers
    // (so the collector never needs to scan/mark it) -- effectively an
    // untyped byte blob, same as the other entries in this list.
    "ggc_alloc_atomic"};

constexpr absl::string_view kABSLContainerSwissMapTypes[] = {
    "absl::container_internal::raw_hash_map<",
    "absl::container_internal::raw_hash_set<",
};

constexpr absl::string_view kContiguousTemplateContainerTypes[] = {
    "xalanc_1_10::XalanVector<",
    "xercesc::ValueVectorOf<",
    "xercesc_3_1::ValueVectorOf<",
    "xercesc_3_2::ValueVectorOf<",
};

constexpr absl::string_view kABSLContainerFlatHashTypes[] = {
    "absl::container_internal::FlatHashMapPolicy",
    "absl::container_internal::FlatHashSetPolicy",
};

constexpr absl::string_view kABSLContainerBtreeTypes[] = {
    "absl::container_internal::btree<",
};

constexpr absl::string_view kSpecialAllocatingFunctions[] = {
    "std::get_temporary_buffer",
    "std::get_temporary_buffer",
    "__gnu_cxx::get_temporary_buffer",
};

constexpr absl::string_view kAllocatorWrappers[] = {
    "std::allocator",
    "std::__new_allocator",
    "__gnu_cxx::new_allocator",
    "muppet::instant::PolymorphicAllocator",
    "xalanc_1_10::MemoryManagedConstructionTraits",
    "llama::bloballoc::AlignedAllocator"};

constexpr absl::string_view kProtobufArenaConstructingFunctions[] = {
    "google::protobuf::Arena::DefaultConstruct<",
    "google::protobuf::Arena::Create<",
    // Copy-constructs T on the arena (protobuf message copy, e.g. via
    // Message::New(Arena*) + CopyFrom, or RepeatedPtrField element copies).
    "google::protobuf::Arena::CopyConstruct<",
    // Allocates an array of T on the arena (e.g. RepeatedField<T> growth
    // backed by arena storage).
    "google::protobuf::Arena::CreateArray<",
};

// grpc_core::Arena::Alloc/AllocZone are untyped size_t-only bump allocators,
// so the type can only be recovered from the templated factory that calls
// them. grpc_core::Arena::New<T>(...) is that factory; its mangled name
// carries T even though the frames below it (Alloc/AllocZone/
// gpr_malloc_aligned) do not. ManagedNew<T> and MakeRefCounted<T> both
// forward into New<...> (e.g. as New<ManagedNewImpl<T>>), so matching New<
// alone covers them too.
constexpr absl::string_view kGrpcArenaConstructingFunctions[] = {
    "grpc_core::Arena::New<",
};

// xcallocator<Type>::data_alloc(size_t) (gcc/hash-table.h) is a static
// member function template that does
// `return static_cast<Type*>(xcalloc(count, sizeof(Type)))`. Type appears
// only in the class template argument (there is no allocator instance and
// no formal parameter carrying it, since data_alloc is static), so it has
// to be pulled from the function's own demangled name, same as the arena
// factories above.
constexpr absl::string_view kXcallocatorConstructingFunctions[] = {
    "xcallocator<",
};

// GCC's garbage collector allocation entry point, `template<typename T> T*
// ggc_alloc()` (ggc.h). Behaves exactly like grpc_core::Arena::New<T> and
// google::protobuf::Arena::Create<T>: T only shows up in the function's own
// template argument.
constexpr absl::string_view kGgcAllocConstructingFunctions[] = {
    "ggc_alloc<",
};

// GCC's gengtype code generator also emits one non-template allocation
// function per GC-managed type, named `ggc_alloc_<Type>_stat` (or
// `ggc_alloc_cleared_<Type>_stat` for the zero-initializing variant), e.g.
// `ggc_alloc_cleared_tree_node_stat` allocates a `tree_node`. Unlike the
// template factories above, Type has to be recovered from the function
// name's own prefix/suffix rather than a template argument -- see
// ExtractGgcAllocStatType.
constexpr absl::string_view kGgcAllocStatPrefix = "ggc_alloc_";
constexpr absl::string_view kGgcAllocStatClearedInfix = "cleared_";
constexpr absl::string_view kGgcAllocStatSuffix = "_stat";

constexpr absl::string_view kMemprofInsertedFunctions[] = {
    "__memprof_ctrl_alloc",
};

static std::string stripTrailingColons(const std::string& str) {
  size_t last_non_colon = str.find_last_not_of(':');
  if (last_non_colon == std::string::npos) {
    return "";
  }
  return str.substr(0, last_non_colon + 1);
}

static std::optional<std::string> ExtractFirstTemplateArgument(
    absl::string_view demangled_name, absl::string_view function_prefix) {
  const size_t prefix_pos = demangled_name.find(function_prefix);
  if (prefix_pos == absl::string_view::npos) {
    return std::nullopt;
  }

  const size_t argument_begin = prefix_pos + function_prefix.size();
  int angle_depth = 0;
  int paren_depth = 0;
  int bracket_depth = 0;
  int brace_depth = 0;

  for (size_t i = argument_begin; i < demangled_name.size(); ++i) {
    const char c = demangled_name[i];

    switch (c) {
      case '<':
        ++angle_depth;
        break;
      case '>':
        if (angle_depth == 0) {
          absl::string_view argument =
              demangled_name.substr(argument_begin, i - argument_begin);
          argument = absl::StripAsciiWhitespace(argument);
          if (argument.empty()) {
            return std::nullopt;
          }
          return std::string(argument);
        }
        --angle_depth;
        break;
      case '(':
        ++paren_depth;
        break;
      case ')':
        if (paren_depth > 0) {
          --paren_depth;
        }
        break;
      case '[':
        ++bracket_depth;
        break;
      case ']':
        if (bracket_depth > 0) {
          --bracket_depth;
        }
        break;
      case '{':
        ++brace_depth;
        break;
      case '}':
        if (brace_depth > 0) {
          --brace_depth;
        }
        break;
      case ',':
        if (angle_depth == 0 && paren_depth == 0 && bracket_depth == 0 &&
            brace_depth == 0) {
          absl::string_view argument =
              demangled_name.substr(argument_begin, i - argument_begin);
          argument = absl::StripAsciiWhitespace(argument);
          if (argument.empty()) {
            return std::nullopt;
          }
          return std::string(argument);
        }
        break;
      default:
        break;
    }
  }

  return std::nullopt;
}

// Given the content already inside a template's angle brackets (e.g.
// "float, 64" from AlignedAllocator<float, 64>), returns just the first
// top-level, comma-separated argument ("float"). Returns the whole input
// unchanged if it contains no top-level comma.
static std::string TakeFirstTopLevelTemplateArgument(absl::string_view s) {
  int angle_depth = 0;
  int paren_depth = 0;
  int bracket_depth = 0;
  int brace_depth = 0;

  for (size_t i = 0; i < s.size(); ++i) {
    switch (s[i]) {
      case '<':
        ++angle_depth;
        break;
      case '>':
        if (angle_depth > 0) --angle_depth;
        break;
      case '(':
        ++paren_depth;
        break;
      case ')':
        if (paren_depth > 0) --paren_depth;
        break;
      case '[':
        ++bracket_depth;
        break;
      case ']':
        if (bracket_depth > 0) --bracket_depth;
        break;
      case '{':
        ++brace_depth;
        break;
      case '}':
        if (brace_depth > 0) --brace_depth;
        break;
      case ',':
        if (angle_depth == 0 && paren_depth == 0 && bracket_depth == 0 &&
            brace_depth == 0) {
          return std::string(absl::StripAsciiWhitespace(s.substr(0, i)));
        }
        break;
      default:
        break;
    }
  }
  return std::string(s);
}

static std::optional<std::string> ExtractProtobufArenaType(
    absl::string_view demangled_name) {
  for (absl::string_view function_prefix :
       kProtobufArenaConstructingFunctions) {
    if (auto type_name =
            ExtractFirstTemplateArgument(demangled_name, function_prefix)) {
      return type_name;
    }
  }
  return std::nullopt;
}

static std::optional<std::string> ExtractGrpcArenaType(
    absl::string_view demangled_name) {
  for (absl::string_view function_prefix : kGrpcArenaConstructingFunctions) {
    if (auto type_name =
            ExtractFirstTemplateArgument(demangled_name, function_prefix)) {
      return type_name;
    }
  }
  return std::nullopt;
}

static std::optional<std::string> ExtractXcallocatorType(
    absl::string_view demangled_name) {
  for (absl::string_view function_prefix : kXcallocatorConstructingFunctions) {
    if (auto type_name =
            ExtractFirstTemplateArgument(demangled_name, function_prefix)) {
      return type_name;
    }
  }
  return std::nullopt;
}

static std::optional<std::string> ExtractGgcAllocType(
    absl::string_view demangled_name) {
  for (absl::string_view function_prefix : kGgcAllocConstructingFunctions) {
    if (auto type_name =
            ExtractFirstTemplateArgument(demangled_name, function_prefix)) {
      return type_name;
    }
  }
  return std::nullopt;
}

// Matches gengtype-generated allocators named `ggc_alloc_<Type>_stat` or
// `ggc_alloc_cleared_<Type>_stat` and returns <Type>, e.g.
// "ggc_alloc_cleared_tree_node_stat" -> "tree_node".
static std::optional<std::string> ExtractGgcAllocStatType(
    absl::string_view demangled_name) {
  if (!absl::StartsWith(demangled_name, kGgcAllocStatPrefix) ||
      !absl::EndsWith(demangled_name, kGgcAllocStatSuffix)) {
    return std::nullopt;
  }
  absl::string_view middle = demangled_name.substr(
      kGgcAllocStatPrefix.size(),
      demangled_name.size() - kGgcAllocStatPrefix.size() -
          kGgcAllocStatSuffix.size());
  absl::ConsumePrefix(&middle, kGgcAllocStatClearedInfix);
  if (middle.empty()) {
    return std::nullopt;
  }
  return std::string(middle);
}

std::string DwarfTypeResolver::MakeTypePrefixPattern(
    absl::string_view canonical) {
  while (!canonical.empty() && canonical.back() == ':') {
    canonical.remove_suffix(1);
  }
  const size_t lt = canonical.find('<');
  absl::string_view head =
      (lt == absl::string_view::npos) ? canonical : canonical.substr(0, lt);

  const size_t first = head.find("::");
  if (first == absl::string_view::npos) {
    return "^(" + re2::RE2::QuoteMeta(std::string(head)) + ")(?:$|::|<)";
  }

  std::string ns_root(head.substr(0, first));
  std::string tail(head.substr(first + 2));
  return "^(" + re2::RE2::QuoteMeta(ns_root) + "::(?:[^:]+::)?" +
         re2::RE2::QuoteMeta(tail) + ")(?:$|::|<)";
}

std::optional<std::string> DwarfTypeResolver::TypeStartsWith(
    absl::string_view s, absl::string_view canonical) {
  const std::string pat = MakeTypePrefixPattern(canonical);
  re2::RE2 re(pat);
  std::string m;
  if (re2::RE2::PartialMatch(s, re, &m)) {
    return m;
  }
  return std::nullopt;
}

static std::optional<const std::string> StartsWithAnyOf(
    absl::string_view str, const absl::string_view keywords[], size_t N) {
  for (int i = 0; i < N; ++i) {
    if (DwarfTypeResolver::TypeStartsWith(str, keywords[i])) {
      return std::string(keywords[i]);
    }
  }
  return std::nullopt;
}

std::string BuildCallstackString(
    const DwarfTypeResolver::CallStack& callstack) {
  std::string callstack_string;
  for (const auto& frame : callstack) {
    absl::StrAppend(&callstack_string, frame.function_name,
                    " l:", frame.line_offset, " c:", frame.column, "\n");
  }
  return callstack_string;
}

std::string BuildErrorMessageInResolution(
    const std::vector<std::string>& formal_params,
    const DwarfTypeResolver::CallStack& callstack,
    DwarfTypeResolver::ContainerResolutionStrategy strategy,
    absl::string_view extra_info = "") {
  std::string error_message =
      absl::StrCat("Type resolution strategy failed: ",
                   DwarfTypeResolver::ContainerResolutionStrategy::TypeToString(
                       strategy.container_type),
                   " for container: ", strategy.container_name,
                   " with container class name: ", strategy.lookup_type,
                   " with formal params: ");
  for (const auto& param : formal_params) {
    absl::StrAppend(&error_message, param, " ");
  }
  absl::StrAppend(&error_message, " at callstack: \n",
                  BuildCallstackString(callstack));
  if (!extra_info.empty()) {
    absl::StrAppend(&error_message, "\n", extra_info);
  }
  return error_message;
}

absl::StatusOr<std::vector<const DwarfMetadataFetcher::FieldData*>>
DwarfTypeResolver::ResolveFieldConflicts(
    const DwarfMetadataFetcher::TypeData* type_data) {
  std::vector<const DwarfMetadataFetcher::FieldData*> resolved_fields;

  if (type_data->data_type == DwarfMetadataFetcher::DataType::UNION) {
    resolved_fields.reserve(type_data->fields.size());
    for (auto& field : type_data->fields) {
      resolved_fields.push_back(field.get());
    }
    return resolved_fields;
  }

  absl::flat_hash_set<size_t> offsets;
  for (auto& field : type_data->fields) {
    offsets.insert(field->offset);
  }
  std::vector<size_t> sorted_offsets(offsets.begin(), offsets.end());
  std::sort(sorted_offsets.begin(), sorted_offsets.end());

  for (size_t offset : sorted_offsets) {
    const DwarfMetadataFetcher::TypeData* type_data_for_offset = nullptr;
    const DwarfMetadataFetcher::FieldData* field_data_for_offset = nullptr;

    auto it = type_data->offset_idx.find(offset);
    if (it == type_data->offset_idx.end()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Dwarf data is invalid, field offset index and "
                       "field data invalid for type: ",
                       type_data->name));
    }

    if (it->second.size() == 1) {
      resolved_fields.push_back(type_data->fields[*(it->second.begin())].get());
      continue;
    }

    absl::StatusOr<const DwarfMetadataFetcher::TypeData*> status_or_type_data;
    for (const auto& idx : it->second) {
      const DwarfMetadataFetcher::FieldData* field_data =
          type_data->fields[idx].get();

      if (!field_data) {
        return absl::InternalError(
            absl::StrCat("Field data is null for type: ", type_data->name,
                         " at offset: ", offset));
      }

      status_or_type_data = metadata_fetcher_->GetType(field_data->type_name);
      if (!status_or_type_data.ok()) {
        continue;
      }

      if (!type_data_for_offset || !field_data_for_offset) {
        type_data_for_offset = status_or_type_data.value();
        field_data_for_offset = field_data;
        continue;
      }

      if (type_data_for_offset->size == status_or_type_data.value()->size &&
          type_data_for_offset->fields.size() ==
              status_or_type_data.value()->fields.size()) {
        if (!field_data_for_offset->inherited && field_data->inherited) {
          type_data_for_offset = status_or_type_data.value();
          field_data_for_offset = field_data;
        } else if (absl::StartsWith(field_data_for_offset->name, "_") &&
                   !absl::StartsWith(field_data->name, "_")) {
          type_data_for_offset = status_or_type_data.value();
          field_data_for_offset = field_data;
        }
        continue;
      }

      if (type_data_for_offset->size < status_or_type_data.value()->size) {
        type_data_for_offset = status_or_type_data.value();
        field_data_for_offset = field_data;
        continue;
      }

      if (type_data_for_offset->fields.size() <
          status_or_type_data.value()->fields.size()) {
        type_data_for_offset = status_or_type_data.value();
        field_data_for_offset = field_data;
      }
    }

    if (!type_data_for_offset || !field_data_for_offset) {
      return std::vector<const DwarfMetadataFetcher::FieldData*>();
    }

    resolved_fields.push_back(field_data_for_offset);
  }

  if (resolved_fields.size() != type_data->offset_idx.size()) {
    return absl::InternalError(absl::StrCat(
        "Panic! Resolve field conflicts was not able to resolve "
        "all fields for type: ",
        type_data->name, ". Size after resolve: ", resolved_fields.size(),
        " vs original size: ", type_data->offset_idx.size(),
        " Size before resolve: ", type_data->fields.size(),
        " Should be size: ", type_data->offset_idx.size()));
  }
  return resolved_fields;
}

bool DwarfTypeResolver::IsIndirection(absl::string_view type_name) {
  return absl::EndsWith(type_name, "*") || absl::EndsWith(type_name, "&") ||
         absl::EndsWith(type_name, "()") || absl::EndsWith(type_name, ")>");
}

int64_t DwarfTypeResolver::GetArrayMultiplicity(absl::string_view type_name) {
  if (type_name.size() < 3 || type_name.back() != ']') {
    return 1;
  }

  const size_t open_bracket = type_name.rfind('[');
  if (open_bracket == absl::string_view::npos ||
      open_bracket + 1 == type_name.size() - 1) {
    return 1;
  }

  int64_t multiplicity = 0;
  for (size_t i = open_bracket + 1; i + 1 < type_name.size(); ++i) {
    const char c = type_name[i];
    if (c < '0' || c > '9') {
      return 1;
    }

    const int64_t digit = c - '0';
    if (multiplicity > (std::numeric_limits<int64_t>::max() - digit) / 10) {
      return 1;
    }
    multiplicity = multiplicity * 10 + digit;
  }

  return multiplicity;
}

std::string DwarfTypeResolver::GetArrayChildTypeName(
    absl::string_view type_name) {
  std::string child_type_name = std::string(type_name);
  RE2::GlobalReplace(&child_type_name, "\\[(\\d+)\\]$", "");
  return child_type_name;
}

void DwarfTypeResolver::DereferencePointer(std::string* type_name) {
  RE2::GlobalReplace(type_name, " \\*$", "");
}

void DwarfTypeResolver::CleanTypeName(std::string* type_name) {
  RE2::GlobalReplace(type_name, " \\*$", "*");
  *type_name = absl::StripPrefix(*type_name, "const");
  *type_name = absl::StripLeadingAsciiWhitespace(*type_name);
}

std::string DwarfTypeResolver::UnwrapAndCleanTypeName(
    absl::string_view type_name) {
  std::string alloc_type = DwarfMetadataFetcher::ConsumeAngleBracket(
      type_name.begin(), type_name.end());
  DwarfTypeResolver::CleanTypeName(&alloc_type);

  // Allocator templates may carry trailing non-type template arguments after
  // the value_type (e.g. CompressedTuple<T, false> or
  // llama::bloballoc::AlignedAllocator<T, Alignment>). Keep only the first
  // top-level template argument, which is always the value_type.
  alloc_type = TakeFirstTopLevelTemplateArgument(alloc_type);
  return alloc_type;
}

std::string WrapType(absl::string_view outer_type,
                     absl::string_view inner_type) {
  return absl::StrCat(outer_type, "<", inner_type,
                      inner_type.ends_with(">") ? " >" : ">");
}

absl::StatusOr<std::unique_ptr<TypeTree::Node>> DwarfTypeResolver::BuildTree(
    absl::string_view type_name) {
  if (IsIndirection(type_name)) {
    return TypeTree::Node::CreatePointerNode(
        type_name, type_name, /*offset_bits=*/0, /*multiplicity=*/1,
        metadata_fetcher_->GetPointerSize() * 8,
        /*parent_node=*/nullptr);
  }

  ASSIGN_OR_RETURN(const DwarfMetadataFetcher::TypeData* type_data,
                   metadata_fetcher_->GetType(type_name));
  std::unique_ptr<TypeTree::Node> root_node =
      TypeTree::Node::CreateRootNode(type_name, type_data);

  ASSIGN_OR_RETURN(
      const std::vector<const DwarfMetadataFetcher::FieldData*> resolved_fields,
      ResolveFieldConflicts(type_data));

  uint32_t field_index = 0;
  for (auto field_data : resolved_fields) {
    std::unique_ptr<TypeTree::Node> child_node = BuildTreeRecursive({
        .type_name = field_data->type_name,
        .field_name = field_data->name,
        .field_index = field_index,
        .field_offset = field_data->offset * 8,
        .multiplicity = 1,
        .parent_node = root_node.get(),
        .resolved_fields = resolved_fields,
    });
    QCHECK(child_node != nullptr)
        << "Child node is null for type: " << type_name
        << " at offset: " << field_data->offset;
    root_node->AddChildAndInsertPaddingIfNecessary(
        std::move(child_node), root_node.get(), field_index, resolved_fields);
    ++field_index;
  }
  return root_node;
}

std::unique_ptr<TypeTree::Node> DwarfTypeResolver::BuildTreeRecursive(
    BuilderCtxt ctxt) {
  constexpr int kMaxTypeTreeRecursionDepth = 256;

  QCHECK(ctxt.parent_node != nullptr) << "Parent can't be null.";

  auto create_unresolved_node = [&]() {
    int64_t inferred_size =
        ctxt.resolved_fields.empty()
            ? ctxt.parent_node->GetSizeBits()
            : ctxt.field_index >= ctxt.resolved_fields.size() - 1
                  ? ctxt.parent_node->GetSizeBits() -
                        ctxt.resolved_fields[ctxt.field_index]->offset * 8
                  : ctxt.resolved_fields[ctxt.field_index + 1]->offset * 8 -
                        ctxt.resolved_fields[ctxt.field_index]->offset * 8;
    return TypeTree::Node::CreateUnresolvedTypeNode(
        ctxt.field_name, ctxt.type_name, ctxt.field_offset, ctxt.multiplicity,
        inferred_size, ctxt.parent_node);
  };

  if (ctxt.recursion_depth >= kMaxTypeTreeRecursionDepth) {
    LOG(WARNING) << "Type-tree recursion depth exceeded while resolving "
                 << ctxt.type_name;
    return create_unresolved_node();
  }

  if (IsIndirection(ctxt.type_name)) {
    return TypeTree::Node::CreatePointerNode(
        ctxt.field_name, ctxt.type_name, ctxt.field_offset, ctxt.multiplicity,
        metadata_fetcher_->GetPointerSize() * 8, ctxt.parent_node);
  }

  const int64_t child_multiplicity = GetArrayMultiplicity(ctxt.type_name);
  if (child_multiplicity > 1) {
    std::unique_ptr<TypeTree::Node> curr_node =
        TypeTree::Node::CreateArrayTypeNode(
            ctxt.field_name, ctxt.type_name, /*size_bits=*/-1,
            ctxt.field_offset, ctxt.multiplicity, ctxt.parent_node);

    std::unique_ptr<TypeTree::Node> subtree = BuildTreeRecursive({
        .type_name = GetArrayChildTypeName(ctxt.type_name),
        .field_name = "[_]",
        .field_index = 0,
        .field_offset = 0,
        .multiplicity = child_multiplicity,
        .parent_node = curr_node.get(),
        .resolved_fields = {},
        .active_types = ctxt.active_types,
        .recursion_depth = ctxt.recursion_depth + 1,
    });

    curr_node->SetSizeBits(subtree->GetSizeBits() * subtree->GetMultiplicity());
    curr_node->AddChildAndInsertPaddingIfNecessary(
        std::move(subtree), curr_node.get(), /*field_index=*/0, {});
    return curr_node;
  }

  auto status_or = metadata_fetcher_->GetType(ctxt.type_name);
  if (!status_or.ok()) {
    return create_unresolved_node();
  }

  const DwarfMetadataFetcher::TypeData* type_data = status_or.value();
  if (!ctxt.active_types.insert(type_data).second) {
    LOG(WARNING) << "Recursive type expansion detected while resolving "
                 << ctxt.type_name;
    return TypeTree::Node::CreateNodeFromTypedata(
        ctxt.field_name, ctxt.type_name, ctxt.field_offset, ctxt.multiplicity,
        type_data, ctxt.parent_node);
  }

  auto remove_active_type = absl::MakeCleanup(
      [&ctxt, type_data] { ctxt.active_types.erase(type_data); });

  std::unique_ptr<TypeTree::Node> curr_node =
      TypeTree::Node::CreateNodeFromTypedata(
          ctxt.field_name, ctxt.type_name, ctxt.field_offset, ctxt.multiplicity,
          type_data, ctxt.parent_node);

  absl::StatusOr<std::vector<const DwarfMetadataFetcher::FieldData*>>
      resolved_fields_or = ResolveFieldConflicts(type_data);
  if (!resolved_fields_or.ok()) {
    LOG(WARNING) << resolved_fields_or.status();
    return curr_node;
  }

  const std::vector<const DwarfMetadataFetcher::FieldData*>& resolved_fields =
      resolved_fields_or.value();
  if (resolved_fields.empty()) {
    return curr_node;
  }

  uint32_t field_index = 0;
  for (const auto* field_data : resolved_fields) {
    std::unique_ptr<TypeTree::Node> subtree = BuildTreeRecursive({
        .type_name = field_data->type_name,
        .field_name = field_data->name,
        .field_index = field_index,
        .field_offset = field_data->offset * 8,
        .multiplicity = 1,
        .parent_node = curr_node.get(),
        .resolved_fields = resolved_fields,
        .active_types = ctxt.active_types,
        .recursion_depth = ctxt.recursion_depth + 1,
    });
    curr_node->AddChildAndInsertPaddingIfNecessary(
        std::move(subtree), curr_node.get(), field_index, resolved_fields);
    ++field_index;
  }

  return curr_node;
}

absl::StatusOr<std::unique_ptr<TypeTree>>
DwarfTypeResolver::CreateTreeFromDwarf(absl::string_view type_name,
                                       bool from_container,
                                       absl::string_view container_name) {
  ASSIGN_OR_RETURN(std::unique_ptr<TypeTree::Node> root, BuildTree(type_name));
  return std::make_unique<TypeTree>(std::move(root), type_name,
                                    /*from_container=*/from_container,
                                    /*container_name=*/container_name);
}

absl::StatusOr<std::unique_ptr<TypeTree>>
DwarfTypeResolver::ResolveTypeFromTypeName(absl::string_view type_name) {
  return CreateTreeFromDwarf(type_name);
}

std::optional<std::string> CallStackContainsMemprof(
    const AbstractTypeResolver::CallStack& callstack) {
  for (const auto& frame : callstack) {
    for (absl::string_view memprof : kMemprofInsertedFunctions) {
      if (absl::StrContains(frame.function_name, memprof)) {
        return frame.function_name;
      }
    }
  }
  return std::nullopt;
}

absl::StatusOr<DwarfTypeResolver::ContainerResolutionStrategy>
DwarfTypeResolver::GetCallStackContainerResolutionStrategy(
    const CallStack& callstack) {
  ContainerResolutionStrategy fallthrough_strategy;

  bool has_seen_alloc = false;
  bool last_frame_has_allocator_formal_param = true;

  if (callstack.empty()) {
    return absl::InvalidArgumentError("Empty callstack.");
  }

  if (auto memprof_func_name = CallStackContainsMemprof(callstack)) {
    return ContainerResolutionStrategy(
        "__memprof::abseil_container_internal::raw_hash_set",
        *memprof_func_name,
        ContainerResolutionStrategy::kAbseilContainerInserted);
  }

  bool is_leaf = true;
  for (const auto& frame : callstack) {
    const std::string& func_name = frame.function_name;
    if (func_name.empty()) {
      return absl::InvalidArgumentError("Empty function name in callstack.");
    }

    if (const auto smart_ptr_type = StartsWithAnyOf(
            func_name, kSmartPointersTypes, ARRAY_SIZE(kSmartPointersTypes))) {
      return ContainerResolutionStrategy(
          *smart_ptr_type, func_name,
          ContainerResolutionStrategy::kSpecialAllocatingFunction);
    }

    char* demangled_name_no_params_char =
        llvm::itaniumDemangle(func_name, /*ParseParams=*/false);
    if (demangled_name_no_params_char != nullptr) {
      std::string demangled_name_no_params(demangled_name_no_params_char);
      free(demangled_name_no_params_char);

      if (auto arena_type =
              ExtractProtobufArenaType(demangled_name_no_params)) {
        std::string triggering_type = std::move(*arena_type);
        CleanTypeName(&triggering_type);

        return ContainerResolutionStrategy(
            "google::protobuf::Arena", func_name,
            ContainerResolutionStrategy::kProtobufArena, triggering_type);
      }

      if (auto grpc_arena_type =
              ExtractGrpcArenaType(demangled_name_no_params)) {
        std::string triggering_type = std::move(*grpc_arena_type);
        CleanTypeName(&triggering_type);

        return ContainerResolutionStrategy(
            "grpc_core::Arena", func_name,
            ContainerResolutionStrategy::kGrpcArena, triggering_type);
      }

      if (auto xcallocator_type =
              ExtractXcallocatorType(demangled_name_no_params)) {
        std::string triggering_type = std::move(*xcallocator_type);
        CleanTypeName(&triggering_type);

        return ContainerResolutionStrategy(
            "xcallocator", func_name,
            ContainerResolutionStrategy::kXcallocator, triggering_type);
      }

      if (auto ggc_alloc_type = ExtractGgcAllocType(demangled_name_no_params)) {
        std::string triggering_type = std::move(*ggc_alloc_type);
        CleanTypeName(&triggering_type);

        return ContainerResolutionStrategy(
            "ggc_alloc", func_name, ContainerResolutionStrategy::kGgcAlloc,
            triggering_type);
      }

      if (auto ggc_alloc_stat_type =
              ExtractGgcAllocStatType(demangled_name_no_params)) {
        std::string triggering_type = std::move(*ggc_alloc_stat_type);
        CleanTypeName(&triggering_type);

        return ContainerResolutionStrategy(
            "ggc_alloc_stat", func_name,
            ContainerResolutionStrategy::kGgcAllocStat, triggering_type);
      }

      if (auto special_allocating_function = StartsWithAnyOf(
              demangled_name_no_params, kSpecialAllocatingFunctions,
              ARRAY_SIZE(kSpecialAllocatingFunctions))) {
        return ContainerResolutionStrategy(
            *special_allocating_function, func_name,
            ContainerResolutionStrategy::kSpecialAllocatingFunction);
      }

      if (const auto container_name = StartsWithAnyOf(
              demangled_name_no_params, kCharContainerTypesLeafFrame,
              ARRAY_SIZE(kCharContainerTypesLeafFrame))) {
        return ContainerResolutionStrategy(
            stripTrailingColons(*container_name), func_name,
            ContainerResolutionStrategy::kCharContainer);
      }
    }

    auto status_or_formal_params =
        metadata_fetcher_->GetFormalParameters(func_name);
    if (!status_or_formal_params.ok()) {
      continue;
    }
    const std::vector<std::string>& formal_params =
        status_or_formal_params.value();

    last_frame_has_allocator_formal_param = false;
    for (const absl::string_view formal_param_dirty : formal_params) {
      std::string formal_param(formal_param_dirty);
      formal_param = absl::StripPrefix(formal_param, "const");
      formal_param = absl::StripLeadingAsciiWhitespace(formal_param);

      std::string cleaned_formal_param(formal_param);
      DereferencePointer(&cleaned_formal_param);
      CleanTypeName(&cleaned_formal_param);

      if (const auto allocator_type =
              StartsWithAnyOf(formal_param, kAllocatorWrappers,
                              ARRAY_SIZE(kAllocatorWrappers))) {
        if (!has_seen_alloc &&
            DwarfTypeResolver::TypeStartsWith(formal_param, *allocator_type)) {
          std::string type_name = UnwrapAndCleanTypeName(formal_param);

          fallthrough_strategy.container_type =
              ContainerResolutionStrategy::kDefaultStrategy;
          fallthrough_strategy.func_name = func_name;
          fallthrough_strategy.container_name = "unknown";
          fallthrough_strategy.lookup_type = type_name;
        }
      }

      if (is_leaf) {
        if (const auto container_type =
                StartsWithAnyOf(formal_param, kSTLContainerLeafCheckTypes,
                                ARRAY_SIZE(kSTLContainerLeafCheckTypes))) {
          return ContainerResolutionStrategy(
              *container_type, frame.function_name,
              ContainerResolutionStrategy::kLeafContainerGWPStrategy,
              formal_param);
        }
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kSTLContainerTypes,
                              ARRAY_SIZE(kSTLContainerTypes))) {
        return ContainerResolutionStrategy(
            *container_type, callstack.at(0).function_name,
            ContainerResolutionStrategy::kAllocatorAllocate);
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kADTContainerTypes,
                              ARRAY_SIZE(kADTContainerTypes))) {
        return ContainerResolutionStrategy(
            container_type->substr(0, container_type->length() - 1), func_name,
            ContainerResolutionStrategy::kADTContainer, cleaned_formal_param);
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kADTDenseContainerTypes,
                              ARRAY_SIZE(kADTDenseContainerTypes))) {
        return ContainerResolutionStrategy(
            *container_type, func_name,
            ContainerResolutionStrategy::kADTDenseContainer,
            cleaned_formal_param);
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kContiguousTemplateContainerTypes,
                              ARRAY_SIZE(kContiguousTemplateContainerTypes))) {
        return ContainerResolutionStrategy(
            container_type->substr(0, container_type->length() - 1), func_name,
            ContainerResolutionStrategy::kADTContainer, cleaned_formal_param);
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kABSLContainerSwissMapTypes,
                              ARRAY_SIZE(kABSLContainerSwissMapTypes))) {
        absl::StatusOr<int64_t> alignment =
            GetAlignmentFromAbslAllocatorCall(callstack.at(0).function_name);
        if (!alignment.ok()) {
          alignment = 64;
        }

        absl::StatusOr<const DwarfMetadataFetcher::TypeData*>
            hash_set_typedata_status = metadata_fetcher_->GetType(formal_param);
        if (!hash_set_typedata_status.ok()) {
          return ContainerResolutionStrategy(
              *container_type, callstack.at(0).function_name,
              ContainerResolutionStrategy::kAbslAllocatorAllocate,
              cleaned_formal_param);
        }
        const DwarfMetadataFetcher::TypeData* hash_set_typedata =
            hash_set_typedata_status.value();

        if (hash_set_typedata->formal_parameters.empty()) {
          return absl::NotFoundError(
              "No formal parameters found for the hash set type.");
        }

        const std::string& policy_param =
            hash_set_typedata->formal_parameters[0];
        if (const auto policy =
                StartsWithAnyOf(policy_param, kABSLContainerFlatHashTypes,
                                ARRAY_SIZE(kABSLContainerFlatHashTypes))) {
          return ContainerResolutionStrategy(
              container_type->substr(0, container_type->length() - 1),
              func_name,
              ContainerResolutionStrategy::kAbseilContainerSwissMapFlatHash,
              cleaned_formal_param);
        }

        return ContainerResolutionStrategy(
            container_type->substr(0, container_type->length() - 1), func_name,
            ContainerResolutionStrategy::kAbseilContainerSwissMapNodeHash,
            cleaned_formal_param);
      }

      if (const auto container_type =
              StartsWithAnyOf(formal_param, kABSLContainerBtreeTypes,
                              ARRAY_SIZE(kABSLContainerBtreeTypes))) {
        return ContainerResolutionStrategy(
            container_type->substr(0, container_type->length() - 1), func_name,
            ContainerResolutionStrategy::kAbseilContainerBtree,
            cleaned_formal_param);
      }

      for (const absl::string_view allocator_type : kAllocatorWrappers) {
        if (DwarfTypeResolver::TypeStartsWith(formal_param, allocator_type) ||
            DwarfTypeResolver::TypeStartsWith(formal_param,
                                              "absl::container_internal::")) {
          last_frame_has_allocator_formal_param = true;
          has_seen_alloc = true;
        }
      }
      is_leaf = false;
    }
  }

  if (fallthrough_strategy.lookup_type.empty()) {
    return absl::NotFoundError(absl::StrCat(
        "No heap alloc or container resolution strategy found in callstack:",
        BuildCallstackString(callstack)));
  }

  return fallthrough_strategy;
}

absl::StatusOr<int64_t> DwarfTypeResolver::GetAlignmentFromAbslAllocatorCall(
    absl::string_view function_name) {
  ASSIGN_OR_RETURN(std::vector<std::string> formal_params,
                   metadata_fetcher_->GetFormalParameters(function_name));
  if (formal_params.empty()) {
    return absl::NotFoundError(
        "No formal parameters found for the allocator call.");
  }
  DereferencePointer(&formal_params[0]);
  ASSIGN_OR_RETURN(const DwarfMetadataFetcher::TypeData* allocator_type_data,
                   metadata_fetcher_->GetType(formal_params[0]));
  formal_params = allocator_type_data->formal_parameters;
  if (formal_params.empty()) {
    return absl::NotFoundError(
        "No formal parameters found for the allocator call.");
  }
  DereferencePointer(&formal_params[0]);
  ASSIGN_OR_RETURN(allocator_type_data,
                   metadata_fetcher_->GetType(formal_params[0]));
  auto alignment_it = allocator_type_data->constant_variables.find("Alignment");
  if (alignment_it == allocator_type_data->constant_variables.end()) {
    return absl::NotFoundError(
        "No constant variable Alignment found in Absl allocator call.");
  }
  return alignment_it->second * 8;
}

absl::StatusOr<std::unique_ptr<TypeTree>>
DwarfTypeResolver::ResolveTypeFromResolutionStrategy(
    const ContainerResolutionStrategy& resolution_strategy,
    const CallStack& callstack, int64_t request_size) {
  if (resolution_strategy.container_type ==
          ContainerResolutionStrategy::kProtobufArena ||
      resolution_strategy.container_type ==
          ContainerResolutionStrategy::kGrpcArena ||
      resolution_strategy.container_type ==
          ContainerResolutionStrategy::kXcallocator ||
      resolution_strategy.container_type ==
          ContainerResolutionStrategy::kGgcAlloc ||
      resolution_strategy.container_type ==
          ContainerResolutionStrategy::kGgcAllocStat) {
    if (resolution_strategy.lookup_type.empty()) {
      return absl::NotFoundError(
          "Arena strategy has no triggering template type.");
    }

    return CreateTreeFromDwarf(resolution_strategy.lookup_type,
                               /*from_container=*/true,
                               resolution_strategy.container_name);
  }

  ASSIGN_OR_RETURN(
      std::vector<std::string> formal_params,
      metadata_fetcher_->GetFormalParameters(resolution_strategy.func_name));

  switch (resolution_strategy.container_type) {
    case ContainerResolutionStrategy::kDefaultStrategy: {
      return CreateTreeFromDwarf(resolution_strategy.lookup_type,
                                 /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    case ContainerResolutionStrategy::kSpecialAllocatingFunction: {
      std::string type_name = formal_params[0];
      CleanTypeName(&type_name);
      return CreateTreeFromDwarf(type_name, /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    case ContainerResolutionStrategy::kCharContainer: {
      return CreateTreeFromDwarf("char", /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    case ContainerResolutionStrategy::kAbslAllocatorAllocate:
    case ContainerResolutionStrategy::kAllocatorAllocate: {
      for (const auto& frame : callstack) {
        ASSIGN_OR_RETURN(formal_params, metadata_fetcher_->GetFormalParameters(
                                            frame.function_name));
        for (const absl::string_view formal_param : formal_params) {
          for (const absl::string_view allocator_type : kAllocatorWrappers) {
            if (DwarfTypeResolver::TypeStartsWith(formal_param,
                                                  allocator_type)) {
              std::string type_name = UnwrapAndCleanTypeName(formal_param);
              return CreateTreeFromDwarf(type_name, /*from_container=*/true,
                                         resolution_strategy.container_name);
            }
          }
        }
      }
      return absl::NotFoundError(BuildErrorMessageInResolution(
          formal_params, callstack, resolution_strategy,
          "There should be formal param with an allocator type."));
    }

    case ContainerResolutionStrategy::kLeafContainerGWPStrategy: {
      ASSIGN_OR_RETURN(
          const DwarfMetadataFetcher::TypeData* container_type_data,
          metadata_fetcher_->GetType(resolution_strategy.lookup_type));
      for (const auto& formal_param : container_type_data->formal_parameters) {
        if (StartsWithAnyOf(formal_param, kAllocatorWrappers,
                            ARRAY_SIZE(kAllocatorWrappers))) {
          return CreateTreeFromDwarf(UnwrapAndCleanTypeName(formal_param),
                                     /*from_container=*/true,
                                     resolution_strategy.container_name);
        }
      }
      return absl::NotFoundError(BuildErrorMessageInResolution(
          formal_params, callstack, resolution_strategy,
          "No formal parameters found for the container class."));
    }

    case ContainerResolutionStrategy::kADTContainer: {
      ASSIGN_OR_RETURN(
          const DwarfMetadataFetcher::TypeData* type_data,
          metadata_fetcher_->GetType(resolution_strategy.lookup_type));
      if (type_data->formal_parameters.empty()) {
        return absl::NotFoundError(BuildErrorMessageInResolution(
            formal_params, callstack, resolution_strategy,
            "No formal parameters found for the container class."));
      }
      return CreateTreeFromDwarf(type_data->formal_parameters[0],
                                 /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    case ContainerResolutionStrategy::kADTDenseContainer: {
      ASSIGN_OR_RETURN(
          const DwarfMetadataFetcher::TypeData* type_data,
          metadata_fetcher_->GetType(resolution_strategy.lookup_type));
      if (type_data->formal_parameters.size() < 5) {
        return absl::NotFoundError(BuildErrorMessageInResolution(
            formal_params, callstack, resolution_strategy));
      }
      return CreateTreeFromDwarf(type_data->formal_parameters[4],
                                 /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    case ContainerResolutionStrategy::kAbseilContainerSwissMapNodeHash:
    case ContainerResolutionStrategy::kAbseilContainerSwissMapFlatHash: {
      int64_t Alignment = 8;

      const std::string absl_internal = *DwarfTypeResolver::TypeStartsWith(
          resolution_strategy.lookup_type, "absl::container_internal");

      absl::StatusOr<const DwarfMetadataFetcher::TypeData*> group_type_data_or =
          metadata_fetcher_->GetType(absl::StrCat(absl_internal, "::Group"));
      if (!group_type_data_or.ok()) {
        std::cout << "Group not found\n";
        return absl::NotFoundError(BuildErrorMessageInResolution(
            formal_params, callstack, resolution_strategy,
            "Group type not found."));
      }
      const DwarfMetadataFetcher::TypeData* group_type_data =
          group_type_data_or.value();

      const auto it = group_type_data->constant_variables.find("kWidth");
      if (it == group_type_data->constant_variables.end()) {
        std::cout << BuildErrorMessageInResolution(
            formal_params, callstack, resolution_strategy,
            "No constant variable kWidth found.");
        return absl::NotFoundError(BuildErrorMessageInResolution(
            formal_params, callstack, resolution_strategy,
            "No constant variable kWidth found."));
      }
      int64_t kWidth = it->second;

      ASSIGN_OR_RETURN(const DwarfMetadataFetcher::TypeData* size_type_data,
                       metadata_fetcher_->GetType("size_t"));
      int64_t size_t_size = size_type_data->size * 8;

      bool hashtablez_info = false;
      int64_t hashtablez_info_handle_size =
          metadata_fetcher_->GetPointerSize() * 8;

      ASSIGN_OR_RETURN(
          const DwarfMetadataFetcher::TypeData* type_data,
          metadata_fetcher_->GetType(resolution_strategy.lookup_type));

      for (const absl::string_view formal_param :
           type_data->formal_parameters) {
        for (const absl::string_view allocator_type : kAllocatorWrappers) {
          if (DwarfTypeResolver::TypeStartsWith(formal_param, allocator_type)) {
            std::string type_name = UnwrapAndCleanTypeName(formal_param);
            if (resolution_strategy.container_type ==
                ContainerResolutionStrategy::kAbseilContainerSwissMapNodeHash) {
              absl::StrAppend(&type_name, "*");
            }
            ASSIGN_OR_RETURN(
                std::unique_ptr<TypeTree> type_tree,
                CreateTreeFromDwarf(type_name, /*from_container=*/true,
                                    resolution_strategy.container_name));

            if (IsLocalTypeResolver()) {
              return type_tree;
            }

            ASSIGN_OR_RETURN(
                ObjectLayout template_object_layout,
                TypeTreeContainerBlueprints::GetSwissMapTemplate(
                    type_tree->Name(), type_tree->Root()->GetFullSizeBits(),
                    Alignment, size_t_size, kWidth, request_size * 8,
                    hashtablez_info, hashtablez_info_handle_size));
            std::unique_ptr<TypeTree> outer_tree =
                TypeTree::CreateTreeFromObjectLayout(
                    template_object_layout,
                    WrapType("absl::container_internal::raw_hash_set",
                             type_tree->Name()),
                    "absl::container_internal::raw_hash_set");
            RETURN_IF_ERROR(outer_tree->MergeTreeIntoThis(type_tree.get()));
            if ((!IsLocalTypeResolver() &&
                 request_size != outer_tree->Root()->GetFullSizeBytes()) ||
                (IsLocalTypeResolver() &&
                 request_size % outer_tree->Root()->GetFullSizeBytes() != 0)) {
              return absl::InternalError(BuildErrorMessageInResolution(
                  formal_params, callstack, resolution_strategy,
                  absl::StrCat(
                      "Raw hash set backing array does not match allocation "
                      "size: request_size: ",
                      request_size,
                      " tree size: ", outer_tree->Root()->GetFullSizeBytes())));
            }
            return outer_tree;
          }
        }
      }
      return absl::NotFoundError(BuildErrorMessageInResolution(
          formal_params, callstack, resolution_strategy,
          absl::StrCat("Type name: ", type_data->name)));
    }

    case ContainerResolutionStrategy::kAbseilContainerBtree: {
      ASSIGN_OR_RETURN(int64_t Alignment, GetAlignmentFromAbslAllocatorCall(
                                              callstack.at(0).function_name));
      ASSIGN_OR_RETURN(
          const DwarfMetadataFetcher::TypeData* type_data,
          metadata_fetcher_->GetType(resolution_strategy.lookup_type));
      for (const absl::string_view formal_param :
           type_data->formal_parameters) {
        if (DwarfTypeResolver::TypeStartsWith(
                formal_param, "absl::container_internal::set_params<") ||
            DwarfTypeResolver::TypeStartsWith(
                formal_param, "absl::container_internal::map_params<")) {
          ASSIGN_OR_RETURN(type_data, metadata_fetcher_->GetType(formal_param));

          std::string wrapper = *DwarfTypeResolver::TypeStartsWith(
              formal_param, "absl::container_internal");
          std::string constant_lookup_type =
              WrapType(absl::StrCat(wrapper, "::btree_node"), formal_param);

          absl::StatusOr<const DwarfMetadataFetcher::TypeData*>
              generation_typedata = metadata_fetcher_->GetType(absl::StrCat(
                  wrapper, "::btree_iterator_generation_info_enabled"));

          bool generation_enabled = generation_typedata.ok();

          ASSIGN_OR_RETURN(
              const DwarfMetadataFetcher::TypeData* constant_typedata,
              metadata_fetcher_->GetType(constant_lookup_type));
          auto it = constant_typedata->constant_variables.find("kNodeSlots");
          if (it == constant_typedata->constant_variables.end()) {
            return absl::NotFoundError(BuildErrorMessageInResolution(
                formal_params, callstack, resolution_strategy,
                "No constant variable kNodeSlots found."));
          }
          int64_t kNodeSlots = it->second;

          std::string btree_field_type_name = absl::StrCat(
              WrapType(absl::StrCat(wrapper, "::btree"), formal_param),
              "::field_type");

          ASSIGN_OR_RETURN(
              const DwarfMetadataFetcher::TypeData* btree_field_type,
              metadata_fetcher_->GetType(btree_field_type_name));
          int64_t btree_field_type_size = btree_field_type->size * 8;

          for (const absl::string_view formal_param_set_params :
               type_data->formal_parameters) {
            for (const absl::string_view allocator_type : kAllocatorWrappers) {
              if (DwarfTypeResolver::TypeStartsWith(formal_param_set_params,
                                                    allocator_type)) {
                std::string type_name =
                    UnwrapAndCleanTypeName(formal_param_set_params);
                ASSIGN_OR_RETURN(
                    std::unique_ptr<TypeTree> slot_type_tree,
                    CreateTreeFromDwarf(type_name, /*from_container=*/true,
                                        resolution_strategy.container_name));

                if (IsLocalTypeResolver()) {
                  return slot_type_tree;
                }

                ASSIGN_OR_RETURN(
                    ObjectLayout template_object_layout,
                    TypeTreeContainerBlueprints::GetBtreeNodeTypeTemplate(
                        slot_type_tree->Name(),
                        slot_type_tree->Root()->GetFullSizeBits(), Alignment,
                        btree_field_type_size, kNodeSlots,
                        metadata_fetcher_->GetPointerSize() * 8,
                        request_size * 8, generation_enabled));
                std::unique_ptr<TypeTree> btree_node_type_tree =
                    TypeTree::CreateTreeFromObjectLayout(
                        template_object_layout,
                        WrapType(absl::StrCat(wrapper, "::btree_node"),
                                 slot_type_tree->Name()),
                        "absl::container_internal::btree");
                RETURN_IF_ERROR(btree_node_type_tree->MergeTreeIntoThis(
                    slot_type_tree.get()));
                if (btree_node_type_tree->Root()->GetFullSizeBytes() !=
                    request_size) {
                  return absl::InternalError(BuildErrorMessageInResolution(
                      formal_params, callstack, resolution_strategy,
                      absl::StrCat(
                          "Btree node does not match allocation size: "
                          "request_size: ",
                          request_size, " tree size: ",
                          btree_node_type_tree->Root()->GetFullSizeBytes())));
                }
                return btree_node_type_tree;
              }
            }
          }
        }
      }
      return absl::NotFoundError(BuildErrorMessageInResolution(
          formal_params, callstack, resolution_strategy));
    }

    case ContainerResolutionStrategy::kAbseilContainerInserted: {
      return CreateTreeFromDwarf("char", /*from_container=*/true,
                                 resolution_strategy.container_name);
    }

    default: {
      return absl::InvalidArgumentError(
          "Unknown container type resolution strategy.");
    }
  }
}

absl::StatusOr<TypeResolutionResult>
DwarfTypeResolver::ResolveTypeFromCallstack(const CallStack& callstack,
                                            int64_t request_size,
                                            absl::Time deadline) {
  if (absl::Now() >= deadline) {
    LOG(WARNING) << "[heapalloc] Resolution timed out before starting";
    return absl::DeadlineExceededError("Type resolution timed out");
  }

  absl::StatusOr<ContainerResolutionStrategy> strategy =
      GetCallStackContainerResolutionStrategy(callstack);

  absl::Status container_resolution_status = strategy.status();

  if (strategy.ok()) {
    if (absl::Now() >= deadline) {
      return absl::DeadlineExceededError("Type resolution timed out");
    }

    auto type_tree =
        ResolveTypeFromResolutionStrategy(*strategy, callstack, request_size);

    if (type_tree.ok()) {
      AllocationCategory allocation_category =
          AllocationCategory::kAllocationAware;

      switch (strategy->container_type) {
        case ContainerResolutionStrategy::kADTContainer:
        case ContainerResolutionStrategy::kADTDenseContainer:
        case ContainerResolutionStrategy::kProtobufArena:
        case ContainerResolutionStrategy::kGrpcArena:
        case ContainerResolutionStrategy::kXcallocator:
        case ContainerResolutionStrategy::kGgcAlloc:
        case ContainerResolutionStrategy::kGgcAllocStat:
        case ContainerResolutionStrategy::kAbslAllocatorAllocate:
        case ContainerResolutionStrategy::kAbseilContainerSwissMapNodeHash:
        case ContainerResolutionStrategy::kAbseilContainerSwissMapFlatHash:
        case ContainerResolutionStrategy::kAbseilContainerBtree:
        case ContainerResolutionStrategy::kAbseilContainerInserted:
          allocation_category = AllocationCategory::kComplex;
          break;
        default:
          break;
      }

      TypeResolutionResult resolution_result;
      resolution_result.type_tree = std::move(type_tree.value());
      resolution_result.allocation_category = allocation_category;
      resolution_result.resolution_strategy =
          ContainerResolutionStrategy::TypeToString(strategy->container_type);
      return resolution_result;
    }

    container_resolution_status = type_tree.status();
  }

  // Walk outer frames (closest to main) before inner ones (closest to the
  // malloc/calloc/realloc call), so that a caller-level typed cast wins over
  // an alloc-wrapper pass-through deeper in the stack.
  for (size_t steps_from_outer = 0; steps_from_outer < callstack.size();
       ++steps_from_outer) {
    if (absl::Now() >= deadline) {
      return absl::DeadlineExceededError("Type resolution timed out");
    }

    const size_t frame_index =
        callstack.size() - 1 - steps_from_outer;
    const auto& frame = callstack[frame_index];
    auto frame_type_tree = ResolveTypeFromFrame(frame);

    if (frame_type_tree.ok()) {
      TypeResolutionResult resolution_result;
      resolution_result.type_tree = std::move(frame_type_tree.value());
      resolution_result.allocation_category = AllocationCategory::kSimple;
      resolution_result.resolution_strategy = "heapalloc";
      return resolution_result;
    }
  }

  return container_resolution_status;
}

absl::StatusOr<std::unique_ptr<TypeTree>>
DwarfTypeResolver::ResolveTypeFromFrame(
    const DwarfMetadataFetcher::Frame& frame) {
  DwarfMetadataFetcher::Frame frame_copy(frame);

  absl::StatusOr<std::string> type_name =
      metadata_fetcher_->GetHeapAllocType(frame_copy);

  if (!type_name.ok() && frame_copy.column != 0) {
    frame_copy.column = 0;
    type_name = metadata_fetcher_->GetHeapAllocType(frame_copy);
  }

  if (!type_name.ok()) {
    return type_name.status();
  }

  auto type_tree = CreateTreeFromDwarf(type_name.value(), false, "none");
  if (!type_tree.ok()) {
    return type_tree.status();
  }

  return type_tree;
}

}  // namespace devtools_crosstool_fdo_field_access
