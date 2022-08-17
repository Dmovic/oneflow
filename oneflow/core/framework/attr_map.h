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
#ifndef ONEFLOW_CORE_FRAMEWORK_ATTR_MAP_H_
#define ONEFLOW_CORE_FRAMEWORK_ATTR_MAP_H_

#include "oneflow/core/common/util.h"
#include "oneflow/core/common/symbol.h"
#include "oneflow/core/common/throw.h"
#include "oneflow/core/common/small_vector.h"

namespace oneflow {

namespace user_op {
class AttrVal;
}
class AttrValue;
class CachedMutableAttrMap;
class UserOpConf;

class AttrMap final {
 public:
  static constexpr int kInitializedSize = 4;
  AttrMap();
  AttrMap(const CachedMutableAttrMap& other);
  AttrMap(const UserOpConf& user_op_conf);

  AttrMap(const AttrMap&) = default;
  AttrMap(AttrMap&&) = default;
  ~AttrMap() = default;

  AttrMap& operator=(const AttrMap& other);

  bool operator==(const AttrMap& other) const;

  template<typename T>
  Maybe<const T&> GetAttr(const std::string& attr_name) const;

  const std::shared_ptr<const user_op::AttrVal>& Attr4Name(const std::string& attr_name) const;
  bool HasAttr4Name(const std::string& attr_name) const;

  size_t size() const { return data_->size; }
  bool empty() const { return data_->size > 0; }

  size_t hash_value() const { return data_->hash_value; }

  struct AttrData {
    size_t capacity = 0;
    size_t size = 0;
    size_t hash_value = 0;
    std::shared_ptr<small_vector<std::string, kInitializedSize>> attr_names;
    small_vector<std::pair<std::shared_ptr<const user_op::AttrVal>, bool>, 16> attrs;
  };

  class const_iterator {
   public:
    using const_reference = const std::pair<std::string, std::shared_ptr<const user_op::AttrVal>>&;
    using const_pointer = const std::pair<std::string, std::shared_ptr<const user_op::AttrVal>>*;

    const_iterator(size_t pos, const AttrData* data);
    ~const_iterator() = default;

    const_reference operator*() const { return kv_; }
    const_pointer operator->() const { return &kv_; }

    const_iterator& operator++();
    bool operator==(const const_iterator& x) const { return pos_ == x.pos_ && data_ == x.data_; }
    bool operator!=(const const_iterator& x) const { return !(*this == x); }

   private:
    size_t pos_;
    const AttrData* data_;
    std::pair<std::string, std::shared_ptr<const user_op::AttrVal>> kv_;
  };

  const_iterator begin() const { return const_iterator(0, data_.get()); }
  const_iterator end() const { return const_iterator(data_->capacity, data_.get()); }

 private:
  std::shared_ptr<AttrData> data_;
};

AttrMap MakeAttrMapFromUserOpConf(const UserOpConf& user_op_conf);

class ComposedAttrMap final {
 public:
  ComposedAttrMap(const ComposedAttrMap&) = default;
  ComposedAttrMap(ComposedAttrMap&&) = default;
  ComposedAttrMap(const AttrMap& base) : base_(base) {}
  ComposedAttrMap(const AttrMap& prior, const AttrMap& base) : prior_(prior), base_(base) {}

  template<typename T>
  Maybe<const T&> GetAttr(const std::string& attr_name) const;

  const std::shared_ptr<const user_op::AttrVal>& Attr4Name(const std::string& attr_name) const;

  bool HasAttr4Name(const std::string& attr_name) const;

  void ResetPrior(const AttrMap& prior) { prior_ = prior; }
  void ResetBase(const AttrMap& base) { base_ = base; }

 private:
  AttrMap prior_;
  AttrMap base_;
};

}  // namespace oneflow

namespace std {

template<>
struct hash<oneflow::AttrMap> final {
  size_t operator()(const oneflow::AttrMap& attr_map) const { return attr_map.hash_value(); }
};

}  // namespace std

#endif  // ONEFLOW_CORE_FRAMEWORK_ATTR_MAP_H_
