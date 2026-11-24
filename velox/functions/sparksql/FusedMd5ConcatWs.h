/*
 * Copyright (c) Meta Platforms, Inc. and its affiliates.
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

// FusedMd5ConcatWs computes md5(concat_ws(sep, args...)) in one pass,
// skipping the varchar-to-varbinary cast and the intermediate concat string.

#pragma once

#include "velox/core/Expressions.h"
#include "velox/expression/VectorFunction.h"
#include "velox/external/md5/md5.h"

namespace facebook::velox::functions::sparksql {

/// Signatures for fused_md5_concat_ws (Spark concat_ws + md5):
///   fused_md5_concat_ws(varchar) -> varchar
///   fused_md5_concat_ws(varchar, varchar, ...) -> varchar
/// First arg is the separator. NULL data args are skipped (not default-null).
/// Only separator, or all data args NULL, hashes "" (same as concat_ws).
std::vector<std::shared_ptr<exec::FunctionSignature>>
fusedMd5ConcatWsSignatures();

/// Factory function for registering as a stateful VectorFunction.
std::shared_ptr<exec::VectorFunction> makeFusedMd5ConcatWs(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

/// Metadata for the fused function.
exec::VectorFunctionMetadata fusedMd5ConcatWsMetadata();

/// Rewrite md5(cast(concat_ws(...) as binary)) to fused_md5_concat_ws.
/// Returns nullptr when the pattern does not match.
core::TypedExprPtr rewriteMd5ConcatWs(
    const std::string& prefix,
    const core::TypedExprPtr& expr);

} // namespace facebook::velox::functions::sparksql
