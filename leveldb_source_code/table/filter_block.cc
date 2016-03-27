// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.txt for an explanation of the filter block format.

// Generate new filter every 2KB of data
// base的大小是2KB
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;
// FilterBlockBuilder的构造函数，接受一个FilterPolicy
FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {
}
// startBlock是在tableBuilder中被调用的
// 调用时间是当tableBuilder开始要为dataBlock构建filterBlock的时候
// 在这里可以看出两个连续的datablock中，如果第一个的datablock的size小于kFilterBase
// 则会等待第二个datablock一起来构建filterblock

// 而如果大于的话就会等待下一个block的都来构建，或者是直接用finish来构建（之前要addkey）
// ps：有可能filter_offset里面的元素对应的filter是空的
//     例子：第一个构建的block的size >= 2*kFilterBase的时候
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // 根据block_offset来计算出filter_index
  uint64_t filter_index = (block_offset / kFilterBase);
  // filter_index要大于等于filter_offsets_.size()
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    // 一直生成Filter直到filter_offsets_.size() == filter_index
    GenerateFilter();
  }
}
// 简单的Addkey逻辑
// 先把键在keys_的开始位置放到start_中
// 之后加入到keys_中
void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}
// Finish函数
Slice FilterBlockBuilder::Finish() {
  // 看下start_还有没有值
  // 有的话就继续生成filrer
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  const uint32_t array_offset = result_.size();
  // 把一个个的filter_offsets_编码到result后面
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }
  // 把最后的filter_offsets_数组的开始位置也记录下来，这里的offset是相对于result_的开始的
  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  // 如过start_.size()==0 则直接进入快速通道，把result.size()压入filter_offsets_就行
  // 这里处理的是空的start_的构建，原因看StartBlock函数
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }
  // 这里的目的是把现有的key都弄到tmp_keys_中，用于CreateFilter的创建
  // 最终结果append到result上
  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation //用于简化计算的，加多个长度在后面就可以后面-前面得出长度。之前只记录了开始位置，所以最后一个元素长度计算不方便
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i+1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  // 记录filter的开始位置
  filter_offsets_.push_back(result_.size());
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);
  // 重置
  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}
// 构造FilterBlockReader：其中contents是filterBlock的开始
FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy),
      data_(NULL),
      offset_(NULL),
      num_(0),
      base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  // 最后1byte是base_lg_
  base_lg_ = contents[n-1];
  // 最后5到最后2 byte是存着offset_ = offset array的开始的值
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  data_ = contents.data();
  // offset_ = offset array的开始
  offset_ = data_ + last_word;
  // offset array的长度
  num_ = (n - 5 - last_word) / 4;
}
// 根据提供的data_block的offset 和key 来进行KeyMayMatch
bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  // 得到filter_index 相当于 block_offset/2kB 默认，这里的计算之所能行，很大原因是得益于StartBlock的while循环的作用
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // 根据filter_index来找到对应的filter block
    uint32_t start = DecodeFixed32(offset_ + index*4);
    uint32_t limit = DecodeFixed32(offset_ + index*4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  // 错误当作找到来处理，因为默认的bloom filter对于不存在的key一定要返回false，而这里不确定多以返回true
  return true;  // Errors are treated as potential matches
}

}
