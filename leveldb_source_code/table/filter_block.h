// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A filter block is stored near the end of a Table file.  It contains
// filters (e.g., bloom filters) for all data blocks in the table combined
// into a single filter block.

#ifndef STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "leveldb/slice.h"
#include "util/hash.h"

/*====================================
=            filter_block            =
====================================*/

// filter_block是存储在sstable后面的
// 集合了所用blocks的key/value值的“另一种表示集合”
// 默认用布隆过滤器（在这情款下，“另一种表示集合”就是bit vector）
// 用来减少IO

/*=====  End of filter_block  ======*/


namespace leveldb {

class FilterPolicy;

// A FilterBlockBuilder is used to construct all of the filters for a
// particular Table.  It generates a single string which is stored as
// a special block in the Table.
//
// The sequence of calls to FilterBlockBuilder must match the regexp:
//      (StartBlock AddKey*)* Finish
// 用来构建FilterBlock的helper类
// 一个sstable可能有多个filter
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy*);

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  // 所有keys的串接
  std::string keys_;              // Flattened key contents
  // 对上面keys_每个key的开始位置
  std::vector<size_t> start_;     // Starting index in keys_ of each key
  // Filter data计算到现在的结果
  // 还记得吗，bloom filter的CreateFilter函数是不把存放结果的string直接覆盖，而是append的
  // 所以可以多个filters一起生成结果，而只需要记住对应的开始点就好，下面的filter_offsets_就是干这个
  // 具体请看filter_policy.h和bloom.c
  std::string result_;            // Filter data computed so far
  // policy_->CreateFilter()的第一个参数
  std::vector<Slice> tmp_keys_;   // policy_->CreateFilter() argument
  // 记住result_中每个filter的开始点
  std::vector<uint32_t> filter_offsets_;

  // No copying allowed
  FilterBlockBuilder(const FilterBlockBuilder&);
  void operator=(const FilterBlockBuilder&);
};

class FilterBlockReader {
 public:
 // REQUIRES: "contents" and *policy must stay live while *this is live.
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);
  bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  const FilterPolicy* policy_;
  // filter data的指针（在block的开始）
  const char* data_;    // Pointer to filter data (at block-start)
  // filter data可能是分多段的，所以要有offset来区分
  const char* offset_;  // Pointer to beginning of offset array (at block-end)
  // 有多少offset_，相当于数组长度
  size_t num_;          // Number of entries in offset array
  // 编码的参数（详情请看filter_block.cc的kFilterBaseLg）
  size_t base_lg_;      // Encoding parameter (see kFilterBaseLg in .cc file)
};

}

#endif  // STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
