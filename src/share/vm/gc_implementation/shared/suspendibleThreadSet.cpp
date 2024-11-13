/*
 * Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc_implementation/shared/suspendibleThreadSet.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"

uint   SuspendibleThreadSet::_nthreads          = 0;
uint   SuspendibleThreadSet::_nthreads_stopped  = 0;
bool   SuspendibleThreadSet::_suspend_all       = false;
double SuspendibleThreadSet::_suspend_all_start = 0.0;

// 线程加入到可暂停集合
void SuspendibleThreadSet::join() {
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  while (_suspend_all) { // STW信号，那么就持续的block住
    ml.wait(Mutex::_no_safepoint_check_flag);
  }
  // 当前没有_suspend_all，或者，已经在锁上面受到通知，即STW已经结束，那么退出
  _nthreads++; // 暂停线程的数量+1
}

// 线程离开可暂停集合
void SuspendibleThreadSet::leave() {
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_nthreads > 0, "Invalid");
  _nthreads--; // 可暂停线程集合数量减去1
  if (_suspend_all) { // STW信号, 如果发现VMThread线程已经要求暂停， 则会等待
    ml.notify_all(); // 通知所有的并发线程，VMThread要求他们暂停
  }
}

// 线程终止,即让出执行权，直到_suspend_all被重置为false，才恢复执行权
void SuspendibleThreadSet::yield() {
  if (_suspend_all) { //如果没有_suspend_all信号，那么yield()操作直接返回，不会block任何事情
    MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
    if (_suspend_all) { // 当前有STW信号
      _nthreads_stopped++; // 终止线程数量加1
      if (_nthreads_stopped == _nthreads) { // 所有线程已经全部终止
        if (ConcGCYieldTimeout > 0) {
          double now = os::elapsedTime();
          guarantee((now - _suspend_all_start) * 1000.0 < (double)ConcGCYieldTimeout, "Long delay");
        }
      }
      ml.notify_all(); //告知 VMThread，我现在已经暂停了，你可以开始工作了
      while (_suspend_all) {
          // 本线程暂时停止，直到VMThread通知可以继续工作
        ml.wait(Mutex::_no_safepoint_check_flag);
      }
      // 收了了VMThread通知，可以继续工作了
      assert(_nthreads_stopped > 0, "Invalid");
      _nthreads_stopped--; // 暂停线程数量减去1
      ml.notify_all(); // 告知VMThread,我收到了你的恢复工作的信号，我将继续工作
    }
  }
}

/**
 * 静态方法，等待直到所有线程都已经暂停，或者线程已经离开了可暂停集合中
 */
void SuspendibleThreadSet::synchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");
  if (ConcGCYieldTimeout > 0) {
    _suspend_all_start = os::elapsedTime();
  }
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(!_suspend_all, "Only one at a time");
  _suspend_all = true;
  // 持续等待，直到回收集合中的线程已经全部停止，即当前已经停止的线程数量(_nthreads_stopped)不小于在暂停集合中的线程数量(_nthreads)
  while (_nthreads_stopped < _nthreads) {
    ml.wait(Mutex::_no_safepoint_check_flag);
  }
}

void SuspendibleThreadSet::desynchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_nthreads_stopped == _nthreads, "Invalid");
  _suspend_all = false; // 在yield中等待的所有线程都将受到通知，重新执行
  ml.notify_all();
}
