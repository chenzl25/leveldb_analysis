// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The representation of a DBImpl consists of a set of Versions.  The
// newest version is called "current".  Older versions may be kept
// around to provide a consistent view to live iterators.
//
// Each Version keeps track of a set of Table files per level.  The
// entire set of versions is maintained in a VersionSet.
//
// Version,VersionSet are thread-compatible, but require external
// synchronization on all accesses.

#ifndef STORAGE_LEVELDB_DB_VERSION_SET_H_
#define STORAGE_LEVELDB_DB_VERSION_SET_H_

#include <map>
#include <set>
#include <vector>
#include "db/dbformat.h"
#include "db/version_edit.h"
#include "port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

namespace log { class Writer; }

class Compaction;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
// REQUIRES: "files" contains a sorted list of non-overlapping files.
// 返回最小的使得files[i]->largest >= key成立的i
// 如果没有就返回files.size()
// files 要是没有overlap的 (可以理解成：非level-0层)
extern int FindFile(const InternalKeyComparator& icmp,
                    const std::vector<FileMetaData*>& files,
                    const Slice& key);

// Returns true iff some file in "files" overlaps the user key range
// [*smallest,*largest].
// smallest==NULL represents a key smaller than all keys in the DB.
// largest==NULL represents a key largest than all keys in the DB.
// REQUIRES: If disjoint_sorted_files, files[] contains disjoint ranges
//           in sorted order.
// 判断时候对于给的的range里[*smallest,*largest]
// files里是否有file与其overlap
// 要求：如果disjoint_sorted_files == true
// 则files[] 里面每个file的range是不相交的
extern bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const std::vector<FileMetaData*>& files,
    const Slice* smallest_user_key,
    const Slice* largest_user_key);


/*===============================
=            Version            =
===============================*/

// 将每次compact后的最新数据状态定义为Version
// 也就是当前db元信息以及没层level上的最新数据状态的sstable集合
// compact在每个level上新加入或者删除的一些sstable。但这个时候可能一些
// 要被删除的sstable正在被读，为了处理这样的情况，每个Version会加入引用计数
// 读以及接触读操作会对应地+1或-1.而这样db就会可能有多个Version的存在
// Version就是通过链表来连接的，当不是最新的Version的引用为0的时候
// 它会从对应的链表中移除，Version的sstable就可以删除了（这些sstable会在
// 下一次的compact完成时候被清理掉）

/*=====  End of Version  ======*/

class Version {
 public:
  // Append to *iters a sequence of iterators that will
  // yield the contents of this Version when merged together.
  // REQUIRES: This version has been saved (see VersionSet::SaveTo)
  // 将version中的所有的用来遍历整个version的数据push到iters中
  void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

  // Lookup the value for key.  If found, store it in *val and
  // return OK.  Else return a non-OK status.  Fills *stats.
  // REQUIRES: lock is not held
  // 对于给定的key，找对应的value
  struct GetStats {
    FileMetaData* seek_file;
    int seek_file_level;
  };
  Status Get(const ReadOptions&, const LookupKey& key, std::string* val,
             GetStats* stats);

  // Adds "stats" into the current state.  Returns true if a new
  // compaction may need to be triggered, false otherwise.
  // REQUIRES: lock is held
  // 添加stats到current state. 如果一个新的compaction要被触发，就返回true，否则false
  bool UpdateStats(const GetStats& stats);

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.  Returns true if a new compaction may need to be triggered.
  // REQUIRES: lock is held
  // 用来对key(internal_key)进行RecordReadSample，具体什么意思看version_set.cc
  bool RecordReadSample(Slice key);

  // Reference count management (so Versions do not disappear out from
  // under live iterators)
  // Version引用计数的管理
  void Ref();
  void Unref();
  // 给定一个range，和level，拿到对应的files到input中
  // 对于level-0会有特殊处理（不同一般理解的overlap）
  void GetOverlappingInputs(
      int level,
      const InternalKey* begin,         // NULL means before all keys
      const InternalKey* end,           // NULL means after all keys
      std::vector<FileMetaData*>* inputs);

  // Returns true iff some file in the specified level overlaps
  // some part of [*smallest_user_key,*largest_user_key].
  // smallest_user_key==NULL represents a key smaller than all keys in the DB.
  // largest_user_key==NULL represents a key largest than all keys in the DB.
  // 如果给定的level中有何给定的range overlap的话就返回true
  bool OverlapInLevel(int level,
                      const Slice* smallest_user_key,
                      const Slice* largest_user_key);

  // Return the level at which we should place a new memtable compaction
  // result that covers the range [smallest_user_key,largest_user_key].
  // 返回我们应该把memtable的compaction产生的结果放到哪个level
  // memtable覆盖了range [smallest_user_key,largest_user_key]
  // 注意memtable有机会push到更高的level，具体看实现
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);
  // 返回给定level中file的数目
  int NumFiles(int level) const { return files_[level].size(); }

  // Return a human readable string that describes this version's contents.
  // 返回一个适合人阅读的用来描述Version的string
  std::string DebugString() const;

 private:
  friend class Compaction;
  friend class VersionSet;

  class LevelFileNumIterator;
  Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

  // Call func(arg, level, f) for every file that overlaps user_key in
  // order from newest to oldest.  If an invocation of func returns
  // false, makes no more calls.
  //
  // REQUIRES: user portion of internal_key == user_key.
  // 对于每个overlaps了user_key的file，调用func(arg, level, f)，按新到久的顺序
  // 如果其中一个函数返回了false，就停止该过程
  // ps：user_key要是internal_key的一部分
  void ForEachOverlapping(Slice user_key, Slice internal_key,
                          void* arg,
                          bool (*func)(void*, int, FileMetaData*));
  // 所属的versionSet
  VersionSet* vset_;            // VersionSet to which this Version belongs
  // Version是通过链表来链接在一起的
  Version* next_;               // Next version in linked list
  Version* prev_;               // Previous version in linked list
  // 该version的引用
  int refs_;                    // Number of live refs to this version

  // List of files per level
  // 每个level的所有sstable的元信息
  std::vector<FileMetaData*> files_[config::kNumLevels];

  // Next file to compact based on seek stats.
  // 需要compact的文件(allowed_seeks用光)
  FileMetaData* file_to_compact_;
  // file_to_compact_的level 
  int file_to_compact_level_;

  // Level that should be compacted next and its compaction score.
  // Score < 1 means compaction is not strictly needed.  These fields
  // are initialized by Finalize().
  // 当前最大的compact权重以及对应的level
  // 如果Score < 1就不需要compaction
  double compaction_score_;
  int compaction_level_;

  explicit Version(VersionSet* vset)
      : vset_(vset), next_(this), prev_(this), refs_(0),
        file_to_compact_(NULL),
        file_to_compact_level_(-1),
        compaction_score_(-1),
        compaction_level_(-1) {
  }

  ~Version();

  // No copying allowed
  Version(const Version&);
  void operator=(const Version&);
};
// VersionSet是Version的集合
// 管理着整个db的状态(SequenceNumber, FileNumber, 当前manifest_file_number，
// TableCache....)
class VersionSet {
 public:
  VersionSet(const std::string& dbname,
             const Options* options,
             TableCache* table_cache,
             const InternalKeyComparator*);
  ~VersionSet();

  // Apply *edit to the current version to form a new descriptor that
  // is both saved to persistent state and installed as the new
  // current version.  Will release *mu while actually writing to the file.
  // REQUIRES: *mu is held on entry.
  // REQUIRES: no other thread concurrently calls LogAndApply()
  // 把*edit应用到VersionSet中
  Status LogAndApply(VersionEdit* edit, port::Mutex* mu)
      EXCLUSIVE_LOCKS_REQUIRED(mu);

  // Recover the last saved descriptor from persistent storage.
  // 恢复到最后记录的descriptor的状态
  Status Recover(bool *save_manifest);

  // Return the current version.
  // 返回现在的Version
  Version* current() const { return current_; }

  // Return the current manifest file number
  // 返回现在的manifest的number
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  // Allocate and return a new file number
  // 返回一个新的FileNumber
  uint64_t NewFileNumber() { return next_file_number_++; }

  // Arrange to reuse "file_number" unless a newer file number has
  // already been allocated.
  // REQUIRES: "file_number" was returned by a call to NewFileNumber().
  // 重用file_number，其实就是后退一步
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  // Return the number of Table files at the specified level.
  // 返回给定level的file数目
  int NumLevelFiles(int level) const;

  // Return the combined file size of all files at the specified level.
  // 返回给定level中所有file的占用byte的总和
  int64_t NumLevelBytes(int level) const;

  // Return the last sequence number.
  // 返回最后用的sequence number
  uint64_t LastSequence() const { return last_sequence_; }

  // Set the last sequence number to s.
  // 设置最后一个用的sequence number
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  // Mark the specified file number as used.
  // 标记给定数字的file以表示正在使用
  void MarkFileNumberUsed(uint64_t number);

  // Return the current log file number.
  // 返回现在log file的number
  uint64_t LogNumber() const { return log_number_; }

  // Return the log file number for the log file that is currently
  // being compacted, or zero if there is no such log file.
  // 返回之前
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  // Pick level and inputs for a new compaction.
  // Returns NULL if there is no compaction to be done.
  // Otherwise returns a pointer to a heap-allocated object that
  // describes the compaction.  Caller should delete the result.
  // 选取level和inputs用于构造一个Compaction返回
  Compaction* PickCompaction();

  // Return a compaction object for compacting the range [begin,end] in
  // the specified level.  Returns NULL if there is nothing in that
  // level that overlaps the specified range.  Caller should delete
  // the result.
  // 返回一个Compaction用来compact 给定level的range
  // 如果没有这样的range在这level中就返回NULL
  Compaction* CompactRange(
      int level,
      const InternalKey* begin,
      const InternalKey* end);

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  int64_t MaxNextLevelOverlappingBytes();

  // Create an iterator that reads over the compaction inputs for "*c".
  // The caller should delete the iterator when no longer needed.
  // 创造一个可以迭代访问Compaction中input files的iterator
  Iterator* MakeInputIterator(Compaction* c);

  // Returns true iff some level needs a compaction.
  // 判断是否需要compact
  bool NeedsCompaction() const {
    Version* v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != NULL);
  }

  // Add all files listed in any live version to *live.
  // May also mutate some internal state.
  // 把还有iterator引用到的version放到live集合里
  void AddLiveFiles(std::set<uint64_t>* live);

  // Return the approximate offset in the database of the data for
  // "key" as of version "v".
  // 返回给定key在给定的version的db data里的offset
  uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

  // Return a human-readable short (single-line) summary of the number
  // of files per level.  Uses *scratch as backing store.
  // 返回一个leveldb的summary
  // scratch是backing store
  struct LevelSummaryStorage {
    char buffer[100];
  };
  const char* LevelSummary(LevelSummaryStorage* scratch) const;

 private:
  // 将VersionEdit应用到Version的一个helper类
  class Builder;

  friend class Compaction;
  friend class Version;

  bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

  void Finalize(Version* v);

  void GetRange(const std::vector<FileMetaData*>& inputs,
                InternalKey* smallest,
                InternalKey* largest);

  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2,
                 InternalKey* smallest,
                 InternalKey* largest);

  void SetupOtherInputs(Compaction* c);

  // Save current contents to *log
  Status WriteSnapshot(log::Writer* log);

  void AppendVersion(Version* v);
  // 实际的Env
  Env* const env_;
  // db的路径
  const std::string dbname_;
  // 传入的option
  const Options* const options_;
  // 操作sstable的TableCache
  TableCache* const table_cache_;
  // InternalKey比较器
  const InternalKeyComparator icmp_;
  // 下一个可用的fileNumber
  uint64_t next_file_number_;
  // manifest文件的FileNumber
  uint64_t manifest_file_number_;
  // 最后用过的SequenceNumber
  uint64_t last_sequence_;
  // log文件的FileNumber
  uint64_t log_number_;
  // 辅助log文件的FileNumber，在compact memtable时，置为0
  uint64_t prev_log_number_;  // 0 or backing store for memtable being compacted

  // Opened lazily
  // manifest 文件的封装
  WritableFile* descriptor_file_;
  // manifest文件的writer
  log::Writer* descriptor_log_;
  // 正在服务的Version的链表
  Version dummy_versions_;  // Head of circular doubly-linked list of versions.
  // 当前最新的Version
  Version* current_;        // == dummy_versions_.prev_

  // Per-level key at which the next compaction at that level should start.
  // Either an empty string, or a valid InternalKey.
  // 为了尽量均匀compact每个level
  // 所以会将这一次compact的end-key作为
  // 下一次compact的start-key
  // compact_pointer_则是保存了每个level下一次compact时的start-key
  // 又因为只有current_这个Version会进行compact，所以compact_pointer_只保存在Version_Set中
  std::string compact_pointer_[config::kNumLevels];

  // No copying allowed
  VersionSet(const VersionSet&);
  void operator=(const VersionSet&);
};

// A Compaction encapsulates information about a compaction.
// compaction类
class Compaction {
 public:
  ~Compaction();

  // Return the level that is being compacted.  Inputs from "level"
  // and "level+1" will be merged to produce a set of "level+1" files.
  // 返回正在被compact的level
  int level() const { return level_; }

  // Return the object that holds the edits to the descriptor done
  // by this compaction.
  // 返回一个根据compaction来生成的VersionEdit，用于descriptor file
  VersionEdit* edit() { return &edit_; }

  // "which" must be either 0 or 1
  // 返回level-n 或 level-n+1 用与compact的file的个数
  // which只能是0 或 1
  int num_input_files(int which) const { return inputs_[which].size(); }

  // Return the ith input file at "level()+which" ("which" must be 0 or 1).
  // 返回返回level-n 或 level-n+1中第i个file
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  // Maximum size of files to build during this compaction.
  // 返回max_output_file_size_（compact生成的file的大小限制）
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // Is this a trivial compaction that can be implemented by just
  // moving a single input file to the next level (no merging or splitting)
  // 判断这是否是一个trivial的compaction
  // 即不用merge，就直接把level-n的file放到level-n+1
  bool IsTrivialMove() const;

  // Add all inputs to this compaction as delete operations to *edit.
  // 把所有的input files 到edit的deletion中
  void AddInputDeletions(VersionEdit* edit);

  // Returns true if the information we have available guarantees that
  // the compaction is producing data in "level+1" for which no data exists
  // in levels greater than "level+1".
  // 
  bool IsBaseLevelForKey(const Slice& user_key);

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  // 返回true，当且仅当我们在处理internal_key前应该停止产生output
  bool ShouldStopBefore(const Slice& internal_key);

  // Release the input version for the compaction, once the compaction
  // is successful.
  // release input version，一旦compaction成功
  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionSet;

  explicit Compaction(int level);
  // 要compact的level
  int level_;
  // 生成sstable的最大size
  uint64_t max_output_file_size_;
  // compact时当前的Version
  Version* input_version_;
  // 记录compact过程中的操作
  VersionEdit edit_;

  // Each compaction reads inputs from "level_" and "level_+1"
  // input[0]是level-n的sstable的信息
  // input[1]是level-n+1的sstable的信息
  std::vector<FileMetaData*> inputs_[2];      // The two sets of inputs

  // State used to check for number of of overlapping grandparent files
  // (parent == level_ + 1, grandparent == level_ + 2)
  // 用来记录位于levele-n+2，并且与compact的key-range有overlap的sstable
  // 保存levele-n+2的目的是为了避免compact生成的在levele-n+1的sstable 和levele-n+2的sstable
  // 有过多的overlap，否则compact levele-n+1层的时候会造成过多的merge 
  std::vector<FileMetaData*> grandparents_;
  // 记录grandparents_中已经overlap的index
  size_t grandparent_index_;  // Index in grandparent_starts_
  // 记录是否已经有key检查overlap
  bool seen_key_;             // Some output key has been seen
  // 记录compact产生的file和grandparents_的file已经overlap的byte数
  int64_t overlapped_bytes_;  // Bytes of overlap between current output
                              // and grandparent files

  // compact时，当key的ValueType是kTypeDeletion时
  // 要检查其在level-n+1以上是否存在（IsBaseLevelForKey）
  // 来决定是否丢弃该key，
  // State for implementing IsBaseLevelForKey

  // level_ptrs_ holds indices into input_version_->levels_: our state
  // is that we are positioned at one of the file ranges for each
  // higher level than the ones involved in this compaction (i.e. for
  // all L >= level_ + 2).
  // 接上面，因为compact时，key的遍历是顺序的，
  // 所以每次建厂上一次检查结束的地方开始即可
  // level_ptrs_[i]中就记录了input_version_->levels_[i]中上一次比较结束的sstable容器的下标
  size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H_
