// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// An iterator yields a sequence of key/value pairs from a source.
// The following class defines the interface.  Multiple implementations
// are provided by this library.  In particular, iterators are provided
// to access the contents of a Table or a DB.
//
// Multiple threads can invoke const methods on an Iterator without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Iterator must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_ITERATOR_H_
#define STORAGE_LEVELDB_INCLUDE_ITERATOR_H_

#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {
/*===================================
=            iterator迭代器          =
===================================*/

// 迭代器，leveldb中打量使用了迭代器来计划设计
// 这是提供了interface，方法是virtual的
// 内部实现的迭代器有很多，如twoleveliterator。。。
// 下面的source指的是iterator对应的迭代容器

/*=====  End of iterator迭代器  ======*/

class Iterator {
 public:
  // 构造函数、析构函数
  Iterator();
  virtual ~Iterator();

  // An iterator is either positioned at a key/value pair, or
  // not valid.  This method returns true iff the iterator is valid.
  // 迭代器是否有效，即指向key／value对
  virtual bool Valid() const = 0;

  // Position at the first key in the source.  The iterator is Valid()
  // after this call iff the source is not empty.
  // 迭代器跳到对应source的首位
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source.  The iterator is
  // Valid() after this call iff the source is not empty.
  // 迭代器跳到对应source的末尾
  virtual void SeekToLast() = 0;

  // Position at the first key in the source that is at or past target.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  // 跳到一个target的位置，或者后一个位置
  // 之所以有后一个位置是因为seek是根据Slice的大小来seek的
  // target可以不存在对应source当中
  virtual void Seek(const Slice& target) = 0;

  // Moves to the next entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  // 下一位
  virtual void Next() = 0;

  // Moves to the previous entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the first entry in source.
  // REQUIRES: Valid()
  // 前一位
  virtual void Prev() = 0;

  // Return the key for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  // 返回当前entry的key
  virtual Slice key() const = 0;

  // Return the value for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  // 返回当前entry的value
  virtual Slice value() const = 0;

  // If an error has occurred, return it.  Else return an ok status.
  // 状态返回，这里又看到了抽象的好处，直接包涵对象的好处，简单，易模块化
  virtual Status status() const = 0;

  // Clients are allowed to register function/arg1/arg2 triples that
  // will be invoked when this iterator is destroyed.
  //
  // Note that unlike all of the preceding methods, this method is
  // not abstract and therefore clients should not override it.
  // 当迭代器destroy的时候会调用这个hook函数
  // 注意，这里不是virtual的方法所以，继承的时候不能override它
  typedef void (*CleanupFunction)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  // cleanup的结构体定义
  // 在这里看到可能有多个hook函数
  // 还有参数传入类型均是void*，这经常会这样用，要熟悉
  struct Cleanup {
    CleanupFunction function;
    void* arg1;
    void* arg2;
    Cleanup* next;
  };
  Cleanup cleanup_;
  // 迭代器不允许copy
  // No copying allowed
  Iterator(const Iterator&);
  void operator=(const Iterator&);
};

// Return an empty iterator (yields nothing).
// 返回一个空的迭代器
// extern关键字的使用，从别的文件引用函数过来
// 应该是头文件的关系把相关实现这样弄走
extern Iterator* NewEmptyIterator();

// Return an empty iterator with the specified status.
// 返回一个空的，带有特定状态的迭代器
extern Iterator* NewErrorIterator(const Status& status);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_ITERATOR_H_
