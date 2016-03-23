// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Log format information shared by reader and writer.
// See ../doc/log_format.txt for more detail.

// Log文件的format信息，被log_reader log_writer共享

#ifndef STORAGE_LEVELDB_DB_LOG_FORMAT_H_
#define STORAGE_LEVELDB_DB_LOG_FORMAT_H_

// 补充：block的整体结构
// [record0]
// [record1]
// [.......]
// [recordN]
// [trailer]

// trailer是当block小于record的header部分的大小（7bytes）时会填充全0，不使用

// record结构
// [checksum (4 bytes), length (2 bytes), type (1 byte), data (data_length)]

// record可以横跨多个block

namespace leveldb {
namespace log {
// RecordType
// kZeroType：为preallocated files 保留用的
// kFullType：完整地存在一个block中
/* 如果一个record横跨多个block */
// kFirstType： 是record在第一个block的部分
// kMiddleType： 是record在非第一个和非最后一个clock的部分
// kLastType 是record在最后一个block的部分
enum RecordType {
  // Zero is reserved for preallocated files
  kZeroType = 0,

  kFullType = 1,

  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};
// 最大的类型
static const int kMaxRecordType = kLastType;
// block的大小
static const int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
// block头的大小checksum (4 bytes), length (2 bytes), type (1 byte).
static const int kHeaderSize = 4 + 2 + 1;

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_FORMAT_H_
