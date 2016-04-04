// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_set.h"

#include <algorithm>
#include <stdio.h>
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "leveldb/env.h"
#include "leveldb/table_builder.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {
// sstable文件的大小
static const int kTargetFileSize = 2 * 1048576;

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
// compact过程中最大可容忍的(level-n和level-n+2)overlap的bytes
static const int64_t kMaxGrandParentOverlapBytes = 10 * kTargetFileSize;

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
// 我们的每一个compact都有一个expeand的机会，这个expand会让我们可以更大范围地compact
// 但总的compact不能超过这个阈值
// 详细用法见“SetupOtherInputs”函数
static const int64_t kExpandedCompactionByteSizeLimit = 25 * kTargetFileSize;
// 这里是计算出每一层level的最大占用的bytes
// 其中不包括level-0，level-0是用file的数量来限定的（因为它是memtable dump下来的，大小可能不固定）
static double MaxBytesForLevel(int level) {
  // Note: the result for level zero is not really used since we set
  // the level-0 compaction threshold based on number of files.
  double result = 10 * 1048576.0;  // Result for both level-0 and level-1
  while (level > 1) {
    result *= 10;
    level--;
  }
  return result;
}
// 每层的file的最大size的大小，现在都是一样的，以后可以修改
static uint64_t MaxFileSizeForLevel(int level) {
  return kTargetFileSize;  // We could vary per level to reduce number of files?
}
// 给定一个FileMetaData的vector，计算总共的大小
static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}
// Version的析构函数
Version::~Version() {
  // 析构的时候肯定是要保证引用refs = 0的
  assert(refs_ == 0);

  // Remove from linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  // 当前的Version要被析构的时候会对其所引用到的sstable元信息进行解引用，当引用<=0是delete
  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData* f = files_[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        delete f;
      }
    }
  }
}
// 给定一组files，一个InternalKeyComparator，一个key
// 用二分查找来找距离key最近的file
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files,
             const Slice& key) {
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData* f = files[mid];
    if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      right = mid;
    }
  }
  return right;
}
// 判断一个userkey是否在给定的file后面
// 用的是userKey-comparator
// 当user_key是NULL时表示最小
static bool AfterFile(const Comparator* ucmp,
                      const Slice* user_key, const FileMetaData* f) {
  // NULL user_key occurs before all keys and is therefore never after *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}
// 判断一个userkey是否在给定的file前面
// 用的是userKey-comparator
// 当user_key是NULL时表示最大
static bool BeforeFile(const Comparator* ucmp,
                       const Slice* user_key, const FileMetaData* f) {
  // NULL user_key occurs after all keys and is therefore never before *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}
// 给定一组files，一个range，一个InternalKeyComparator
// 还有判定是否互不重叠的disjoint_sorted_files
// 返回是否给定的range与files有重叠
bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const std::vector<FileMetaData*>& files,
    const Slice* smallest_user_key,
    const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    // Need to check against all files
    // 线性逐一比较
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // Binary search over file list
  // 用FindFile来完成，FindFile是二分查找找到距离key最近的file
  uint32_t index = 0;
  if (smallest_user_key != NULL) {
    // Find the earliest possible internal key for smallest_user_key
    InternalKey small(*smallest_user_key, kMaxSequenceNumber,kValueTypeForSeek);
    index = FindFile(icmp, files, small.Encode());
  }

  if (index >= files.size()) {
    // beginning of range is after all files, so no overlap.
    return false;
  }

  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
// 内部的迭代器，用来迭代给定(version)level的files，
// 其中key返回当前file中的最大的key
// value返回一个16byte的数字，其中包括了file的FileNumber和size，它们均是EncodeFixed64
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>* flist)
      : icmp_(icmp),
        flist_(flist),
        index_(flist->size()) {        // Marks as invalid
  }
  virtual bool Valid() const {
    return index_ < flist_->size();
  }
  // seek用FindFile来实现
  virtual void Seek(const Slice& target) {
    index_ = FindFile(icmp_, *flist_, target);
  }
  // SeekToFirst则指向第一个
  virtual void SeekToFirst() { index_ = 0; }
  // SeekToLast就是最后一个
  virtual void SeekToLast() {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  // 下一个
  virtual void Next() {
    assert(Valid());
    index_++;
  }
  // Prev，当index_==0是如果还想向前就会index_ = flist_->size(); 使得Valid返回false
  virtual void Prev() {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();  // Marks as invalid
    } else {
      index_--;
    }
  }
  // 当前file的最大键的encode
  Slice key() const {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  // fileNumber和size的encode
  Slice value() const {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_+8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  virtual Status status() const { return Status::OK(); }
 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData*>* const flist_;
  // 存放当前iter所指向file在flist的index
  uint32_t index_;

  // Backing store for value().  Holds the file number and size.
  // mutable的用法，使得value()这个被const修饰的方法可以修改value_buf_变量
  mutable char value_buf_[16];
};
// 用于NewConcatenatingIterator的将LevelFileNumIterator转换成tableCache的NewIterator
// 而tableCache的NewIterator又会调用table的NewTwoLevelIterator
// table的NewTwoLevelIterator则会继续下去把table的全部record都读出来
static Iterator* GetFileIterator(void* arg,
                                 const ReadOptions& options,
                                 const Slice& file_value) {
  TableCache* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16) {
    return NewErrorIterator(
        Status::Corruption("FileReader invoked with unexpected value"));
  } else {
    return cache->NewIterator(options,
                              DecodeFixed64(file_value.data()),
                              DecodeFixed64(file_value.data() + 8));
  }
}
// NewTwoLevelIterator再次闪亮登场，之前是table，block，record这层的
// 现在是files，table，block，record这层一路下去，因为，twoLevelIterator，在table->NewIterator中也用到了
// 抽象的好处运用得淋漓尽致
// 要是忘记了LevelFileNumIterator的构造可以去查
Iterator* Version::NewConcatenatingIterator(const ReadOptions& options,
                                            int level) const {
  return NewTwoLevelIterator(
      new LevelFileNumIterator(vset_->icmp_, &files_[level]),
      &GetFileIterator, vset_->table_cache_, options);
}
// 将Version中的所有的sstable的iterator push到iters中
// 对于level-0因为可能会有overlap，所以单独对每一个file创建一个iter来加入
// 对于level-n(n > 0)则使用上面提到的NewConcatenatingIterator来遍历某一level的iterator
void Version::AddIterators(const ReadOptions& options,
                           std::vector<Iterator*>* iters) {
  // Merge all level zero files together since they may overlap
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters->push_back(
        vset_->table_cache_->NewIterator(
            options, files_[0][i]->number, files_[0][i]->file_size));
  }

  // For levels > 0, we can use a concatenating iterator that sequentially
  // walks through the non-overlapping files in the level, opening them
  // lazily.
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

// Callback from TableCache::Get()
// 这是用在TableCache::Get()的方法
// 当TableCache::Get()找到ikey/value的时候就会调用
// 其中SaveValue的arg是Saver*而当找到了ikey和value时候
// 就会更新对应Saver*的状态和值
namespace {
enum SaverState {
  kNotFound,
  kFound,
  kDeleted,
  kCorrupt,
};
struct Saver {
  SaverState state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;
};
}
// 在table中的GET都是用iterator的seek来实现的（seek可以确保snapshot）
// 所以查找的key不一定准确
// 所以还差那不比较相等的逻辑在这里
static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else {
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      if (s->state == kFound) {
        s->value->assign(v.data(), v.size());
      }
    }
  }
}
// 判断文件a比文件b更新
// 用来排序的
static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
  return a->number > b->number;
}
// 对于每个overlaps了user_key的file，调用func(arg, level, f)，按新到久的顺序
// 如果其中一个函数返回了false，就停止该过程
// ps：user_key要是internal_key的一部分(为了调用FindFile函数)
void Version::ForEachOverlapping(Slice user_key, Slice internal_key,
                                 void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
  // TODO(sanjay): Change Version::Get() to use this function.
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  // Search level-0 in order from newest to oldest.
  // 从level-0开始找
  std::vector<FileMetaData*> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    FileMetaData* f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(f);
    }
  }
  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!(*func)(arg, 0, tmp[i])) {
        return;
      }
    }
  }

  // Search other levels.
  // level-n(n>0)找，方法和下面的Version::Get用得方法差不多
  for (int level = 1; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Binary search to find earliest index whose largest key >= internal_key.
    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      FileMetaData* f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
        // All of "f" is past any data for user_key
      } else {
        if (!(*func)(arg, level, f)) {
          return;
        }
      }
    }
  }
}
// GetStats的数据结构
// struct GetStats {
//   FileMetaData* seek_file;
//   int seek_file_level;
// };
// 这里用到了LookupKey来查找
Status Version::Get(const ReadOptions& options,
                    const LookupKey& k,
                    std::string* value,
                    GetStats* stats) {
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();
  // vset_是versionSet
  const Comparator* ucmp = vset_->icmp_.user_comparator();
  Status s;

  stats->seek_file = NULL;
  stats->seek_file_level = -1;
  FileMetaData* last_file_read = NULL;
  int last_file_read_level = -1;

  // We can search level-by-level since entries never hop across
  // levels.  Therefore we are guaranteed that if we find data
  // in an smaller level, later levels are irrelevant.
  // 从level-0往level-n一直查
  std::vector<FileMetaData*> tmp;
  FileMetaData* tmp2;
  for (int level = 0; level < config::kNumLevels; level++) {
    // 该层没文件
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Get the list of files to search in this level
    // 第level层的头指针
    FileMetaData* const* files = &files_[level][0];
    // 这里都是找出可能存在的file
    if (level == 0) {
      // Level-0 files may overlap each other.  Find all files that
      // overlap user_key and process them in order from newest to oldest.
      // level-0是有可能overlap的所以要先遍历找出重复的，再来排序找出最新的file
      tmp.reserve(num_files);
      for (uint32_t i = 0; i < num_files; i++) {
        FileMetaData* f = files[i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
            ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
          tmp.push_back(f);
        }
      }
      if (tmp.empty()) continue;
      // 让fileNumber从大到小排
      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      // tmp的首地址赋值到files指针中
      files = &tmp[0];
      // size改变
      num_files = tmp.size();
    } else {
      // Binary search to find earliest index whose largest key >= ikey.
      // level-n(n>0)则二分查找，此时的files_[level]有序
      uint32_t index = FindFile(vset_->icmp_, files_[level], ikey);
      if (index >= num_files) {
        files = NULL;
        num_files = 0;
      } else {
        tmp2 = files[index];
        // 如果user_key比该层的file最小的key都小就不在这层了
        if (ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0) {
          // All of "tmp2" is past any data for user_key
          files = NULL;
          num_files = 0;
        } else {
          // 否则找到了
          files = &tmp2;
          num_files = 1;
        }
      }
    }

    for (uint32_t i = 0; i < num_files; ++i) {
      if (last_file_read != NULL && stats->seek_file == NULL) {
        // We have had more than one seek for this read.  Charge the 1st file.
        stats->seek_file = last_file_read;
        stats->seek_file_level = last_file_read_level;
      }

      FileMetaData* f = files[i];
      last_file_read = f;
      last_file_read_level = level;

      Saver saver;
      saver.state = kNotFound;
      saver.ucmp = ucmp;
      saver.user_key = user_key;
      saver.value = value;
      s = vset_->table_cache_->Get(options, f->number, f->file_size,
                                   ikey, &saver, SaveValue);
      if (!s.ok()) {
        return s;
      }
      // 当前的file可能不存在这个key(kNotFound),因为这只是可能在范围内，继续找
      // kFound当然就是找到了，可以返回了
      // 也有可能kDeleted掉了，因为从最新的找起，所以可以断定是被删除了
      // kCorrupt也就是出错了(上面的Parsekey错误)
      switch (saver.state) {
        case kNotFound:
          break;      // Keep searching in other files
        case kFound:
          return s;
        case kDeleted:
          s = Status::NotFound(Slice());  // Use empty error message for speed
          return s;
        case kCorrupt:
          s = Status::Corruption("corrupted key for ", user_key);
          return s;
      }
    }
  }

  return Status::NotFound(Slice());  // Use an empty error message for speed
}
// 当Version::Get方法找到了key之后会得到GetStats
// 接下来要进行file的allowed_seeks--
// 如果allowed_seeks <= 0了，同时file_to_compact_ == NULL
// 那么file_to_compact_就要被赋值成要compact的file
// 还有file_to_compact_level_被设置成对应的level
// 同时返回true
// 否则返回false
bool Version::UpdateStats(const GetStats& stats) {
  FileMetaData* f = stats.seek_file;
  if (f != NULL) {
    f->allowed_seeks--;
    if (f->allowed_seeks <= 0 && file_to_compact_ == NULL) {
      file_to_compact_ = f;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

// 对于传进来的internal_key
// 在本Version中用ForEachOverlapping来对其有overlap的file进行记录，
// 如果在这里的方法找到第一个file会记录下来
// 如果找到了第二个file就停止(或者根本找不到第二个file)
// 最后如果找到了2个file就会对第一个曾经记录下来的file进行UpdateStats()
// allowed_seeks就会改变
bool Version::RecordReadSample(Slice internal_key) {
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }

  struct State {
    GetStats stats;  // Holds first matching file
    int matches;

    static bool Match(void* arg, int level, FileMetaData* f) {
      State* state = reinterpret_cast<State*>(arg);
      state->matches++;
      if (state->matches == 1) {
        // Remember first match.
        state->stats.seek_file = f;
        state->stats.seek_file_level = level;
      }
      // We can stop iterating once we have a second match.
      return state->matches < 2;
    }
  };

  State state;
  state.matches = 0;
  ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

  // Must have at least two matches since we want to merge across
  // files. But what if we have a single file that contains many
  // overwrites and deletions?  Should we have another mechanism for
  // finding such files?
  // 这里作者发起了一个疑问
  // 就是如果一个file中出现了多个重复的key怎么办，如果是这样的话理应也要进行merge来减少
  // 但是现在的机制提供的是必须找到第二个file才会进行UpdateStats
  if (state.matches >= 2) {
    // 1MB cost is about 1 seek (see comment in Builder::Apply).
    return UpdateStats(state.stats);
  }
  return false;
}
// 对Version进行引用增
void Version::Ref() {
  ++refs_;
}
// 对Version进行引用减，如果refs == 0 就删除该Version
void Version::Unref() {
  // &vset_->dummy_versions_是VersionSet的链表
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}
// 在Version中给定的level和range判断是否有overlap
// 内部调用了SomeFileOverlapsRange
// 说实话这里函数的名字就很清晰了
bool Version::OverlapInLevel(int level,
                             const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}
// 回我们应该把memtable的compaction产生的结果放到哪个level
// 如果给定的range和level-0有overlap就返回0
// 如果和level-0没有overlap就可以有机会推到更高的level
// 但是必须满足两个条件
// 1.和parent-level没有overlap
// 2.和grandparent-level的overlap不能过大（否则在parent-level做compact就会merge得很慢）
int Version::PickLevelForMemTableOutput(
    const Slice& smallest_user_key,
    const Slice& largest_user_key) {
  int level = 0;
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<FileMetaData*> overlaps;
    while (level < config::kMaxMemCompactLevel) {
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      if (level + 2 < config::kNumLevels) {
        // Check that file does not overlap too many grandparent bytes.
        GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
        const int64_t sum = TotalFileSize(overlaps);
        if (sum > kMaxGrandParentOverlapBytes) {
          break;
        }
      }
      level++;
    }
  }
  return level;
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// 给定一个range，和level，拿到对应的files到input中
// begin和end可以使NULL，分别对应着无穷小和无穷大
// 对于level-0会有特殊处理（不同一般理解的overlap）
void Version::GetOverlappingInputs(
    int level,
    const InternalKey* begin,
    const InternalKey* end,
    std::vector<FileMetaData*>* inputs) {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != NULL) {
    user_begin = begin->user_key();
  }
  if (end != NULL) {
    user_end = end->user_key();
  }
  const Comparator* user_cmp = vset_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size(); ) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != NULL && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != NULL && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs->push_back(f);
      // 对于level-0会特殊处理
      // 如果level-0中的file的start < user_begin会把user_begin赋值为file_start
      // 如果level-0中的file的limit > user_end会把user_end赋值为file_limit
      // 并重新开始（因为level-0的file是乱序的）
      // 所以level-0的overlap会像是链式反应一样传播出去
      if (level == 0) {
        // Level-0 files may overlap each other.  So check if the newly
        // added file has expanded the range.  If so, restart search.
        if (begin != NULL && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != NULL && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}
// 打印出当前Version的file的信息，用来debug
std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" ---\n");
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      AppendNumberTo(&r, files[i]->number);
      r.push_back(':');
      AppendNumberTo(&r, files[i]->file_size);
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append("]\n");
    }
  }
  return r;
}

// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
// 将VersionEdit应用到Version上的过程封装成VersionSet::Builder
// 主要是更新Version::files[]
class VersionSet::Builder {
 private:
  // Helper to sort by v->files_[file_number].smallest
  // 定义了用于排序FileMetaData的比较方法
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      int r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        // Break ties by file number
        return (f1->number < f2->number);
      }
    }
  };
  // 排序的sstable（FileMetaData）的集合
  typedef std::set<FileMetaData*, BySmallestKey> FileSet;
  // 要添加和删除的sstable集合
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet* added_files;
  };
  // 要更新的VersionSet
  VersionSet* vset_;
  // 基准的Version，compact后，将VersionSet的current_传入作为base
  Version* base_;
  // 各个level上要更新的文件集合
  LevelState levels_[config::kNumLevels];

 public:
  // Initialize a builder with the files from *base and other info from *vset
  // 构造函数
  // 新建一个VersionSet::Builder，主要是对要更新的levels_进行初始化，
  // 还有Version base_的引用增
  Builder(VersionSet* vset, Version* base)
      : vset_(vset),
        base_(base) {
    base_->Ref();
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      levels_[level].added_files = new FileSet(cmp);
    }
  }
  // 析构函数
  // 都是对需要更新的levels_的所有file进行引用减，如果到了0就delete
  // 还有就是Version base_的引用减
  ~Builder() {
    for (int level = 0; level < config::kNumLevels; level++) {
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      for (FileSet::const_iterator it = added->begin();
          it != added->end(); ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }
    base_->Unref();
  }

  // Apply all of the edits in *edit to the current state.
  // 将edit的所有操作一次性地作用到base_中
  void Apply(VersionEdit* edit) {
    // Update compaction pointers
    // 把VersionEdit的compact_pointers_设置到VersionSet中以供compact
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] =
          edit->compact_pointers_[i].second.Encode().ToString();
    }

    // Delete files
    // 把VersionEdit的delete file放到要更新的level_的对应位置
    const VersionEdit::DeletedFileSet& del = edit->deleted_files_;
    for (VersionEdit::DeletedFileSet::const_iterator iter = del.begin();
         iter != del.end();
         ++iter) {
      const int level = iter->first;
      const uint64_t number = iter->second;
      levels_[level].deleted_files.insert(number);
    }

    // Add new files
    // 把VersionEdit的new file放到要更新的level_的对应位置
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      // 将edit->new_files_中的FileMetaData拷贝构造到新的f中
      // 同时对f引用增
      FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
      f->refs = 1;

      // We arrange to automatically compact this file after
      // a certain number of seeks.  Let's assume:
      //   (1) One seek costs 10ms
      //   (2) Writing or reading 1MB costs 10ms (100MB/s)
      //   (3) A compaction of 1MB does 25MB of IO:
      //         1MB read from this level
      //         10-12MB read from next level (boundaries may be misaligned)
      //         10-12MB written to next level
      // This implies that 25 seeks cost the same as the compaction
      // of 1MB of data.  I.e., one seek costs approximately the
      // same as the compaction of 40KB of data.  We are a little
      // conservative and allow approximately one seek for every 16KB
      // of data before triggering a compaction.
      // 这就是那岩翻译过来的allowed_seeks根据吧，还以为是他自己想出来的。。。。
      f->allowed_seeks = (f->file_size / 16384);
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;
      // 这里如果新增的file在deleted_filesh就会把deleted_files中的file去掉来报纸一致性
      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files->insert(f);
    }
  }

  // Save the current state in *v.
  // 把Edit作用后的的Version存储到v中
  void SaveTo(Version* v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    // 逐个level循环
    for (int level = 0; level < config::kNumLevels; level++) {
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      // 获取待更新的Version  base_的files
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      // 获取base_files的迭代器的begin
      std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
      // 获取base_files的迭代器的end
      std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
      // 获取添加到到待更新的Version  base_的file
      const FileSet* added = levels_[level].added_files;
      // 创建合适的大小
      v->files_[level].reserve(base_files.size() + added->size());
      // 迭代added_files和base_files，重要的是要保持顺序
      // file中smallest小的在最前面
      // 看Jeff Dean大神对stl库运用多熟悉，upper_bound我是没用过
      // 在这里我也不会想到用upper_bound来实现
      for (FileSet::const_iterator added_iter = added->begin();
           added_iter != added->end();
           ++added_iter) {
        // Add all smaller files listed in base_
        for (std::vector<FileMetaData*>::const_iterator bpos
                 = std::upper_bound(base_iter, base_end, *added_iter, cmp);
             base_iter != bpos;
             ++base_iter) {
          MaybeAddFile(v, level, *base_iter);
        }

        MaybeAddFile(v, level, *added_iter);
      }

      // Add remaining base files
      // 把剩余的base_files加入进去
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(v, level, *base_iter);
      }

#ifndef NDEBUG
      // Make sure there is no overlap in levels > 0
      // 用了debug使用的，确保file没有overlap
      if (level > 0) {
        for (uint32_t i = 1; i < v->files_[level].size(); i++) {
          const InternalKey& prev_end = v->files_[level][i-1]->largest;
          const InternalKey& this_begin = v->files_[level][i]->smallest;
          if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
            fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                    prev_end.DebugString().c_str(),
                    this_begin.DebugString().c_str());
            abort();
          }
        }
      }
#endif
    }
  }
  // 对于给定的要添加到v的level层的f
  // 先要看看是否在levels_的deleted_files中出现过
  // 再则要确保没有overlap
  // 最后才添加进去
  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // File is deleted: do nothing
    } else {
      std::vector<FileMetaData*>* files = &v->files_[level];
      if (level > 0 && !files->empty()) {
        // Must not overlap
        assert(vset_->icmp_.Compare((*files)[files->size()-1]->largest,
                                    f->smallest) < 0);
      }
      f->refs++;
      files->push_back(f);
    }
  }
};
// 以下就是VersionSet的方法了
// 构造函数
VersionSet::VersionSet(const std::string& dbname,
                       const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(NULL),
      descriptor_log_(NULL),
      dummy_versions_(this),
      current_(NULL) {
  // 顺带创建一个Version,current
  AppendVersion(new Version(this));
}
// 析构函数
// Unref完当前的Versioncurrent_后还要保证没有其他还在用的Version了
// 其实也就是要保证iterator用完要删除
VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
  // file要删除
  delete descriptor_log_;
  delete descriptor_file_;
}
// 添加Version，就是简单的双向链表插入
void VersionSet::AppendVersion(Version* v) {
  // Make "v" current
  assert(v->refs_ == 0);
  assert(v != current_);
  // 如果没有current就令这个version变成current
  if (current_ != NULL) {
    current_->Unref();
  }
  current_ = v;
  // 引用增
  v->Ref();

  // Append to linked list
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}
// 把*edit应用到VersionSet中
// 同时把过程记录到descriptor(manifest）文件中
// 同时会弄出一个新的current Version
Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
  // 先对edit的has_log_number_进行判断，有则检查，没则新建
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }
  // edit的SetPrevLogNumber
  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }
  // edit的SetNextFile和SetLastSequence设置为VersionSet的当前值
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);
  // 新建一个新的version，用来作为current的Version
  // 利用VersionSet:;Builder来把edit 应用current_中并把结果存到v中
  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  // 计算每level的scroe，来找要compact的level
  Finalize(v);

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == NULL) {
    // No reason to unlock *mu here since we only hit this path in the
    // first call to LogAndApply (when opening the database).
    // 这个方法只会在第一次用LogAndApply时(打开database的时候）,
    // 但也可能会复用了descriptor_log_就不在WriteSnapshot
    // 所以没必要那么快进行mu->Unlock()
    // PS：这个方法的运行需要lock了mu先
    assert(descriptor_file_ == NULL);
    // 这里用到的manifest_file_number_会在Recover函数中被赋值，这里安心用就好了
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    edit->SetNextFile(next_file_number_);
    // 新建manifest_file
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      // 新建一个descriptor_log_(把record记录到descriptior的一个Writer)
      descriptor_log_ = new log::Writer(descriptor_file_);
      // 把snapshot记录下来,snapshot是根据current的Version来记录的
      // 不包括edit
      s = WriteSnapshot(descriptor_log_);
    }
  }

  // Unlock during expensive MANIFEST log write
  {
    // 可以解锁了,可能是进行IO操作
    mu->Unlock();

    // Write new record to MANIFEST log
    // 把应用的edit log到descriptor中
    // 这里的descriptor可以使复用的也可以是，上面新建的
    if (s.ok()) {
      std::string record;
      edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
      if (s.ok()) {
        // Sync到disk中
        s = descriptor_file_->Sync();
      }
      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
      }
    }

    // If we just created a new descriptor file, install it by writing a
    // new CURRENT file that points to it.
    // 如果新建了一个manifest，则要创建一个新的CURRENT file来指向它
    if (s.ok() && !new_manifest_file.empty()) {
      s = SetCurrentFile(env_, dbname_, manifest_file_number_);
    }
    // 再次上锁
    mu->Lock();
  }

  // Install the new version
  // 把新的Version加入到VersionSet中
  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    // 如果失败的处理
    // 如果是刚刚新建了manifest就把manifest删除
    // 如果不是就不管(当然这里会出现corrputed manifest的情况，要依靠RepairDB来完成)
    delete v;
    if (!new_manifest_file.empty()) {
      delete descriptor_log_;
      delete descriptor_file_;
      descriptor_log_ = NULL;
      descriptor_file_ = NULL;
      env_->DeleteFile(new_manifest_file);
    }
  }

  return s;
}
// 从save_manifest中恢复出最后的状态
Status VersionSet::Recover(bool *save_manifest) {
  // 先继承虚基类log::Reader::Reporter构造出一个类
  // 其中把报错的status都收集起来
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      if (this->status->ok()) *this->status = s;
    }
  };

  // Read "CURRENT" file, which contains a pointer to the current manifest file
  // 读 "CURRENT" file，其中current包含了manifest的file number像这样“MANIFEST-000004”
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current[current.size()-1] != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }

  current.resize(current.size() - 1);
  // 构造出db的manifest的名字。ps：dbname_也就是目录名
  std::string dscname = dbname_ + "/" + current;
  SequentialFile* file;
  // 打开manifest文件
  s = env_->NewSequentialFile(dscname, &file);
  if (!s.ok()) {
    return s;
  }

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  // 开始构造最新的current Version
  Builder builder(this, current_);

  {
    // 新建一个Log::Reader
    LogReporter reporter;
    reporter.status = &s;
    log::Reader reader(file, &reporter, true/*checksum*/, 0/*initial_offset*/);
    Slice record;
    std::string scratch;
    // 逐个record读出来，在这里就是多个VersionEdit
    // 头一个是完整的Version，后面都是对第一个Version来进行修改
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (s.ok()) {
        if (edit.has_comparator_ &&
            edit.comparator_ != icmp_.user_comparator()->Name()) {
          s = Status::InvalidArgument(
              edit.comparator_ + " does not match existing comparator ",
              icmp_.user_comparator()->Name());
        }
      }

      if (s.ok()) {
        builder.Apply(&edit);
      }

      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }

      if (edit.has_prev_log_number_) {
        prev_log_number = edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
  }

  delete file;
  file = NULL;

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    } else if (!have_log_number) {
      s = Status::Corruption("no meta-lognumber entry in descriptor");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence-number entry in descriptor");
    }

    if (!have_prev_log_number) {
      prev_log_number = 0;
    }
    // 记录log file已经用了
    MarkFileNumberUsed(prev_log_number);
    MarkFileNumberUsed(log_number);
  }

  if (s.ok()) {
    // 将上面一直累加edit得到的最终Version记录下来，
    Version* v = new Version(this);
    builder.SaveTo(v);
    // Install recovered version
    // 计算各level scroe
    Finalize(v);
    // 加入到VersionSet中
    AppendVersion(v);
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;

    // See if we can reuse the existing MANIFEST file.
    if (ReuseManifest(dscname, current)) {
      // No need to save new manifest
    } else {
      *save_manifest = true;
    }
  }

  return s;
}
// 
bool VersionSet::ReuseManifest(const std::string& dscname,
                               const std::string& dscbase) {
  // 实验功能，如果为true就append到已经存在的manifest和log中
  // 加速功效，加速database的打开，因为每次打开多会重新写一遍manifest以更新到最新的状态
  if (!options_->reuse_logs) {
    return false;
  }
  FileType manifest_type;
  uint64_t manifest_number;
  uint64_t manifest_size;
  if (!ParseFileName(dscbase, &manifest_number, &manifest_type) ||
      manifest_type != kDescriptorFile ||
      !env_->GetFileSize(dscname, &manifest_size).ok() ||
      // Make new compacted MANIFEST if old one is too big
      manifest_size >= kTargetFileSize) {
    return false;
  }

  assert(descriptor_file_ == NULL);
  assert(descriptor_log_ == NULL);
  // 利用已经存在的manifest
  Status r = env_->NewAppendableFile(dscname, &descriptor_file_);
  if (!r.ok()) {
    Log(options_->info_log, "Reuse MANIFEST: %s\n", r.ToString().c_str());
    assert(descriptor_file_ == NULL);
    return false;
  }

  Log(options_->info_log, "Reusing MANIFEST %s\n", dscname.c_str());
  descriptor_log_ = new log::Writer(descriptor_file_, manifest_size);
  manifest_file_number_ = manifest_number;
  return true;
}
// 记录number已经被用
void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}
// 计算最应该compact的level
// 也就是计算没层对应的scroe，得分最高的就去compact
void VersionSet::Finalize(Version* v) {
  // Precomputed best level for next compaction
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < config::kNumLevels-1; level++) {
    double score;
    if (level == 0) {
      // We treat level-0 specially by bounding the number of files
      // instead of number of bytes for two reasons:
      //
      // (1) With larger write-buffer sizes, it is nice not to do too
      // many level-0 compactions.
      //
      // (2) The files in level-0 are merged on every read and
      // therefore we wish to avoid too many files when the individual
      // file size is small (perhaps because of a small write-buffer
      // setting, or very high compression ratios, or lots of
      // overwrites/deletions).
      // 上面给出了level-0用文件数目而不是文件占bytes得总大小了计算分数的理由
      // level-0 scroe的计算方式是把 当前level-0的文件总数/kL0_CompactionTrigger(要compact的触发值)
      score = v->files_[level].size() /
          static_cast<double>(config::kL0_CompactionTrigger);
    } else {
      // Compute the ratio of current size to size limit.
      // level-n(n > 0)的scroe等于 当前level所有file的总大小/该level允许的最大占用bytes
      const uint64_t level_bytes = TotalFileSize(v->files_[level]);
      score = static_cast<double>(level_bytes) / MaxBytesForLevel(level);
    }
    // 找出最大值
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  // 记录找到的最大值和对应的level
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}
// 先把当前current Version的comparator，compact_pointer_，files_等信息
// 记录到一个VersionEdit中，再利用它的EncodeTo方法字符串化，再利用log::Writer log到manifest中
Status VersionSet::WriteSnapshot(log::Writer* log) {
  // TODO: Break up into multiple records to reduce memory usage on recovery?
  // 这里作者又提供了需求，就是这样一下子log了整个VersionEdit到manifest中
  // 因为record会较大，所以recover的时候占用内存就会高，所以要考虑一下分多个record来存
  // 以便减少recover的使用

  // Save metadata
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  // Save compaction pointers
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  // Save files
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}
// 返回level层有多少file
int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return current_->files_[level].size();
}
// 用来总结当前VersionSet最新的各level状态，是level的文件个数
const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
  // Update code if kNumLevels changes
  assert(config::kNumLevels == 7);
  snprintf(scratch->buffer, sizeof(scratch->buffer),
           "files[ %d %d %d %d %d %d %d ]",
           int(current_->files_[0].size()),
           int(current_->files_[1].size()),
           int(current_->files_[2].size()),
           int(current_->files_[3].size()),
           int(current_->files_[4].size()),
           int(current_->files_[5].size()),
           int(current_->files_[6].size()));
  return scratch->buffer;
}
// 返回给定key在给定的version的db data里的offset
// 每level来搜，一直加
// 其中由于level-0比较特殊所以只能得到Approximate的offset了
// 而level-n(n > 0)则可以准确知道
uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        // 整个文件都在ikey后面，所以offset要加上这个file
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        // Entire file is after "ikey", so ignore
        // 整个file在ikey的前面
        // 如果又不是level-0则可以返回了，因为file是有序没overlap得
        // 否则要穷举完所有的level-0的file
        if (level > 0) {
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for
          // "ikey".
          break;
        }
      } else {
        // "ikey" falls in the range for this table.  Add the
        // approximate offset of "ikey" within the table.
        Table* tableptr;
        Iterator* iter = table_cache_->NewIterator(
            ReadOptions(), files[i]->number, files[i]->file_size, &tableptr);
        if (tableptr != NULL) {
          result += tableptr->ApproximateOffsetOf(ikey.Encode());
        }
        delete iter;
      }
    }
  }
  return result;
}
// 把所有还"live"的file加入到live中
// 三重循环
void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_;
       v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}
// 最新的curren给定level file占得总数
int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}
// 对于所有的level-1到level-(max-1)
// 计算每一个file与next level的overlap，并比较大小
// 返回overlap的最大值
int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      const FileMetaData* f = current_->files_[level][i];
      current_->GetOverlappingInputs(level+1, &f->smallest, &f->largest,
                                     &overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        result = sum;
      }
    }
  }
  return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
// 把inputs中最小的key，最大的key分别记录到smallest，largest中
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs,
                          InternalKey* smallest,
                          InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
// 对于给定的两组input，计算包含它们的最小range
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2,
                           InternalKey* smallest,
                           InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

// 创造一个可以迭代访问Compaction中input files的iterator
Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;

  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  // inputs包含了要compact的level和下一层的level
  // 如果要compact的level是level-0则需要level-0所有的iterator和下一层level的一个mergingIter
  // 否则只需要2个iter(inputs[0]的一个普通iter，和下一层level的一个mergingIter)
  const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
  Iterator** list = new Iterator*[space];
  int num = 0;
  for (int which = 0; which < 2; which++) {
    if (!c->inputs_[which].empty()) {
      if (c->level() + which == 0) {
        // 对于inputs是在level-0的话，就对每一个input的file新建一个iter
        const std::vector<FileMetaData*>& files = c->inputs_[which];
        for (size_t i = 0; i < files.size(); i++) {
          list[num++] = table_cache_->NewIterator(
              options, files[i]->number, files[i]->file_size);
        }
      } else {
        // Create concatenating iterator for the files from this level
        // 对于inputs是level-n(n > 0)的话，就就可以进行对整个inputs来合并成一个iter
        list[num++] = NewTwoLevelIterator(
            new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
            &GetFileIterator, table_cache_, options);
      }
    }
  }
  assert(num <= space);
  // 最后把所有iter都合并起来，再返回
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}

// 选取level和inputs用于构造一个Compaction返回
Compaction* VersionSet::PickCompaction() {
  Compaction* c;
  int level;

  // We prefer compactions triggered by too much data in a level over
  // the compactions triggered by seeks.
  // 这里会优先对data过多的level来进行compact，而由allow_seek触发的compact优先级较低
  // 下面的两个bool变量就是对应着这两种情况，如果compaction_score_ >= 1就代表了size不平衡的compact
  // 而如果compaction_score_ < 1还有file_to_compact_就会是由allow-seek触发的compact
  const bool size_compaction = (current_->compaction_score_ >= 1);
  const bool seek_compaction = (current_->file_to_compact_ != NULL);
  if (size_compaction) {
    // 如果是由size不平衡触发的compact
    level = current_->compaction_level_;
    assert(level >= 0);
    // 这里限定了compact level的大小
    assert(level+1 < config::kNumLevels);
    c = new Compaction(level);

    // Pick the first file that comes after compact_pointer_[level]
    // 根据compact_pointer_来找要compact的file
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      FileMetaData* f = current_->files_[level][i];
      if (compact_pointer_[level].empty() ||
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(f);
        break;
      }
    }
    // 如果上面的找file没找到(就是到了level的最大FILE的尽头了)，就要重新回到该level的第一个file
    if (c->inputs_[0].empty()) {
      // Wrap-around to the beginning of the key space
      c->inputs_[0].push_back(current_->files_[level][0]);
    }
  } else if (seek_compaction) {
    // 如果是allow-seek触发的话就很简单了，直接把当前要compact的file放进compact中
    level = current_->file_to_compact_level_;
    c = new Compaction(level);
    c->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return NULL;
  }
  // 设置要compact的当前的version
  c->input_version_ = current_;
  c->input_version_->Ref();

  // Files in level 0 may overlap each other, so pick up all overlapping ones
  // 如果要compact的是level-0的file，则由于overlap的存在，要把所有的overlap过的FILE都囊括进来
  if (level == 0) {
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    // Note that the next call will discard the file we placed in
    // c->inputs_[0] earlier and replace it with an overlapping set
    // which will include the picked file.
    // 这里就用到了GetOverlappingInputs对level-0的特殊处理的情况，“链式反应”
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }
  // 调用SetupOtherInputs(多是inputs_[1])，见下面
  SetupOtherInputs(c);

  return c;
}
// 设置compaction的其他inputs(多是inputs_[1])，其中包括扩展compact等更妙的技巧
void VersionSet::SetupOtherInputs(Compaction* c) {
  // 找到要compact的level的range
  const int level = c->level();
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);
  // 根据range去找与next-level有overlap的files，放大inputs_[1]中
  current_->GetOverlappingInputs(level+1, &smallest, &largest, &c->inputs_[1]);

  // Get entire range covered by compaction
  // 获取inputs_整体的range
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  // 如果inputs_[1]为空，我们就可以直接merge inputs_[0]的file到下一level就好了
  // (也要考虑与grandparent)的那层overlap关系
  // 如果inputs_[1]非空，看是否可以扩大compact范围
  if (!c->inputs_[1].empty()) {
    // 先根据之前compact的总range来确定expand-compact在inputs0层的新files，记作expand0
    std::vector<FileMetaData*> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const int64_t expanded0_size = TotalFileSize(expanded0);
    // expand0的size>当初inputs_[0]的size，并且加上原来的inputs_[1]占得总byte数小于expand阈值
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size < kExpandedCompactionByteSizeLimit) {
      // 就会继续由expanded0的总range找到expand-compact中新的inputs[1]中的overlap files
      // 记作expanded1
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      current_->GetOverlappingInputs(level+1, &new_start, &new_limit,
                                     &expanded1);
      // 如果expanded1的文件数和原来的inputs_[1]的文件数一样才会执行expand-compact
      // 原因是为了不动原来inputs_[1]的情况下，尽量扩展inputs_[0]
      if (expanded1.size() == c->inputs_[1].size()) {
        // 记下日志
        Log(options_->info_log,
            "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
            level,
            int(c->inputs_[0].size()),
            int(c->inputs_[1].size()),
            long(inputs0_size), long(inputs1_size),
            int(expanded0.size()),
            int(expanded1.size()),
            long(expanded0_size), long(inputs1_size));
        // 更新expand-compaction的相关信息
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  // 计算出与这次要compaction的总range有overlap的grandparent-level的file
  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }
  // debug用的
  if (false) {
    Log(options_->info_log, "Compacting %d '%s' .. '%s'",
        level,
        smallest.DebugString().c_str(),
        largest.DebugString().c_str());
  }

  // Update the place where we will do the next compaction for this level.
  // We update this immediately instead of waiting for the VersionEdit
  // to be applied so that if the compaction fails, we will try a different
  // key range next time.
  // 在还没进行真正的compact的时候就进行了compact_pointer_的移位
  // 这样的话，如果compact失败了，我们下一次compact就会从新的点开始，从而跳过失败的compact点
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}
// 手工设定要compact的level和range
Compaction* VersionSet::CompactRange(
    int level,
    const InternalKey* begin,
    const InternalKey* end) {
  std::vector<FileMetaData*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) {
    return NULL;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // But we cannot do this for level-0 since level-0 files can overlap
  // and we must not pick one file and drop another older file if the
  // two files overlap.
  // 对于要compact的inputsfile，要限制一次compact的大小，不能超过当前MaxFileSizeForLevel(level)
  // 但这只能对level>0有效，因为，level-0会有overlap，我们不能直接input.resize
  if (level > 0) {
    const uint64_t limit = MaxFileSizeForLevel(level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }
  // 新建一个compact，同时SetupOtherInputs
  Compaction* c = new Compaction(level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}
// compact的构造函数
Compaction::Compaction(int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(level)),
      input_version_(NULL),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
  for (int i = 0; i < config::kNumLevels; i++) {
    level_ptrs_[i] = 0;
  }
}
// 析构函数
Compaction::~Compaction() {
  if (input_version_ != NULL) {
    input_version_->Unref();
  }
}
// 判断这是否是一个trivial的compaction
// 即不用merge，就直接把level-n的file放到level-n+1
// 如果trivial同时要满足compact-level和grandparent-level的overlap不能超过阈值
bool Compaction::IsTrivialMove() const {
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  return (num_input_files(0) == 1 &&
          num_input_files(1) == 0 &&
          TotalFileSize(grandparents_) <= kMaxGrandParentOverlapBytes);
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->DeleteFile(level_ + which, inputs_[which][i]->number);
    }
  }
}
// caller在确保了我们的compact是produce在“level_+1”层的情况下
// 判断“level_+1”是否是user_key的BaseLevel
// 也就是对所用从“level_+2”开始的level，没有file的range涵盖了user_key
// ps：涵盖不代表user_key就在真实在file里。涵盖只是在范围里
bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Maybe use binary search to find right entry instead of linear search?
  // 这里作者提出了疑问，问是否要用二分查找来代替线性查找，我感觉是要的
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  // 迭代从level_ + 2开始的每个level
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    // 迭代特定level的每个file
    for (; level_ptrs_[lvl] < files.size(); ) {
      FileMetaData* f = files[level_ptrs_[lvl]];
      // 判断是否在file里面
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // Key falls in this file's range, so definitely not base level
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}
// 这个函数可能会被调用多次，用来统计和grandparents_file的overlap程度
// 但有internal_key来限定
bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  // Scan to find earliest grandparent file that contains key.
  const InternalKeyComparator* icmp = &input_version_->vset_->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
      icmp->Compare(internal_key,
                    grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    // 要在seen_key_的情款下才会计数，所以推测这个函数会调用多次
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  // seen_key_设置为true
  seen_key_ = true;
  // 如果超过了与grandparents_file的overlap程度的阈值就返回true
  // 同时重新计数
  if (overlapped_bytes_ > kMaxGrandParentOverlapBytes) {
    // Too much overlap for current output; start new output
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}
// 对input-Version引用减
void Compaction::ReleaseInputs() {
  if (input_version_ != NULL) {
    input_version_->Unref();
    input_version_ = NULL;
  }
}

}  // namespace leveldb
