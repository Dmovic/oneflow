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
#include "oneflow/core/common/str_util.h"
#include "oneflow/core/framework/parallel_conf_util.h"
#include "oneflow/core/common/shape.pb.h"

namespace oneflow {

std::tuple<std::string, std::vector<std::string>, std::shared_ptr<ShapeProto>, bool>
ParseParallelConf(const ParallelConf& parallel_conf) {
  std::vector<std::string> machine_device_ids;
  machine_device_ids.reserve(parallel_conf.device_name().size());
  for (const std::string& device_name : parallel_conf.device_name()) {
    machine_device_ids.emplace_back(device_name);
  }
  std::shared_ptr<ShapeProto> hierarchy;
  if (parallel_conf.has_hierarchy()) { hierarchy.reset(new ShapeProto(parallel_conf.hierarchy())); }
  return std::make_tuple(parallel_conf.device_tag(), machine_device_ids, hierarchy, parallel_conf.rematable());
}

Maybe<ParallelConf> MakeParallelConf(const std::string& device_tag,
                                     const std::vector<std::string>& machine_device_ids,
                                     const std::shared_ptr<Shape>& hierarchy, bool rematable) {
  std::shared_ptr<ParallelConf> parallel_conf = std::make_shared<ParallelConf>();
  parallel_conf->set_device_tag(device_tag);
  for (const std::string& machine_device_id : machine_device_ids) {
    size_t pos = machine_device_id.find(':');
    CHECK_NE_OR_RETURN(pos, std::string::npos) << "device_name: " << machine_device_id;
    std::string machine_id = machine_device_id.substr(0, pos);
    CHECK_OR_RETURN(
        (IsStrInt(machine_id) || (machine_id[0] == '@' && IsStrInt(machine_id.substr(1)))))
        << " machine_id: " << machine_id;
    std::string device_id = machine_device_id.substr(pos + 1);
    size_t minus_pos = device_id.rfind('-');
    if (minus_pos == std::string::npos) {
      CHECK_OR_RETURN(IsStrInt(device_id));
    } else {
      std::string min_id = device_id.substr(0, minus_pos);
      CHECK_OR_RETURN(IsStrInt(min_id));
      std::string max_id = device_id.substr(minus_pos + 1);
      CHECK_OR_RETURN(IsStrInt(max_id));
    }
    parallel_conf->add_device_name(machine_device_id);
    if (hierarchy) {
      ShapeProto proto;
      hierarchy->ToProto(&proto);
      parallel_conf->mutable_hierarchy()->CopyFrom(proto);
    }
  }
  return parallel_conf;
}

}  // namespace oneflow
