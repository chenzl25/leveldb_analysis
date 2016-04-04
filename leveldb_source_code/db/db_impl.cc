// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
// 封装了WriteBatch的Writer，可以实现同步机制
struct DBImpl::Writer {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;

  explicit Writer(port::Mutex* mu) : cv(mu) { }
};
// 用来管理compact的结构体
struct DBImpl::CompactionState {
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  // Files produced by compaction
  // compact过程产生的file就记录在这里
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };
  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;

  Output* current_output() { return &outputs[outputs.size()-1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        outfile(NULL),
        builder(NULL),
        total_bytes(0) {
  }
};

// Fix user-supplied options to be reasonable
template <class T,class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != NULL) ? ipolicy : NULL;
  ClipToRange(&result.max_open_files,    64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64<<10,                      1<<30);
  ClipToRange(&result.block_size,        1<<10,                       4<<20);
  if (result.info_log == NULL) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = NULL;
    }
  }
  if (result.block_cache == NULL) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      db_lock_(NULL),
      shutting_down_(NULL),
      bg_cv_(&mutex_),
      mem_(NULL),
      imm_(NULL),
      logfile_(NULL),
      logfile_number_(0),
      log_(NULL),
      seed_(0),
      tmp_batch_(new WriteBatch),
      bg_compaction_scheduled_(false),
      manual_compaction_(NULL) {
  has_imm_.Release_Store(NULL);

  // Reserve ten files or so for other uses and give the rest to TableCache.
  const int table_cache_size = options_.max_open_files - kNumNonTableCacheFiles;
  table_cache_ = new TableCache(dbname_, &options_, table_cache_size);

  versions_ = new VersionSet(dbname_, &options_, table_cache_,
                             &internal_comparator_);
}

DBImpl::~DBImpl() {
  // Wait for background work to finish
  mutex_.Lock();
  shutting_down_.Release_Store(this);  // Any non-NULL value is ok
  while (bg_compaction_scheduled_) {
    bg_cv_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != NULL) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != NULL) mem_->Unref();
  if (imm_ != NULL) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}
// 删除废弃了的文件
void DBImpl::DeleteObsoleteFiles() {
  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    // 如果bg_error发生了，就不知道一个新的version是否commit了
    // 所以就停止garbage college
    return;
  }

  // Make a set of all of the live files
  // 先把受保护的文件pending_outputs_先保存起来
  std::set<uint64_t> live = pending_outputs_;
  // 将当前Version的"live"文件加入到live中
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  // 获取dbname目录下的所有files
  env_->GetChildren(dbname_, &filenames); // Ignoring errors on purpose
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < filenames.size(); i++) {
    // 根据类型判断是否要保留
    if (ParseFileName(filenames[i], &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          // Log-file要看number和PrevLogNumber
          keep = ((number >= versions_->LogNumber()) ||
                  (number == versions_->PrevLogNumber()));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          // kTempFile是在pending_outputs_中的
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        // 如果ktable要删除，那么table-cache中的缓存要删除
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n",
            int(type),
            static_cast<unsigned long long>(number));
        // 删除文件
        env_->DeleteFile(dbname_ + "/" + filenames[i]);
      }
    }
  }
}
// 从在disk中储存的descriptor file中恢复Version。结果存到VersionEdit中
Status DBImpl::Recover(VersionEdit* edit, bool *save_manifest) {
  // Recover的时候要持有Mutex
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  // 创建新目录，忽视出错，原因看上面
  env_->CreateDir(dbname_);
  assert(db_lock_ == NULL);
  // 现在要对LOCK文件进行flock来
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }
  // 根据options的一些设置的处理
  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      s = NewDB();
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(
          dbname_, "exists (error_if_exists is true)");
    }
  }
  // 调用VersionSet的Recover
  s = versions_->Recover(save_manifest);
  if (!s.ok()) {
    return s;
  }
  SequenceNumber max_sequence(0);

  // Recover from all newer log files than the ones named in the
  // descriptor (new log files may have been added by the previous
  // incarnation without registering them in the descriptor).
  //
  // Note that PrevLogNumber() is no longer used, but we pay
  // attention to it in case we are recovering a database
  // produced by an older version of leveldb.
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log = versions_->PrevLogNumber();
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected;
  // 现在VersionSet中找到所有应该存在的file
  versions_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  // 如果disk中存在expect中的file就把expect中的file去掉
  // 然后，如果遇到log-file就push到logs中，其中会根据>=VersionSet的LogNumber和==PrevLogNumber的条件来判断
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }
  // 如果expected的文件再上面没有被删除完整，就意味着缺失
  if (!expected.empty()) {
    char buf[50];
    snprintf(buf, sizeof(buf), "%d missing files; e.g.",
             static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }

  // Recover in the order in which the logs were generated
  // 如果存在多个log-file，就将其从旧到新的顺序一路recover
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      return s;
    }

    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    // 手动更新fileNumber，理由看英文
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }

  return Status::OK();
}
//从log-file中恢复数据 
Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  // 老伎俩先构建一个LogReporter，用于处理LogReader错误的
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // NULL if options_.paranoid_checks==false
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == NULL ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != NULL && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  // 用SequentialFile打开log-file文件
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  // 创建一个log-reader
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : NULL);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  // 内部会主动检查checksum，原因看英文
  log::Reader reader(file, &reporter, true/*checksum*/,
                     0/*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long) log_number);

  // Read all the records and add to a memtable
  // 在log中读取每一条record，作用到mmtable中，如果memtable上线到达了，就compact
  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = NULL;
  while (reader.ReadRecord(&record, &scratch) &&
         status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      continue;
    }
    // 把读到的内容记录到WriteBatch中
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == NULL) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    // 把WriteBatch写到到memtable中
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    // 更新max_sequence，最终max_sequence会赋值回VersionSet的LastSequence中
    const SequenceNumber last_seq =
        WriteBatchInternal::Sequence(&batch) +
        WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }
    // 如果memtable的大小超过阈值就进行compact
    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      // 把mem写到level-0中
      status = WriteLevel0Table(mem, edit, NULL);
      mem->Unref();
      mem = NULL;
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
    }
  }

  delete file;

  // See if we should keep reusing the last log file.
  // 看下是否我是否要reuse最后一个log-file
  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == NULL);
    assert(log_ == NULL);
    assert(mem_ == NULL);
    uint64_t lfile_size;
    // reuse就是在最后一个log-file和面追加
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      log_ = new log::Writer(logfile_, lfile_size);
      logfile_number_ = log_number;
      // 同时会把mem_table复用过来
      if (mem != NULL) {
        mem_ = mem;
        mem = NULL;
      } else {
        // mem can be NULL if lognum exists but was empty.
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }
  // 如果mem != NULL则表明上面的resue log-file没成功
  // 所以要对剩下的mem进行compact
  if (mem != NULL) {
    // mem did not get reused; compact it.
    if (status.ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, NULL);
    }
    mem->Unref();
  }
  // 从上面的过程我们就可以看到recover的过程就是先找到CURRENT找到对应的manifest
  // 再更具manifest的一系列的VersionEdit来更新到上一次退出的Version状态，
  // 最后就会再根据log-file来进行可能丢失的操作，最后还会判断是否对log-file进行复用
  // ------------------这段以后补充-------------
  // 现在我只是看到过一个log-file的情况，所以就一个log-file的情况的话，可以把log恢复中用到的
  // memtable转化成正常使用的memtable，这样就不用多一次整合了
  // -----------------------------------------
  return status;
}
// 将memtable写到level-0中，因为这个操作会影响version
// 所以记录到了edit中
Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
  mutex_.AssertHeld();
  // 开始记录时间
  const uint64_t start_micros = env_->NowMicros();
  // 新建一个metaFile
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  // 加入被保护的文件集合中
  pending_outputs_.insert(meta.number);
  // 获取memtable的iter用来构建table
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long) meta.number);

  Status s;
  {
    // 这里要解锁应该是由于table_cache_自带同步机制的原因，同时这个函数也会被另外一个线程(bg-compact）调用
    mutex_.Unlock();
    // 根据memtable的iter来构建table
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long) meta.number,
      (unsigned long long) meta.file_size,
      s.ToString().c_str());
  delete iter;
  // 从保护文件集合中删除
  pending_outputs_.erase(meta.number);


  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    // 如果base不为空就会选择一个最合适的level来output，否则就是level-0
    if (base != NULL) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    // edit中加入file
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  // 把stats记录到stats_中，stats_记录着所有compact的信息
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != NULL);

  // Save the contents of the memtable as a new Table
  // 将imm的compact根据Version状态来写入edit
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();

  if (s.ok() && shutting_down_.Acquire_Load()) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  // 把edit作用到VersionSet中
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = NULL;
    // 用atomic pointer来确保顺序
    has_imm_.Release_Store(NULL);
    // 这里可能是为了除去上次compact遗留下来的文件
    DeleteObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable(); // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == NULL) {
    manual.begin = NULL;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == NULL) {
    manual.end = NULL;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()) {
    if (manual_compaction_ == NULL) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      bg_cv_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = NULL;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // NULL batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), NULL);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != NULL && bg_error_.ok()) {
      bg_cv_.Wait();
    }
    if (imm_ != NULL) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  // 记录错误的status到bg_error_，bg_cv_.SignalAll是当后台进程完成后就发起信号
  if (bg_error_.ok()) {
    bg_error_ = s;
    bg_cv_.SignalAll();
  }
}
// 尝试进行compact
void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (bg_compaction_scheduled_) {
    // Already scheduled
    // 已近在调度compact中了
  } else if (shutting_down_.Acquire_Load()) {
    // DB is being deleted; no more background compactions
    // 如果shutting_down_.Acquire_Load()成功就是DB正在被delete
  } else if (!bg_error_.ok()) {
    // Already got an error; no more changes
  } else if (imm_ == NULL &&
             manual_compaction_ == NULL &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
    // 开始调度compact
    bg_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}
// 后台调度的包装
void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  // 先上锁，MutexLock的构造函数就是上锁
  MutexLock l(&mutex_);
  assert(bg_compaction_scheduled_);
  if (shutting_down_.Acquire_Load()) {
    // No more background work when shutting down.
    // DB正在被delete，所以不再工作
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    // 真正的后台compact
    BackgroundCompaction();
  }

  bg_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  // 可能之前的compact产生了很多的files，需要多次地compact才能完成
  MaybeScheduleCompaction();
  bg_cv_.SignalAll();
}

void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();
  // 如果immutable非空就直接CompactMemTable就可以返回额
  if (imm_ != NULL) {
    CompactMemTable();
    return;
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != NULL);
  InternalKey manual_end;
  // 如果是进行手动compact
  if (is_manual) {
    // 根据手动设定的range去让VersionSet构造compact
    ManualCompaction* m = manual_compaction_;
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == NULL);
    if (c != NULL) {
      // 根据compact选定的compact的最后一个file的最大值赋值到manual_end中
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    // 从log-info中可以看出手动compact的过程
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level,
        (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    // 如果不是手动的compact就从VersionSet中选取合适的compact
    c = versions_->PickCompaction();
  }

  Status status;
  if (c == NULL) {
    // Nothing to do
  } else if (!is_manual && c->IsTrivialMove()) {
    // Move file to next level
    // 如果compact是IsTrivialMove的那么直接把file放到level-n+1就行了
    // ps:IsTrivialMove的时候num_input_files只有一个
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    // 构造edit
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size,
                       f->smallest, f->largest);
    // 应用edit
    status = versions_->LogAndApply(c->edit(), &mutex_);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    // 写日志log-info
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number),
        c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(),
        versions_->LevelSummary(&tmp));
  } else {
    // not-trivial的compact，具体逻辑在DoCompactionWork中
    CompactionState* compact = new CompactionState(c);
    status = DoCompactionWork(compact);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);
    c->ReleaseInputs();
    DeleteObsoleteFiles();
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.Acquire_Load()) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log,
        "Compaction error: %s", status.ToString().c_str());
  }
  // 如果是手动compact
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      // 因为手动compact还没完全弄完，更新还剩下需要继续手动compact的值到tmp_storage
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = NULL;
  }
}
// 通过CompactionState来析构compact过程的东西
void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != NULL) {
    // May happen if we get a shutdown call in the middle of compaction
    // 当在后头compact的时候，db要shutdown了，就会丢弃所有的正在构建的table
    // 同时可能造成垃圾，但是会在下一次compact中被清理
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == NULL);
  }
  delete compact->outfile;
  // 从保护队列中去掉outputs
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}
// 打开compact的output file，并建立tableBuilder
Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != NULL);
  assert(compact->builder == NULL);
  uint64_t file_number;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}
// 完成compation，并且进行output file
Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != NULL);
  assert(compact->outfile != NULL);
  assert(compact->builder != NULL);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  // 获取compact中已经插入的record个数
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  // 获取compact中已经插入的record的总byte数
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = NULL;

  // Finish and check for file errors
  // 将compact的output，Sync到disk中
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = NULL;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    // 检查刚生产的table是可用了
    Iterator* iter = table_cache_->NewIterator(ReadOptions(),
                                               output_number,
                                               current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log,
          "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long) output_number,
          compact->compaction->level(),
          (unsigned long long) current_entries,
          (unsigned long long) current_bytes);
    }
  }
  return s;
}

// 将compact的结果应用到VersionSet中并记录到log-file中
Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  // 确保通过mutex进行锁同步了，因为更新version是一个整体的事务
  mutex_.AssertHeld();
  // 写日志
  Log(options_.info_log,  "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  // 先取出compact内部的edit，再把input添加到edit的待删除队列中
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    // 把compact的output加入到edit的添加队列中
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(
        level + 1,
        out.number, out.file_size, out.smallest, out.largest);
  }
  // 记录compact的edit到manifest中，并应用
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}
// non-trivial的compact
Status DBImpl::DoCompactionWork(CompactionState* compact) {
  // 记录开始compact时间
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions
  // 记录日志，compact
  Log(options_.info_log,  "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == NULL);
  assert(compact->outfile == NULL);
  // compact过程先把smallest_snapshot记录好
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->number_;
  }

  // Release mutex while we're actually doing the compaction work
  // 解锁，异步执行
  mutex_.Unlock();
  // 获得compaction中遍历input的iter
  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  // 遍历iter，同时判断是否在shutdown，在这里应该可以看出atomic操作的用处
  for (; input->Valid() && !shutting_down_.Acquire_Load(); ) {
    // Prioritize immutable compaction work
    // 优先检查并且compact存在的immutable memtable
    if (has_imm_.NoBarrier_Load() != NULL) {
      const uint64_t imm_start = env_->NowMicros();
      // CompactMemTable这里要进行同步，
      // 当然在CompactMemTable内部调用的LogAndApply会在IO的时候进行解锁，IO完成后再进行上锁
      mutex_.Lock();
      if (imm_ != NULL) {
        CompactMemTable();
        bg_cv_.SignalAll();  // Wakeup MakeRoomForWrite() if necessary
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }
    // 如果当前与grandparent层产生的overlap超过了阈值
    // 就会停止当前的迭代，
    Slice key = input->key();
    // 印证了当时在ShouldStopBefore(在"version_set.cc"）函数会调用多次的说法
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != NULL) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    // 根据条件判断读到的key/value是否要记录
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      // 更新current_user_key
      // 通过迭代得来的key有可能是相等的，也就是多次PUT，或者DELETE，这些都通过maergingIter来排序到一起了
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key,
                                     Slice(current_user_key)) != 0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }
      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        // 到了这里表明遇到了重复的key，同时还没有last_sequence_for_key会引用到，所以就要drop
        drop = true;    // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        // 到了这里key是第一个，前没有重复的key
        // 这里删除这个键的理由有
        // (1) 因为IsBaseLevelForKey成立，所以在更高的level里没有data
        // (2) 在更低的level里如果有这个key的话，它们的sequence值更大，所以不会影响
        // (3) 在这次compact中，后面如果有相同的key的话会被上面(A)规则在后面删除掉
        // 所以这里可以drop掉这个键
        // 先不考虑snapshot会好理解点，后面再加上这个snapshot规则就行了
        drop = true;
      }
      // 这里更新了last_sequence_for_key
      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    if (!drop) {
      // Open output file if necessary
      // 如果还没打开compact的output file就打开
      if (compact->builder == NULL) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      // 如果是第一个插入的key就记录为最小的key
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      // 一路记录最大key
      compact->current_output()->largest.DecodeFrom(key);
      // 通过table-builder来加入key/value
      compact->builder->Add(key, input->value());

      // Close output file if it is big enough
      // 如果已经当前的sstable大小超过了限定了阈值(2M)就进行写入
      // 然后继续循环
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    input->Next();
  }
  // 检查是否shutdown了
  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Deleting DB during compaction");
  }
  // 完成compact，output到file里
  if (status.ok() && compact->builder != NULL) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = NULL;
  // 记录compact的input大小
  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  // 记录compact的output大小
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }
  // 由于上面的IO操作都完成了，剩下要进行同步更新Version
  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log,
      "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

namespace {
struct IterState {
  port::Mutex* mu;
  Version* version;
  MemTable* mem;
  MemTable* imm;
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != NULL) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}
}  // namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  IterState* cleanup = new IterState;
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != NULL) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  cleanup->mu = &mutex_;
  cleanup->mem = mem_;
  cleanup->imm = imm_;
  cleanup->version = versions_->current();
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, NULL);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}
// DBImpl的Get方法
Status DBImpl::Get(const ReadOptions& options,
                   const Slice& key,
                   std::string* value) {
  Status s;
  // 先上锁
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  // 查看时候要进行在中的snapshot的Get
  if (options.snapshot != NULL) {
    snapshot = reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_;
  } else {
    snapshot = versions_->LastSequence();
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  // 引用增
  mem->Ref();
  if (imm != NULL) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  // 在memtable查询前，先解锁。提高并发性
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    // 新建LookupKey 加入snapshot作为sequenceNumber来限制查询
    // 依次在memtable，immutable，current Version中找
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, &s)) {
      // Done
    } else if (imm != NULL && imm->Get(lkey, value, &s)) {
      // Done
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    // 最后上锁，后面看看是否需要compact
    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  // 引用减
  mem->Unref();
  if (imm != NULL) imm->Unref();
  current->Unref();
  return s;
}
// DBImpl的迭代器就是NewDBIterator实现的
Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(
      this, user_comparator(), iter,
      (options.snapshot != NULL
       ? reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_
       : latest_snapshot),
      seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  MutexLock l(&mutex_);
  snapshots_.Delete(reinterpret_cast<const SnapshotImpl*>(s));
}

// Convenience methods
// DBImpl的Put方法，调用了DB::Put的方法
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}
// DBImpl的Delete方法，调用了DB::Delete的方法
Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}
// DBImpl的Write实现
Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  // 先创建一个Writer
  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.done = false;
  // 上锁
  MutexLock l(&mutex_);
  // 把Writer加入到writers_队列
  writers_.push_back(&w);
  // 一个个地等待writers_队列前面的writer完成，直到等到自己在队列的最前端
  // 这里是可以想象成同时又多个线程调用Write，但每个Write都需要先确保MakeRoomForWrite
  // 在找到Room后就可以解锁来提高并发(concurrent)性，如果没room就要靠这套while循环来避免洪水般的write
  // 也可以想象如果一直都有room，这套机制是依然可以并发(concurrent并发还是并行，看物理核数)的
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  // 这里之所以要判断是因为，在BuildBatchGroup中会组合多个batch，有可能就已经提前完成write了
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  // 不断循环等待可以写的空间
  Status status = MakeRoomForWrite(my_batch == NULL);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && my_batch != NULL) {  // NULL batch is for compactions
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    // 解锁提高写log，写memtable的并发性
    {
      mutex_.Unlock();
      // 先写log-file
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));
      bool sync_error = false;
      // 看是否需要Sync，防止OS crash
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      // 写入memtable
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_);
      }
      mutex_.Lock();
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        // logfile_的sync失败了，DB的下一次开启的时候，刚才新添加的log record不一定会在
        // 所以强制后面的write失败
        RecordBackgroundError(status);
      }
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();
    // writebatch后要更新VersionSet的LastSequence
    versions_->SetLastSequence(last_sequence);
  }
  // 把writer队列中比last_writer前面的所有writer全都设置为done
  // 因为前面BuildBatchGroup做了合并来提高性能
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  // 唤醒在writer队列的头部writer
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-NULL batch
// 把多个WriteBatch组成一个更大的一个Batch
// last-writer记录了最后一个合并的writer
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != NULL);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  // 这里分了情况来决定GroupBatch的大小上限
  // 如果当前的batch比较小(<= (128)<<10)
  // 则max-size也会较小，否则max-size就会为1 << 20
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      // 如果在试图要进行合并的batch中发现了一个batch指定了sync的话，就会group到这里为止
      // （不包括sync的batch）
      break;
    }
    // 开始组合batch
    if (w->batch != NULL) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  // 返回组合后的结果batch
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
// 要求：mutex_被拿到了，当前的线程是在writer queue的最前端
// 循环检测当前的db状态，确定策略
Status DBImpl::MakeRoomForWrite(bool force) {
  // force的意思大概是强制强制把memtable转化为immutalbe memtable，以便腾出空间
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      // 后台线程出错了，返回
      s = bg_error_;
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      // 如果现在的writer是allow_delay的，并且level-0的file的数量达到了
      // 要停止写的数量的时候，就要进行延迟写1ms。而这1ms的CPU可能会被用在compact 
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // There is room in current memtable
      // 这里表示当前的memtable的占用空间没超过阈值write_buffer_size
      // 可以进行write
      break;
    } else if (imm_ != NULL) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      // 当前的memtable已近满了，并且imm_还在没compact完成，所以要等待
      Log(options_.info_log, "Current memtable full; waiting...\n");
      bg_cv_.Wait();
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      // 如果当前level-0的文件数目多到超过了kL0_StopWritesTrigger的阈值就要停止写
      // 继续等待策略的决定
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      bg_cv_.Wait();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      assert(versions_->PrevLogNumber() == 0);
      // 创建一个先得log-file
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = NULL;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        // status表示出错，所以复用FileNumber
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      // 替代以前的log-file，在disk上还存在着
      delete log_;
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      // 把memtable转化为immutalbe memtable
      imm_ = mem_;
      has_imm_.Release_Store(imm_);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      // 把force改成false，force的意思大概是强制强制把memtable转化为immutalbe memtable
      // 以便腾出空间
      force = false;   // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d",
               versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
             "--------------------------------------------------\n"
             );
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        snprintf(
            buf, sizeof(buf),
            "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
            level,
            files,
            versions_->NumLevelBytes(level) / 1048576.0,
            stats_[level].micros / 1e6,
            stats_[level].bytes_read / 1048576.0,
            stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(
    const Range* range, int n,
    uint64_t* sizes) {
  // TODO(opt): better implementation
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  {
    MutexLock l(&mutex_);
    v->Unref();
  }
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
// DB的Put方法，其实是调用了Write方法的
// (这其实是DB-impl调用Put时会转到这里，而Write方法是通过多态接到了DB-impl的Write上)
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}
// DB的Delete方法，其实也是调用了Write方法
// (这其实是DB-impl调用Put时会转到这里，而Write方法是通过多态接到了DB-impl的Write上)
Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() { }
// DB::open的定义
Status DB::Open(const Options& options, const std::string& dbname,
                DB** dbptr) {
  *dbptr = NULL;
  // 先新建一个DBImpl
  // 最后会把*dbptr = impl这样就接通了DB::open调用者和内部实现的关系
  // 由于我们返回的是impl所以运用了多态的特性，后面调用者的Put，Delete，Write都会通过
  // 多态来接通到impl上
  DBImpl* impl = new DBImpl(options, dbname);
  // 上锁
  impl->mutex_.Lock();
  // 这个edit主要是记录log-file的重演的
  VersionEdit edit;
  // Recover handles create_if_missing, error_if_exists
  bool save_manifest = false;
  Status s = impl->Recover(&edit, &save_manifest);
  // 如果impl->mem_ == NULL则说明recover中没有resuse log-file
  if (s.ok() && impl->mem_ == NULL) {
    // Create new log and a corresponding memtable.
    // 创建log-file和memtable
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
    }
  }
  // save_manifest如果为false的话，就是reuse manifest，
  // 这样就可以不适用LogAndApply了，当然就算reuse了manifest，
  // 如果recover的中途log-file的重演过程中触发了compact也会有save_manifest = true
  // 这是就需要调用LogAndApply了。而我们所说的可以加速database的open是因为
  // 在versions_->recover中复用了manifest，
  // 而LogAndApply遇到已经有manifest的logWriter了，就会跳过那个全体db记录的Snapshot的write
  if (s.ok() && save_manifest) {
    // recover后不再需要old-log-file
    edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
    // 设置当前的logfile_number_
    edit.SetLogNumber(impl->logfile_number_);
    // 调用LogAndApply写到manifest和应用到version中
    s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }
  // logfile_如果没用reuse的话，上面的LogAndApply就已经把状态更新到最新的了
  // 如果reuse的话就会有logfile剩下来，Version就会没更新到最新的状态，还有一部分是在logfile中
  if (s.ok()) {
    // 上面的一切结束后就已经把最新的manifest写好了
    // DeleteObsoleteFiles就是删除没用的文件
    impl->DeleteObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }
  // 解锁
  impl->mutex_.Unlock();
  if (s.ok()) {
    assert(impl->mem_ != NULL);
    // 返回打开后的db
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Snapshot::~Snapshot() {
}
// 删除整个DB遍历db目录删除整个目录
Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  // Ignore error in case directory does not exist
  env->GetChildren(dbname, &filenames);
  if (filenames.empty()) {
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  Status result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->DeleteFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->DeleteFile(lockname);
    env->DeleteDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

}  // namespace leveldb
