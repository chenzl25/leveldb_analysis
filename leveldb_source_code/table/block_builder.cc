// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// BlockBuilder generates blocks where keys are prefix-compressed:
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
//
// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

// 上面的英文解释讲得很清楚
// blockBuilder是用来让我们构造前缀压缩过的key的block
// 该block的结构就是一系列的key/value，格式在上面
// 后面有一系列的restart points，具体格式看上面

#include "table/block_builder.h"

#include <algorithm>
#include <assert.h>
#include "leveldb/comparator.h"
#include "leveldb/table_builder.h"
#include "util/coding.h"

namespace leveldb {
BlockBuilder::BlockBuilder(const Options* options)
    : options_(options),
      restarts_(),
      counter_(0),
      finished_(false) {
  // option其中的一个作用就是用来控制restart的间隔
  assert(options->block_restart_interval >= 1);
  // 第一个restart point的位置是0
  restarts_.push_back(0);       // First restart point is at offset 0
}
// 恢复到刚创建的样子
void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);       // First restart point is at offset 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}
// 估计大小值得计算方法，其中buffer_是raw data的大小 + restart points的大小 + restat ponit的size占得大小
size_t BlockBuilder::CurrentSizeEstimate() const {
  return (buffer_.size() +                        // Raw data buffer
          restarts_.size() * sizeof(uint32_t) +   // Restart array
          sizeof(uint32_t));                      // Restart array length
}
// 结束build，直接把restart ponit都encode到buffer_（raw data）后面，再加上restart ponit的size
Slice BlockBuilder::Finish() {
  // Append restart array
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  PutFixed32(&buffer_, restarts_.size());
  finished_ = true;
  return Slice(buffer_);
}
// 加入key/value的方法
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  // 构建last_key_
  Slice last_key_piece(last_key_);
  // 必须满足的条件：1，没调用Finish. 2。当前count<=restart的区间. 3.新加入的键要比最后加的一个键要大
  assert(!finished_);
  assert(counter_ <= options_->block_restart_interval);
  assert(buffer_.empty() // No values yet?
         || options_->comparator->Compare(key, last_key_piece) > 0);

  size_t shared = 0;
  // 现在counter_ < restart间隔
  if (counter_ < options_->block_restart_interval) {
    // See how much sharing to do with previous string
    // 前缀的压缩是现在的要加入的key和上一次加入的key(即last_key_piece)的共享前缀
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    // 统计有多少前缀是相同的
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression
    // 重新开始restart，所以记录restart的点
    restarts_.push_back(buffer_.size());
    // 重新开始计算是第几个前缀压缩值得
    counter_ = 0;
  }
  // 下面英文注释太美，不忍心加入中文进去。。。
  // 未共享的长度
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  // 修改last_key_，这里感觉好高效
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  // 增加当前的counter
  counter_++;
}

}  // namespace leveldb
