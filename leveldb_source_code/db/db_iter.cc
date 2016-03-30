// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"

#include "db/filename.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace leveldb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.
class DBIter: public Iterator {
 public:
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  // 这里有两个方向kForward和kReverse
  // (1) 如果是kForward，那么internal iterator刚好指向那个user-key的entry
  // (2) 如果是kReverse，那么internal iterator指向那个user-key的entry的前一个
  enum Direction {
    kForward,
    kReverse
  };
  // DBIter的构造函数
  DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
         uint32_t seed)
      : db_(db),
        user_comparator_(cmp),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        rnd_(seed),
        bytes_counter_(RandomPeriod()) {
  }
  // 析构函数
  virtual ~DBIter() {
    delete iter_;
  }
  // 返回是否valid
  virtual bool Valid() const { return valid_; }
  // 返回key，根据方向，实现内部的internal iter指向有所不同
  // 还有要ExtractUserKey提取出来
  virtual Slice key() const {
    assert(valid_);
    return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
  }
  // 返回value，根据方向，实现内部的internal iter指向有所不同
  virtual Slice value() const {
    assert(valid_);
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }
  // 返回状态
  virtual Status status() const {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  virtual void Next();
  virtual void Prev();
  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);
  // 把key存在dst中
  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }
  // 如果saved_value_占用内存过多就会清理下
  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  // Pick next gap with average value of config::kReadBytesPeriod.
  // 用来取用的读键周期
  // 获取一个[0, 2*config::kReadBytesPeriod-1]的均匀分布
  // 期望就是config::kReadBytesPeriod
  ssize_t RandomPeriod() {
    return rnd_.Uniform(2*config::kReadBytesPeriod);
  }
  // db数据路径
  DBImpl* db_;
  // user-key的比较器
  const Comparator* const user_comparator_;
  // 指向MergingIterator的指针
  Iterator* const iter_;
  // 通过sequence_来控制遍历数据的时间点
  // 如果指定了SnapShot则赋值为Snapshot::sequenceNumber
  // 这样就能只遍历之前的数据了
  // 否则，赋值为VersionSet::last_sequence_就可以遍历出所有的数据了
  SequenceNumber const sequence_;
  // 遍历过程中的data
  Status status_;
  // 当方向为kReverse时用来返回的key和value值
  // 这里为了在kReverse的时候可以处理相同的key和删除的key的逻辑，所采取的做法
  // 如果方向为kForward是则直接用iter的指向的就entry就可以了
  std::string saved_key_;     // == current key when direction_==kReverse
  std::string saved_value_;   // == current raw value when direction_==kReverse
  // 方向
  Direction direction_;
  // 是否有效
  bool valid_;
  // 用来产生随机数的
  Random rnd_;
  // 这个用来记录read了多少byte了，用于取样的
  ssize_t bytes_counter_;

  // No copying allowed
  DBIter(const DBIter&);
  void operator=(const DBIter&);
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();
  ssize_t n = k.size() + iter_->value().size();
  bytes_counter_ -= n;
  // 在这里进行了取样
  while (bytes_counter_ < 0) {
    bytes_counter_ += RandomPeriod();
    db_->RecordReadSample(k);
  }
  // 将iter指向的internal-key 解析到ikey里
  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    // 如果之前是kReverse，则要改为kForward，再next一步
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already contains the key to skip past.
    // saved_key_在kReverse已经刚好指向了要跳过的key了
  } else {
    // Store in saved_key_ the current key so we skip it below.
    // 把iter_->key 存到saved_key_中，用于下面我们遇到同样的key可以跳过
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
  }
  // 在方向kForward的情况下next
  FindNextUserEntry(true, &saved_key_);
}
// skipping用于给调用者决定是否第一个不是kTypeDeletion的值就返回
// skipping同样就有在遇到kTypeDeletion后，跳过相同的键的功能
void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  // Loop until we hit an acceptable entry to yield
  // 一直循环直到找到合适的key
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    // 将当前的iter指向的key解析出来
    // 还有ikey.sequence <= sequence_，用来实现snapshot
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      switch (ikey.type) {
        // 如果标记了删除
        // 则记到skip中，同时标记skipping
        case kTypeDeletion:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          // 把ikey.user_key存到skip中
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        // 如果找到了键，就看下skipping是否为true，还有找到的key是否大于给定的键skip
        // 如果是==的情况就可能是已经被删除的key了
        case kTypeValue:
          if (skipping &&
              user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // Entry hidden
          } else {
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

void DBIter::Prev() {
  assert(valid_);
  // 如果方向是kForward，则要把iter移动到刚好小于当前key的位置
  // 并改变方向为kReverse
  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                    saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }
  // 在方向kReverse的情况下prev
  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);
  // value_type初始化为kTypeDeletion
  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        // 第一次do的时候绝对不会进入这里，因为value_type初始化为kTypeDeletion
        // 如果第二次或以后，发现了value_type != kTypeDeletion
        // 而起到了ikey.user_key< saved_key_就可以返回了
        // 因为到此为止解决了delete的问题
        // 什么问题？ 例如
        // V:'k'->5,V:'key'->4, D:'key', V:'key'->2, V:'key'->3
        // 这样的话我们必须到了V:‘k’->5才能判断V:'key'->4是存在的
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          // We encountered a non-deleted value in entries for previous keys,
          break;
        }
        // value_type的值得改变
        value_type = ikey.type;
        // 如果value_type是kTypeDeletion，就清空saved_key_
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          // 否则，这有可能是delete后的值，所以还不能直接返回
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
          }
          SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // End
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}
// 下面的seek函数内部都用到了FindNextUserEntry，FindPrevUserEntry的函数，
// 这是为了解决kTypeDeletion的问题而设计的
void DBIter::Seek(const Slice& target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  AppendInternalKey(
      &saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

}  // anonymous namespace
// 工厂函数
Iterator* NewDBIterator(
    DBImpl* db,
    const Comparator* user_key_comparator,
    Iterator* internal_iter,
    SequenceNumber sequence,
    uint32_t seed) {
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace leveldb
