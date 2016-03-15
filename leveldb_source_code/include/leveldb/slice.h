// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Slice is a simple structure containing a pointer into some external
// storage and a size.  The user of a Slice must ensure that the slice
// is not used after the corresponding external storage has been
// deallocated.
//
// Multiple threads can invoke const methods on a Slice without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Slice must use
// external synchronization.


/*=============================================
=            Section comment block            =
=============================================*/

// Slice是一个对c字符串(char* data_)的简单封装，加上长度size_
// 同时为了提高操作的效率，私有成员内部采取了指针存储的方式，
// 既然采取了指针的存储，指针指向字符串的责任就交给了外部来管理
// ps：字符串可以包含'\0'，'\0'可以不作为默认结束符

/*=====  End of Section comment block  ======*/




#ifndef STORAGE_LEVELDB_INCLUDE_SLICE_H_
#define STORAGE_LEVELDB_INCLUDE_SLICE_H_

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <string>

// 命名空间。这是需要学习的地方
namespace leveldb {

class Slice {
 public:
  // Create an empty slice.
  // 构造函数：空字符串的Slice
  Slice() : data_(""), size_(0) { }

  // Create a slice that refers to d[0,n-1].
  // 构造函数：用指针char* d的前n位，构造Slice
  Slice(const char* d, size_t n) : data_(d), size_(n) { }

  // Create a slice that refers to the contents of "s"
  // 构造函数：将stl中的string转换成Slice
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) { }

  // Create a slice that refers to s[0,strlen(s)-1]
  // 构造函数：不提供长度限制，默认使用strlen来决定长度
  Slice(const char* s) : data_(s), size_(strlen(s)) { }

  // Return a pointer to the beginning of the referenced data
  // 返回内部的data_
  const char* data() const { return data_; }

  // Return the length (in bytes) of the referenced data
  // 返回内部的长度
  size_t size() const { return size_; }

  // Return true iff the length of the referenced data is zero
  // 判断是否为空，通过size_ == 0
  bool empty() const { return size_ == 0; }

  // Return the ith byte in the referenced data.
  // REQUIRES: n < size()
  // Slice下标取值.
  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }

  // Change this slice to refer to an empty array
  // 清除Slice，只是简单的 data_ = "" ,size_ = 0. 效率高
  void clear() { data_ = ""; size_ = 0; }

  // Drop the first "n" bytes from this slice.
  // 去掉前缀长度n. 用于后面sstable -> block -> record的前缀压缩
  void remove_prefix(size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }

  // Return a string that contains the copy of the referenced data.
  // 真正得到复制了内容返回出去
  std::string ToString() const { return std::string(data_, size_); }

  // Three-way comparison.  Returns value:
  //   <  0 iff "*this" <  "b",
  //   == 0 iff "*this" == "b",
  //   >  0 iff "*this" >  "b"
  // 比较两个Slice
  int compare(const Slice& b) const;

  // Return true iff "x" is a prefix of "*this"
  // 判断前缀是否吻合。 用于后面sstable -> block -> record的前缀压缩
  bool starts_with(const Slice& x) const {
    return ((size_ >= x.size_) &&
            (memcmp(data_, x.data_, x.size_) == 0));
  }

 private:
  const char* data_;  // 字符串指针
  size_t size_;       // 字符串长度

  // Intentionally copyable
};
// 比较两个Slice是否相等。 使用平常见的方法。长度比较，再memcmp。
// inline 较少简单操作的函数调用
inline bool operator==(const Slice& x, const Slice& y) {
  return ((x.size() == y.size()) &&
          (memcmp(x.data(), y.data(), x.size()) == 0));
}

// 标准的复用写法
inline bool operator!=(const Slice& x, const Slice& y) {
  return !(x == y);
}

//  标准字符串比较
inline int Slice::compare(const Slice& b) const {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size_) r = -1;
    else if (size_ > b.size_) r = +1;
  }
  return r;
}

}  // namespace leveldb


#endif  // STORAGE_LEVELDB_INCLUDE_SLICE_H_
