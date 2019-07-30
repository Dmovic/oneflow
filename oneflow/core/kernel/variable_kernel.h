#ifndef ONEFLOW_CORE_KERNEL_VARIABLE_KERNEL_H_
#define ONEFLOW_CORE_KERNEL_VARIABLE_KERNEL_H_

#include "oneflow/core/kernel/kernel.h"

namespace oneflow {

template<DeviceType device_type, typename T>
class VariableKernel final : public KernelIfWithModel<device_type, T> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(VariableKernel);
  VariableKernel() : tick_(new int64_t(0)) {}
  ~VariableKernel() = default;

 private:
  void ForwardDataContent(const KernelCtx&,
                          std::function<Blob*(const std::string&)>) const override;
  const PbMessage& GetCustomizedOpConf() const override;
  const std::string& ModelName() const;

  std::unique_ptr<int64_t> tick_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_KERNEL_VARIABLE_KERNEL_H_
