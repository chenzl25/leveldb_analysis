// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_CRC32C_H_
#define STORAGE_LEVELDB_UTIL_CRC32C_H_

#include <stddef.h>
#include <stdint.h>

/*==============================
=            crc32c            =
==============================*/

// 循环冗余校验

/*=====  End of crc32c  ======*/


namespace leveldb {
namespace crc32c {

// Return the crc32c of concat(A, data[0,n-1]) where init_crc is the
// crc32c of some string A.  Extend() is often used to maintain the
// crc32c of a stream of data.
// 返回concat(A, data[0, n-1])的crc32的值
// 其中init_crc是A的crc32c的值，data是后加进去的数据
// 具体实现没去详细看，以后有空再补充 
extern uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Return the crc32c of data[0,n-1]
// 返回长度为n的data的crc32c值
inline uint32_t Value(const char* data, size_t n) {
  return Extend(0, data, n);
}

static const uint32_t kMaskDelta = 0xa282ead8ul;

// Return a masked representation of crc.
//
// Motivation: it is problematic to compute the CRC of a string that
// contains embedded CRCs.  Therefore we recommend that CRCs stored
// somewhere (e.g., in files) should be masked before being stored.
// 没看懂应为什么原因要加mask，以后补充
inline uint32_t Mask(uint32_t crc) {
  // Rotate right by 15 bits and add a constant.
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

// Return the crc whose masked representation is masked_crc.
// 对应的unmask
inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace crc32c
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_CRC32C_H_
