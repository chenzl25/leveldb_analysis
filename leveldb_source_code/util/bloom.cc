// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

#include "leveldb/slice.h"
#include "util/hash.h"
/*=========================================
=            BloomFilterPolicy            =
=========================================*/

// BloomFilterPolicy的实现

/*=====  End of BloomFilterPolicy  ======*/


namespace leveldb {

namespace {
// Bloom的hash，使用了内部的hash函数，第三位参数是种子seed
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}
// 集成abstract类
class BloomFilterPolicy : public FilterPolicy {
 private:
  size_t bits_per_key_;
  size_t k_; //这个是改变后的bits_per_key_，见构造函数

 public:
  // 构造函数
  explicit BloomFilterPolicy(int bits_per_key)
      : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    // 每个键分得bit的平均值，衡量负载
    // 会面会倾向于简单bits_per_key，来减少probing的开销
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    // 当然不可能然k小于1，否则盘错率都会到100%了，1也可能出现满了的情况，当hash全部均匀
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }
  // filterPolicy的名字
  virtual const char* Name() const {
    return "leveldb.BuiltinBloomFilter2";
  }
  // 创建filter，高技术含量
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const {
    // Compute bloom filter size (in both bits and bytes)
    // 计算所需要的bit数目，依据bits_per_key_
    size_t bits = n * bits_per_key_;

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    // 最小的bits，因为bit更小的话，盘错率极其高
    if (bits < 64) bits = 64;
    // 计算那么多bit要多少byte
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;
    // 改变dst的长度，这里不会改变原来dst的内容，只会在后面直接添加
    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_));  // Remember # of probes in filter //把_k也加上
    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      // 只用了double-hashing，有论文分析的理论支撑
      // 一般来说bloom依靠多个hash函数来映射一个key
      // 但在这个实现中只要依靠一个BloomHash的内部实现，就能做到很好

      // hash那个key
      uint32_t h = BloomHash(keys[i]);
      // 计算delta
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      // 相当于hash了k_次 
      for (size_t j = 0; j < k_; j++) {
        // 讲hash到的值映射到bit array中
        const uint32_t bitpos = h % bits;
        array[bitpos/8] |= (1 << (bitpos % 8));
        // 下一个hash值仅仅有原来的hash值加上delta得到
        h += delta;
      }
    }
  }
  // 看key match不match了
  virtual bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    // len-1是因为最后一位是_k
    const size_t bits = (len - 1) * 8;

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    const size_t k = array[len-1];
    // 其实上面保证了k最多为30
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos/8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }
};
}
// 工厂方法
const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
