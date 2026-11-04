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

#include "velox/exec/RowContainer.h"
#include "velox/exec/TaperHashTable.h"
#include "velox/functions/lib/aggregates/SumAggregateBase.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <sstream>

#include <folly/ScopeGuard.h>

using namespace facebook::velox;

constexpr int64_t kKB = 1024L;
constexpr int64_t kMB = 1024L * kKB;
constexpr int64_t kGB = 1024L * kMB;

namespace {

char*& rowFromData(char* data) {
  return *reinterpret_cast<char**>(data);
}

class FileMapper {
 public:
  FileMapper() : content_{nullptr}, fileSize_{0} {}

  bool open(const std::string& fileName) {
    if (content_) {
      return false;
    }

    int fd = ::open(fileName.c_str(), O_RDONLY);
    if (fd == -1) {
      perror("open");
      return false;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) {
      perror("fstat");
      close(fd);
      return false;
    }

    fileSize_ = sb.st_size;
    content_ = mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (content_ == MAP_FAILED) {
      perror("mmap");
      close(fd);
      return false;
    }

    close(fd);
    return true;
  }

  template <typename T>
  T* getContent() {
    return reinterpret_cast<T*>(content_);
  }

  size_t getSize() const {
    return fileSize_;
  }

  ~FileMapper() {
    if (content_ && munmap(content_, fileSize_) == -1) {
      perror("munmap");
    }
  }

 private:
  void* content_;
  size_t fileSize_;
};

std::string formatDuration(std::chrono::system_clock::duration dur) {
  std::ostringstream oss;
  auto durMicro = std::chrono::duration_cast<std::chrono::microseconds>(dur);
  auto durMilli = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
  oss << durMilli.count() << " milli seconds (" << durMicro.count()
      << " micro seconds)";
  return oss.str();
}

void testEmplace(
    memory::MemoryPool& aggPool,
    const std::vector<TypePtr>& keyTypes,
    const std::vector<exec::Accumulator>& accs,
    const uint64_t* hashVals,
    size_t numHashVals) {
  auto pool = aggPool.addLeafChild("leaf");
  SCOPE_EXIT {
    pool.reset();
  };
  exec::RowContainer rowContainer{
      keyTypes,
      true,
      accs,
      std::vector<TypePtr>{},
      false, // hasNext
      false, // isJoinBuild
      false, // hasProbedFlag
      false, // hasNormalizedKey
      pool.get()};

  exec::TaperFlatHashTable<uint64_t, sizeof(uint64_t) + sizeof(void*), true>
      table{*pool};

  constexpr size_t kBatchSize = 8192;
  std::vector<vector_size_t> newGroups;
  std::vector<char*> groups{kBatchSize};
  std::vector<vector_size_t> updateIndices;

  newGroups.reserve(kBatchSize);
  updateIndices.reserve(kBatchSize);
  auto initRow = [&](uint32_t rowIdx, char* data) {
    auto* row = rowContainer.newRow();
    rowFromData(data) = row;
    newGroups.push_back(rowIdx);
    return row;
  };

  for (size_t i = 0; i < numHashVals; i += kBatchSize) {
    auto numRows = std::min(kBatchSize, numHashVals - i);
    newGroups.clear();
    updateIndices.clear();
    table.emplaceBatch(
        hashVals + i,
        numRows,
        [&](uint32_t) { return false; },
        [&](uint32_t rowIdx, char* data) { initRow(rowIdx, data); },
        [&](uint32_t rowIdx, char* data, bool initFlag) {
          groups[rowIdx] = rowFromData(data);
          if (!initFlag) {
            updateIndices.push_back((rowIdx));
          }
        });
  }
  std::cout << "hash table capacity: " << table.capacity()
            << ", size: " << table.size() << std::endl;
}

} // namespace

int main(int argc, char** argv) {
  FileMapper hashValsMapper;
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: <cmd> <hash-values-path> [iteration-num]" << std::endl;
    return 1;
  }
  if (!hashValsMapper.open(argv[1])) {
    throw std::runtime_error("Open hash keys file");
  }
  auto* hashVals = hashValsMapper.getContent<uint64_t>();
  auto numHashVals = hashValsMapper.getSize() / sizeof(uint64_t);

  size_t iterationNum = 1;
  if (argc == 3) {
    iterationNum = std::stoi(argv[2]);
  }

  memory::MemoryManager::Options options;
  options.alignment = 64;
  options.allocatorCapacity = 8 * kGB;
  memory::MemoryManager manager{options};
  auto root = manager.addRootPool("Taper", 4 * kGB);
  auto aggPool = root->addAggregateChild("agg");

  functions::aggregate::SumAggregateBase<double, double, double, false> fun{
      DOUBLE()};
  std::vector<exec::Accumulator> accs;
  accs.emplace_back(&fun, DOUBLE());
  std::vector<TypePtr> keyTypes{INTEGER(), INTEGER()};

  auto start = std::chrono::system_clock::now();
  for (size_t i = 0; i < iterationNum; i++) {
    testEmplace(*aggPool, keyTypes, accs, hashVals, numHashVals);
  }
  auto duration = std::chrono::system_clock::now() - start;

  std::cout << "insert duration: " << formatDuration(duration) << std::endl;

  return 0;
}
