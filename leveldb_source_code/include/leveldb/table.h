// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_H_

#include <stdint.h>
#include "leveldb/iterator.h"

namespace leveldb {

class Block;
class BlockHandle;
class Footer;
struct Options;
class RandomAccessFile;
struct ReadOptions;
class TableCache;

// A Table is a sorted map from strings to strings.  Tables are
// immutable and persistent.  A Table may be safely accessed from
// multiple threads without external synchronization.
/*=============================
=            Table            =
=============================*/

// Talbe是一个有序的map<string,  string>
// 同时是不可修改的
// 多线程访问是安全的，如果在没有external synchronization的时候是
// 这里对table的访问也是通过iterator来实现的

/*=====  End of Table  ======*/


class Table {
 public:
  // Attempt to open the table that is stored in bytes [0..file_size)
  // of "file", and read the metadata entries necessary to allow
  // retrieving data from the table.
  //
  // If successful, returns ok and sets "*table" to the newly opened
  // table.  The client should delete "*table" when no longer needed.
  // If there was an error while initializing the table, sets "*table"
  // to NULL and returns a non-ok status.  Does not take ownership of
  // "*source", but the client must ensure that "source" remains live
  // for the duration of the returned table's lifetime.
  //
  // *file must remain live while this Table is in use.
  // 静态方法
  // table的持续存储室在硬盘中的
  // 这个方法可以打开存储在file中的table
  // 如果成功打开返回success的status，同时会把*table置为新开的table
  // 否则status为error，同时指针table为NULL
  static Status Open(const Options& options,
                     RandomAccessFile* file,
                     uint64_t file_size,
                     Table** table);

  ~Table();

  // Returns a new iterator over the table contents.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  // 返回一个在table上的迭代的Iterator
  // 注意：刚返回时iterator是invalid的
  // 须要使用seek相关的函数使其有效，例如：SeekToFirst
  Iterator* NewIterator(const ReadOptions&) const;

  // Given a key, return an approximate byte offset in the file where
  // the data for that key begins (or would begin if the key were
  // present in the file).  The returned value is in terms of file
  // bytes, and so includes effects like compression of the underlying data.
  // E.g., the approximate offset of the last key in the table will
  // be close to the file length.
  // 给定一个key返回这个key大概在table的offset， //为什么是大概了，现在不大了解，以后补充
  // 例如： 如果给了最后一个key，那么结果就很接近file的size
  uint64_t ApproximateOffsetOf(const Slice& key) const;

 private:
  struct Rep;
  Rep* rep_;
  // 注意table的构造函数是私有的，table只能通过静态方法打开
  explicit Table(Rep* rep) { rep_ = rep; }

  // 转换一个index iterator的value到一个可以访问block内容的iterator
  // 用在two-level-iterator中的
  static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

  // Calls (*handle_result)(arg, ...) with the entry found after a call
  // to Seek(key).  May not make such a call if filter policy says
  // that key is not present.
  // TableCache是友元
  friend class TableCache;
  // handle_result函数在entry找到后调用，但是filter 说不在的时候就不会调用
  // 这个函数会在table_cache中使用
  // 用来找table中某一个key的方法，找到了就调用其中的handle_result方法
  // 这里是抽象了的一层，因为里面可能会用到filter_policy和其他通过block来找key的手段
  Status InternalGet(
      const ReadOptions&, const Slice& key,
      void* arg,
      void (*handle_result)(void* arg, const Slice& k, const Slice& v));

  // 读footer，footer是table中最后的一部分 //现在也不大确定是meta还是footer，以后补充
  void ReadMeta(const Footer& footer);
  // 读filter，table后面貌似在filterCreate那里会append进来 
  void ReadFilter(const Slice& filter_handle_value);

  // No copying allowed
  Table(const Table&);
  void operator=(const Table&);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_H_
