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

#include "velox/functions/sparksql/FusedMd5ConcatWs.h"
#include <cstring>
#include "velox/core/Expressions.h"
#include "velox/expression/ConstantExpr.h"
#include "velox/expression/StringWriter.h"

namespace facebook::velox::functions::sparksql {

namespace {

// Spark / Velox concat_ws(sep) and concat_ws(sep, NULL, ...) with no
// remaining pieces both yield "". md5 of that is this constant.
constexpr char kEmptyMd5Hex[] = "d41d8cd98f00b204e9800998ecf8427e";

class FusedMd5ConcatWs : public exec::VectorFunction {
 public:
  explicit FusedMd5ConcatWs(const std::optional<StringView>& separator)
      : separator_(separator) {}

  bool isConstantSeparator() const {
    return separator_.has_value();
  }

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& /*outputType*/,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    context.ensureWritable(rows, VARCHAR(), result);
    auto* flatResult = result->asFlatVector<StringView>();

    const auto numArgs = args.size();

    // NULL separator => all results NULL
    if (isConstantSeparator() && args[0]->isNullAt(0)) {
      auto localResult = BaseVector::createNullConstant(
          VARCHAR(), rows.end(), context.pool());
      context.moveOrCopyResult(localResult, rows, result);
      return;
    }

    // Only separator, no data => md5("") = d41d8cd98f00b204e9800998ecf8427e
    if (numArgs == 1) {
      auto rawStringBuffer = flatResult->getRawStringBufferWithSpace(
          static_cast<size_t>(rows.countSelected()) * 32, true);
      char* hexWritePos = rawStringBuffer;
      rows.applyToSelected([&](auto row) {
        std::memcpy(hexWritePos, kEmptyMd5Hex, 32);
        flatResult->setNoCopy(row, StringView(hexWritePos, 32));
        hexWritePos += 32;
      });
      return;
    }

    // Decode separator
    exec::LocalDecodedVector decodedSeparator(context);
    if (!isConstantSeparator()) {
      decodedSeparator = exec::LocalDecodedVector(context, *args[0], rows);
    }

    // Decode vectors for data args (args[1..]), following ConcatWs pattern
    std::vector<exec::LocalDecodedVector> decodedArrays;
    std::vector<DecodedVector> decodedElements;
    std::vector<exec::LocalDecodedVector> decodedStringArgs;

    std::vector<int32_t> argIsArray;
    std::vector<std::optional<std::string>> constantStrings;
    std::vector<column_index_t> argMapping;

    for (auto i = 1; i < numArgs; ++i) {
      if (args[i] && args[i]->typeKind() == TypeKind::ARRAY) {
        decodedArrays.emplace_back(context, *args[i], rows);
        auto arrayVector =
            decodedArrays.back().get()->base()->as<ArrayVector>();
        SelectivityVector nestedRows(arrayVector->elements()->size());
        decodedElements.emplace_back(*arrayVector->elements(), nestedRows);
        argIsArray.push_back(static_cast<int32_t>(decodedElements.size() - 1));
        continue;
      }
      argIsArray.push_back(-1);
      argMapping.push_back(i);
      if (!isConstantSeparator()) {
        constantStrings.push_back(std::nullopt);
        continue;
      }
      if (args[i] && args[i]->as<ConstantVector<StringView>>() &&
          !args[i]->isNullAt(0)) {
        std::ostringstream out;
        out << args[i]->as<ConstantVector<StringView>>()->valueAt(0);
        auto j = i + 1;
        for (; j < numArgs; ++j) {
          if (!args[j] || args[j]->typeKind() == TypeKind::ARRAY ||
              !args[j]->as<ConstantVector<StringView>>() ||
              args[j]->isNullAt(0)) {
            break;
          }
          out << separator_.value()
              << args[j]->as<ConstantVector<StringView>>()->valueAt(0);
        }
        constantStrings.emplace_back(out.str());
        i = j - 1;
      } else {
        constantStrings.push_back(std::nullopt);
      }
    }

    // Decode non-constant string args
    for (auto i = 0; i < static_cast<int>(constantStrings.size()); ++i) {
      if (!constantStrings[i].has_value() && argIsArray[i] < 0) {
        auto index = argMapping[0];
        int mappingIdx = 0;
        for (int k = 0; k <= i; ++k) {
          if (argIsArray[k] < 0) {
            if (mappingIdx < static_cast<int>(argMapping.size())) {
              if (k == i) {
                index = argMapping[mappingIdx];
              }
              mappingIdx++;
            }
          }
        }
        decodedStringArgs.emplace_back(context, *args[index], rows);
      }
    }

    // Single-pass feed+finish per row. Two-phase ILP finish needed 96B×N
    // partialStates and only paid off at 3+ MD5Transform; this keeps work in L1.

    // Pre-allocate string buffer for MD5 hex output (worst case: all non-NULL)
    const int numSelected = rows.countSelected();
    auto rawStringBuffer = flatResult->getRawStringBufferWithSpace(
        static_cast<size_t>(numSelected) * 32, true);
    char* hexWritePos = rawStringBuffer;

    // Reuse a single MD5Context across rows — reset() avoids reallocation
    crypto::MD5Context md5Ctx;

    rows.applyToSelected([&](auto row) {
      // NULL separator => NULL result
      if (!isConstantSeparator() && decodedSeparator->isNullAt(row)) {
        result->setNull(row, true);
        return;
      }

      md5Ctx.reset();

      const auto separator = isConstantSeparator()
          ? separator_.value()
          : decodedSeparator->valueAt<StringView>(row);

      bool isFirst = true;

      auto feedValue = [&](const char* data, size_t size) {
        if (isFirst) {
          isFirst = false;
        } else {
          md5Ctx.Add(reinterpret_cast<const unsigned char*>(separator.data()),
                     separator.size());
        }
        md5Ctx.Add(reinterpret_cast<const unsigned char*>(data), size);
      };

      int arrayIdx = 0;
      int strArgIdx = 0;

      for (auto j = 0; j < static_cast<int>(argIsArray.size()); ++j) {
        if (argIsArray[j] >= 0) {
          auto& decArr = decodedArrays[arrayIdx];
          if (decArr->isNullAt(row)) {
            arrayIdx++;
            continue;
          }
          auto indices = decArr.get()->indices();
          auto arrayVector = decArr.get()->base()->as<ArrayVector>();
          auto size = arrayVector->sizeAt(indices[row]);
          auto offset = arrayVector->offsetAt(indices[row]);

          for (auto k = 0; k < size; ++k) {
            if (!decodedElements[argIsArray[j]].isNullAt(offset + k)) {
              auto element =
                  decodedElements[argIsArray[j]].valueAt<StringView>(offset + k);
              feedValue(element.data(), element.size());
            }
          }
          arrayIdx++;
          continue;
        }

        if (constantStrings[j].has_value()) {
          feedValue(constantStrings[j]->data(), constantStrings[j]->size());
        } else {
          auto& decArg = decodedStringArgs[strArgIdx];
          if (decArg->isNullAt(row)) {
            strArgIdx++;
            continue;
          }
          auto value = decArg->valueAt<StringView>(row);
          feedValue(value.data(), value.size());
          strArgIdx++;
        }
      }

      // All data args NULL (or only skipped pieces): concat_ws returns "".
      if (isFirst) {
        std::memcpy(hexWritePos, kEmptyMd5Hex, 32);
        flatResult->setNoCopy(row, StringView(hexWritePos, 32));
        hexWritePos += 32;
        return;
      }

      // Direct finish — data is hot in L1 cache, no DRAM round-trip
      md5Ctx.FinishHex(hexWritePos);
      flatResult->setNoCopy(row, StringView(hexWritePos, 32));
      hexWritePos += 32;
    });
  }

 private:
  const std::optional<StringView> separator_;
};

} // namespace

std::vector<std::shared_ptr<exec::FunctionSignature>>
fusedMd5ConcatWsSignatures() {
  return {
      // concat_ws(sep) with no data args → "".
      exec::FunctionSignatureBuilder()
          .returnType("varchar")
          .argumentType("varchar")
          .build(),
      exec::FunctionSignatureBuilder()
          .returnType("varchar")
          .argumentType("varchar")
          .argumentType("varchar")
          .variableArity("varchar")
          .build(),
  };
}

std::shared_ptr<exec::VectorFunction> makeFusedMd5ConcatWs(
    const std::string& /*name*/,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& /*config*/) {
  std::optional<StringView> separator = std::nullopt;
  if (auto* constantVector =
          inputArgs[0].constantValue
              ? inputArgs[0].constantValue->as<ConstantVector<StringView>>()
              : nullptr) {
    if (!constantVector->isNullAt(0)) {
      separator = constantVector->valueAt(0);
    }
  }
  return std::make_shared<FusedMd5ConcatWs>(separator);
}

exec::VectorFunctionMetadata fusedMd5ConcatWsMetadata() {
  return exec::VectorFunctionMetadataBuilder()
      .defaultNullBehavior(false)
      .build();
}
core::TypedExprPtr rewriteMd5ConcatWs(
    const std::string& prefix,
    const core::TypedExprPtr& expr) {
  // Match the outer md5(...) call.
  auto md5Call = std::dynamic_pointer_cast<const core::CallTypedExpr>(expr);
  if (md5Call == nullptr || md5Call->name() != prefix + "md5") {
    return nullptr;
  }
  if (md5Call->inputs().size() != 1) {
    return nullptr;
  }

  // Drop an optional varchar→varbinary cast.
  auto child = md5Call->inputs()[0];
  if (auto cast = std::dynamic_pointer_cast<const core::CastTypedExpr>(child)) {
    child = cast->inputs()[0];
  }

  // Match the inner concat_ws(...) call.
  // concat_ws is registered without a prefix (RegisterString.cpp).
  auto concatCall = std::dynamic_pointer_cast<const core::CallTypedExpr>(child);
  if (concatCall == nullptr || concatCall->name() != "concat_ws") {
    return nullptr;
  }

  // Rewrite to fused_md5_concat_ws(sep, ...); reuse concat_ws children.
  return std::make_shared<core::CallTypedExpr>(
      VARCHAR(),  // md5 returns varchar (hex)
      concatCall->inputs(),
      prefix + "fused_md5_concat_ws");
}

} // namespace facebook::velox::functions::sparksql
