// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_

#include <stdint.h>
#include "leveldb/options.h"
#include "leveldb/status.h"

/*====================================
=            TableBuilder            =
====================================*/

// TableBuilder是创建Table的interface
// table是immutable、sorted的key／value  的 map

/*=====  End of TableBuilder  ======*/

namespace leveldb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

class TableBuilder {
 public:
  // Create a builder that will store the contents of the table it is
  // building in *file.  Does not close the file.  It is up to the
  // caller to close the file after calling Finish().
  // 构造函数：TableBuilder把正在构造的table存进与file挂钩
  // 还有构建时候的一些配置options
  // 注意，当调用Finish后再把file clode掉
  TableBuilder(const Options& options, WritableFile* file);

  // REQUIRES: Either Finish() or Abandon() has been called.
  // 析构函数：须要在调用Finish 或者 Abandon()后才使用
  ~TableBuilder();

  // Change the options used by this builder.  Note: only some of the
  // option fields can be changed after construction.  If a field is
  // not allowed to change dynamically and its value in the structure
  // passed to the constructor is different from its value in the
  // structure passed to this method, this method will return an error
  // without changing any fields.
  // 在构建过程中动态地改变options
  // 注意：如果修改在动态过程中不能修改的属性
  // 该方法会返回一个error的status，同时不进行修改
  Status ChangeOptions(const Options& options);

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  // 往table中进行add key／value值
  // 其中key要比之前的key要大，即after
  // 注意： table是有序的
  void Add(const Slice& key, const Slice& value);

  // Advanced operation: flush any buffered key/value pairs to file.
  // Can be used to ensure that two adjacent entries never live in
  // the same data block.  Most clients should not need to use this method.
  // REQUIRES: Finish(), Abandon() have not been called
  // 用于强行flush相应的block到file中
  // 从而可以保证相邻的key不在同一个block中
  // 一般不使用
  void Flush();

  // Return non-ok iff some error has been detected.
  // table_builder的status
  Status status() const;

  // Finish building the table.  Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  // 完成table的building，停止对file的使用
  Status Finish();

  // Indicate that the contents of this builder should be abandoned.  Stops
  // using the file passed to the constructor after this function returns.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  // 抛弃掉这个builder里所用的内容，同时停止对file的使用
  void Abandon();

  // Number of calls to Add() so far.
  // 调用了Add方法的次数，也就是entry的个数
  uint64_t NumEntries() const;

  // Size of the file generated so far.  If invoked after a successful
  // Finish() call, returns the size of the final generated file.
  // 返回目前的file的size，如果Finish已近被调用则，返回最终生成file的size
  uint64_t FileSize() const;

 private:
  // 返回status是否ok
  bool ok() const { return status().ok(); }
  // 写入正常的block
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  // 写入Raw的block
  void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

  struct Rep;
  Rep* rep_;

  // No copying allowed
  TableBuilder(const TableBuilder&);
  void operator=(const TableBuilder&);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
