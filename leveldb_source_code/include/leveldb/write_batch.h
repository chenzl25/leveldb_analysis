// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.

/*=============================
=            batch            =
=============================*/

// batch提供了原子性操作
// 可以把一系列的动作捆绑再一次性做
// 对应于数据库中的compaction（事务）

/*=====  End of batch  ======*/


#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include <string>
#include "leveldb/status.h"

namespace leveldb {

class Slice;

class WriteBatch {
 public:
  // 构造析构函数
  WriteBatch();
  ~WriteBatch();

  // Store the mapping "key->value" in the database.
  // 插入或更新
  void Put(const Slice& key, const Slice& value);

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  // 删除
  void Delete(const Slice& key);

  // Clear all updates buffered in this batch.
  // 清除buffer，应该是对应的操作全部清除
  void Clear();

  // Support for iterating over the contents of a batch.
  // 内部类Handler，感叹自己都没用过c++的内部类
  // 处理函数，用于迭代batch内容
  class Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

 private:
  // 友元，内部的实现WriteBatchInternal
  friend class WriteBatchInternal;
  // 移步write_batch.cc去看rep_的对应格式
  std::string rep_;  // See comment in write_batch.cc for the format of rep_

  // Intentionally copyable
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
