// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Simple hash function used for internal data structures

#ifndef STORAGE_LEVELDB_UTIL_HASH_H_
#define STORAGE_LEVELDB_UTIL_HASH_H_

#include <stddef.h>
#include <stdint.h>

namespace leveldb {
// 一个简单的内部用的hash函数，根据传入的长度为n的data，加上种子seed返回一个32int值
extern uint32_t Hash(const char* data, size_t n, uint32_t seed);

}

#endif  // STORAGE_LEVELDB_UTIL_HASH_H_
