// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include <string>
#include "leveldb/db.h"
#include "db/dbformat.h"
#include "db/skiplist.h"
#include "util/arena.h"

namespace leveldb {

class InternalKeyComparator;
class Mutex;
class MemTableIterator;

/*================================
=            memTable            =
================================*/

// memTable是在内存中的相当于一个缓存，当写入的操作是，会先把key/value
// 放到memTalbe，等到满了以后再dump到disk中（期间会先变成immutalbe memTalbe）
// 内部用skiplist实现

/*=====  End of memTable  ======*/


class MemTable {
 public:
  // MemTables are reference counted.  The initial reference count
  // is zero and the caller must call Ref() at least once.
  // MemTalbe是引用计数的，初始为0，调用者要调用至少一次
  explicit MemTable(const InternalKeyComparator& comparator);

  // Increase reference count.
  // 增加引用计数
  void Ref() { ++refs_; }

  // Drop reference count.  Delete if no more references exist.
  // 减少引用计数，当等于0时delete
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  // Returns an estimate of the number of bytes of data in use by this
  // data structure. It is safe to call when MemTable is being modified.
  // 返回大概用了多少的内存
  size_t ApproximateMemoryUsage();

  // Return an iterator that yields the contents of the memtable.
  //
  // The caller must ensure that the underlying MemTable remains live
  // while the returned iterator is live.  The keys returned by this
  // iterator are internal keys encoded by AppendInternalKey in the
  // db/format.{h,cc} module.
  // 返回用于遍历memTable的iterator，其中它返回的key是internalKey
  Iterator* NewIterator();

  // Add an entry into memtable that maps key to value at the
  // specified sequence number and with the specified type.
  // Typically value will be empty if type==kTypeDeletion.
  // 插入一个值到memTable中，参数用于构成internalKey和value，
  // 最后组成skiplist存的形式， LookUpKey+ValueSize+Value
  // 其中如果ValueType==kTypeDeletion时value会是NULL
  void Add(SequenceNumber seq, ValueType type,
           const Slice& key,
           const Slice& value);

  // If memtable contains a value for key, store it in *value and return true.
  // If memtable contains a deletion for key, store a NotFound() error
  // in *status and return true.
  // Else, return false.
  // 通过lookUpKey
  // 如果memtable有a value for key就把它对应的值存到value中，并返回true
  // 如果memtalbe有a deletion for key就把NotFound() error存到status中，并返回true
  // 否则返回false
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  // 析构函数是私有的，只有通过引用计数来销毁
  ~MemTable();  // Private since only Unref() should be used to delete it
  // KeyComparator时基于InternalKeyComparator
  // 实际上是LookUpKeyComparator
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) { }
    int operator()(const char* a, const char* b) const;
  };
  // 迭代器，友元
  friend class MemTableIterator;
  friend class MemTableBackwardIterator;
  // table就是skiplist
  typedef SkipList<const char*, KeyComparator> Table;

  KeyComparator comparator_;
  int refs_;
  // 用到table_构造是传入的arena_
  Arena arena_;
  Table table_;

  // No copying allowed
  MemTable(const MemTable&);
  void operator=(const MemTable&);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
