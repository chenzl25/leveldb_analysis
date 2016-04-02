// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {
// 用来创建Table的函数，需要传入dbname， env，options
// 还有table_cache，iter是可以迭代table的内容的
// 如果成功就把meta填充成功创建的Table的信息
// （例如：size，refs， allowed_seeks，number，smallest，largest）
// ps：iter不是在TableCache中产生的，否则就没有意义了，因为我们是为了构造table
Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  // iter的SeekToFirst开始迭代
  iter->SeekToFirst();
  // 利用dbname和number来进行TableFileName的构造
  // 注意传进来的meta中包含了file的number
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file;
    // 新建一个WritableFile来写入存储table
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    // 利用TableBuilder来进行build
    TableBuilder* builder = new TableBuilder(options, file);
    // meta的smallest的构建
    meta->smallest.DecodeFrom(iter->key());
    // 迭代iter，把得到的key/value加入到builder中
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      meta->largest.DecodeFrom(key); //这里每次迭代都decodeFrom一次会不会效率低呢？
      builder->Add(key, iter->value());
    }

    // Finish and check for builder errors
    // 调用builder->Finish()来完成table的构建
    if (s.ok()) {
      s = builder->Finish();
      if (s.ok()) {
        // 把构建好的file的大小记录到meta中
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
    } else {
      // 构建失败就要Abandon
      builder->Abandon();
    }

    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
      // 用Sync来强推到disk中，避过OS的buffer
      s = file->Sync();
    }
    if (s.ok()) {
      // 关闭file
      s = file->Close();
    }
    delete file;
    file = NULL;

    if (s.ok()) {
      // Verify that the table is usable
      // 检查table是否可用，也就是试图在table_cache中创建一个对应table的iterator
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                              meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    // 到了现在这一步出错就是table_cache->NewIterator失败了
    // 就必须进行commit or  rollback操作了，也就是删除打开不了的FILE
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
