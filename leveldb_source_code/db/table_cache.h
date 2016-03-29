// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Thread-safe (provides internal synchronization)
// table_cache线程安全的，提供内部的同步机制（在cache中可以有Mutex体现出来）
#ifndef STORAGE_LEVELDB_DB_TABLE_CACHE_H_
#define STORAGE_LEVELDB_DB_TABLE_CACHE_H_

#include <string>
#include <stdint.h>
#include "db/dbformat.h"
#include "leveldb/cache.h"
#include "leveldb/table.h"
#include "port/port.h"

/*===================================
=            table_cache            =
===================================*/

// 在table_cache中，我们来详细的讲述下过程
// 构造TableCache：首先接受一个数据库的名字dbname，和options，
// 要缓存的容量entries，其实缓存的是一个TableAndFile（定义在table_cache.cc中）指针
// TableAndFile指针保存有file和table
// 在这个类中用到的方法会对table进行缓存如NewIterator，Get（可以对一个table的一个key进行操作）
// Evict则是清理某个table的缓存
// 私有方法FindTable则是会根据是否缓存了数据来进行再缓存中找table，还是进行IO操作

/*=====  End of table_cache  ======*/


namespace leveldb {

class Env;

class TableCache {
 public:
  TableCache(const std::string& dbname, const Options* options, int entries);
  ~TableCache();

  // Return an iterator for the specified file number (the corresponding
  // file length must be exactly "file_size" bytes).  If "tableptr" is
  // non-NULL, also sets "*tableptr" to point to the Table object
  // underlying the returned iterator, or NULL if no Table object underlies
  // the returned iterator.  The returned "*tableptr" object is owned by
  // the cache and should not be deleted, and is valid for as long as the
  // returned iterator is live.
  // 返回一个iterator对于一个给定number的file（sstable），如果tableptr != NULL
  // 则tableptr赋值为Table的指针
  // 其中file_size一定要等于file的大小
  Iterator* NewIterator(const ReadOptions& options,
                        uint64_t file_number,
                        uint64_t file_size,
                        Table** tableptr = NULL);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value).
  // 如果在file里找到一个interal key则调用(*handle_result)(arg, found_key, found_value).
  Status Get(const ReadOptions& options,
             uint64_t file_number,
             uint64_t file_size,
             const Slice& k,
             void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // Evict any entry for the specified file number
  // 删除cache中所有与file_number相关的entry
  void Evict(uint64_t file_number);

 private:
  Env* const env_;
  const std::string dbname_;
  const Options* options_;
  Cache* cache_;

  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_TABLE_CACHE_H_
