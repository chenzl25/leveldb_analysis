// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "port/port_posix.h"

#include <cstdlib>
#include <stdio.h>
#include <string.h>

// 使用posix的pthread库来实现

namespace leveldb {
namespace port {
// 一个用于判断pthred函数调用是否出错的wrapper
static void PthreadCall(const char* label, int result) {
  if (result != 0) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    abort();
  }
}
// Mutex的init
Mutex::Mutex() { PthreadCall("init mutex", pthread_mutex_init(&mu_, NULL)); }
// Mutex的destory
Mutex::~Mutex() { PthreadCall("destroy mutex", pthread_mutex_destroy(&mu_)); }
// Mutex的lock
void Mutex::Lock() { PthreadCall("lock", pthread_mutex_lock(&mu_)); }
// Mutex的unlock
void Mutex::Unlock() { PthreadCall("unlock", pthread_mutex_unlock(&mu_)); }

// condition varible 可以访问Mutex，是Mutex的友元
CondVar::CondVar(Mutex* mu)
    : mu_(mu) {
    PthreadCall("init cv", pthread_cond_init(&cv_, NULL));
}
// destory  cond
CondVar::~CondVar() { PthreadCall("destroy cv", pthread_cond_destroy(&cv_)); }
//  cond的wait
void CondVar::Wait() {
  PthreadCall("wait", pthread_cond_wait(&cv_, &mu_->mu_));
}
// cont的signal
void CondVar::Signal() {
  PthreadCall("signal", pthread_cond_signal(&cv_));
}
// cont的broadcast
void CondVar::SignalAll() {
  PthreadCall("broadcast", pthread_cond_broadcast(&cv_));
}
// 初始化
void InitOnce(OnceType* once, void (*initializer)()) {
  PthreadCall("once", pthread_once(once, initializer));
}

}  // namespace port
}  // namespace leveldb
