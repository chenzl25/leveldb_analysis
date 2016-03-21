// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DBFORMAT_H_
#define STORAGE_LEVELDB_DB_DBFORMAT_H_

#include <stdio.h>
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
#include "leveldb/table_builder.h"
#include "util/coding.h"
#include "util/logging.h"

/*================================
=            dbformat            =
================================*/

// 有关db的一些格式，以及一些配置信息

/*=====  End of dbformat  ======*/


namespace leveldb {

// Grouping of constants.  We may want to make some of these
// parameters set via options.
// 与options一起控制的常量
namespace config {
// level的最大层数
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
// level-0层中如果大于这个数值就会触发compact操作，默认是4
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
// 如果level-0层中的file数目大于次（默认是8），减慢写的操作
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
// 如果level-0层中的file数目大于次（默认是12），会阻塞到compact memtable完成，
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
// memtable dump成sstable时，允许推向的最高的level
// 相关流程参照VersionSet::PickLevelForMemTableOuput()
// 选择2是因为这样可以避免0=>1层compact时的一些开销和some expensive manifest file operations.
// 而不选取更大的层数是因为，这会造成disk的空间的浪费，如果相同的key重复的写的时候
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
// 暂时不理解，以后补充（可能是一个读了多少bytes后会停止一段时间的一个统计周期）
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
// 用于区分究竟是添加操作好事删除操作，因为leveldb把删除操作先用插入一个
// 删除的记号来实现着先，在后面的compact的时候才会真正的删除
// ps：不要修改小面的值，因为它们会被写入disk
enum ValueType {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1
};
// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
// 没看到实际上是怎样用的，以后补充
// 或者看下面的类
static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
// SequenceNumber，leveldb的每次更新操作（put/delete）都会拥有一个版本，
// 由SequnceNumber来标识，整个db有一个全局值保存着当前使用到的SequenceNumber
// key的排序，compact，snapshot都依赖于它
// 储存结构[0...56...63]=>[SequenceNumber‘+ValueType]
// 注意我们用SequenceNumber’  加了（’） 去区分内部与整体SequenceNumber，
// 内部的SequenceNumber‘只占用56bits，底部的ValueType占了8bits
static const SequenceNumber kMaxSequenceNumber =
    ((0x1ull << 56) - 1);
// 内部用的ParsedInternalKey，三部分组成，用户的user_key，SequenceNumber’（54bits），ValueType（8bits）
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() { }  // Intentionally left uninitialized (for speed)
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) { }
  std::string DebugString() const;
};

// Return the length of the encoding of "key".
// 返回InternalKey的length，就是从ParsedInternalKey 可以看出
// 是用户的user_key+8bytes（SequenceNumber‘+ValueType）
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
// 将ParsedInternalKey append到result中
extern void AppendInternalKey(std::string* result,
                              const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
//
// On error, returns false, leaves "*result" in an undefined state.
// 将internal_key 解析到result里
extern bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result);

// Returns the user key portion of an internal key.
// 从internal_key中返回UserKey
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}
// 从internal_key中返回valueType
inline ValueType ExtractValueType(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  const size_t n = internal_key.size();
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  unsigned char c = num & 0xff;
  return static_cast<ValueType>(c);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
// 首先说明下leveldb得internal_key的储存是升序的
// InternalKey的Comparator，其中user_key 部分用user-comparator先比较（默认是byte字典序）
// 然后是SequenceNumber'的比较，小的然后是SequenceNumber会更大
// 因为SequenceNumber的生成是由小到大的，
// 所以后生成的key的SequenceNumber（数字上大，逻辑上小）会排在相同的user_key前面（因为升序）
// 所以新的key会被更快的找到
class InternalKeyComparator : public Comparator {
 private:
  const Comparator* user_comparator_;
 public:
  // 接受user_key的comparator
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) { }
  virtual const char* Name() const;
  virtual int Compare(const Slice& a, const Slice& b) const;
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const;
  virtual void FindShortSuccessor(std::string* key) const;

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Filter policy wrapper that converts from internal keys to user keys
// InternalFilterPolicy其实只是user_policy_的一层封装，内部是用user_policy_的
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;
 public:
  // 接受user_policy_
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) { }
  virtual const char* Name() const;
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const;
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const;
};

// Modules in this directory should keep internal keys wrapped inside
// the following class instead of plain strings so that we do not
// incorrectly use string comparisons instead of an InternalKeyComparator.
// InternalKey的类,用一个类而不是纯string来管理InternalKey
// 防止误用string comparisons 代替  InternalKey Coparator
class InternalKey {
 private:
  std::string rep_;
 public:
  InternalKey() { }   // Leave rep_ as empty to indicate it is invalid
  // 构造函数，将user_key，SequenceNumber，ValueType三部分组合成InternalKey
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }
  // 这里的Encode和Decode其实只是简单的复制返回，只是抽象了个层次
  void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }
  // 返回user_key
  Slice user_key() const { return ExtractUserKey(rep_); }
  // 将ParsedInternalKey转换成user_key
  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};
// 比较的实现
inline int InternalKeyComparator::Compare(
    const InternalKey& a, const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}
// 从internal_key中解析成ParsedInternalKey
inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  // 这个数字是编码过的，也就是说AppendInternalKey会encode数字
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  unsigned char c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<unsigned char>(kTypeValue));
}

// A helper class useful for DBImpl::Get()
// lookup_key就是internal_key前面多了个internal_key的长度（varint32）
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  // 构造函数
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  // memtable的key就是lookupkey
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  // internal的key就是不要前面的internal_key的length
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  // user的key就是不要前面的length和后面的SequenceNumber
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32               <-- start_  //internal_key 的length
  //    userkey  char[klength]          <-- kstart_
  //    tag      uint64
  //                                    <-- end_
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  const char* start_;
  const char* kstart_;
  const char* end_;
  // 对于小的键，会要这个预先定制好的内存
  char space_[200];      // Avoid allocation for short keys

  // No copying allowed
  LookupKey(const LookupKey&);
  void operator=(const LookupKey&);
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DBFORMAT_H_
