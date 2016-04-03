// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_OPTIONS_H_
#define STORAGE_LEVELDB_INCLUDE_OPTIONS_H_

#include <stddef.h>

namespace leveldb {

class Cache;
class Comparator;
class Env;
class FilterPolicy;
class Logger;
class Snapshot;

// DB contents are stored in a set of blocks, each of which holds a
// sequence of key,value pairs.  Each block may be compressed before
// being stored in a file.  The following enum describes which
// compression method (if any) is used to compress a block.
// 压缩的类型分为两种
// 1:不压缩
// 2:使用默认的Snappy压缩
enum CompressionType {
  // NOTE: do not change the values of existing entries, as these are
  // part of the persistent format on disk.
  // 值不要修改，因为这是数据记录在硬盘中格式的一部分
  kNoCompression     = 0x0,
  kSnappyCompression = 0x1
};

/*=============================================
=            Options                          =
=============================================*/
// open时候的Options
// read时候的ReadOptions
// write时候的WriteOPtions

/*=====  End of Options                ======*/


// Options to control the behavior of a database (passed to DB::Open)
// 在打开数据库的时候传进去的Options.配置
struct Options {
  // -------------------
  // Parameters that affect behavior

  // Comparator used to define the order of keys in the table.
  // Default: a comparator that uses lexicographic byte-wise ordering
  //
  // REQUIRES: The client must ensure that the comparator supplied
  // here has the same name and orders keys *exactly* the same as the
  // comparator provided to previous open calls on the same DB.
  // －－－－－－－－－－－－－－－－－－－－－－－－－－－－－－－－－
  // 用于以后leveldb的键的大小比较
  // 如果自定义了以后打开leveldb是要保证有相同的名字与和开始一样的比较顺序
  // 因为后面leveldb的存储依赖与键的大小顺序来存储，修改了就乱套了。
  const Comparator* comparator;

  // If true, the database will be created if it is missing.
  // Default: false
  // 如果数据库不存在，是否创建数据库。
  // 默认 false
  bool create_if_missing;

  // If true, an error is raised if the database already exists.
  // Default: false
  // 如果数据库已近存在，是否抛出异常。
  // 默认：false
  bool error_if_exists;

  // If true, the implementation will do aggressive checking of the
  // data it is processing and will stop early if it detects any
  // errors.  This may have unforeseen ramifications: for example, a
  // corruption of one DB entry may cause a large number of entries to
  // become unreadable or for the entire DB to become unopenable.
  // Default: false
  // 是否疯狂的检查数据，一有错误马上停止。//现在不太清楚在哪用了。以后补充
  // 默认：false
  bool paranoid_checks;

  // Use the specified object to interact with the environment,
  // e.g. to read/write files, schedule background work, etc.
  // Default: Env::Default()
  // Env 环境。跟操作系统平台有关。转到env.h看介绍
  Env* env;

  // Any internal progress/error information generated by the db will
  // be written to info_log if it is non-NULL, or to a file stored
  // in the same directory as the DB contents if info_log is NULL.
  // Default: NULL
  // 内部的操作，错误的记录。
  // 如果自己提供了Logger则写往自己提供的info_log
  // 否则纪录会放到DB contents的统一目录下
  Logger* info_log;

  // -------------------
  // Parameters that affect performance

  // Amount of data to build up in memory (backed by an unsorted log
  // on disk) before converting to a sorted on-disk file.
  //
  // Larger values increase performance, especially during bulk loads.
  // Up to two write buffers may be held in memory at the same time,
  // so you may wish to adjust this parameter to control memory usage.
  // Also, a larger write buffer will result in a longer recovery time
  // the next time the database is opened.
  //
  // Default: 4MB
  // 写的buffer的大小。貌似是memtable的大小，现在不确定，以后补充
  // 大小自己权衡，跟性能，recovery time相关
  // 默认：4MB
  size_t write_buffer_size;

  // Number of open files that can be used by the DB.  You may need to
  // increase this if your database has a large working set (budget
  // one open file per 2MB of working set).
  //
  // Default: 1000
  // 最大的打开文件的数量
  // 因为sstable的默认大小为2MB
  // 各种纪录信息都是以文件为载体的。
  // 默认：1000
  int max_open_files;

  // Control over blocks (user data is stored in a set of blocks, and
  // a block is the unit of reading from disk).

  // If non-NULL, use the specified cache for blocks.
  // If NULL, leveldb will automatically create and use an 8MB internal cache.
  // Default: NULL
  // block的Cache
  // 注意IO操作读取是以block为单位的
  // 如果自己提供cache就往这里传。
  // cache的定义请移步cache.h
  // 默认：8MB
  Cache* block_cache;

  // Approximate size of user data packed per block.  Note that the
  // block size specified here corresponds to uncompressed data.  The
  // actual size of the unit read from disk may be smaller if
  // compression is enabled.  This parameter can be changed dynamically.
  //
  // Default: 4K
  // 这也就是上面提到的block的大小
  // 注意下这事未压缩block的大小，压缩后IO的读取就会更小，想提升IO效率要自己权衡
  // （可以动态改变，暂时不知道怎样动态改变。这有待继续了解）
  // 默认：4K  大概等于IO一次读的量
  size_t block_size;

  // Number of keys between restart points for delta encoding of keys.
  // This parameter can be changed dynamically.  Most clients should
  // leave this parameter alone.
  //
  // Default: 16
  // 这里说的是block中restart的间距
  // 每个block都由record组成。
  // record之间为了更加节约空间会采取共享前缀的方法
  // 这里的restart就是共享前缀的间距
  // 间距大就会提高压缩效率
  // 间距小就能提高搜索的速度
  // ＊＊这个可以动态改变，还能理解，因为这只与后面写进去的block造成影响。有待考证
  // 默认：16
  int block_restart_interval;

  // Compress blocks using the specified compression algorithm.  This
  // parameter can be changed dynamically.
  //
  // Default: kSnappyCompression, which gives lightweight but fast
  // compression.
  //
  // Typical speeds of kSnappyCompression on an Intel(R) Core(TM)2 2.4GHz:
  //    ~200-500MB/s compression
  //    ~400-800MB/s decompression
  // Note that these speeds are significantly faster than most
  // persistent storage speeds, and therefore it is typically never
  // worth switching to kNoCompression.  Even if the input data is
  // incompressible, the kSnappyCompression implementation will
  // efficiently detect that and will switch to uncompressed mode.
  // 压缩类型，实际上默认也就是是否要压缩
  CompressionType compression;

  // EXPERIMENTAL: If true, append to existing MANIFEST and log files
  // when a database is opened.  This can significantly speed up open.
  //
  // Default: currently false, but may become true later.
  // 实验功能，如果为true就append到已经存在的manifest和log中
  // 如果没有的话没打开一次就会将manifest和log一起合并在一起创造一个最新Version的manifest
  bool reuse_logs;

  // If non-NULL, use the specified filter policy to reduce disk reads.
  // Many applications will benefit from passing the result of
  // NewBloomFilterPolicy() here.
  //
  // Default: NULL
  // 移步filter_policy.h 这个有待考证
  const FilterPolicy* filter_policy;

  // Create an Options object with default values for all fields.
  Options();
};

// Options that control read operations
// 读的options 配置
struct ReadOptions {
  // If true, all data read from underlying storage will be
  // verified against corresponding checksums.
  // Default: false
  // 是否检查数据校验和（硬盘上的）
  // 默认：false
  bool verify_checksums;

  // Should the data read for this iteration be cached in memory?
  // Callers may wish to set this field to false for bulk scans.
  // Default: true
  // 读完后是否进行缓存
  // 建议：对于大规模的scan（例如：顺序扫描）设置为false
  // 因为默认cache policy 是LRU的。（注意，这里是一个个的record缓存，不是整个block的）
  // 默认；true
  bool fill_cache;

  // If "snapshot" is non-NULL, read as of the supplied snapshot
  // (which must belong to the DB that is being read and which must
  // not have been released).  If "snapshot" is NULL, use an implicit
  // snapshot of the state at the beginning of this read operation.
  // Default: NULL
  // 快照
  // 可以传入想要的snapshot，主要是为了数据一致性的需求
  // ps：别release了在用的snapshot
  // 默认：使用这次read的时候的snapshot
  const Snapshot* snapshot;

  ReadOptions()
      : verify_checksums(false),
        fill_cache(true),
        snapshot(NULL) {
  }
};

// Options that control write operations
// 写入的options配置
struct WriteOptions {
  // If true, the write will be flushed from the operating system
  // buffer cache (by calling WritableFile::Sync()) before the write
  // is considered complete.  If this flag is true, writes will be
  // slower.
  //
  // If this flag is false, and the machine crashes, some recent
  // writes may be lost.  Note that if it is just the process that
  // crashes (i.e., the machine does not reboot), no writes will be
  // lost even if sync==false.
  //
  // In other words, a DB write with sync==false has similar
  // crash semantics as the "write()" system call.  A DB write
  // with sync==true has similar crash semantics to a "write()"
  // system call followed by "fsync()".
  //
  // Default: false
  // 是否马上同步
  // 考虑速度与重要性
  // 如果 sync ＝ false 一般的进程crash是不会造成数据丢失的，
  // ＊＊因为会有log文件纪录
  // 但是宕机了就会
  // 类比 系统调用的 write() fsyn()
  // 默认：false
  bool sync;

  WriteOptions()
      : sync(false) {
  }
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_OPTIONS_H_

