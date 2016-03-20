// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

namespace leveldb {
// filter_policy的虚构函数还是要定义的
// 自己可以实现自己的filter_policy
// 如果不自己是实现可以使用bloom.cc的布隆实现
FilterPolicy::~FilterPolicy() { }

}  // namespace leveldb
