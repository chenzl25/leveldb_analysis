// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// File names used by DB code

#ifndef STORAGE_LEVELDB_DB_FILENAME_H_
#define STORAGE_LEVELDB_DB_FILENAME_H_

#include <stdint.h>
#include <string>
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "port/port.h"

/*================================
=            filename            =
================================*/

// leveldb中的文件类型
// 1. kLogFile        ：名为number.log的日志文件
//                      每次的写操作会先作用在日志文件中先
// 2. kDBLockFile     ：名为LOCK的锁文件
//                      一个db同时只能由一个db实例操作，通过对LOCK文件加上文件锁(flock)实现主动保护
// 3. kTableFile      ：名为number+sst的sstable文件
//                      用在disk中储存key/value数据的文件
// 4. kDescriptorFile ：名为MANIFEST-number的元信息文件
//                      每当db中的状态改变(VersionSet)
//                      会将这次改变(VersionEdit)追加到descriptor文件中
// 5. kCurrentFile    ：名为CURRENT的文件
//                      保存当前使用的descriptor的文件名的文件
// 6. kTempFile       ：名为number.dbtmp的文件
//                      对db做修复是会产生的临时文件
// 7. kInfoLogFile    ：名为LOG或LOG.old的文件
//                      db运行时，打印的info日志保存在LOG中，每次重新运行，
//                      如果已近存在LOG文件，会先将LOG文件重命名成LOG.old
/*=====  End of filename  ======*/


namespace leveldb {

class Env;

enum FileType {
  kLogFile,
  kDBLockFile,
  kTableFile, 
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kInfoLogFile  // Either the current one, or an old one
};

// 下面的函数用于返回给定名字为dbname的数据库，和一个数字number所对应的文件 
// 当然有些文件不一定会有数字number

// Return the name of the log file with the specified number
// in the db named by "dbname".  The result will be prefixed with
// "dbname".
extern std::string LogFileName(const std::string& dbname, uint64_t number);

// Return the name of the sstable with the specified number
// in the db named by "dbname".  The result will be prefixed with
// "dbname".
// 这里是sstable的新版的name，后缀ldb
extern std::string TableFileName(const std::string& dbname, uint64_t number);

// Return the legacy file name for an sstable with the specified number
// in the db named by "dbname". The result will be prefixed with
// "dbname".
// 这里是sstable的旧版的name，后缀sst
extern std::string SSTTableFileName(const std::string& dbname, uint64_t number);

// Return the name of the descriptor file for the db named by
// "dbname" and the specified incarnation number.  The result will be
// prefixed with "dbname".
extern std::string DescriptorFileName(const std::string& dbname,
                                      uint64_t number);

// Return the name of the current file.  This file contains the name
// of the current manifest file.  The result will be prefixed with
// "dbname".
extern std::string CurrentFileName(const std::string& dbname);

// Return the name of the lock file for the db named by
// "dbname".  The result will be prefixed with "dbname".
extern std::string LockFileName(const std::string& dbname);

// Return the name of a temporary file owned by the db named "dbname".
// The result will be prefixed with "dbname".
extern std::string TempFileName(const std::string& dbname, uint64_t number);

// Return the name of the info log file for "dbname".
extern std::string InfoLogFileName(const std::string& dbname);

// Return the name of the old info log file for "dbname".
extern std::string OldInfoLogFileName(const std::string& dbname);

// If filename is a leveldb file, store the type of the file in *type.
// The number encoded in the filename is stored in *number.  If the
// filename was successfully parsed, returns true.  Else return false.
// 解析filename到number和type中
extern bool ParseFileName(const std::string& filename,
                          uint64_t* number,
                          FileType* type);

// Make the CURRENT file point to the descriptor file with the
// specified number.
// 设置CURRENT文件的内容到指定的descriptor_number的descriptor文件中
extern Status SetCurrentFile(Env* env, const std::string& dbname,
                             uint64_t descriptor_number);


}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_FILENAME_H_
