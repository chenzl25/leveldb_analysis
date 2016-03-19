// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// This file contains the specification, but not the implementations,
// of the types/operations/etc. that should be defined by a platform
// specific port_<platform>.h file.  Use this file as a reference for
// how to port this package to a new platform.

/*======================================
=            Port_example.h            =
======================================*/

// 这是一个头文件的example，用于指导写相关平台的port.h

/*=====  End of Port_example.h  ======*/


#ifndef STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_
#define STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_

namespace leveldb {
namespace port {

// TODO(jorlow): Many of these belong more in the environment class rather than
//               here. We should try moving them and see if it affects perf.

// The following boolean constant must be true on a little-endian machine
// and false otherwise.
// 当机器是以little-endain来编码的机器是要为true，这样可以节省时间，不用leveldb自己编码解码
static const bool kLittleEndian = true /* or some other expression */;

// ------------------ Threading -------------------

// A Mutex represents an exclusive lock.
// 互斥锁
class Mutex {
 public:
  Mutex();
  ~Mutex();

  // Lock the mutex.  Waits until other lockers have exited.
  // Will deadlock if the mutex is already locked by this thread.
  // 去锁住Mutex，如果发现被锁住了，就等待其他locker解开再锁
  // 如果已近上了锁还调用这些方法就会造成deadlock
  void Lock();

  // Unlock the mutex.
  // REQUIRES: This mutex was locked by this thread.
  // 解开thread自己锁上的锁
  void Unlock();

  // Optionally crash if this thread does not hold this mutex.
  // The implementation must be fast, especially if NDEBUG is
  // defined.  The implementation is allowed to skip all checks.
  // 断言自己有锁，否则直接crash掉，debug或保障用
  void AssertHeld();
};
// condition varibale，如果不理解，可以google
// 一种线程同步的手段，也已与Mutex使用
class CondVar {
 public:
  explicit CondVar(Mutex* mu);
  ~CondVar();

  // Atomically release *mu and block on this condition variable until
  // either a call to SignalAll(), or a call to Signal() that picks
  // this thread to wakeup.
  // REQUIRES: this thread holds *mu
  // 线程使用的时候需要先拥有Mutex
  // 之后使用Wait，同时自动解锁Mutex,
  // 之后等待被Signal或者SignallAll唤醒
  void Wait();

  // If there are some threads waiting, wake up at least one of them.
  // 如果有等待的线程，该方法至少叫醒一个
  void Signal();

  // Wake up all waiting threads.
  // 叫醒所有的线程
  void SignallAll();
};

// Thread-safe initialization.
// Used as follows:
//      static port::OnceType init_control = LEVELDB_ONCE_INIT;
//      static void Initializer() { ... do something ...; }
//      ...
//      port::InitOnce(&init_control, &Initializer);
// 线程安全的initialization，使用方法如上，
// 先声明一个OnceType放到第一个参数 再把函数放到第二个参数
typedef intptr_t OnceType;
#define LEVELDB_ONCE_INIT 0
extern void InitOnce(port::OnceType*, void (*initializer)());

// A type that holds a pointer that can be read or written atomically
// (i.e., without word-tearing.)
// 一个可以保存进行原子读和写的指针的类型
// 例如，without word-tearing
class AtomicPointer {
 private:
  intptr_t rep_;
 public:
  // Initialize to arbitrary value
  AtomicPointer();

  // Initialize to hold v
  explicit AtomicPointer(void* v) : rep_(v) { }

  // Read and return the stored pointer with the guarantee that no
  // later memory access (read or write) by this thread can be
  // reordered ahead of this read.
  // read并且返回指针，要保证：没有比现在迟的该线程的访问内存的read或者write，会比现在的read更早执行
  void* Acquire_Load() const;

  // Set v as the stored pointer with the guarantee that no earlier
  // memory access (read or write) by this thread can be reordered
  // after this store.
  // 把指针v store到内部的stored pointer， 要保证没有比现在早的该线程的访问内存的read或者write，会比现在的的store更后
  void Release_Store(void* v);

  // Read the stored pointer with no ordering guarantees.
  // 没顺序保障的load
  void* NoBarrier_Load() const;

  // Set va as the stored pointer with no ordering guarantees.
  // 没顺序保障的store
  void NoBarrier_Store(void* v);
};

// ------------------ Compression -------------------

// Store the snappy compression of "input[0,input_length-1]" in *output.
// Returns false if snappy is not supported by this port.
// 把长度为input_length的input用snappy压缩到output中，返回true
// 如果该port不支持snappy的话返回false
extern bool Snappy_Compress(const char* input, size_t input_length,
                            std::string* output);

// If input[0,input_length-1] looks like a valid snappy compressed
// buffer, store the size of the uncompressed data in *result and
// return true.  Else return false.
// 如果input看起来像是有效的被snappy压缩的数据就
// 把长度为input_length的input用snappy解缩到output中，返回true
// 如果失败返回false
// ps：最后发现都是调用相应的snappy库函数
extern bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                         size_t* result);

// Attempt to snappy uncompress input[0,input_length-1] into *output.
// Returns true if successful, false if the input is invalid lightweight
// compressed data.
//
// REQUIRES: at least the first "n" bytes of output[] must be writable
// where "n" is the result of a successful call to
// Snappy_GetUncompressedLength.
// 这应该是真正的解压，其中应该会调用Snappy_GetUncompressedLength，
// 如果input是invalid的返回false
// ps：最后发现都是调用相应的snappy库函数
extern bool Snappy_Uncompress(const char* input_data, size_t input_length,
                              char* output);

// ------------------ Miscellaneous -------------------

// If heap profiling is not supported, returns false.
// Else repeatedly calls (*func)(arg, data, n) and then returns true.
// The concatenation of all "data[0,n-1]" fragments is the heap profile.
// 貌似是堆栈分析
// 如果不支持，返回false
// 否则（看上面英文解析）
extern bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg);

}  // namespace port
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_
