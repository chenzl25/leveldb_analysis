// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "leveldb/cache.h"
#include "port/port.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {
}
/*=============================
=            cache            =
=============================*/

// leveldb cache的实现
// 默认策略LRU
// 有LRUHandle
// HandleTable 内置hash table
// LRUCache 是 ShardedLRUCache的其中一个
// ShardedLRUCache 有多个ShardedLRUCache构成，为了多线程或进程访问的效率，进行了cache分区

/*=====  End of cache  ======*/

namespace {

// LRU cache implementation

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
// LRUHandle的key是copy的value是指针
// key要实例保存，可能是外面的数据修改了值或者删除了以后会导致cache的问题（这个问题有待讨论）
struct LRUHandle {
  void* value;                                //value值
  void (*deleter)(const Slice&, void* value); // 删除用的函数
  LRUHandle* next_hash;                       // 用于HandleTable的单向链表
  LRUHandle* next;                            // 用于LRUCache的双向链表
  LRUHandle* prev;                            // 用于LRUCache的双向链表
  size_t charge;      // TODO(opt): Only allow uint32_t?
  size_t key_length;                          // 键长
  uint32_t refs;                              // 引用数
  uint32_t hash;      // Hash of key(); used for fast sharding and comparisons
  // 这只是key的开始
  // 通过后面的malloc
  // LRUHandle* e = reinterpret_cast<LRUHandle*>(
  // malloc(sizeof(LRUHandle)-1 + key.size()));
  // 来超出结构体访问key_data
  // 这样好处有变长，申请内存连续，不用申请了结构体的指针后又要再申请key的变长指针
  // free掉的时候也很简单
  char key_data[1];   // Beginning of key

  Slice key() const {
    // For cheaper lookups, we allow a temporary Handle object
    // to store a pointer to a key in "value".
    // 为了cheaper lookups,我们允许一个暂时的HandleObject存一个key在value中
    if (next == this) {
      return *(reinterpret_cast<Slice*>(value));
    } else {
      return Slice(key_data, key_length);
    }
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
// 内部的hashTable， 因为减去了很多不必要的porting hacks，所以速度回快一点
// 使用开链法
// 同时把hash值给外包出去了，自己不提供hash的管理
class HandleTable {
 public:
  // 构造与析构函数
  HandleTable() : length_(0), elems_(0), list_(NULL) { Resize(); }
  ~HandleTable() { delete[] list_; }
  // 根据key和hash值来查找handle
  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }
  // 插入一个handle
  LRUHandle* Insert(LRUHandle* h) {
    // 见下面FindPointer
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    // old == NULL 即是不存在，不存在就插入，存在就修改
    h->next_hash = (old == NULL ? NULL : old->next_hash);
    *ptr = h;
    //原来不存在，所以elems_+1
    if (old == NULL) {
      ++elems_;
      // 如果elems_大于length，resize
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        // 应为这个表要达到O（1），平均每条链元素个数小于1
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != NULL) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  // table 就是 一个bucket数组，每个bucket有相同的hash值和一条链
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;  //buckets

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // FindPointer返回一个hash值对应的链中key对应的slot的指针
  // 如果不存在这样的key返回一个指向对应list中最后一位的指针
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    // hash通过取与&来定位bucket的位置
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != NULL &&
           ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }
  // resize策略
  void Resize() {
    // 初始新的length为
    uint32_t new_length = 4;
    // 一直double，直到大于elems
    while (new_length < elems_) {
      new_length *= 2;
    }
    // 新的buckets 
    LRUHandle** new_list = new LRUHandle*[new_length];
    // 初始化0（NULL）
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    // 对原来的每一条链进行重新的排位
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      // 构建每一条新的链
      while (h != NULL) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
// 一个shardedLRUCache的一部分
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  // 可以动态改变
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  // 像是cache.h中的方法，但是多了个hash值
  Cache::Handle* Insert(const Slice& key, uint32_t hash,
                        void* value, size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  size_t TotalCharge() const {
    // 用到了MutexLock，在“mutexlock.h”可以查到，这是个wrapper
    // 创建的时候就锁住了线程，在函数结束后，就会释放
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* e);
  void Unref(LRUHandle* e);

  // Initialized before use.
  size_t capacity_;

  // mutex_ protects the following state.
  // 每个cache都有自己的mutex来保护state
  mutable port::Mutex mutex_;
  size_t usage_;

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // 一个dummy的head，为了设计的优雅。lru.prev是最新的entry，lru.next是最久的entry
  LRUHandle lru_;
  // hash table，为LRUCache访问从双向链表的O(n)减到O(1)
  HandleTable table_;
};

LRUCache::LRUCache()
    : usage_(0) {
  // Make empty circular linked list
  // 初始化的时候lrc_这样的
  lru_.next = &lru_;
  lru_.prev = &lru_;
}
// 析构的时候，全部调用Unref方法，应为其中会调用对应的deleter方法
LRUCache::~LRUCache() {
  for (LRUHandle* e = lru_.next; e != &lru_; ) {
    LRUHandle* next = e->next;
    // 如果用户没释放掉handle就会造成error
    assert(e->refs == 1);  // Error if caller has an unreleased handle
    Unref(e);
    e = next;
  }
}
// 当e->refs大于0时-1，否则，调用LRUHandle的deleter方法
// 同事free掉LRUHandle
void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs <= 0) {
    usage_ -= e->charge;
    (*e->deleter)(e->key(), e->value);
    free(e);
  }
}
// 链表的remove操作
void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}
// 链表的append，遵循lru_的prev最新原则
void LRUCache::LRU_Append(LRUHandle* e) {
  // Make "e" newest entry by inserting just before lru_
  e->next = &lru_;
  e->prev = lru_.prev;
  e->prev->next = e;
  e->next->prev = e;
}
// 查是否存在，通过HandleTable
Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  // 先锁住线程，析构后会自动释放
  MutexLock l(&mutex_);
  // 查找
  LRUHandle* e = table_.Lookup(key, hash);
  // 找到就刷到最新
  if (e != NULL) {
    e->refs++;
    LRU_Remove(e);
    LRU_Append(e);
  }
  // 通过reinterpret_cast改变e是指针的xxx（不知道怎样说）
  // 因为Cache::Handle是个空的结构体
  // 从而实现信息隐藏
  // 下次用户通过这个Handle创进来我们再修改就行了
  return reinterpret_cast<Cache::Handle*>(e);
}
// 释放Handle，其实对应内部的就是应用值的减少1
void LRUCache::Release(Cache::Handle* handle) {
  // 先上锁
  MutexLock l(&mutex_);
  // 再reinterpret_cast来Unref
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(
    const Slice& key, uint32_t hash, void* value, size_t charge,
    void (*deleter)(const Slice& key, void* value)) {
  // 先上锁
  MutexLock l(&mutex_);
  // reinterpret_cast重新解析指针
  // 这里用了点trick，见LRUHandle定义的解析
  // insert即使是更新也会创造出一个新的LRUHandle
  LRUHandle* e = reinterpret_cast<LRUHandle*>(
      malloc(sizeof(LRUHandle)-1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  // 引用初始为2，一个是来自LRUCache的，另外一个是返回的handle的
  e->refs = 2;  // One from LRUCache, one for the returned handle
  // copy key
  memcpy(e->key_data, key.data(), key.size());
  // 放到最新的位置
  LRU_Append(e);
  // charge是大概占用的估计值（用户传入，通常是key的length）， 加到usage去
  usage_ += charge;
  LRUHandle* old = table_.Insert(e);
  if (old != NULL) {
    // 如果是更新的话，就把旧的LRUHandle从cache的list移除，同时降低引用值
    // 注意，这里没有把LRUHandle free掉
    // 因为可能还有别的用户引用到了它，所以等到最后一个用户release的时候才会free掉
    LRU_Remove(old);
    Unref(old);
  }
  // 如果用的空间大于容量的，就会把最久的数据去掉，不会马上free掉，会等待最后一个用户relaease才会free掉
  // 还有就是usage_只有在Unref是才会减小，
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    LRU_Remove(old);
    table_.Remove(old->key(), old->hash);
    Unref(old);
  }

  return reinterpret_cast<Cache::Handle*>(e);
}
// 在LRUCache的双向链表中remove掉key对应的Handle
// 但实际上还不一定free掉
void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Remove(key, hash);
  if (e != NULL) {
    LRU_Remove(e);
    Unref(e);
  }
}
// 去掉所有没有被引用到的cache，来限制内存
void LRUCache::Prune() {
  MutexLock l(&mutex_);
  for (LRUHandle* e = lru_.next; e != &lru_; ) {
    LRUHandle* next = e->next;
    if (e->refs == 1) {
      table_.Remove(e->key(), e->hash);
      LRU_Remove(e);
      Unref(e);
    }
    e = next;
  }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits; //16
// 管理多个LRUCache的类
class ShardedLRUCache : public Cache {
 private:
  // 数组管理，mutex管理
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    // “util/hash.h”的hash函数， 0是种子seed
    return Hash(s.data(), s.size(), 0);
  }
  // 将hash值得头kNumShardBits位映射到出去
  static uint32_t Shard(uint32_t hash) {
    return hash >> (32 - kNumShardBits);
  }

 public:
  // 根据ShardedLRUCache的capacity来等分给各自的LRUCache（取整）
  explicit ShardedLRUCache(size_t capacity)
      : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  virtual ~ShardedLRUCache() { }
  // 插入操作，先找到分片，再insert
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  // 查找操作，先找到分片，再lookup
  virtual Handle* Lookup(const Slice& key) {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  // release操作，先找到分片，再release
  virtual void Release(Handle* handle) {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  // erase操作，先找到分片，再erase
  virtual void Erase(const Slice& key) {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  // 返回handle的value，即是insert时的value
  virtual void* Value(Handle* handle) {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  virtual uint64_t NewId() {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  // 全部Prune
  virtual void Prune() {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  // 总共的估计值
  virtual size_t TotalCharge() const {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace
// 工程函数返回ShardedLRUCache
Cache* NewLRUCache(size_t capacity) {
  return new ShardedLRUCache(capacity);
}

}  // namespace leveldb
