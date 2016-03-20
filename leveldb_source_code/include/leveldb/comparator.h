// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
#define STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_

#include <string>

namespace leveldb {

class Slice;

// A Comparator object provides a total order across slices that are
// used as keys in an sstable or a database.  A Comparator implementation
// must be thread-safe since leveldb may invoke its methods concurrently
// from multiple threads.

/*==================================
=            comparator            =
==================================*/
// 比较器的interface
// leveldb内部的各种排序均依靠Comparator
// 同时者应该是线程安全的因为这会在多个线程里调用。
// thread-safe自行google。


/*=====  End of comparator  ======*/

class Comparator {
 public:
  // 析构函数
  virtual ~Comparator();

  // Three-way comparison.  Returns value:
  //   < 0 iff "a" < "b",
  //   == 0 iff "a" == "b",
  //   > 0 iff "a" > "b"
  // 构造函数，必须满足上面的标准
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // The name of the comparator.  Used to check for comparator
  // mismatches (i.e., a DB created with one comparator is
  // accessed using a different comparator.
  //
  // The client of this package should switch to a new name whenever
  // the comparator implementation changes in a way that will cause
  // the relative ordering of any two keys to change.
  //
  // Names starting with "leveldb." are reserved and should not be used
  // by any clients of this package.
  // Comparator的名字
  // leveldb打开时会需要导入一个comparator（或者默认的）
  // 其中名字是其中的一道防线
  // 因为leveldb中储存与Comparator关系很大，所以要重视着个Name
  // ps： ‘leveldb.’是leveldb保留的名字前缀，请不要使用
  virtual const char* Name() const = 0;

  // Advanced functions: these are used to reduce the space requirements
  // for internal data structures like index blocks.

  // If *start < limit, changes *start to a short string in [start,limit).
  // Simple comparator implementations may return with *start unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 找出区间[start, limit]之间的最短的分割Slice
  // 主要用以后面sstable中的block的endkey，节约空间
  // start会改变，如果找到不同于start的
  // 简单什么都不做的也是正确的实现
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const = 0;

  // Changes *key to a short string >= *key.
  // Simple comparator implementations may return with *key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 改变key为new_key满足new_key >= key， 同时new_key长度尽量短
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering.  The result remains the property of this module and
// must not be deleted.
// 内部Slice的比较，基于memcmp，所以叫bytewisecomparator
// builtin的Comparator，BytewiseComparator
extern const Comparator* BytewiseComparator();

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
