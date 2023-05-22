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

#include "oneflow/core/auto_parallel/straighten_memory.h"
#include "oneflow/core/auto_parallel/algorithm_util.h"
#include "oneflow/core/auto_parallel/auto_memory.h"
#include "oneflow/core/common/data_type.h"
#include "oneflow/core/common/hash_container.h"
#include "oneflow/core/rpc/include/global_process_ctx.h"

namespace oneflow {
namespace auto_parallel {

namespace {

const int64_t kPriorityOffset = GetMaxVal<int64_t>() / 4;
const int64_t kPriorityBound = 2 * kPriorityOffset;
const int32_t kOriginNode = -123;

class NoCleaningMarkerAncestor {
 public:
  static int32_t marker;
  int32_t status = 0;

  bool IfMarked() const { return status == marker; }
  bool IfNotMarked() const { return status != marker; }
  void Mark() { status = marker; }
  void UnMark() { status = 0; }
};

int32_t NoCleaningMarkerAncestor::marker = 1;

void ResetNoCleaningMarkerAncestor() { ++NoCleaningMarkerAncestor::marker; }

void InitNoCleaningMarkerAncestor() { NoCleaningMarkerAncestor::marker = 1; }

class NoCleaningMarkerDescendant {
 public:
  static int32_t marker;
  int32_t status = 0;

  bool IfMarked() const { return status == marker; }
  bool IfNotMarked() const { return status != marker; }
  void Mark() { status = marker; }
  void UnMark() { status = 0; }
};

int32_t NoCleaningMarkerDescendant::marker = 1;

void ResetNoCleaningMarkerDescendant() { ++NoCleaningMarkerDescendant::marker; }

void InitNoCleaningMarkerDescendant() { NoCleaningMarkerDescendant::marker = 1; }

class TopoStruct {
 public:
  const OpNode* op_node = nullptr;
  // Memory increment = (memory of out registers) - (memory of in registers)
  int64_t memory_increment = -1;
  int64_t peak_memory = -1;
  // max difference = peak memory - final memory increment
  int64_t max_difference = 0;
  int32_t min_layer = -1;
  bool is_reusable = false;
  int32_t blob_id = -1;
  // Blocking means you must execute this node before executing any other nodes in the set
  HashSet<TopoStruct*> blocking_topo_structs;
  int32_t blocking_count = -1;
  // executed means that it has been executed
  bool executed = false;
  // Accumulate memory increment of all the necessary topological structures
  int64_t accumulate_memory_increment = 0;
  int64_t peak_memory_during_accumulation = 0;
  int64_t max_difference_during_accumulation = 0;
  // Whether visited during memory accumulating
  NoCleaningMarkerAncestor visited_ancestors;
  // Whether visited while finding descendants
  NoCleaningMarkerDescendant visited_descendant;
  // waiting in the map before execution
  bool waiting = false;

  HashSet<TopoStruct*> in_topo_structs;
  HashSet<TopoStruct*> out_topo_structs;

  // The topo structs to be executed in a reverse order right before this topo struct
  // For example:
  // This topo struct: A, Pre topo structs: {B, C, D}
  // This topo struct: B, Pre topo structs: {E}
  // This topo struct: D, Pre topo structs: {F, G}
  // And the graph is: H -> A -> I
  // Then the execution order is H, G, F, D, C, E, B, A, I
  std::vector<TopoStruct*> pre_topo_structs;
  // The topo structs to be executed immediately after this topo struct
  std::vector<TopoStruct*> post_topo_structs;

  // Execute the positive ancestors in order with the smallest peak memory
  std::vector<TopoStruct*> ordered_ancestors;

  explicit TopoStruct(int32_t blob_id_) : blob_id(blob_id_){};

  // Compute the minimum layer of this node
  int32_t ComputeMinLayer();
  // Block the descendants with negative memory increment
  void BlockDescendants();

  void ComputeIsReusable();

  void SetAccumulateMemoryIncrement();
  void MarkAncestors();
  int64_t SingleNodePriority();
  int64_t AccumulationPriority();

  void MarkDescendantFromThis2Layer(int32_t max_layer);

 private:
  void VisitAncestorsAndItself(const std::function<void(TopoStruct*)>& Handle);
  // Mark all its descendant with min_layer <= max_layer
  void MarkDescendantUp2Layer(int32_t max_layer);
  // Block descendants and store the blocking nodes in the given hash set
  void BlockDescendants(HashSet<TopoStruct*>* blocking_nodes);
};

// Compute the minimum layer of this node
int32_t TopoStruct::ComputeMinLayer() {
  if (min_layer >= 0) { return min_layer; }
  for (auto& in_topo_struct : in_topo_structs) {
    min_layer = std::max(min_layer, in_topo_struct->ComputeMinLayer());
  }
  return ++min_layer;
}

// Make sure max_difference = peak_memory - memory_increment
int64_t Priority(int64_t memory_increment, int64_t peak_memory, int64_t max_difference) {
  CHECK_EQ(peak_memory, memory_increment + max_difference);
  if (memory_increment < 0) { return peak_memory - kPriorityOffset; }
  if (memory_increment > 0) { return kPriorityBound - max_difference; }
  // memory_increment == 0
  return kPriorityOffset - max_difference;
}

int64_t TopoStruct::SingleNodePriority() {
  return Priority(memory_increment, peak_memory, max_difference);
}

int64_t TopoStruct::AccumulationPriority() {
  if (accumulate_memory_increment < 0) { return peak_memory_during_accumulation - kPriorityOffset; }
  if (accumulate_memory_increment > 0) { return kPriorityOffset + accumulate_memory_increment; }
  // accumulate_memory_increment == 0
  return kPriorityOffset - peak_memory_during_accumulation;
}

void TopoStruct::VisitAncestorsAndItself(const std::function<void(TopoStruct*)>& Handle) {
  for (const auto& in_topo_struct : in_topo_structs) {
    // Accumulate the non-executed topological structures only once
    if ((!in_topo_struct->executed) && in_topo_struct->visited_ancestors.IfNotMarked()) {
      in_topo_struct->VisitAncestorsAndItself(Handle);
    }
  }
  if (visited_ancestors.IfNotMarked()) { Handle(this); }
  visited_ancestors.Mark();
}

void TopoStruct::MarkDescendantUp2Layer(int32_t max_layer) {
  if (visited_descendant.IfMarked()) { return; }
  visited_descendant.Mark();
  if (min_layer < max_layer) {
    for (auto* out_node : out_topo_structs) { out_node->MarkDescendantUp2Layer(max_layer); }
  }
}

// .back() return a reference. But if the original map is destroyed in the same piece of code,
// the reference would point to [0xfffffffffffffff8], giving out an error.
TopoStruct* TakeBackFromVector(const std::vector<TopoStruct*>& v) { return v.back(); }

void TopoStruct::SetAccumulateMemoryIncrement() {
  ResetNoCleaningMarkerAncestor();
  // There are several lemma and propositions for this part. (Some of them omitted here)
  // Proposition 1:
  //    In the sub-graph of all the nodes with positive memory increment, picking the node with
  //    maximum difference would be picking the node with maximum accumulate memory increment.
  // Proposition 2:
  //    In the sub-graph of all the nodes with positive memory increment, picking the node with
  //    maximum difference in descending order would give us the lowest peak memory for this
  //    sub-graph.
  // We would prove this in the paper "Auto Memory" in the future.
  std::map<int64_t, std::vector<TopoStruct*>> max_difference2topo_structs;
  auto Add2Map = [&](TopoStruct* node) {
    max_difference2topo_structs[node->SingleNodePriority()].push_back(node);
  };
  // Take itself out and only add its ancestors
  visited_ancestors.Mark();
  VisitAncestorsAndItself(Add2Map);

  // Reset the status, no cleaning up would cause bug
  ResetNoCleaningMarkerAncestor();
  accumulate_memory_increment = 0;
  peak_memory_during_accumulation = 0;
  ordered_ancestors.clear();

  auto Execute = [&](TopoStruct* node) {
    ordered_ancestors.push_back(node);
    accumulate_memory_increment += node->memory_increment;
    peak_memory_during_accumulation = std::max(peak_memory_during_accumulation,
                                               accumulate_memory_increment + node->max_difference);
    // Remove from the map
    // At the end, we would remove itself from an empty map. Therefore, the return status might be
    // false.
    CheckAndRemoveFromMap(max_difference2topo_structs, node->SingleNodePriority(), node);
  };
  while (!max_difference2topo_structs.empty()) {
    TakeBackFromVector(max_difference2topo_structs.begin()->second)
        ->VisitAncestorsAndItself(Execute);
  }
  // Do not forget to execute itself at the end.
  accumulate_memory_increment += memory_increment;
  peak_memory_during_accumulation =
      std::max(peak_memory_during_accumulation, accumulate_memory_increment + max_difference);
  max_difference_during_accumulation =
      peak_memory_during_accumulation - accumulate_memory_increment;
}

void TopoStruct::MarkAncestors() {
  ResetNoCleaningMarkerAncestor();
  auto DoNothing = [](TopoStruct* node) {};
  VisitAncestorsAndItself(DoNothing);
}

void TopoStruct::MarkDescendantFromThis2Layer(int32_t max_layer) {
  ResetNoCleaningMarkerDescendant();
  MarkDescendantUp2Layer(max_layer);
}

// Block the descendants with negative memory increment
void TopoStruct::BlockDescendants(HashSet<TopoStruct*>* blocking_nodes) {
  if (blocking_topo_structs.empty()) {
    for (auto* out_node : out_topo_structs) {
      if (out_node->memory_increment < 0) {
        out_node->BlockDescendants();
        blocking_nodes->insert(out_node->blocking_topo_structs.begin(),
                               out_node->blocking_topo_structs.end());
        blocking_nodes->insert(out_node);
      } else {
        out_node->BlockDescendants(blocking_nodes);
      }
    }
  }
}

void TopoStruct::BlockDescendants() {
  if (memory_increment < 0 && blocking_topo_structs.empty()) {
    BlockDescendants(&blocking_topo_structs);
  }
}

void ComputeLayer(std::vector<TopoStruct*>* topo_structs) {
  // Initial all the layer to -1
  for (auto& topo_struct : *topo_structs) { topo_struct->min_layer = -1; }
  // Compute the minimum layer for the whole graph
  for (auto& topo_struct : *topo_structs) { topo_struct->ComputeMinLayer(); }
}

void ConnectTwoNodes(TopoStruct* producer, TopoStruct* consumer) {
  // Check if the edge exists
  if (consumer->in_topo_structs.find(producer) == consumer->in_topo_structs.end()) {
    consumer->in_topo_structs.insert(producer);
    producer->out_topo_structs.insert(consumer);
  }
}

void InitInOutTopoStructs(HashMap<const OpNode*, TopoStruct>& op_node2topo_struct) {
  // Generate the map from operator names to topological structure
  HashMap<std::string, TopoStruct*> op_name2topo_structs;
  for (auto& pair : op_node2topo_struct) {
    op_name2topo_structs[pair.first->op().op_name()] = &pair.second;
  }

  // Traverse the topological structures
  for (auto& pair : op_node2topo_struct) {
    auto& node = pair.first;
    auto* this_topo_struct = &pair.second;
    // Initialize input nodes for edges with data
    node->ForEachNodeOnInEdge([&](OpNode* in) {
      // Since we might be looking at a sub-graph of the operator graph.
      // We need to check if the op_node exists in the sub-graph.
      auto it = op_name2topo_structs.find(in->op().op_name());
      if (it != op_name2topo_structs.end()) { ConnectTwoNodes(it->second, this_topo_struct); }
    });
    // Initialize input nodes for control edges
    for (const auto& ctrl_in_op_name : node->op().op_conf().ctrl_in_op_name()) {
      auto it = op_name2topo_structs.find(ctrl_in_op_name);
      if (it != op_name2topo_structs.end()) { ConnectTwoNodes(it->second, this_topo_struct); }
    }
  }
}

// Compute the memory increment for all the topological structures
void ComputeAllMemoryIncrement(std::vector<TopoStruct*>& topo_structs,
                               std::vector<TopoStruct>& release_topo_structs,
                               std::vector<TopoStruct*>& id2producer_topo_struct,
                               std::vector<std::vector<TopoStruct*>>& id2consumer_topo_structs,
                               std::vector<int64_t>& id2blob_size) {
  // Prepare to insert the release blob
  for (int32_t id = 0; id < id2consumer_topo_structs.size(); id++) {
    if (id2consumer_topo_structs.at(id).empty()) {
      // If a blob does not have a consumer, then the blob is consumed by its producer itself
      id2consumer_topo_structs.at(id).push_back(id2producer_topo_struct[id]);
    } else {
      // Sort the consumer topological structure for later matching
      std::sort(id2consumer_topo_structs.at(id).begin(), id2consumer_topo_structs.at(id).end(),
                [](const TopoStruct* a, const TopoStruct* b) { return a < b; });
    }
  }

  // Compute the memory increment for produced blobs
  for (auto& topo_struct : topo_structs) {
    topo_struct->memory_increment = 0;
    topo_struct->peak_memory = 0;
  }

  for (int32_t id = 0; id < id2producer_topo_struct.size(); id++) {
    const auto& topo_struct = id2producer_topo_struct[id];
    if (topo_struct->is_reusable) {
      topo_struct->memory_increment += id2blob_size[id];
      topo_struct->peak_memory += id2blob_size[id];
    }
  }
  // Subtract the consumed memory
  for (int32_t id = 0; id < id2consumer_topo_structs.size(); id++) {
    auto* producer_topo_struct = id2producer_topo_struct[id];
    if (producer_topo_struct->is_reusable) {
      auto& consumer_topo_structs = id2consumer_topo_structs[id];
      // Release the blob in the blocking node
      if (consumer_topo_structs.size() == 1) {
        producer_topo_struct->memory_increment -= id2blob_size[id];
        producer_topo_struct->max_difference += id2blob_size[id];
        continue;
      }
      // Check whether two blobs have the same consumer_topo_structs
      auto& first_consumer_outs = consumer_topo_structs[0]->out_topo_structs;
      bool not_merged = true;
      for (auto* first_consumer_out_node : first_consumer_outs) {
        int32_t curr_release_blob_id = first_consumer_out_node->blob_id;
        if (curr_release_blob_id < 0) { continue; }
        // Compare whether the consumer_topo_structs are the same
        const auto& curr_topo_structs = id2consumer_topo_structs[curr_release_blob_id];
        bool is_same = curr_topo_structs.size() == consumer_topo_structs.size();
        for (int32_t consumer_id = 0; consumer_id < consumer_topo_structs.size(); consumer_id++) {
          if (consumer_topo_structs[consumer_id] != curr_topo_structs[consumer_id]) {
            is_same = false;
            break;
          }
        }
        // If they have the same consumer_topo_structs, merge them
        if (is_same) {
          first_consumer_out_node->memory_increment -= id2blob_size[id];
          first_consumer_out_node->max_difference += id2blob_size[id];
          not_merged = false;
          break;
        }
      }
      // If they have different consumer_topo_structs, add a new release_topo_struct
      if (not_merged) {
        release_topo_structs.emplace_back(id);
        auto& release_topo_struct = release_topo_structs.back();
        topo_structs.push_back(&release_topo_struct);
        release_topo_struct.memory_increment = -id2blob_size[id];
        release_topo_struct.peak_memory = -id2blob_size[id];
        // We need to execute all the consumers before releasing the blob
        for (auto& consumer_topo_struct : consumer_topo_structs) {
          ConnectTwoNodes(consumer_topo_struct, &release_topo_struct);
        }
      }
    }
  }
}

void InitAllParameters(HashMap<const OpNode*, TopoStruct>& op_node2topo_struct,
                       HashMap<LogicalBlobId, int32_t>* lbi2id,
                       std::vector<TopoStruct*>* id2producer_topo_struct,
                       std::vector<std::vector<TopoStruct*>>* id2consumer_topo_structs,
                       std::vector<int64_t>* id2blob_size) {
  for (auto& pair : op_node2topo_struct) {
    const auto& producer = pair.first->op();
    auto* topo_struct = &pair.second;

    // Find all the blobs produced by this operator
    for (const auto& obn : producer.output_bns()) {
      const LogicalBlobId& lbi = producer.BnInOp2Lbi(obn);
      auto it = lbi2id->find(lbi);
      // We check existence in case of inplace operators, whose producer and consumer produce the
      // same blob
      if (it == lbi2id->end()) {
        (*lbi2id)[lbi] = id2blob_size->size();
        const BlobDesc& logical_blob_desc = pair.first->LogicalBlobDesc4Lbi(lbi);
        id2blob_size->push_back(TotalByteSize4BlobDesc(logical_blob_desc));
        id2producer_topo_struct->push_back(topo_struct);
      }
    }
  }

  // initialize the id2consumer_topo_structs
  id2consumer_topo_structs->resize(id2blob_size->size());
  // Find all the blobs consumed by this operator
  for (auto& pair : op_node2topo_struct) {
    const auto& consumer = pair.first->op();
    for (const auto& ibn : consumer.input_bns()) {
      const LogicalBlobId& lbi = consumer.BnInOp2Lbi(ibn);
      id2consumer_topo_structs->at(lbi2id->find(lbi)->second).push_back(&pair.second);
    }
  }

  // Construct all the data edges and control edges
  InitInOutTopoStructs(op_node2topo_struct);
}

void ClipOneEdge(TopoStruct* producer, TopoStruct* consumer) {
  producer->out_topo_structs.erase(consumer);
  consumer->in_topo_structs.erase(producer);
}

void EatNodes(std::vector<TopoStruct*>& topo_structs) {
  for (int32_t id = topo_structs.size() - 1; id >= 0; id--) {
    auto* node = topo_structs[id];
    bool not_merged = true;
    // If a node only has one output with higher priority, then it would be executed at the last
    // moment before the execution of its output
    if (node->out_topo_structs.size() == 1) {
      // d: a, b, c -> d(+) -> g
      // g: b, d, e, f -> g -> ...
      // d has non-negative memory increment (>=0), d only have one out edge: d -> g.
      // But g might have multiple inputs.
      auto* out_node = *node->out_topo_structs.begin();
      // Only merge if the out node have higher priority
      // (higher priority means smaller value in SingleNodePriority())
      if (node->SingleNodePriority() >= out_node->SingleNodePriority()) {
        // Merge d into g: (d)g
        out_node->pre_topo_structs.push_back(node);
        out_node->memory_increment += node->memory_increment;
        out_node->peak_memory =
            std::max(node->peak_memory, node->memory_increment + out_node->peak_memory);
        out_node->max_difference = out_node->peak_memory - out_node->memory_increment;
        // Clip d -> g
        ClipOneEdge(node, out_node);
        // g takes all the inputs from d
        // g: a, b, c, e, f -> (d)g -> ...
        // Note that b is also an input of the origin g
        // and we need to make sure that b does not occur twice.
        for (auto* in_node : node->in_topo_structs) {
          // Insert a -> g, b -> g, c -> g
          ConnectTwoNodes(in_node, out_node);
          // Clip a -> d, b -> d, c -> d
          in_node->out_topo_structs.erase(node);
        }
        node->in_topo_structs.clear();
        // Eliminate d
        RemoveFrom(topo_structs, id);
        not_merged = false;
      }
    }
    // A negative node with only one input and the highest priority (non-positive peak memory)
    // would be executed immediately after the execution of its input
    if (not_merged && node->in_topo_structs.size() == 1 && node->peak_memory <= 0) {
      // b: a -> b(-) -> c, d, e
      // a: ... -> a -> b, d, f, g
      // b has negative memory increment (<0), b only have one in edge: a -> b.
      // But a might have multiple outputs
      auto* in_node = *node->in_topo_structs.begin();
      // Merge b into a: a(b)
      in_node->post_topo_structs.push_back(node);
      in_node->memory_increment += node->memory_increment;
      in_node->peak_memory =
          std::max(in_node->peak_memory, in_node->memory_increment + node->peak_memory);
      in_node->max_difference = in_node->peak_memory - in_node->memory_increment;
      // Clip a -> b
      ClipOneEdge(in_node, node);
      // a takes all the outputs from b
      // a: ... -> a(b) -> c, d, e, f, g
      // Note tht d is also an output of the origin a
      // and we need to make sure that d does not occur twice
      for (auto* out_node : node->out_topo_structs) {
        // Insert a -> c, a -> d, a -> e
        ConnectTwoNodes(in_node, out_node);
        // Clip b -> c, b -> d, b -> e
        out_node->in_topo_structs.erase(node);
      }
      node->out_topo_structs.clear();
      // Eliminate b
      RemoveFrom(topo_structs, id);
    }
  }
}

// Clip those useless edges
// For example: a -> b -> c -> d, a -> c, a -> d.
// Then we could clip the two edges a -> c and a -> d.
void ClipEdges(std::vector<TopoStruct*>& topo_structs) {
  ComputeLayer(&topo_structs);
  // In the implementation, we only focus on a node with multiple inputs,
  // since max(in_nodes->min_layer) == this_node->min_layer - 1.
  for (auto* node : topo_structs) {
    auto& node_in_topo_structs = node->in_topo_structs;
    // Suppose we have multiple input nodes
    // a, b, c -> d
    if (node_in_topo_structs.size() >= 2) {
      int32_t max_layer = node->min_layer - 1;
      for (auto it = node_in_topo_structs.begin(); it != node_in_topo_structs.end();) {
        auto* in_node = *it;
        bool not_removed = true;
        // Find all the descendants of node a
        in_node->MarkDescendantFromThis2Layer(max_layer);
        for (auto* brother : node_in_topo_structs) {
          // If we found a -> ... -> b (or a -> ... -> c)
          // The first judgement is to make sure we are comparing a different node,
          // i.e., brother != in_node
          if (brother->min_layer > in_node->min_layer && brother->visited_descendant.IfMarked()) {
            // Remove a -> d
            // Be careful that we need to remove the edge from two sides.
            it = node_in_topo_structs.erase(it);
            in_node->out_topo_structs.erase(node);
            not_removed = false;
            break;
          }
        }
        if (not_removed) { ++it; }
      }
    }
  }
}

// Adjust the order between two topo structs with negative memory increment by adding edges
void SortReleaseTopoStructs(std::vector<TopoStruct*>& topo_structs) {
  // First, we define a release node as a node with negative memory increment.
  // If all the producers of a release node is executed, this node should be executed immediately.
  // Therefore, a release node "a" should be executed before the execution of another release node
  // "b" if the ancestors of "b" contains all the producers of "a". As a result, the edge "a -> b"
  // would be added.

  // Collect all the release nodes
  std::vector<TopoStruct*> release_nodes;
  release_nodes.reserve(topo_structs.size());
  for (auto* node : topo_structs) {
    if (node->memory_increment < 0) { release_nodes.push_back(node); }
  }
  // Suppose two release nodes c(-) and d(-)
  // a -> ... -> c(-)
  // b -> ... -> c(-)
  // d(-): a, b -> d(-) -> ...
  // Then we add the edge d(-) -> c(-)
  for (auto* node_c : release_nodes) {
    // Mark all the ancestors
    node_c->MarkAncestors();
    // Un-mark itself to prevent circle
    node_c->visited_ancestors.UnMark();
    for (auto* node_d : release_nodes) {
      // Current condition is that d has higher priority
      // Another stricter condition is the node_d->peak_memory <= 0
      if (node_c != node_d && node_d->peak_memory <= 0 && node_d->visited_ancestors.IfNotMarked()) {
        bool should_add_edge_d2c = true;
        // Check a, b for d(-): a, b -> d(-) -> ...
        for (auto* producer_of_d : node_d->in_topo_structs) {
          // Try to find a -> ... -> c(-) and b -> ... -> c(-)
          if (producer_of_d->visited_ancestors.IfNotMarked()) {
            should_add_edge_d2c = false;
            break;
          }
        }
        if (should_add_edge_d2c) {
          // Add the edge d(-) -> c(-)
          ConnectTwoNodes(node_d, node_c);
        }
      }
    }
  }
}

void GraphSimplification(std::vector<TopoStruct*>& topo_structs) {
  if (GlobalProcessCtx::Rank() == 0) {
    std::cout << "Origin, Topo size: " << topo_structs.size() << std::endl;
  }
  EatNodes(topo_structs);
  if (GlobalProcessCtx::Rank() == 0) {
    std::cout << "After 1st Eats, Topo size: " << topo_structs.size() << std::endl;
  }
  ClipEdges(topo_structs);
  EatNodes(topo_structs);
  if (GlobalProcessCtx::Rank() == 0) {
    std::cout << "2nd, clip and eat, Topo size: " << topo_structs.size() << std::endl;
  }
  ClipEdges(topo_structs);
  EatNodes(topo_structs);
  if (GlobalProcessCtx::Rank() == 0) {
    std::cout << "3rd, clip and eat, Topo size: " << topo_structs.size() << std::endl;
  }
  for (int32_t i = 4; i < 120; i++) {
    SortReleaseTopoStructs(topo_structs);
    ClipEdges(topo_structs);
    EatNodes(topo_structs);
    if (GlobalProcessCtx::Rank() == 0) {
      std::cout << i << "th, sort, clip and eat, Topo size: " << topo_structs.size() << std::endl;
    }
  }
  // TODO: Clip edges
}

void InitBlockingNodes(std::vector<TopoStruct*>& topo_structs) {
  for (auto* node : topo_structs) {
    if (node->memory_increment < 0) {
      node->BlockDescendants();
      node->blocking_count = 0;
    }
  }
  for (auto* node : topo_structs) {
    if (node->memory_increment < 0) {
      for (auto* out_release_node : node->blocking_topo_structs) {
        out_release_node->blocking_count++;
      }
    }
  }
}

void StraightenMemoryOpNodes(std::vector<TopoStruct*>* topo_structs,
                             HashMap<LogicalBlobId, int32_t>* lbi2id,
                             std::vector<TopoStruct*>* id2producer_topo_struct,
                             std::vector<std::vector<TopoStruct*>>* id2consumer_topo_structs,
                             std::vector<int64_t>* id2blob_size,
                             std::vector<TopoStruct*>* ordered_topo_structs) {
  // The number of executing topographical structures
  int32_t executing_topo_struct_num = topo_structs->size();
  // Extra release topological structures
  std::vector<TopoStruct> release_topo_structs;
  // Reserve the space for release topological structures
  // We do not define a copy method for TopoStruct. During each size expansion, the data might be
  // screwed up if we do not reserve the space.
  release_topo_structs.reserve(id2blob_size->size());
  // Initialize the no cleaning marker
  InitNoCleaningMarkerAncestor();
  InitNoCleaningMarkerDescendant();

  // Compute the memory increment for all the topological structures
  ComputeAllMemoryIncrement(*topo_structs, release_topo_structs, *id2producer_topo_struct,
                            *id2consumer_topo_structs, *id2blob_size);

  if (GlobalProcessCtx::Rank() == 0) {
    std::cout << "Print all the topological structures:" << std::endl;
    int64_t total_memory = 0;
    int32_t total_in_size = 0, total_out_size = 0;
    for (const auto& topo_struct : *topo_structs) {
      topo_struct->SetAccumulateMemoryIncrement();
      std::cout << "In size: " << topo_struct->in_topo_structs.size()
                << ", out size: " << topo_struct->out_topo_structs.size() << ", "
                << ", accumulate memory increment: " << topo_struct->accumulate_memory_increment
                << ", ";
      total_in_size += topo_struct->in_topo_structs.size();
      total_out_size += topo_struct->out_topo_structs.size();
      if (topo_struct->blob_id != -1) {
        std::cout << "Blob id: " << topo_struct->blob_id
                  << " Memory increment: " << topo_struct->memory_increment << std::endl;
      } else {
        std::cout << "Op node: " << topo_struct->op_node->op().op_name()
                  << " Memory increment: " << topo_struct->memory_increment << std::endl;
      }
      total_memory += topo_struct->memory_increment;
    }
    std::cout << "Total memory: " << total_memory << ", total in size: " << total_in_size
              << ", total out size: " << total_out_size << std::endl;
  }

  // GraphSimplification(*topo_structs);

  // Those nodes that we need to visit their descendants
  // At the beginning, them would be the source nodes.
  // After each execution, them would be those executed nodes.
  std::vector<TopoStruct*> prepare_topo_structs;
  // wait in the map
  std::map<int64_t, std::vector<TopoStruct*>> waiting_map;
  // Erase a node from the waiting map
  auto StopWaiting = [&](TopoStruct* node) {
    if (node->waiting) {
      node->waiting = false;
      CHECK(CheckAndRemoveFromMap(waiting_map, node->AccumulationPriority(), node));
    }
  };
  // Wait in the map
  auto Wait = [&](TopoStruct* node) {
    if (node->executed || node->blocking_count > 0) { return; }
    StopWaiting(node);
    node->SetAccumulateMemoryIncrement();
    waiting_map[node->AccumulationPriority()].push_back(node);
    node->waiting = true;
  };
  // Visit one node
  std::function<void(TopoStruct*)> Visit = [&](TopoStruct* node) {
    if (node->visited_descendant.IfMarked()) { return; }
    node->visited_descendant.Mark();
    if (node->memory_increment < 0 && !node->executed) {
      Wait(node);
    } else {
      for (auto* out_node : node->out_topo_structs) { Visit(out_node); }
    }
  };
  // Prepare all the release nodes before picking one for the next round
  auto Prepare = [&]() {
    ResetNoCleaningMarkerDescendant();
    for (auto* node : prepare_topo_structs) { Visit(node); }
  };
  int64_t peak_memory = 0;
  int64_t total_memory = 0;
  std::function<void(TopoStruct*)> ExecuteOpNode = [&](TopoStruct* node) {
    for (int32_t i = node->pre_topo_structs.size() - 1; i >= 0; i--) {
      ExecuteOpNode(node->pre_topo_structs[i]);
    }
    if (node->blob_id == kOriginNode) { ordered_topo_structs->push_back(node); }
    for (auto* post_node : node->post_topo_structs) { ExecuteOpNode(post_node); }
  };
  // Execute one node and its ancestors
  std::function<void(TopoStruct*)> Execute = [&](TopoStruct* node) {
    // Post-order traversal
    for (auto* ancestor : node->ordered_ancestors) {
      if (!ancestor->executed) {
        ExecuteOpNode(ancestor);
        ancestor->executed = true;
        StopWaiting(ancestor);
        prepare_topo_structs.push_back(ancestor);
      }
    }
    // Execute the current node
    ExecuteOpNode(node);
    node->executed = true;
    total_memory += node->accumulate_memory_increment;
    peak_memory = std::max(peak_memory, total_memory + node->max_difference_during_accumulation);
    StopWaiting(node);
    prepare_topo_structs.push_back(node);
    if (GlobalProcessCtx::Rank() == 0) {
      std::cout << "Executing ";
      if (node->op_node) {
        std::cout << node->op_node->op().op_name();
      } else {
        std::cout << "blob id: " << node->blob_id;
      }
      std::cout << ", memory increment: " << node->memory_increment
                << ", current total: " << total_memory << std::endl;
    }
    if (node->memory_increment < 0) {
      for (auto* out_release_node : node->blocking_topo_structs) {
        // TODO: Remove prepare and use blocking count instead
        out_release_node->blocking_count--;
      }
    }
  };

  // TODO: expand to all the topo structures
  // Initialize source topological structures
  for (int32_t i = 0; i < topo_structs->size(); i++) {
    if (topo_structs->at(i)->in_topo_structs.empty()) {
      prepare_topo_structs.push_back(topo_structs->at(i));
    }
  }
  // Init blocking release nodes
  InitBlockingNodes(*topo_structs);
  // Straighten memory
  while (ordered_topo_structs->size() < executing_topo_struct_num) {
    // Prepare the release node for this round
    Prepare();
    // Clean up the prepare_topo_structs before executing any node
    prepare_topo_structs.clear();
    // Pick the one with the smallest accumulate memory increment and then execute it
    if (waiting_map.empty()) { break; }
    Execute(TakeBackFromVector(waiting_map.begin()->second));
  }

  // Execute the rest of the nodes
  for (auto& node : *topo_structs) {
    if (!node->executed) {
      CHECK(node->memory_increment >= 0)
          << "All the blobs should be release during straighten memory!";
      node->SetAccumulateMemoryIncrement();
      Execute(node);
    }
  }
}
}  // namespace

// Straighten a subset of the op graph
void StraightenMemorySubGraph(const std::vector<const OpNode*>& sub_graph,
                              std::vector<const OpNode*>* ordered_op_nodes) {
  // Generate topological data structure for each op node
  HashMap<const OpNode*, TopoStruct> op_node2topo_struct;
  std::vector<TopoStruct*> topo_structs;
  std::vector<TopoStruct*> ordered_topo_structs;
  HashMap<TopoStruct*, const OpNode*> topo_struct2op_node;

  // Traverse all the nodes in the sub graph
  for (const auto* node : sub_graph) {
    op_node2topo_struct.insert({node, TopoStruct(kOriginNode)});
    auto& topo_struct = op_node2topo_struct.at(node);
    topo_struct.op_node = node;
    topo_struct.is_reusable = IsProducedRegisterReusable(node->op());
    topo_structs.push_back(&topo_struct);
    topo_struct2op_node[&topo_struct] = node;
  }

  // Construct the map from a lbi to its id, producer, consumers, blob size
  HashMap<LogicalBlobId, int32_t> lbi2id;
  std::vector<TopoStruct*> id2producer_topo_struct;
  std::vector<std::vector<TopoStruct*>> id2consumer_topo_structs;
  std::vector<int64_t> id2blob_size;

  InitAllParameters(op_node2topo_struct, &lbi2id, &id2producer_topo_struct,
                    &id2consumer_topo_structs, &id2blob_size);

  StraightenMemoryOpNodes(&topo_structs, &lbi2id, &id2producer_topo_struct,
                          &id2consumer_topo_structs, &id2blob_size, &ordered_topo_structs);

  for (auto& ordered_topo_struct : ordered_topo_structs) {
    ordered_op_nodes->push_back(topo_struct2op_node[ordered_topo_struct]);
  }
}

// Straighten the whole op graph
void StraightenMemoryOpGraph(const OpGraph& op_graph,
                             std::vector<const OpNode*>* ordered_op_nodes) {
  std::vector<const OpNode*> sub_graph;

  // Traverse and store all the nodes in the op graph
  op_graph.ForEachNode([&](OpNode* node) { sub_graph.push_back(node); });

  StraightenMemorySubGraph(sub_graph, ordered_op_nodes);
}

}  // namespace auto_parallel
}  // namespace oneflow