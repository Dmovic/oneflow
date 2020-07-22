/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_VM_STORAGE_H_
#define ONEFLOW_CORE_VM_STORAGE_H_

#include <mutex>
#include "oneflow/core/common/util.h"

namespace oneflow {

class ParallelDesc;
class ParallelConf;

namespace vm {

template<typename T>
struct ConstructArgType4Symbol final {
  using type = T;
};

template<>
struct ConstructArgType4Symbol<ParallelDesc> final {
  using type = ParallelConf;
};

template<typename T>
class SymbolStorage final {
 public:
  SymbolStorage(const SymbolStorage&) = delete;
  SymbolStorage(SymbolStorage&&) = delete;

  SymbolStorage() = default;
  ~SymbolStorage() = default;

  bool Has(int64_t logical_object_id) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return logical_object_id2data_.find(logical_object_id) != logical_object_id2data_.end();
  }

  const T& Get(int64_t logical_object_id) const { return *GetPtr(logical_object_id); }

  const std::shared_ptr<T>& GetPtr(int64_t logical_object_id) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto& iter = logical_object_id2data_.find(logical_object_id);
    CHECK(iter != logical_object_id2data_.end());
    return iter->second;
  }

  void Add(int64_t logical_object_id, const typename ConstructArgType4Symbol<T>::type& data) {
    CHECK_GT(logical_object_id, 0);
    const auto& ptr = std::make_shared<T>(data);
    std::unique_lock<std::mutex> lock(mutex_);
    CHECK(logical_object_id2data_.emplace(logical_object_id, ptr).second);
  }
  void Clear(int64_t logical_object_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    logical_object_id2data_.erase(logical_object_id);
  }
  void ClearAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    logical_object_id2data_.clear();
  }

 private:
  mutable std::mutex mutex_;
  HashMap<int64_t, std::shared_ptr<T>> logical_object_id2data_;
};

}  // namespace vm
}  // namespace oneflow

#endif  // ONEFLOW_CORE_VM_STORAGE_H_
