// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in leveldb and are consulted
// automatically by leveldb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).

/*====================================
=            FilterPolicy            =
====================================*/

// FilterPolicy目的是为了达到减少IO的目的
// 原理：通过filter可以确知某些key不在硬盘中
// 从而可以加速，不用进行IO，直接返回不存在
// builtin FilterPlicy是布隆（bloom）filter
// 自行google：bloom filter
// 这里简单叙述下效果和原理
// 效果：bloom filter 使用bit vector 占用较少的内存资源的条件下
// 一定的误判概率下， 快速地判断一个key是否在sstable（硬盘）中
// 其中用了多个hash函数
// 1.对于某个key，如果bloom filter返回了“不存在”，那么可以肯定不存在
// 2.对于某个key，如果bloom filetr返回了“存在”， 那么有一定的几率误判，即可能不存在
//   这是就需要进行IO确认。

/*=====  End of FilterPolicy  ======*/


#ifndef STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
#define STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_

#include <string>

namespace leveldb {

class Slice;

class FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  // 返回过滤器的名字，作用暂时不大了解，以后补充
  virtual const char* Name() const = 0;

  // keys[0,n-1] contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys[0,n-1] to *dst.
  //
  // Warning: do not change the initial contents of *dst.  Instead,
  // append the newly constructed filter to *dst.
  // 创建filter，其中keys是一系列的key的集合，通过指针（数组）组织起来
  // 上面英文说的是把filter append 到 dst里。 还没看到使用在那里，以后补充
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst)
      const = 0;

  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class.  This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  // 这里有bloom filter的味道
  // 这个函数是对上面CreateFilter的函数中闯入的keys进行match
  // 如果key 在fileter 的list中则返回true
  // 否则尽量以较高的概率返回false
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key.  A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
//
// Callers must delete the result after any database that is using the
// result has been closed.
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
// 创造builtin的布隆过滤器
// 参数bits_per_key建议取10，这样可以使误判降低到1%
// bits_per_key的作用就是像hash table那样用于降低负载率的
// 如果负载率过高，误判几率就会很高
extern const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}

#endif  // STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
