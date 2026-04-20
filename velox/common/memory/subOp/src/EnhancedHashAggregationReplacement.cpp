/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/core/PlanNode.h"
#include "velox/exec/Driver.h"
#include "velox/exec/HashAggregation.h"
#include "velox/exec/Task.h"
#include "velox/functions/Udf.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <iostream>

#include "velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedHashAggregation.h"
#include "velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedHashAggregationReplacement.h"

namespace facebook::velox::exec {

namespace {

// Thread-safe registration tracking
std::mutex registrationMutex;
std::unordered_set<std::string> registeredAdapters;
constexpr const char* kAdapterLabel = "enhancedHashAggRep";

// Helper function to find a plan node by ID in the plan tree
core::PlanNodePtr findPlanNodeById(
    const core::PlanNodePtr& node,
    const core::PlanNodeId& targetId) {
  if (node->id() == targetId) {
    return node;
  }
  for (const auto& source : node->sources()) {
    auto found = findPlanNodeById(source, targetId);
    if (found) {
      return found;
    }
  }
  return nullptr;
}

// Helper to extract plan fragment from Task
// Note: This requires Task::planFragment_ to be accessible
// If it's private, you may need to:
// 1. Add Task as a friend class, or
// 2. Add a public getter method to Task, or
// 3. Use a PlanNodeTranslator instead of adapter
// Helper to get plan node by id lookup.
// This follows the same pattern as official Velox cudf replacement adapter.
// The consumerNode is the (local) node that will consume results supplied by
// this pipeline, and it may not be in the planNodes list.
core::PlanNodePtr getPlanNode(
    const DriverFactory& driverFactory,
    const core::PlanNodeId& planNodeId) {
  // First, try to find in driverFactory's planNodes
  auto& nodes = driverFactory.planNodes;
  auto it = std::find_if(
      nodes.cbegin(), nodes.cend(), [&planNodeId](const auto& node) {
        return node && node->id() == planNodeId;
      });
  if (it != nodes.end()) {
    return *it;
  }

  // If not found in planNodes, check if it's the consumerNode
  // The consumerNode may not be in planNodes list as it belongs to the next pipeline
  if (driverFactory.consumerNode && driverFactory.consumerNode->id() == planNodeId) {
    return driverFactory.consumerNode;
  }

  // If still not found, this is unexpected - the planNodeId should exist
  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR,
      "Plan node not found for: " << planNodeId
          << " in driverFactory planNodes or consumerNode.");
  return nullptr;
}

bool EnhancedHashAggregationDriverAdapter(
    const DriverFactory& driverFactory_,
    Driver& driver_) {
  auto operators = driver_.operators();
  // Make sure operator states are initialized.
  // Note: We don't initialize operators here because that happens in runInternal stage.
  // Adapter replaces operators (no initialization needed). runInternal initializes all operators
  // (including newly replaced ones) before execution.
  auto ctx = driver_.driverCtx();
  auto task = ctx->task;

  // Debug: Print Driver and Pipeline information
  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR,
      "Driver info: driverId=" << ctx->driverId << ", pipelineId=" << ctx->pipelineId
          << ", splitGroupId=" << ctx->splitGroupId << ", partitionId=" << ctx->partitionId
          << ", taskId=" << (task ? task->taskId() : "null"));

  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR,
      "Pipeline info: numPlanNodes=" << driverFactory_.planNodes.size()
          << ", maxDrivers=" << driverFactory_.maxDrivers
          << ", numDrivers=" << driverFactory_.numDrivers
          << ", numTotalDrivers=" << driverFactory_.numTotalDrivers
          << ", groupedExecution=" << (driverFactory_.groupedExecution ? "true" : "false")
          << ", inputDriver=" << (driverFactory_.inputDriver ? "true" : "false")
          << ", outputDriver=" << (driverFactory_.outputDriver ? "true" : "false"));

  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR, "Operators count: " << operators.size());
#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
  for (size_t i = 0; i < operators.size(); ++i) {
    Operator* oper = operators[i];
    if (oper) {
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR,
          "  Operator[" << i << "]: operatorId=" << oper->operatorId()
              << ", planNodeId=" << oper->planNodeId()
              << ", operatorType=" << oper->operatorType());
    }
  }
#endif

  bool replacedAny = false;
  for (int32_t operatorIndex = 0; operatorIndex < operators.size();
       ++operatorIndex) {
    Operator* oper = operators[operatorIndex];
    VELOX_CHECK(oper);
    if (auto hashAggOp = dynamic_cast<HashAggregation*>(oper)) {
      std::vector<std::unique_ptr<Operator>> replace_op;

      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR,
          "Found HashAggregation operator: id=" << hashAggOp->operatorId()
              << ", planNodeId=" << hashAggOp->planNodeId());

      auto planNodeId = hashAggOp->planNodeId();
      core::PlanNodePtr planNode = getPlanNode(driverFactory_, planNodeId);

      auto operatorId = hashAggOp->operatorId();
#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
      if (operatorId != operatorIndex) {
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR, "operatorId (" << operatorId << ") != operatorIndex (" << operatorIndex << ")");
      }
#endif

      if (!planNode) {
        // Try to find in plan tree if we can access planFragment
        // This requires access to Task::planFragment_ which is private
        // You may need to modify Task.h to add:
        //   friend class EnhancedHashAggregationAdapter;
        //   const core::PlanFragment& planFragment() const { return planFragment_; }
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "Could not get plan node for: " << planNodeId << ". Skipping replacement.");
        continue;
      }

      auto aggregationNode =
          std::dynamic_pointer_cast<const core::AggregationNode>(planNode);
      if (!aggregationNode) {
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "Plan node is not an AggregationNode: " << planNodeId << ". Skipping replacement.");
        continue;
      }
      // Note: If aggregationNode->isPreGrouped() is true, it should be converted to
      // StreamingAggregation instead of HashAggregation, so we skip replacement in that case.
      if (aggregationNode->isPreGrouped()) {
        continue;
      }

      // Debug: Print detailed AggregationNode information
#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR, "AggregationNode details - PlanNodeId: " << planNodeId);

      // 1. Basic information
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR,
          "Aggregation step: " << core::AggregationNode::stepName(aggregationNode->step())
              << ", ignoreNullKeys: " << (aggregationNode->ignoreNullKeys() ? "true" : "false"));

      // 2. Input types
      const auto& sources = aggregationNode->sources();
      if (!sources.empty()) {
        const auto& inputType = sources[0]->outputType();
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR, "Input type: " << inputType->toString());
#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
        for (size_t i = 0; i < inputType->size(); ++i) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR,
              "  Input field[" << i << "]: name=" << inputType->nameOf(i)
                  << ", type=" << inputType->childAt(i)->toString());
        }
#endif
      }

      // 3. Grouping keys
      const auto& groupingKeys = aggregationNode->groupingKeys();
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR, "Grouping keys count: " << groupingKeys.size());
#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
      for (size_t i = 0; i < groupingKeys.size(); ++i) {
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "  GroupingKey[" << i << "]: " << groupingKeys[i]->toString()
                << ", type=" << groupingKeys[i]->type()->toString());
      }
#endif

      // 4. Aggregate functions details
      const auto& aggregates = aggregationNode->aggregates();
      const auto& aggregateNames = aggregationNode->aggregateNames();
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR, "Aggregate functions count: " << aggregates.size());

#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
      for (size_t i = 0; i < aggregates.size(); ++i) {
        const auto& agg = aggregates[i];
        const auto& aggName = aggregateNames[i];

        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR, "Aggregate[" << i << "]: name=" << aggName);

        // Function call information
        if (agg.call) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR, "  Function: " << agg.call->name());
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR, "  Output type: " << agg.call->type()->toString());

          const auto& callInputs = agg.call->inputs();
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR, "  Call inputs count: " << callInputs.size());
          for (size_t j = 0; j < callInputs.size(); ++j) {
            ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
                ERROR,
                "    CallInput[" << j << "]: " << callInputs[j]->toString()
                    << ", type=" << callInputs[j]->type()->toString());
          }
        }

        // Raw input types
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR, "  Raw input types count: " << agg.rawInputTypes.size());
        for (size_t j = 0; j < agg.rawInputTypes.size(); ++j) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR,
              "    RawInputType[" << j << "]: " << agg.rawInputTypes[j]->toString());
        }

        // Other attributes
        if (agg.mask) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR,
              "  Mask: " << agg.mask->toString()
                  << ", type=" << agg.mask->type()->toString());
        }

        if (agg.distinct) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(ERROR, "  Distinct: true");
        }

        if (!agg.sortingKeys.empty()) {
          ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
              ERROR, "  Sorting keys count: " << agg.sortingKeys.size());
          for (size_t j = 0; j < agg.sortingKeys.size(); ++j) {
            ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
                ERROR,
                "    SortingKey[" << j << "]: " << agg.sortingKeys[j]->toString());
            if (j < agg.sortingOrders.size()) {
              const auto& order = agg.sortingOrders[j];
              ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
                  ERROR,
                  "      Order: " << (order.isAscending() ? "ASC" : "DESC")
                      << ", nullsFirst=" << order.isNullsFirst());
            }
          }
        }
      }
#endif

      // 5. Output type
      const auto& outputType = aggregationNode->outputType();
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR, "Output type: " << outputType->toString());
      for (size_t i = 0; i < outputType->size(); ++i) {
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "  Output field[" << i << "]: name=" << outputType->nameOf(i)
                << ", type=" << outputType->childAt(i)->toString());
      }

#endif

      // Create replacement operator
      // IMPORTANT: EnhancedHashAggregation uses a different operatorType
      // ("EnhancedPartialAggregation" or "EnhancedAggregation") than HashAggregation
      // ("PartialAggregation" or "Aggregation"), so the memory pool names will be different:
      // - Old: "op.{planNodeId}.{pipelineId}.{driverId}.PartialAggregation"
      // - New: "op.{planNodeId}.{pipelineId}.{driverId}.EnhancedPartialAggregation"
      // This avoids memory pool name collision.
      ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
          ERROR, "Creating EnhancedHashAggregation operator");
      replace_op.push_back(
          std::make_unique<EnhancedHashAggregation>(operatorIndex, ctx, aggregationNode));

      // Note: We don't call initialize() here because that happens in driver runInternal stage,
      // not during createDriver stage.

      // Replace the operator
      auto replaced = driverFactory_.replaceOperators(
          driver_, operatorIndex, operatorIndex + 1, std::move(replace_op));

      // replaceOperators returns a vector of replaced operators, so check if it's not empty
      if (!replaced.empty()) {
        replacedAny = true;
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "Successfully replaced HashAggregation operator at index " << operatorIndex);
      } else {
        ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
            ERROR,
            "Failed to replace HashAggregation operator at index " << operatorIndex);
      }
    }
  }

  // return true;
  return replacedAny;
}

} // namespace

void registerEnhancedHashAggregationReplacement() {
  std::lock_guard<std::mutex> lock(registrationMutex);
  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR, "Registering adapter: " << kAdapterLabel);

  // Check if already registered (idempotent)
  if (registeredAdapters.find(kAdapterLabel) != registeredAdapters.end()) {
    ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
        ERROR, "Adapter already registered, skipping.");
    return;
  }

  DriverAdapter adapter{kAdapterLabel, {}, EnhancedHashAggregationDriverAdapter};
  DriverFactory::registerAdapter(adapter);
  registeredAdapters.insert(kAdapterLabel);
}

// Gluten UDF interface definitions
// This allows the plugin to be loaded via UDF library paths without modifying Gluten code
namespace gluten {
struct UdfEntry {
  const char* name;
  const char* dataType;
  int numArgs;
  const char** argTypes;
  bool variableArity{false};
  bool allowTypeConversion{false};
};

#define GLUTEN_GET_NUM_UDF getNumUdf
#define DEFINE_GET_NUM_UDF extern "C" int GLUTEN_GET_NUM_UDF()

#define GLUTEN_GET_UDF_ENTRIES getUdfEntries
#define DEFINE_GET_UDF_ENTRIES extern "C" void GLUTEN_GET_UDF_ENTRIES(gluten::UdfEntry* udfEntries)

#define GLUTEN_REGISTER_UDF registerUdf
#define DEFINE_REGISTER_UDF extern "C" void GLUTEN_REGISTER_UDF()
} // namespace gluten

// Implement Gluten UDF interface - directly register the adapter
extern "C" {

// Return 0 UDFs (we're not registering any UDF functions, just the adapter)
DEFINE_GET_NUM_UDF {
  return 0;
}

// No UDF entries to populate
DEFINE_GET_UDF_ENTRIES {
  // Do nothing - we have no UDF entries
}

// This is the key function - Gluten calls this when loading the library
// We directly register the adapter here
DEFINE_REGISTER_UDF {
  ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(
      ERROR, "Library loaded, registering adapter via Gluten UDF interface");
  registerEnhancedHashAggregationReplacement();
}

} // extern "C"

void unregisterEnhancedHashAggregationReplacement() {
  std::lock_guard<std::mutex> lock(registrationMutex);
  
  // Remove the adapter from DriverFactory::adapters, following the same pattern
  // as official Velox cudf replacement adapter
  DriverFactory::adapters.erase(
      std::remove_if(
          DriverFactory::adapters.begin(),
          DriverFactory::adapters.end(),
          [](const DriverAdapter& adapter) {
            return adapter.label == kAdapterLabel;
          }),
      DriverFactory::adapters.end());
  
  // Also remove from our tracking set
  registeredAdapters.erase(kAdapterLabel);
}

bool isEnhancedHashAggregationRegistered() {
  std::lock_guard<std::mutex> lock(registrationMutex);
  return registeredAdapters.find(kAdapterLabel) != registeredAdapters.end();
}

// namespace {
// // 静态初始化：在库加载时自动执行
// struct AutoRegister {
//   AutoRegister() {
//     ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(ERROR, "AutoRegister: registering adapter");
//     registerEnhancedHashAggregationReplacement();
//   }
// };

// // 全局静态对象，库加载时自动构造
// static AutoRegister g_autoRegister;
// } // namespace

} // namespace facebook::velox::exec

