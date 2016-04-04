x// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DB_IMPL_H_
#define STORAGE_LEVELDB_DB_DB_IMPL_H_

#include <deque>
#include <set>
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "port/port.h"
#include "port/thread_annotations.h"

/*===============================
=            db_impl            =
===============================*/

// 这里是db统一所有实现的最后一步了
// DBImpl继承了DB的接口

/*=====  End of db_impl  ======*/

namespace leveldb {

class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  virtual ~DBImpl();

  // Implementations of the DB interface
  virtual Status Put(const WriteOptions&, const Slice& key, const Slice& value);
  virtual Status Delete(const WriteOptions&, const Slice& key);
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  virtual Status Get(const ReadOptions& options,
                     const Slice& key,
                     std::string* value);
  virtual Iterator* NewIterator(const ReadOptions&);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual void CompactRange(const Slice* begin, const Slice* end);

  // Extra methods (for testing) that are not in the public DB interface
  // 下面的api是测试用的
  // Compact any files in the named level that overlap [*begin,*end]
  void TEST_CompactRange(int level, const Slice* begin, const Slice* end);

  // Force current memtable contents to be compacted.
  Status TEST_CompactMemTable();

  // Return an internal iterator over the current state of the database.
  // The keys of this iterator are internal keys (see format.h).
  // The returned iterator should be deleted when no longer needed.
  Iterator* TEST_NewInternalIterator();

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  int64_t TEST_MaxNextLevelOverlappingBytes();

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.
  // 周期性地RecordReadSample
  void RecordReadSample(Slice key);

 private:
  friend class DB;
  struct CompactionState;
  struct Writer;
  // 返回一个迭代db的iterator
  Iterator* NewInternalIterator(const ReadOptions&,
                                SequenceNumber* latest_snapshot,
                                uint32_t* seed);
  // 新建一个DB
  Status NewDB();

  // Recover the descriptor from persistent storage.  May do a significant
  // amount of work to recover recently logged updates.  Any changes to
  // be made to the descriptor are added to *edit.
  // 从在disk中储存的descriptor file中恢复Version。结果存到VersionEdit中
  Status Recover(VersionEdit* edit, bool* save_manifest)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void MaybeIgnoreError(Status* s) const;

  // Delete any unneeded files and stale in-memory entries.
  // 删除不再需要的file，并且清理相应的缓存(如果有的话)
  void DeleteObsoleteFiles();

  // Compact the in-memory write buffer to disk.  Switches to a new
  // log-file/memtable and writes a new descriptor iff successful.
  // Errors are recorded in bg_error_.
  // compact memtable到disk中，如果成功了就会更换log-file和追加新的manifest
  void CompactMemTable() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  //从log-file中恢复数据 
  Status RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest,
                        VersionEdit* edit, SequenceNumber* max_sequence)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // 将memtable写到level-0中，因为这个操作会影响version
  // 所以记录到了edit中
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // 寻找有足够的空间来WriteBatch，有很多策略判断，情况实现
  // force表示强行令memtable compact到disk，即使memtable还有空的空间
  Status MakeRoomForWrite(bool force /* compact even if there is room? */)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // 将要写入的Batch组合起来一次写，提高性能
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  // 记录background有错误
  void RecordBackgroundError(const Status& s);
  // 看是否需要进行compact调度
  // 后面的一系列函数都跟compact有关
  void MaybeScheduleCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  static void BGWork(void* db);
  void BackgroundCall();
  void  BackgroundCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void CleanupCompaction(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status DoCompactionWork(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // 打开compact的output file，并建立tableBuilder
  Status OpenCompactionOutputFile(CompactionState* compact);
  // 完成compation，并且进行output file
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  // 将compact的结果记录到log-file中并应用到VersionSet中
  Status InstallCompactionResults(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Constant after construction
  // 一些db的常量
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const InternalFilterPolicy internal_filter_policy_;
  const Options options_;  // options_.comparator == &internal_comparator_
  bool owns_info_log_;
  bool owns_cache_;
  const std::string dbname_;

  // table_cache_ provides its own synchronization
  // table_cache_自带同步机制
  TableCache* table_cache_;

  // Lock over the persistent DB state.  Non-NULL iff successfully acquired.
  // 用文件来实现的db_lock机制
  FileLock* db_lock_;

  // State below is protected by mutex_
  // 一个锁用来同步下面的State的
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  port::CondVar bg_cv_;          // Signalled when background work finishes
  MemTable* mem_;
  MemTable* imm_;                // Memtable being compacted
  port::AtomicPointer has_imm_;  // So bg thread can detect non-NULL imm_
  WritableFile* logfile_;
  uint64_t logfile_number_;
  log::Writer* log_;
  uint32_t seed_;                // For sampling.

  // Queue of writers.
  // writer的队列
  std::deque<Writer*> writers_;
  WriteBatch* tmp_batch_;
  // snapshots_的链表
  SnapshotList snapshots_;

  // Set of table files to protect from deletion because they are
  // part of ongoing compactions.
  // 这里的files是防止被删除的，因为它们是正在compaction中
  std::set<uint64_t> pending_outputs_;

  // Has a background compaction been scheduled or is running?
  // 用来标示是否有后台compaction在运行着
  bool bg_compaction_scheduled_;

  // Information for a manual compaction
  // 手动compaction的信息
  struct ManualCompaction {
    int level;
    bool done;
    const InternalKey* begin;   // NULL means beginning of key range
    const InternalKey* end;     // NULL means end of key range
    // tmp_storage是用来存放一些还没compaction的file，用key来标示
    // 因为如果手动设置的range过大，内部会截断来防止
    InternalKey tmp_storage;    // Used to keep track of compaction progress
  };
  // 手动的compaction
  ManualCompaction* manual_compaction_;
  // db的VersionSet
  VersionSet* versions_;

  // Have we encountered a background error in paranoid mode?
  // 后台的错误，在paranoid(激进) mode
  Status bg_error_;

  // Per level compaction stats.  stats_[level] stores the stats for
  // compactions that produced data for the specified "level".
  // 用来记录每层level的compaction信息的
  struct CompactionStats {
    int64_t micros;
    int64_t bytes_read;
    int64_t bytes_written;

    CompactionStats() : micros(0), bytes_read(0), bytes_written(0) { }

    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }
  };
  CompactionStats stats_[config::kNumLevels];

  // No copying allowed
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);

  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }
};

// Sanitize db options.  The caller should delete result.info_log if
// it is not equal to src.info_log.
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const InternalFilterPolicy* ipolicy,
                               const Options& src);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DB_IMPL_H_
