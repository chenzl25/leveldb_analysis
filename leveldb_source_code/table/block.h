// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <stddef.h>
#include <stdint.h>
#include "leveldb/iterator.h"

/*=============================
=            block            =
=============================*/

// 用于sstable中的block

/*=====  End of block  ======*/


namespace leveldb {

struct BlockContents;
class Comparator;

class Block {
 public:
  // Initialize the block with the specified contents.
  // 用BlockContents来构造一个block
  explicit Block(const BlockContents& contents);

  ~Block();
  // 返回block的size
  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  // restart的数目，其中restart是为了记录哪里开始压缩前缀使用的
  uint32_t NumRestarts() const;
  // data的开始
  const char* data_;
  // block的大小
  size_t size_;
  // block中restart在data中的offset
  uint32_t restart_offset_;     // Offset in data_ of restart array
  bool owned_;                  // Block owns data_[]

  // No copying allowed
  Block(const Block&);
  void operator=(const Block&);

  class Iter;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
