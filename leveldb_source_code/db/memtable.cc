// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/memtable.h"
#include "db/dbformat.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "util/coding.h"

namespace leveldb {
// 获取LengthPrefixed的Slice
static Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  // p+5是limit，因为Varint32最长为5
  p = GetVarint32Ptr(p, p + 5, &len);  // +5: we assume "p" is not corrupted
  return Slice(p, len);
}
// 构造函数，一开始引用为0，需要caller自己调用Ref()来增加引用
// 把传进来的InternalKeyComparator包装成内部的comparator_结构体
// 再把其和内部的arena_一起用于构造table_（skiplist的实现）
MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp),
      refs_(0),
      table_(comparator_, &arena_) {
}
// 析构函数
MemTable::~MemTable() {
  assert(refs_ == 0);
}
// 返回arena_的内存用量
size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }
// KeyComparator比较大小用于比较LookUpKey的大小
// 调用其内部的InternalKeyComparator，所以要先去掉length-prefixed
int MemTable::KeyComparator::operator()(const char* aptr, const char* bptr)
    const {
  // Internal keys are encoded as length-prefixed strings.
  // Internal keys 是encode成了length-prefixed的string（一般没有length-prefixed）
  // 其实传进来的就是lookUpKey
  Slice a = GetLengthPrefixedSlice(aptr);
  Slice b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
// 其实就是把interalKey encode成lookUpKey
// 其中scratch要足够大，来保存返回的指针所指向的lookUpKey
static const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, target.size());
  scratch->append(target.data(), target.size());
  return scratch->data();
}
// MemTableIterator用于访问MemTalbe
class MemTableIterator: public Iterator {
 public:
  // 构造函数，需要一个MemTable
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) { }
  // 标准的iterator实现
  virtual bool Valid() const { return iter_.Valid(); }
  // 通过internalKey来seek
  virtual void Seek(const Slice& k) { iter_.Seek(EncodeKey(&tmp_, k)); }
  virtual void SeekToFirst() { iter_.SeekToFirst(); }
  virtual void SeekToLast() { iter_.SeekToLast(); }
  virtual void Next() { iter_.Next(); }
  virtual void Prev() { iter_.Prev(); }
  // 返回 internalKey
  virtual Slice key() const { return GetLengthPrefixedSlice(iter_.key()); }
  // 先通过skiplistKey获取internalkey到key_slice
  // 再通过移动到key_slice的最后获取value的值
  virtual Slice value() const {
    Slice key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  virtual Status status() const { return Status::OK(); }

 private:
  // 内部是skiplist的iteator
  MemTable::Table::Iterator iter_;
  // 就是用在encode函数中的scratch，存放返回指针所值得实体
  std::string tmp_;       // For passing to EncodeKey

  // No copying allowed
  MemTableIterator(const MemTableIterator&);
  void operator=(const MemTableIterator&);
};
// 工厂函数
Iterator* MemTable::NewIterator() {
  return new MemTableIterator(&table_);
}

// 最终插入到skiplist的形式LookUpKey+ValueSize+Value
void MemTable::Add(SequenceNumber s, ValueType type,
                   const Slice& key,
                   const Slice& value) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  // 格式在上面LookUpKey+value_size+value bytes
  size_t key_size = key.size();
  size_t val_size = value.size();
  size_t internal_key_size = key_size + 8;
  const size_t encoded_len =
      VarintLength(internal_key_size) + internal_key_size +
      VarintLength(val_size) + val_size;
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);
  memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  p = EncodeVarint32(p, val_size);
  memcpy(p, value.data(), val_size);
  assert((p + val_size) - buf == encoded_len);
  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  // 就是LookupKey的所有。。。换了个名字
  Slice memkey = key.memtable_key();
  Table::Iterator iter(&table_);
  // 跳到大于等于memkey的skiplist那里
  // 妙就妙在Seek是byte比较的，所以缺了data只用lookUpKey都能用来seek
  // 得益于升序存储
  iter.Seek(memkey.data());
  if (iter.Valid()) {
    // entry format is:
    //    klength  varint32
    //    userkey  char[klength]
    //    tag      uint64
    //    vlength  varint32
    //    value    char[vlength]
    // Check that it belongs to same user key.  We do not check the
    // sequence number since the Seek() call above should have skipped
    // all entries with overly large sequence numbers.
    // 返回的结果是skiplist存的key，其中下面比较时不会比较sequence number
    // 因为，Seek会跳过所有过时的sequence number了
    const char* entry = iter.key();
    uint32_t key_length;
    // 获得internalKey
    const char* key_ptr = GetVarint32Ptr(entry, entry+5, &key_length);
    // 根据userKey来比较
    // 比较器是LookUpKey -> internalKey -> userKey
    if (comparator_.comparator.user_comparator()->Compare(
            Slice(key_ptr, key_length - 8),
            key.user_key()) == 0) {
      // Correct user key
      // userKey如果相同就根据ValueType返回
      // 解析sequence number 获取ValueType
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
      switch (static_cast<ValueType>(tag & 0xff)) {
        // kTypeValue则存skiplistKey后面的Value到value中，返回true
        case kTypeValue: {
          Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
          value->assign(v.data(), v.size());
          return true;
        }
        // kTypeDeletion则存NotFound在status中
        case kTypeDeletion:
          *s = Status::NotFound(Slice());
          return true;
      }
    }
  }
  // false代表没存到memtable过
  return false;
}

}  // namespace leveldb
