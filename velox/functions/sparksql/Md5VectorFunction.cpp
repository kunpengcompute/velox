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

#ifndef VELOX_HAVE_ISAL_CRYPTO_MD5
#error "Md5VectorFunction.cpp is only compiled when VELOX_HAVE_ISAL_CRYPTO_MD5=1"
#endif

#include "velox/functions/sparksql/Md5VectorFunction.h"

#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"
#include "velox/functions/lib/string/StringImpl.h"
#include <isa-l_crypto/md5_mb.h>

namespace facebook::velox::functions::sparksql {

namespace {

// Build a lookup table for converting a byte to its two-character hex
// representation stored as (lo << 8 | hi). This enables 16-bit packed stores
// during MD5 digest formatting.
constexpr std::array<uint16_t, 256> build_hex_lut() {
    std::array<uint16_t, 256> lut{};
    const char* hex_chars = "0123456789abcdef";
    for (int i = 0; i < 256; ++i) {
        uint8_t hi_char = hex_chars[(i >> 4) & 0x0F];
        uint8_t lo_char = hex_chars[i & 0x0F];
        lut[i] = (static_cast<uint16_t>(lo_char) << 8) | static_cast<uint16_t>(hi_char);
    }
    return lut;
}

static constexpr auto HEX_LUT = build_hex_lut();
static constexpr int POOL_SIZE = 64;
static constexpr int PREFETCH_DIST = 8;

// Threshold for switching to scalar MD5. Inputs shorter than or equal to this
// value are processed by the scalar implementation to avoid multi-buffer
// overhead for very small strings.
static constexpr int SCALAR_THRESHOLD = 55;

} // namespace

void SparkMd5VectorFunction::apply(
    const SelectivityVector& rows,
    std::vector<VectorPtr>& args,
    const TypePtr& outputType,
    exec::EvalCtx& context,
    VectorPtr& result) const {

    // Decode the input string vector once and reuse across all rows.
    exec::LocalDecodedVector holder(&context); 
    auto& decoded = *holder.get();
    decoded.decode(*args[0], rows);
    auto baseVector = decoded.base()->as<SimpleVector<StringView>>();

    // Allocate flat output vector and a contiguous buffer for MD5 hex strings.
    // Each MD5 hash produces exactly 32 bytes (hex-encoded).
    BaseVector::ensureWritable(rows, outputType, context.pool(), result);
    auto resultFlat = result->asFlatVector<StringView>();
    
    BufferPtr stringBuffer = AlignedBuffer::allocate<char>(rows.end() * 32, context.pool());
    char* rawResultBuffer = stringBuffer->asMutable<char>();

    // Initialize ISA-L multi-buffer MD5 context manager.
    // All contexts are allocated from a fixed-size pool to avoid dynamic
    // allocations inside the hot loop.
    ISAL_MD5_HASH_CTX_MGR mgr;
    isal_md5_ctx_mgr_init(&mgr);
    
    std::array<ISAL_MD5_HASH_CTX, POOL_SIZE> ctxStorage;
    ISAL_MD5_HASH_CTX* freeCtxPool[POOL_SIZE];
    int freeCtxCount = POOL_SIZE;
    for (int i = 0; i < POOL_SIZE; ++i) {
        freeCtxPool[i] = &ctxStorage[i];
    }

    // Callback invoked whenever ISA-L completes an MD5 job.
    // Converts the binary digest to hex and attaches it to the output vector.
    auto processCompleted = [&](ISAL_MD5_HASH_CTX* ctx) {
        if (!ctx) return;
        vector_size_t row = static_cast<vector_size_t>(reinterpret_cast<uint64_t>(ctx->user_data));
        char* targetPos = rawResultBuffer + (row * 32);
        
        auto* targetPos16 = reinterpret_cast<uint16_t*>(targetPos);
        auto* digest = reinterpret_cast<uint8_t*>(ctx->job.result_digest);
        
        // Packed 16-bit hex conversion using the precomputed LUT.
        for (int i = 0; i < 16; ++i) {
            targetPos16[i] = HEX_LUT[digest[i]];
        }
        
        resultFlat->setNoCopy(row, StringView(targetPos, 32));
        freeCtxPool[freeCtxCount++] = ctx;
    };

    // Partition rows into scalar and vectorized buckets based on input length.
    // Short strings use a scalar MD5 implementation to reduce overhead.
    // Longer strings are grouped by size to improve multi-buffer throughput.
    std::vector<vector_size_t> scalar_rows;
    std::vector<vector_size_t> vector_buckets[4];
    
    scalar_rows.reserve(rows.countSelected());

    rows.applyToSelected([&](vector_size_t row) {
        if (decoded.isNullAt(row)) {
            resultFlat->setNull(row, true);
            return;
        }
        
        size_t len = baseVector->valueAt(decoded.index(row)).size();
        
        if (len <= SCALAR_THRESHOLD) {
            scalar_rows.push_back(row);
        } else if (len <= 119) {
            vector_buckets[0].push_back(row);
        } else if (len <= 183) {
            vector_buckets[1].push_back(row);
        } else if (len <= 247) {
            vector_buckets[2].push_back(row);
        } else {
            vector_buckets[3].push_back(row);
        }
    });

    // Scalar MD5 path.
    for (vector_size_t row : scalar_rows) {
        StringView sv = baseVector->valueAt(decoded.index(row));
        char* targetPos = rawResultBuffer + (row * 32);
        
        crypto::MD5Context md5Ctx;
        md5Ctx.Add(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
        md5Ctx.FinishHex(targetPos);
        
        resultFlat->setNoCopy(row, StringView(targetPos, 32));
    }

    // Vectorized ISA-L MD5 path.
    for (int b = 0; b < 4; ++b) {
        const auto& activeRows = vector_buckets[b];
        
        for (size_t i = 0; i < activeRows.size(); ++i) {
            vector_size_t row = activeRows[i];

            if (i + PREFETCH_DIST < activeRows.size()) {
                vector_size_t future_row = activeRows[i + PREFETCH_DIST];
                __builtin_prefetch(baseVector->valueAt(decoded.index(future_row)).data(), 0, 1);
            }

            // Safety flush: ensure at least one context is available.
            // Under normal conditions (kCtxPoolSize >= 16), this never triggers.
            if (freeCtxCount == 0) {
                ISAL_MD5_HASH_CTX* forcedCtx = nullptr;
                isal_md5_ctx_mgr_flush(&mgr, &forcedCtx);
                processCompleted(forcedCtx);
            }
            
            ISAL_MD5_HASH_CTX* currentCtx = freeCtxPool[--freeCtxCount];
            StringView sv = baseVector->valueAt(decoded.index(row));
            
            isal_hash_ctx_init(currentCtx);
            currentCtx->user_data = reinterpret_cast<void*>(static_cast<uint64_t>(row));

            ISAL_MD5_HASH_CTX* completedCtx = nullptr;
            
            isal_md5_ctx_mgr_submit(
                &mgr, currentCtx, &completedCtx, 
                sv.data(), sv.size(), ISAL_HASH_ENTIRE);

            processCompleted(completedCtx);
        }
    }

    ISAL_MD5_HASH_CTX* flushedCtx = nullptr;
    do {
        isal_md5_ctx_mgr_flush(&mgr, &flushedCtx);
        processCompleted(flushedCtx);
    } while (flushedCtx != nullptr);

    resultFlat->addStringBuffer(stringBuffer);
}

} // namespace facebook::velox::functions::sparksql