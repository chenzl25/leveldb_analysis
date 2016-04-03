// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_READER_H_
#define STORAGE_LEVELDB_DB_LOG_READER_H_

#include <stdint.h>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class SequentialFile;

namespace log {

// 读log的抽象

class Reader {
 public:
  // Interface for reporting errors.
  // 一个用来报告错误的类
  // 还未完全实现是个虚基类，所以要用的时候要先继承这个Reporter，写出Corruption的方法
  // 再传递进Reader中的构造函数中使用
  class Reporter {
   public:
    virtual ~Reporter();

    // Some corruption was detected.  "size" is the approximate number
    // of bytes dropped due to the corruption.
    // bytes是大概出错时候logfile的位置
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Create a reader that will return log records from "*file".
  // "*file" must remain live while this Reader is in use.
  //
  // If "reporter" is non-NULL, it is notified whenever some data is
  // dropped due to a detected corruption.  "*reporter" must remain
  // live while this Reader is in use.
  //
  // If "checksum" is true, verify checksums if available.
  //
  // The Reader will start reading at the first record located at physical
  // position >= initial_offset within the file.
  // checksum用来决定是否检查checksum
  // initial_offset可能是跳过一些额外的信息
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset);

  ~Reader();

  // Read the next record into *record.  Returns true if read
  // successfully, false if we hit end of the input.  May use
  // "*scratch" as temporary storage.  The contents filled in *record
  // will only be valid until the next mutating operation on this
  // reader or the next mutation to *scratch.
  // 读record，返回到Slice* record中（仅仅指针），其中scratch是临时存的空间
  bool ReadRecord(Slice* record, std::string* scratch);

  // Returns the physical offset of the last record returned by ReadRecord.
  //
  // Undefined before the first call to ReadRecord.
  // 返回最后一个被ReadRecord读出来是的physical offset，如果没调用过ReadRecord，结果undefined
  uint64_t LastRecordOffset();

 private:
  SequentialFile* const file_;
  Reporter* const reporter_;
  bool const checksum_;
  // 实际上buffer的实体
  char* const backing_store_;
  // 指向buffer区域的指针
  // 由于进行IO的时候为了提高效率，一个block一个block的读
  // 所以当block中的还没处理完的时候会先暂时存在 backing_store_ 和buffer_一起构建的buffer中
  Slice buffer_;
  // 当最后一次Read()返回的block的大小 < kBlockSize的时候标志着eof
  bool eof_;   // Last Read() indicated EOF by returning < kBlockSize

  // Offset of the last record returned by ReadRecord.
  // 返回最后一个被ReadRecord读出来是的physical offset
  // 如果没调用过ReadRecord，结果undefined（实现中貌似是0）
  uint64_t last_record_offset_;
  // Offset of the first location past the end of buffer_.
  // buffer_最后一位的下一位（英文居然是the first location past the end of buffer_.！）
  uint64_t end_of_buffer_offset_;

  // Offset at which to start looking for the first record to return
  // 传入的initial_offset_，应该从这里开始读第一条record记录
  uint64_t const initial_offset_;

  // True if we are resynchronizing after a seek (initial_offset_ > 0). In
  // particular, a run of kMiddleType and kLastType records can be silently
  // skipped in this mode
  // initial_offset_ = 0是可以不管这个变量
  // 当initial_offset_ > 0时初始化为true
  // 看log_reader.cc的实现来了解其作用
  // 其中，可能在initial_offset_中读到的第一个record不是FirstType的
  // 就需要不断移动到下一个FirstType中，所以中途的Middle 和 Last Type 跳过了
  bool resyncing_;

  // Extend record types with the following special values
  // 扩展record type
  enum {
    kEof = kMaxRecordType + 1,
    // Returned whenever we find an invalid physical record.
    // Currently there are three situations in which this happens:
    // * The record has an invalid CRC (ReadPhysicalRecord reports a drop)
    // * The record is a 0-length record (No drop is reported)
    // * The record is below constructor's initial_offset (No drop is reported)
    kBadRecord = kMaxRecordType + 2
  };

  // Skips all blocks that are completely before "initial_offset_".
  //
  // Returns true on success. Handles reporting.
  // 跳到initial_offset_所属的block
  bool SkipToInitialBlock();

  // Return type, or one of the preceding special values
  // 读record
  unsigned int ReadPhysicalRecord(Slice* result);

  // Reports dropped bytes to the reporter.
  // buffer_ must be updated to remove the dropped bytes prior to invocation.
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  // No copying allowed
  Reader(const Reader&);
  void operator=(const Reader&);
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_READER_H_
