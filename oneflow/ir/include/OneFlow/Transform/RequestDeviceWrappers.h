#ifndef ONEFLOW_IR_INCLUDE_ONEFLOW_TRANSFORM_REQUEST_DEVICE_WRAPPERS_H_
#define ONEFLOW_IR_INCLUDE_ONEFLOW_TRANSFORM_REQUEST_DEVICE_WRAPPERS_H_

#include <memory>

namespace mlir {
class Pass;

namespace oneflow {

std::unique_ptr<Pass> createOneFlowRequestDeviceWrappers();

} // namespace oneflow
} // namespace mlir

#endif // ONEFLOW_IR_INCLUDE_ONEFLOW_TRANSFORM_REQUEST_DEVICE_WRAPPERS_H_