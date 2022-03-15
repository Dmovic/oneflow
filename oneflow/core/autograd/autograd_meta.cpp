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

#include "oneflow/core/framework/tensor.h"
#include "oneflow/core/framework/dtype.h"
#include "oneflow/core/framework/tensor_arg.h"
#include "oneflow/core/autograd/autograd_meta.h"
#include "oneflow/core/eager/eager_blob_object.h"
#include "oneflow/core/eager/dtr_eager_blob_object.h"
#include "oneflow/core/functional/functional.h"

namespace oneflow {

namespace one {

TensorInfo::TensorInfo(const Tensor& tensor) : shape_(tensor.shape()), dtype_(tensor.dtype()) {
  if (TRY(tensor.device()).IsOk()) { device_ = CHECK_JUST(tensor.device()); }
  if (TRY(tensor.parallel_desc()).IsOk()) { parallel_desc_ = CHECK_JUST(tensor.parallel_desc()); }
  if (TRY(tensor.nd_sbp()).IsOk()) { nd_sbp_ = CHECK_JUST(tensor.nd_sbp()); }
}

Maybe<const std::vector<Symbol<SbpParallel>>&> GetSbpTuple(Symbol<NdSbp> nd_sbp) {
  static thread_local HashMap<Symbol<NdSbp>, std::vector<Symbol<SbpParallel>>> map;
  auto iter = map.find(nd_sbp);
  if (iter == map.end()) {
    std::vector<Symbol<SbpParallel>> sbp_tuple;
    sbp_tuple.reserve(nd_sbp->sbp_parallel().size());
    for (const auto& sbp_parallel : nd_sbp->sbp_parallel()) {
      sbp_tuple.push_back(SymbolOf(sbp_parallel));
    }
    iter = map.emplace(nd_sbp, sbp_tuple).first;
  }
  return iter->second;
}

Maybe<Tensor> TensorInfo::zeros() const {
  if (device_.has_value()) {
    const auto& device = JUST(device_);
    return functional::Constant(*shape_.get(), 0, dtype_, device);
  } else {
    const auto& parallel_desc = JUST(parallel_desc_);
    const auto& nd_sbp = JUST(nd_sbp_);
    const auto& sbp_tuple = JUST(GetSbpTuple(nd_sbp));
    return functional::ConsistentConstant(*shape_.get(), 0, dtype_, parallel_desc, sbp_tuple);
  }
}

AutogradMeta::AutogradMeta(bool requires_grad, bool is_leaf)
    : is_leaf_(is_leaf),
      requires_grad_(requires_grad),
      retain_grad_(false),
      is_grad_acc_inplace_(false),
      current_grad_(new TensorArg) {}

Maybe<void> AutogradMeta::set_acc_grad(const std::shared_ptr<Tensor>& grad) {
  if (const auto& static_zeros_tensor = std::dynamic_pointer_cast<StaticZerosTensor>(grad)) {
    acc_grad_ = JUST(static_zeros_tensor->AsMirroredTensor());
  } else {
    acc_grad_ = grad;
  }
  if (acc_grad_ != nullptr) {
    if (auto dtr_grad_ebo = std::dynamic_pointer_cast<vm::DTREagerBlobObject>(
            JUST(acc_grad_->eager_blob_object()))) {
      dtr_grad_ebo->set_evict_attr(false);
    }
  }
  return Maybe<void>::Ok();
}

}  // namespace one

}  // namespace oneflow
