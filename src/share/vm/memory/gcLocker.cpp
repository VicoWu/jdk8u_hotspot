/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/gcLocker.inline.hpp"
#include "memory/resourceArea.hpp"
#include "memory/sharedHeap.hpp"
#include "runtime/thread.inline.hpp"

volatile jint GC_locker::_jni_lock_count = 0;
volatile bool GC_locker::_needs_gc       = false;
volatile bool GC_locker::_doing_gc       = false;
unsigned int  GC_locker::_total_collections = 0;

#ifdef ASSERT
volatile jint GC_locker::_debug_jni_lock_count = 0;
#endif


/**
 * 这个方法并不要求处于safepoint，只是说，如果处于safepoint，那么必须要求处于临界区的线程和临界区线程计数一直。
 * 如果不是出在safepoint，那么就不管了
 * 安全点事一个HotSpot全局的概念，而是否处于临界区是每一个线程的概念
 */
#ifdef ASSERT
void GC_locker::verify_critical_count() { // 确保在安全点的情况下，进入临界区的锁数量等于临界区线程数量
  if (SafepointSynchronize::is_at_safepoint()) { // 如果当前所有线程都已经进入safepoint，就必须检查进入临界区的锁数量等于临界区线程数量
    assert(!needs_gc() || _debug_jni_lock_count == _jni_lock_count, "must agree");
    int count = 0;
    // Count the number of threads with critical operations in progress
    //
    for (JavaThread* thr = Threads::first(); thr; thr = thr->next()) {
      if (thr->in_critical()) { // 统计所有处于临界区的线程数量
        count++;
      }
    }
    if (_jni_lock_count != count) {
      tty->print_cr("critical counts don't match: %d != %d", _jni_lock_count, count);
      for (JavaThread* thr = Threads::first(); thr; thr = thr->next()) {
        if (thr->in_critical()) {
          tty->print_cr(INTPTR_FORMAT " in_critical %d", p2i(thr), thr->in_critical());
        }
      }
    }
    assert(_jni_lock_count == count, "must be equal");
  }
}
#endif
/**
 * 这个方法要求必须已经处于安全点
 * 这个方法是在进行gc操作以前调用，判断当前_jni_lock_count是否大于0，如果大于0，说明有线程处在临界区同时，
 * 在jni_lock_count大于0的情况下，设置_needs_gc为true，
 * @return 是否是active，即当前的HotSpot临界区计数是否大于0
 */
bool GC_locker::check_active_before_gc() {
  assert(SafepointSynchronize::is_at_safepoint(), "only read at safepoint");
  if (is_active() && !_needs_gc) {
    verify_critical_count(); //如果是处在安全点，确认当前处于临界区的线程跟临界区的计数是一致的
    _needs_gc = true; // HotSpt此时需要进行gc
    if (PrintJNIGCStalls && PrintGCDetails) {
      ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
      gclog_or_tty->print_cr("%.3f: Setting _needs_gc. Thread \"%s\" %d locked.",
                             gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
    }

  }
  return is_active(); // _jni_lock_count是否大于0, is_active这个方法要求必须处于安全点
}

void GC_locker::stall_until_clear() {
  assert(!JavaThread::current()->in_critical(), "Would deadlock");
  MutexLocker   ml(JNICritical_lock);

  if (needs_gc()) {
    if (PrintJNIGCStalls && PrintGCDetails) {
      ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
      gclog_or_tty->print_cr("%.3f: Allocation failed. Thread \"%s\" is stalled by JNI critical section, %d locked.",
                             gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
    }
  }

  // Wait for _needs_gc  to be cleared
  while (needs_gc()) {
    JNICritical_lock->wait();
  }
}

/**
 * 我们搜索collect(GCCause::_gc_locker)，可以看到在GC_locker::jni_unlock中会调用collect(GCCause::_gc_locker)
 * 如果当前GCLocker中维护的_total_collections和目前G1CollectedHeap返回的值不同，
 *  说明GC_locker::jni_unlock()中发起gc的时候跟当前这个时间区间中已经进行过其他的gc了，没必要再重复进行gc了
 * @param cause
 * @param total_collections
 * @return
 */
bool GC_locker::should_discard(GCCause::Cause cause, uint total_collections) {
  return (cause == GCCause::_gc_locker) &&
         (_total_collections != total_collections);
}

/**
 * 通过快路径离开临界区
 * 在JNICritical_locks上加锁,这是慢路径，即暂停进入关键区，直到收到响应通知才能进入
 * @param thread
 */
void GC_locker::jni_lock(JavaThread* thread) {
  assert(!thread->in_critical(), "shouldn't currently be in a critical region");
  MutexLocker mu(JNICritical_lock); // 所有的lock都定义在mutexLocker.hpp中，由于是一个局部变量，因此
  // Block entering threads if we know at least one thread is in a
  // JNI critical region and we need a GC.
  // We check that at least one thread is in a critical region before
  // blocking because blocked threads are woken up by a thread exiting
  // a JNI critical region.
  /**
   * 如果当前有线程已经处于临界区并且_needs_gc是true，或者，已经有现成正在进行gc，
   *    那么我们主动放弃JNICritical_lock锁，因为待会儿有线程从临界区出来的时候会通知我
   */
  while (is_active_and_needs_gc() || _doing_gc) {
    JNICritical_lock->wait(); // 此时如果有其他线程处于临界区，那么放弃锁，然后一直在这里等待，直到收到通知才会unblock
  }
  // 此时线程获得了JNICritical_lock的锁
  thread->enter_critical(); // 获得锁的该线程进入临界区，当前线程不一定是thread
  _jni_lock_count++; // 临界区的锁数量+1，这是一个静态变量，代表有多少个线程目前进入了临界区
  increment_debug_jni_lock_count();
}


/**
 * 通过慢路径离开临界区
 * 在JNICritical_locks上解锁,这是慢路径，即暂停进入关键区，直到收到响应通知才能进入
 * 从参数可以看到，只有JavaThread才会有jni_lock和jni_unlock的概念
 * @param thread
 */
void GC_locker::jni_unlock(JavaThread* thread) {
  //当jni_unlock的时候，当前临界区必须是这个JavaThread最后一个临界区
  assert(thread->in_last_critical(), "should be exiting critical region");
  MutexLocker mu(JNICritical_lock);
  _jni_lock_count--;// 临界区的锁数量-1，这是一个静态变量，代表有多少个线程目前进入了临界区
  decrement_debug_jni_lock_count();
  thread->exit_critical(); // 离开临界区，然后才有可能进行gc
  // needs_gc 为true，说明之前有gc操作由于临界区有线程的原因放弃了gc，此时，需要重新尝试gc
  if (needs_gc() && !is_active_internal()) { //已经不是active了，即当前已经没有任何线程，并且有gc正在等待，那么，就申请一次gc
    // We're the last thread out. Request a GC.
    // Capture the current total collections, to allow detection of
    // other collections that make this one unnecessary.  The value of
    // total_collections() is only changed at a safepoint, so there
    // must not be a safepoint between the lock becoming inactive and
    // getting the count, else there may be unnecessary GCLocker GCs. 这里的意思是，在当前
    /**
     * 我是最后一个离开临界区的线程，即，我离开了，临界区就空闲了，一次可以尝试进行gc了
     * 计数总共发生的回收次数，这样，我们通过这个计数就知道两次相邻的因为GCCause::_gc_locker导致的gc中间是否插入了其他的gc，如果没有，那么就没必要进行连续两次的重复的基于为GCCause::_gc_locker的gc
     * 这个值只是在这里被修改，因此我们可以看到在G1CollectedHeap::collect中调用GC_locker::should_discard，如果发现_total_collections不等于Universe::heap()->total_collections()，就放弃gc，因为这说明中间可能有其他gc发生了，没必要再插入基于GC_locker的gc
     * 如果GCLocker::_total_collections不等于当前实时更新的Universe::heap()->total_collections()，会放弃gc，因为
     *
     */
    _total_collections = Universe::heap()->total_collections();
    _doing_gc = true; // 接下来开始进行gc操作
    {
      // Must give up the lock while at a safepoint
      MutexUnlocker munlock(JNICritical_lock); // 解除锁定
      if (PrintJNIGCStalls && PrintGCDetails) {
        ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
        gclog_or_tty->print_cr("%.3f: Thread \"%s\" is performing GC after exiting critical section, %d locked",
            gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
      }
      // 这里的Universe::heap返回的是G1CollectedHeap，在文件g1CollectedHeap.cpp中定义
      // 这个collect的调用也可能是System.gc()导致的，传入不同的GCCause
      Universe::heap()->collect(GCCause::_gc_locker);
    }
    _doing_gc = false; // gc j结束
    _needs_gc = false; // 不需要进行gc
    JNICritical_lock->notify_all(); // 通知其他在该锁上等待的线程
  }
}

// Implementation of No_GC_Verifier

#ifdef ASSERT

No_GC_Verifier::No_GC_Verifier(bool verifygc) {
  _verifygc = verifygc;
  if (_verifygc) {
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    _old_invocations = h->total_collections();
  }
}


No_GC_Verifier::~No_GC_Verifier() {
  if (_verifygc) {
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    if (_old_invocations != h->total_collections()) {
      fatal("collection in a No_GC_Verifier secured function");
    }
  }
}

Pause_No_GC_Verifier::Pause_No_GC_Verifier(No_GC_Verifier * ngcv) {
  _ngcv = ngcv;
  if (_ngcv->_verifygc) {
    // if we were verifying, then make sure that nothing is
    // wrong before we "pause" verification
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    if (_ngcv->_old_invocations != h->total_collections()) {
      fatal("collection in a No_GC_Verifier secured function");
    }
  }
}


Pause_No_GC_Verifier::~Pause_No_GC_Verifier() {
  if (_ngcv->_verifygc) {
    // if we were verifying before, then reenable verification
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    _ngcv->_old_invocations = h->total_collections();
  }
}


// JRT_LEAF rules:
// A JRT_LEAF method may not interfere with safepointing by
//   1) acquiring or blocking on a Mutex or JavaLock - checked
//   2) allocating heap memory - checked
//   3) executing a VM operation - checked
//   4) executing a system call (including malloc) that could block or grab a lock
//   5) invoking GC
//   6) reaching a safepoint
//   7) running too long
// Nor may any method it calls.
JRT_Leaf_Verifier::JRT_Leaf_Verifier()
  : No_Safepoint_Verifier(true, JRT_Leaf_Verifier::should_verify_GC())
{
}

JRT_Leaf_Verifier::~JRT_Leaf_Verifier()
{
}

bool JRT_Leaf_Verifier::should_verify_GC() {
  switch (JavaThread::current()->thread_state()) {
  case _thread_in_Java:
    // is in a leaf routine, there must be no safepoint.
    return true;
  case _thread_in_native:
    // A native thread is not subject to safepoints.
    // Even while it is in a leaf routine, GC is ok
    return false;
  default:
    // Leaf routines cannot be called from other contexts.
    ShouldNotReachHere();
    return false;
  }
}
#endif
