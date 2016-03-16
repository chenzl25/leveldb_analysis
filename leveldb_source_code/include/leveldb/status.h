// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Status encapsulates the result of an operation.  It may indicate success,
// or it may indicate an error with an associated error message.
//
// Multiple threads can invoke const methods on a Status without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Status must use
// external synchronization.

/*==============================
=            status            =
==============================*/

// 操作返回的状态结果，例如read，wirite操作
// 会返回success或相关error的状态

/*=====  End of status  ======*/


#ifndef STORAGE_LEVELDB_INCLUDE_STATUS_H_
#define STORAGE_LEVELDB_INCLUDE_STATUS_H_

#include <string>
#include "leveldb/slice.h"

namespace leveldb {

class Status {
 public:
  // Create a success status.
  // 构造函数、析构函数：其中const char* state_
  Status() : state_(NULL) { }
  ~Status() { delete[] state_; }

  // Copy the specified status.
  // 拷贝构造函数，赋值操作符
  Status(const Status& s);
  void operator=(const Status& s);

  /*========================================
  =  静态函数用于直接生成对应的Status           =
  ========================================*/
  
  // Return a success status.
  // 返回success状态，静态函数用于比较操作时返回的状态
  static Status OK() { return Status(); }

  // Return error status of an appropriate type.
  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotFound, msg, msg2);
  }
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kCorruption, msg, msg2);
  }
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kInvalidArgument, msg, msg2);
  }
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kIOError, msg, msg2);
  }
  /*=====  End of 静态函数用于直接生成对应的Status  ======*/

  /*=======================================
  =            用于判断是否处于某些状态的函数   ＝
  =======================================*/
  // Returns true iff the status indicates success.
  bool ok() const { return (state_ == NULL); }

  // Returns true iff the status indicates a NotFound error.
  bool IsNotFound() const { return code() == kNotFound; }

  // Returns true iff the status indicates a Corruption error.
  bool IsCorruption() const { return code() == kCorruption; }

  // Returns true iff the status indicates an IOError.
  bool IsIOError() const { return code() == kIOError; }

  // Returns true iff the status indicates a NotSupportedError.
  bool IsNotSupportedError() const { return code() == kNotSupported; }

  // Returns true iff the status indicates an InvalidArgument.
  bool IsInvalidArgument() const { return code() == kInvalidArgument; }
  /*=====  End of 用于判断是否处于某些状态的函数  ======*/
  

  // Return a string representation of this status suitable for printing.
  // Returns the string "OK" for success.
  // 返回打印友好的String
  std::string ToString() const;

 private:
  // OK status has a NULL state_.  Otherwise, state_ is a new[] array
  // of the following form:
  //    state_[0..3] == length of message
  //    state_[4]    == code
  //    state_[5..]  == message
  // success的状态state_为空   //消息这样管理，高。
  // 其他状态cahr ＊ state_结构对应如下
  //    state_[0..3] == 消息长度
  //    state_[4]    == 消息对应枚举类型的值
  //    state_[5..]  == 消息本身
  const char* state_;
  // 消息的枚举类型
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5
  };
  // 返回status中的状态类型
  Code code() const {
    return (state_ == NULL) ? kOk : static_cast<Code>(state_[4]);
  }
  // 构造函数
  Status(Code code, const Slice& msg, const Slice& msg2);
  // 静态方法，拷贝status内部的 char * state_
  static const char* CopyState(const char* s);
};
// 拷贝构造函数，这里inline如何提高效率？不大熟悉
inline Status::Status(const Status& s) {
  state_ = (s.state_ == NULL) ? NULL : CopyState(s.state_);
}
// 赋值操作符
inline void Status::operator=(const Status& s) {
  // The following condition catches both aliasing (when this == &s),
  // and the common case where both s and *this are ok.
  if (state_ != s.state_) {
    delete[] state_;
    state_ = (s.state_ == NULL) ? NULL : CopyState(s.state_);
  }
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_STATUS_H_
