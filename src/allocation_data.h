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

#ifndef ALLOCATION_DATA_H_
#define ALLOCATION_DATA_H_

#include <cstdint>
#include <vector>

#include "status_macros.h"

namespace devtools_crosstool_fdo_field_access {

struct AllocationData {
  uint64_t allocation_timestamp = 0;
  uint64_t deallocation_timestamp = 0;
  uint64_t start_address = 0;
  uint64_t end_address = 0;
};

class AllocationDataStore {
 public:
  AllocationDataStore() = default;
  ~AllocationDataStore() = default;

  void Insert(const AllocationData& allocation_data) {
    allocations_.push_back(allocation_data);
  }

  const std::vector<AllocationData>& Allocations() const {
    return allocations_;
  }

  size_t Size() const { return allocations_.size(); }

  bool Empty() const { return allocations_.empty(); }

 private:
  std::vector<AllocationData> allocations_;
};

}  // namespace devtools_crosstool_fdo_field_access

#endif  // ALLOCATION_DATA_H_
