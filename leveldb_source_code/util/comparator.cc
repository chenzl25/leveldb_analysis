// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <stdint.h>
#include "leveldb/comparator.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"

namespace leveldb {
// 比较器的实现

Comparator::~Comparator() { }

namespace {
// 内置的compartor
class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() { }
  // 内置的comparator名称
  virtual const char* Name() const {
    return "leveldb.BytewiseComparator";
  }
  // Slice的比较，基于memcmp，所以叫bytewisecomparator
  virtual int Compare(const Slice& a, const Slice& b) const {
    return a.compare(b);
  }
  // 找物理长度最短的分割字符串
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const {
    // Find length of common prefix
    // 先找出最短的字符串的长度，再找第一个不同的字符
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }
    // 如果第一个不同的字符已近是start的结尾了就直接是最短的了
    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other
    } else {
      // 如果可以再diff_byte中+1后不溢出且小于limit的对应byte就修改返回
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        (*start)[diff_index]++;
        start->resize(diff_index + 1);
        assert(Compare(*start, limit) < 0);
      }
    }
  }
  // 找到最短的后继key
  virtual void FindShortSuccessor(std::string* key) const {
    // Find first character that can be incremented
    // 找到第一位可以+1的byte+1就可以了
    // 如果没有，返回自己
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i+1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
  }
};
}  // namespace

static port::OnceType once = LEVELDB_ONCE_INIT;
static const Comparator* bytewise;
// 初始化
static void InitModule() {
  bytewise = new BytewiseComparatorImpl;
}
// 用到了port里面定义的初始化函数，默认基于pthread库的init
const Comparator* BytewiseComparator() {
  port::InitOnce(&once, InitModule);
  return bytewise;
}

}  // namespace leveldb
