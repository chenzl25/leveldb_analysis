// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_
#define STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_

#include "db/dbformat.h"
#include "leveldb/write_batch.h"
/*==========================================
=            WriteBatchInternal            =
==========================================*/

// WriteBatchInternal是WriteBatch的内部实现
// WriteBatchInternal里面有更复杂的东西，所以进一步封装
// WriteBatchInternal是WriteBatch的友元
// 所以可以直接操作WriteBatch，确实是通过WriteBatchInternal操作WriteBatch
// 因为WriteBatchInternal方法都是静态的，可以被WriteBatch调用

/*=====  End of WriteBatchInternal  ======*/

namespace leveldb {

class MemTable;

// WriteBatchInternal provides static methods for manipulating a
// WriteBatch that we don't want in the public WriteBatch interface.
class WriteBatchInternal {
 public:
  // Return the number of entries in the batch.
  // 返回batch中entries的数目
  static int Count(const WriteBatch* batch);

  // Set the count for the number of entries in the batch.
  // 设置batch中entries的数目
  static void SetCount(WriteBatch* batch, int n);

  // Return the sequence number for the start of this batch.
  // 返回batch最开始的sequence number（英文说得更明白）
  static SequenceNumber Sequence(const WriteBatch* batch);

  // Store the specified number as the sequence number for the start of
  // this batch.
  // 设置batch最开始的sequence number
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);
  // 返回Contents就是batch的rep_，所以要看rep_的格式了
  static Slice Contents(const WriteBatch* batch) {
    return Slice(batch->rep_);
  }
  // 返回ByteSize
  static size_t ByteSize(const WriteBatch* batch) {
    return batch->rep_.size();
  }
  // 设置contents
  static void SetContents(WriteBatch* batch, const Slice& contents);
  // 将batch插入到contents
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);
  // 将src的append到dst中
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace leveldb


#endif  // STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_
