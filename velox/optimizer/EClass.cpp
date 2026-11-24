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

#include "velox/optimizer/EClass.h"

namespace facebook::velox::optimizer {

bool EClass::addNode(const ENode& node) {
  // Check for duplicates.
  for (const auto& existing : nodes_) {
    if (existing == node) {
      return false;
    }
  }
  nodes_.push_back(node);
  invalidateCost();
  return true;
}

bool EClass::addNode(ENode&& node) {
  for (const auto& existing : nodes_) {
    if (existing == node) {
      return false;
    }
  }
  nodes_.push_back(std::move(node));
  invalidateCost();
  return true;
}

} // namespace facebook::velox::optimizer
