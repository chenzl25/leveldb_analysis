// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include "leveldb/env.h"
#include "port/port.h"
#include "table/block.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
// BlickHandle是一个指向file的block的一个“指针”
// 这里的encode是把handle的offset，size编码后存到dst
// ps：均为64变长int
void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}
// 把input里面的编码，解码到offset和size中
Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) &&
      GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    return Status::Corruption("bad block handle");
  }
}
// 
void Footer::EncodeTo(std::string* dst) const {
#ifndef NDEBUG
  const size_t original_size = dst->size();
#endif
  // 调用blockHandle的EncodeTo的函数编码到dst
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  // resize是为了制造padding，以为kMaxEncodedLength >= blockHandle的Encode，
  // 使用padding的原因有：blockHandle的Encode是64位变长的数据，长度最长为10，kMaxEncodedLength也是10
  // 如果encode不够10的话，就会有空隙，而resize可以补足这一点
  // 从而使得Footer是定长的
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  // 加入magic number一共8byte，magic number是用来判断一个文件是不是sstable用的
  // little-endian
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == original_size + kEncodedLength);
}
// 
Status Footer::DecodeFrom(Slice* input) {
  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  // magic number是用来判断一个文件是不是sstable用的
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  // 译码metaindex_handle_，
  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    // 译码index_handle_
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    // 使input移动到kEncodedLength个单位，
    // 使得调用完该函数后，可以自动移位
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}
// table.cc中会用到该方法
// 利用提供的block_handle来读取在file中对应的block
// 会对读出来的block判断是否需要解压，校验
// 还有给出是否可以缓存，因为block读出来的时候时候通过RandomAccessFile的
// 而我们会传进一个buffer给RandomAccessFile来存放读出来的block
// 如果不相同的时候就会认为RandomAccessFile没用到我们的buffer
// 所以就不能缓存，因为我们不知道什么时候回失效（也就是所谓的double buffer）
// 这里的block的大小不一定都一样（可能提前用了Finish函数）
// 但是确保会在一次的IO中就能读取尽量多的数据，所以用block来做单位
Status ReadBlock(RandomAccessFile* file,
                 const ReadOptions& options,
                 const BlockHandle& handle,
                 BlockContents* result) {
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  // 获得blockHandel的size，这个size指明了block的大小，大小包括了key/value和restart的相关信息
  // 但不包括trailer
  size_t n = static_cast<size_t>(handle.size());
  // 所以，buf= n + kBlockTrailerSize
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  // file读取失败
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  // file大小不符合
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated block read");
  }

  // Check the crc of the type and the block contents
  // crc是block content和type(表示是否压缩了)的校验和
  const char* data = contents.data();    // Pointer to where Read put the data
  // 根据options来进行是否要检验校验和
  if (options.verify_checksums) {
    // 常规，先Unmask（因为存的时候都会先Mask再存）
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    // 计算block content+type的检验和
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }
  // 判断data的类型
  switch (data[n]) {
    // 没压缩的情况
    case kNoCompression:
      if (data != buf) {
        // File implementation gave us pointer to some other data.
        // Use it directly under the assumption that it will be live
        // while the file is open.
        // 这里如果，只是说如果RandomAccessFile的实现方法里面没有用到我没给的buf
        // 来作为缓存的话，我们就可以删除buf了，因为RandomAccessFile的实现里给我们另外的指向空间
        // 在默认的实现中env_posix.cc中，貌似不会有这种情况
        // 但这种思想很可贵呀
        delete[] buf;
        result->data = Slice(data, n);
        // 所以呢告诉caller说，不要把这个结果不是我们自己在堆里new出来的
        // 所以不要重复缓存它，因为它什么时候不能用了(被delete了)我们决定不了，这有file的实现来决定的
        result->heap_allocated = false;
        result->cachable = false;  // Do not double-cache
      } else {
        // 而如果data == buf
        // 则说明结果就是我们在堆里new出来的东西
        // 告诉caller可以缓存
        result->data = Slice(buf, n);
        result->heap_allocated = true;
        result->cachable = true;
      }

      // Ok
      break;
    // 如果压缩了就要先解压
    case kSnappyCompression: {
      size_t ulength = 0;
      // 获取当前压缩过的block如果解压要有多少byte
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        delete[] buf;
        return Status::Corruption("corrupted compressed block contents");
      }
      // 创建对应byte的buffer来存解压对象
      char* ubuf = new char[ulength];
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        delete[] buf;
        delete[] ubuf;
        return Status::Corruption("corrupted compressed block contents");
      }
      // delete掉buf，这里没有管buf是否等于data，就直接delete掉了，是否合理呢？
      // （因为上面没压缩的情况下用到了这个判断）
      delete[] buf;
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }
    default:
      delete[] buf;
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

}  // namespace leveldb
