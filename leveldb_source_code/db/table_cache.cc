// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};
// 在cache中引用为0是会销毁table file
static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}
// 当iterator销毁的时候就在cache中减少table的引用
static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}
// TableCache的构造函数
// 接受env，dbname，option，默认的分片LRUCache
TableCache::TableCache(const std::string& dbname,
                       const Options* options,
                       int entries)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {
}
// 析构函数，会删除对应的分片LRUCache
TableCache::~TableCache() {
  delete cache_;
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  // 对file_number编好码，到cache中寻找对应table
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  // 如果找不到
  if (*handle == NULL) {
    // 构造table的名字
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = NULL;
    Table* table = NULL;
    // 构造NewRandomAccessFile来读取table的file
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
      // 打开file到table
      s = Table::Open(*options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == NULL);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      // 进行缓存，以及销毁的处理函数
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      // 1代表charge等于1，占用空间为1，这里指缓存指针
      // 实体会在注册的DeleteEntry中删除
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}
// 返回一个对应file的iterator
Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != NULL) {
    *tableptr = NULL;
  }
  // 在cache中查找table，存句柄到handle中
  Cache::Handle* handle = NULL;
  Status s = FindTable(file_number, file_size, &handle);
  // 找不到出错处理
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  // 将对应handle的值获取出来，再拿回table
  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  // 获得table的iterator
  Iterator* result = table->NewIterator(options);
  // 注册iterator销毁时的Cleanup函数
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  // 如果tableptr非空，这令其指向获得的table
  if (tableptr != NULL) {
    *tableptr = table;
  }
  return result;
}
// 如果在file里找到一个interal key则调用(*saver)(arg, found_key, found_value).
Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,
                       void* arg,
                       void (*saver)(void*, const Slice&, const Slice&)) {
  Cache::Handle* handle = NULL;
  // 获取file到handle中
  Status s = FindTable(file_number, file_size, &handle);
  // 如果找到
  if (s.ok()) {
    // 根据handle获得对应的table
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 在table中找key
    s = t->InternalGet(options, k, arg, saver);
    // 减少引用次数
    cache_->Release(handle);
  }
  return s;
}
// 在cache中删除掉key为file_number的缓存
void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
