// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace leveldb {
// table的关键成员
struct Table::Rep {
  ~Rep() {
    delete filter;
    delete [] filter_data;
    delete index_block;
  }

  Options options;
  Status status;
  // table的file
  RandomAccessFile* file;
  // table在cache中的id
  uint64_t cache_id;
  // table的FilterBlockReader
  FilterBlockReader* filter;
  // table的filter_data，在data_block后面，也可以理解为metadata
  const char* filter_data;
  // metaindex_handle存在于footer中
  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  Block* index_block;
};
// 打开file到table中
// size要等于file的大小
// 用RandomAccessFile来读取，这样就可以随机地读到sstable中的各个结构
// open中会读取footer，meta，filter缓存起来
Status Table::Open(const Options& options,
                   RandomAccessFile* file,
                   uint64_t size,
                   Table** table) {
  *table = NULL;
  // 当size连Footer的kEncodedLength都不够的时候就出错了
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }
  // 用来存放Footer的buf
  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  // 读取file中Footer
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  // 解码footer_input到footer中
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Read the index block
  // 读取index block的内容
  BlockContents contents;
  Block* index_block = NULL;
  if (s.ok()) {
    ReadOptions opt;
    // 如果是激进的检查策略的会，那么就要检查校验和了
    if (options.paranoid_checks) {
      opt.verify_checksums = true;
    }
    // 读取index block到content中，这个函数在format.cc中
    // footer.index_handle有index_block的在file中的offset和size，所以能读出来
    s = ReadBlock(file, opt, footer.index_handle(), &contents);
    if (s.ok()) {
      // 如果成功就构造index block
      index_block = new Block(contents);
    }
  }

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    // 看options是否要进行block cache
    // 是的后就对用其中的NewId()方法
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = NULL;
    rep->filter = NULL;
    // 创建Table
    *table = new Table(rep);
    // 读出Meta_block
    (*table)->ReadMeta(footer);
  } else {
    // 读取index_block错误就delete掉index_block
    if (index_block) delete index_block;
  }

  return s;
}

void Table::ReadMeta(const Footer& footer) {
  // 如果没有说用什么filter_policy就直接返回
  if (rep_->options.filter_policy == NULL) {
    return;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  // 激进的校验
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  // 读取metaindex_block，失败就返回
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  // 成功读取就构建metaindex-Block
  Block* meta = new Block(contents);
  // 创建iterator来访问meta
  Iterator* iter = meta->NewIterator(BytewiseComparator());
  // 构造用来进行访问的key
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  // 利用key来进行seek
  iter->Seek(key);
  // 读取filter
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());
  }
  delete iter;
  delete meta;
}

void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  // filter_handle从filter_handle_value解码
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // We might want to unify with ReadBlock() if we start
  // requiring checksum verification in Table::Open.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents block;
  // 读取filter_block
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  if (block.heap_allocated) {
    // 存起filter_data
    // 是堆产生的，我们要delete掉
    rep_->filter_data = block.data.data();     // Will need to delete later
  }
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() {
  delete rep_;
}
// 删除block的静态函数
static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}
// 删除cacheblock的静态函数
static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}
// 减少block应用的静态函数
static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
// 转换一个index iterator的value到一个可以访问block内容的iterator
// 因为对table的访问是通过two-level-iterator来实现的，所以要提供这个函数
// 
Iterator* Table::BlockReader(void* arg,
                             const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = NULL;
  Cache::Handle* cache_handle = NULL;
  // index_value就是raw的BlockHandle，需要解码
  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != NULL) {
      char cache_key_buffer[16];
      // 根据 table->rep_->cache_id + handle.offset()来在block_cache中查找一个block
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer+8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      // 返回的是一个handle
      cache_handle = block_cache->Lookup(key);
      // 如果非空就获得block
      if (cache_handle != NULL) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        // 如果空的话就是读取出来先，再插入到block_cache中
        // 插入的键就是table->rep_->cache_id + handle.offset()
        // 这里就用到了block_cache的NewId方法
        // 因为table->rep_->cache_id就是由这个方法生成的
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          // 还要判断读出来的block可不可以cache
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(
                key, block, block->size(), &DeleteCachedBlock);
          }
        }
      }
    } else {
      // 不缓存block的做法
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }
  // 获得block后就可以进行iterator的获取了
  Iterator* iter;
  if (block != NULL) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == NULL) {
      // 如果没有block_cache的话就直接删除block，当iterator不用的时候
      iter->RegisterCleanup(&DeleteBlock, block, NULL);
    } else {
      //否则就是减少block_cache上的引用 
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}
// Table的NewIterator就是：1.提供迭代index_block的迭代器index_iter
//  2.再根据index_iter的value（raw block_handle）来转化成block内容的iterator的方法
// 生成TwoLevelIterator
Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

// 获取table中的某一个key来，如果找到就进行saver函数（key，value）调用
// 其中有用filter来进行是否在file中，来避免不必要的IO
// 当然如果用户实现了自己的block缓存也会有更好的效果

Status Table::InternalGet(const ReadOptions& options, const Slice& k,
                          void* arg,
                          void (*saver)(void*, const Slice&, const Slice&)) {
  // 这个方法也用到了BlockReader（转index_iter value到block_iter）方法来实现对key的查找
  // 自己构造index_iterator再跳到对应的block，再到key/value
  Status s;
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  // index_iter是可以Seek block的 key的
  iiter->Seek(k);
  if (iiter->Valid()) {
    // 获得对应的block
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    // 这里filter终于出现了
    // 如果要查找的key在filter中查询返回false的话就不在硬盘中所以就不用找了
    if (filter != NULL &&
        handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // Not found
    } else {
      // 毅力要用到blockReader来读取相应的block
      // 其中如果有block_cache的话会进行缓存
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        // 在block中找到对应的key/value后会调用saver函数
        (*saver)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      // 删除block_iter，在BlockReader中block_iter注册了没block-cache就销毁block
      // 或者有cache就减少引用的方法
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  // 删除index-block-iter
  delete iiter;
  return s;
}


uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      // 返回那个block量级别的大小
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
