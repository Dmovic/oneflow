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
#include <glog/logging.h>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>
#include <string>
#include <vector>
#include <chrono>
#include "oneflow/core/common/env_var/env_var.h"
#include "oneflow/core/common/env_var/lazy.h"
#include "oneflow/core/common/singleton.h"
#include "oneflow/core/framework/nn_graph.h"
#include "oneflow/core/framework/shut_down_util.h"
#include "oneflow/core/rpc/include/base.h"
#include "oneflow/core/rpc/include/global_process_ctx.h"
#include "oneflow/core/thread/thread_manager.h"
#include "oneflow/core/control/ctrl_client.h"
#include "oneflow/core/control/global_process_ctx.h"
#include "oneflow/api/cpp/api.h"

using namespace oneflow;

#define WORLD_SIZE 3

bool HasEnvVar(const std::string& key) {
  const char* value = getenv(key.c_str());
  return value != nullptr;
}

std::string GetEnvVar(const std::string& key, const std::string& default_value) {
  const char* value = getenv(key.c_str());
  if (value == nullptr) { return default_value; }
  return std::string(value);
}

int64_t GetEnvVar(const std::string& key, int64_t default_value) {
  const char* value = getenv(key.c_str());
  if (value == nullptr) { return default_value; }
  return std::atoll(value);
}

class DistributeOneFlowEnv {
 public:
  explicit DistributeOneFlowEnv(size_t rank, size_t world_size) {
    EnvProto env_proto;
    CompleteEnvProto(env_proto, rank, world_size);
    env_ctx_ = std::make_shared<EnvGlobalObjectsScope>(env_proto);
  }
  ~DistributeOneFlowEnv() { env_ctx_.reset(); }

  void CompleteEnvProto(EnvProto& env_proto, size_t rank, size_t world_size) {
    auto bootstrap_conf = env_proto.mutable_ctrl_bootstrap_conf();
    auto master_addr = bootstrap_conf->mutable_master_addr();
    // TODO: addr和port作为参数传入
    const std::string addr = "127.0.0.1";
    size_t master_port = 49155;
    size_t port = master_port + rank;

    master_addr->set_host(addr);
    master_addr->set_port(master_port);

    bootstrap_conf->set_world_size(world_size);
    bootstrap_conf->set_rank(rank);
    bootstrap_conf->set_ctrl_port(port);
    bootstrap_conf->set_host("127.0.0.1");

    auto cpp_logging_conf = env_proto.mutable_cpp_logging_conf();
    if (HasEnvVar("GLOG_log_dir")) {
      cpp_logging_conf->set_log_dir(GetEnvVar("GLOG_log_dir", ""));
      LOG(INFO) << "LOG DIR: " << cpp_logging_conf->log_dir();
    }
    if (HasEnvVar("GLOG_logtostderr")) {
      cpp_logging_conf->set_logtostderr(GetEnvVar("GLOG_logtostderr", -1));
    }
    if (HasEnvVar("GLOG_logbuflevel")) {
      cpp_logging_conf->set_logbuflevel(GetEnvVar("GLOG_logbuflevel", -1));
    }
    if (HasEnvVar("GLOG_minloglevel")) {
      cpp_logging_conf->set_minloglevel(GetEnvVar("GLOG_minloglevel", -1));
    }
  }

 private:
  std::shared_ptr<EnvGlobalObjectsScope> env_ctx_;
};

class TestEnvScope {
 public:
  explicit TestEnvScope(size_t rank, size_t world_size) {
    if (Singleton<DistributeOneFlowEnv>::Get() == nullptr) {
      Singleton<DistributeOneFlowEnv>::New(rank, world_size);
    }
  }

  ~TestEnvScope() {
    if (Singleton<DistributeOneFlowEnv>::Get() != nullptr) {
      Singleton<DistributeOneFlowEnv>::Delete();
    }
  }
};

template<typename X, typename Y>
std::set<std::string> MultiThreadBroadcastFromMasterToWorkers(size_t world_size,
                                                              const std::string& prefix,
                                                              const X& master_data,
                                                              Y* worker_data) {
  const size_t thread_num = ThreadLocalEnvInteger<ONEFLOW_LAZY_COMPILE_RPC_THREAD_NUM>();
  const size_t split_num = std::sqrt(world_size);
  BalancedSplitter bs(world_size, split_num);
  std::set<std::string> keys;
  if (GlobalProcessCtx::IsThisProcessMaster()) {
    std::mutex mtx4keys;
    const std::string& data = master_data;
    MultiThreadLoop(
        split_num,
        [&](int i) {
          std::string key = prefix + std::to_string(i);
          Singleton<CtrlClient>::Get()->PushKV(key, data);
          std::lock_guard<std::mutex> lock(mtx4keys);
          CHECK(keys.insert(key).second);
        },
        thread_num);
  } else {
    const int64_t bs_index = bs.GetRangIndex(GlobalProcessCtx::Rank());
    std::string key = prefix + std::to_string(bs_index);
    Singleton<CtrlClient>::Get()->PullKV(key, worker_data);
  }
  return keys;
}

int main(int argc, char* argv[]) {
  if (argc == 1) { LOG(FATAL) << "Error: must set world_size and rank"; }
  size_t world_size = std::stoi(argv[1]);
  size_t rank = std::stoi(argv[2]);

  std::string master_data, worker_data;
  if (rank == 0) {
    master_data.resize(10 * 1024 * 1024);  // 10MB
  }
  std::string prefix("test");

  TestEnvScope scope(rank, world_size);

  LOG(INFO) << "world size: " << Singleton<GlobalProcessCtx>::Get()->WorldSize();
  auto start_time = std::chrono::high_resolution_clock::now();
  std::set<std::string> keys =
      MultiThreadBroadcastFromMasterToWorkers(world_size, prefix, master_data, &worker_data);

  // synchronize all process
  Singleton<CtrlClient>::Get()->Barrier("sync all process");
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  if (rank == 0) {
    LOG(INFO) << "broadcast to all workers"
              << " spends time: " << duration.count() << " ms";
  }

  return 0;
}