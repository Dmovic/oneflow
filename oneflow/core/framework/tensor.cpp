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
#include "oneflow/core/common/maybe.h"
#include "oneflow/core/job/parallel_desc.h"
#include "oneflow/core/job/job_build_and_infer_ctx_mgr.h"
#include "oneflow/core/job/job_build_and_infer_ctx.h"
#include "oneflow/core/framework/device.h"
#include "oneflow/core/framework/dtype.h"
#include "oneflow/core/framework/tensor_tuple.h"
#include "oneflow/core/autograd/autograd_engine.h"
#include "oneflow/core/framework/op_interpreter/eager_mirrored_op_interpreter.h"
#include "oneflow/core/functional/functional.h"

namespace oneflow {

namespace one {

/* static */ Maybe<MirroredTensor> MirroredTensor::MakeTensor(
    const std::shared_ptr<const Shape>& shape, DataType dtype, const Symbol<Device>& device,
    bool is_lazy, bool requires_grad, bool is_leaf, bool enable_dtr) {
  const auto& tensor_meta =
      std::make_shared<MirroredTensorMeta>(std::make_shared<Shape>(*shape), dtype, device);
  if (is_lazy) {
    const auto& impl =
        std::make_shared<LazyMirroredTensorImpl>(tensor_meta, requires_grad, is_leaf);
    return std::make_shared<MirroredTensor>(impl);
  } else if (enable_dtr) {
    const auto& impl =
        std::make_shared<DTREagerMirroredTensorImpl>(tensor_meta, requires_grad, is_leaf);
    const auto& tensor = std::make_shared<DTRMirroredTensor>(impl);
    const auto& outputs = std::make_shared<TensorTuple>();
    outputs->push_back(tensor);
    JUST(RunEmptyOp(outputs.get()));
    return static_cast<std::shared_ptr<MirroredTensor>>(tensor);
  } else {
    const auto& impl =
        std::make_shared<EagerMirroredTensorImpl>(tensor_meta, requires_grad, is_leaf);
    const auto& tensor = std::make_shared<MirroredTensor>(impl);
    const auto& outputs = std::make_shared<TensorTuple>();
    outputs->push_back(tensor);
    JUST(RunEmptyOp(outputs.get()));
    return tensor;
  }
}

/* static */ Maybe<MirroredTensor> MirroredTensor::MakeEagerTensor(
    const std::shared_ptr<vm::EagerBlobObject> eager_blob_object, const Symbol<Device>& device,
    const std::shared_ptr<TensorStorage> tensor_storage, bool requires_grad, bool is_leaf) {
  const auto& blob_desc = eager_blob_object->blob_desc();
  const auto& tensor_meta =
      std::make_shared<MirroredTensorMeta>(blob_desc.shape_ptr(), blob_desc.data_type(), device);
  auto* tensor_impl = new EagerMirroredTensorImpl(tensor_meta, requires_grad, is_leaf);
  JUST(tensor_impl->InitEagerBlobObjectAndTensorStorage(eager_blob_object, tensor_storage));
  return std::make_shared<MirroredTensor>(std::shared_ptr<MirroredTensorImpl>(tensor_impl));
}

bool MirroredTensor::is_cuda() const { return CHECK_JUST(device())->type() == "cuda"; }

int64_t MirroredTensor::ndim() const { return shape()->NumAxes(); }

int64_t MirroredTensor::dim(int64_t index) const { return shape()->At(index); }

int64_t MirroredTensor::nelement() const { return shape()->elem_cnt(); }

std::shared_ptr<Tensor> MirroredTensor::data() const {
  std::shared_ptr<MirroredTensor> t = std::make_shared<MirroredTensor>(impl_);
  return t;
}

Maybe<Tensor> MirroredTensor::detach() const {
  std::shared_ptr<Tensor> tensor = std::make_shared<MirroredTensor>(JUST(impl_->detach()));
  return tensor;
}

Maybe<Tensor> MirroredTensor::clone() const {
  const auto& device_type = JUST(this->device())->type();
  int64_t device_id = JUST(this->device())->device_id();
  std::shared_ptr<MirroredTensor> input =
      std::const_pointer_cast<MirroredTensor>(shared_from_this());
  return JUST(functional::Copy(input, device_type, device_id));
}

Maybe<ConsistentTensor> ConsistentTensor::MakeTensor(
    const std::shared_ptr<const Shape>& shape, DataType dtype,
    Symbol<cfg::ParallelDistribution> parallel_distribution, Symbol<ParallelDesc> parallel_desc,
    bool is_lazy, bool requires_grad, bool is_leaf) {
  std::shared_ptr<ConsistentTensorImpl> impl;
  Symbol<ConsistentTensorMeta> consistent_tensor_meta(
      ConsistentTensorMeta(shape, dtype, parallel_distribution, parallel_desc));
  if (is_lazy) {
    impl =
        std::make_shared<LazyConsistentTensorImpl>(consistent_tensor_meta, requires_grad, is_leaf);
  } else {
    impl = JUST(EagerConsistentTensorImpl::New(consistent_tensor_meta, requires_grad, is_leaf));
  }
  return std::make_shared<ConsistentTensor>(impl);
}

bool ConsistentTensor::is_cuda() const {
  return CHECK_JUST(parallel_desc())->device_type() == DeviceType::kGPU;
}

int64_t ConsistentTensor::dim(int64_t index) const { return shape()->At(index); }

int64_t ConsistentTensor::nelement() const { return shape()->elem_cnt(); }

int64_t ConsistentTensor::ndim() const { return shape()->NumAxes(); }

std::shared_ptr<Tensor> ConsistentTensor::data() const {
  std::shared_ptr<ConsistentTensor> t = std::make_shared<ConsistentTensor>(impl_);
  return t;
}

Maybe<Tensor> ConsistentTensor::detach() const {
  std::shared_ptr<Tensor> t = std::make_shared<ConsistentTensor>(impl_);
  return t;
}

}  // namespace one

}  // namespace oneflow
