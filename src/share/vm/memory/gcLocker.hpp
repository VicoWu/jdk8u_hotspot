/*
 * Copyright (c) 1997, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_GCLOCKER_HPP
#define SHARE_VM_MEMORY_GCLOCKER_HPP

#include "gc_interface/collectedHeap.hpp"
#include "gc_interface/gcCause.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/universe.hpp"
#include "oops/oop.hpp"
#include "runtime/thread.inline.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_bsd.inline.hpp"
#endif

// The direct lock/unlock calls do not force a collection if an unlock
// decrements the count to zero. Avoid calling these if at all possible.

class GC_locker: public AllStatic {
 private:
  // The _jni_lock_count keeps track of the number of threads that are
  // currently in a critical region.  It's only kept up to date when
  // _needs_gc is true.  The current value is computed during
  // safepointing and decremented during the slow path of GC_locker
  // unlocking.
  static volatile jint _jni_lock_count;  // number of jni active instances. 这是静态变量，因为它记录的是整个HotSpot中的进入临界区的锁的数量
  static volatile bool _needs_gc;        // heap is filling, we need a GC // 静态变量，因为它标记的是整个HotSpot是否需要进行gc
                                         // note: bool is typedef'd as jint
  static volatile bool _doing_gc;        // unlock_critical() is doing a GC // 静态变量，因为它标记的是整个HotSpot是否正在进行gc
  static uint _total_collections;        // value for _gc_locker collection // 静态变量，因为它标记的是整个HotSpot的回收数量

#ifdef ASSERT
  // This lock count is updated for all operations and is used to
  // validate the jni_lock_count that is computed during safepoints.
  static volatile jint _debug_jni_lock_count;
#endif

  // At a safepoint, visit all threads and count the number of active
  // critical sections.  This is used to ensure that all active
  // critical sections are exited before a new one is started.
  /**
   * 实现是GC_locker::verify_critical_count()
   * 如果是在安全点，那么必须要争
   */
  static void verify_critical_count() NOT_DEBUG_RETURN;

  static void jni_lock(JavaThread* thread);
  static void jni_unlock(JavaThread* thread);

  /**
   * 这是一个全局静态方法
   * 查看static bool is_active()，可以看到调用这个方法必须确保处于safepoint
   * @return
   */
  static bool is_active_internal() {
    verify_critical_count(); // 确认如果在安全点状态下临界区线程数和临界区锁的数量相同
    return _jni_lock_count > 0; //  当前临界区的锁数量大于0
  }

 public:
  // Accessors
  /**
   * 这个方法要求必须处于安全点
   * @return
   */
  static bool is_active() { // 这个方法要求必须处于安全点safepoint
    assert(SafepointSynchronize::is_at_safepoint(), "only read at safepoint"); // 必须在安全点才能读取这个状态
    return is_active_internal();
  }
  /**
   * GC_locker::check_active_before_gc() 会将_needs_gc设置为true
   * 在GC_locker::jni_unlock即退出安全区的时候中，如果发现needs_gc=true，会直接进行gc，然后设置_needs_gc=false
   * 由于这是一个静态变量，因此当一次内存分配失败之后尝试gc，检查这个变量为true，有可能是其他gc线程准备开始执行
   * @return
   */
  static bool needs_gc()       { return _needs_gc;                        }

  // Shorthand
  /**
   * 静态方法，需要gc并且是active的，active的含义是至少有一个线程目前正处于关键区
   * needs_gc 为true，说明刚刚有回收线程尝试执行，但是发现 is_active()是false，
   *    所以放弃了(在方法check_active_before_gc()中)，因此置needs_gc=true，
   *    当GC_Locker退出关键区的时候，GCLocker就会自己请求一次gc
   * @return
   */
  static bool is_active_and_needs_gc() {
    // Use is_active_internal since _needs_gc can change from true to
    // false outside of a safepoint, triggering the assert in
    // is_active.
    /**
     * 在方法check_active_before_gc()中设置为true，说明有线程请求了gc，但是发现is_active为true，因此设置了needs_gc =true，然后放弃回收
     */
    return needs_gc() //
      && is_active_internal();
  }

  // In debug mode track the locking state at all times
  static void increment_debug_jni_lock_count() {
#ifdef ASSERT
    assert(_debug_jni_lock_count >= 0, "bad value");
    Atomic::inc(&_debug_jni_lock_count);
#endif
  }
  static void decrement_debug_jni_lock_count() {
#ifdef ASSERT
    assert(_debug_jni_lock_count > 0, "bad value");
    Atomic::dec(&_debug_jni_lock_count);
#endif
  }

  // Set the current lock count
  static void set_jni_lock_count(int count) {
    _jni_lock_count = count;
    verify_critical_count();
  }

  // Sets _needs_gc if is_active() is true. Returns is_active().
  /**
   * 将_needs_gc设置为true，同时返回is_active()的结果
   */
  static bool check_active_before_gc();

  // Return true if the designated collection is a GCLocker request
  // that should be discarded.  Returns true if cause == GCCause::_gc_locker
  // and the given total collection value indicates a collection has been
  // done since the GCLocker request was made.
  /**
   * 如果指定的集合是应丢弃的 GCLocker 请求，则返回 true。
   * 判断的方法是，如果 Cause == GCCause::_gc_locker 并且当前G1CollectedHeap给出的当前的
   *    总收集值不等于GCLocker发出请求的时候记录的值，意味着GCLocker自发出回收请求以来已进行过其他收集，则返回 false。
   * @param cause
   * @param total_collections
   * @return
   */
  static bool should_discard(GCCause::Cause cause, uint total_collections);

  // Stalls the caller (who should not be in a jni critical section)
  // until needs_gc() clears. Note however that needs_gc() may be
  // set at a subsequent safepoint and/or cleared under the
  // JNICritical_lock, so the caller may not safely assert upon
  // return from this method that "!needs_gc()" since that is
  // not a stable predicate.
  static void stall_until_clear();

  // The following two methods are used for JNI critical regions.
  // If we find that we failed to perform a GC because the GC_locker
  // was active, arrange for one as soon as possible by allowing
  // all threads in critical regions to complete, but not allowing
  // other critical regions to be entered. The reasons for that are:
  // 1) a GC request won't be starved by overlapping JNI critical
  //    region activities, which can cause unnecessary OutOfMemory errors.
  // 2) even if allocation requests can still be satisfied before GC locker
  //    becomes inactive, for example, in tenured generation possibly with
  //    heap expansion, those allocations can trigger lots of safepointing
  //    attempts (ineffective GC attempts) and require Heap_lock which
  //    slow down allocations tremendously.
  //
  // Note that critical regions can be nested in a single thread, so
  // we must allow threads already in critical regions to continue.
  //
  // JNI critical regions are the only participants in this scheme
  // because they are, by spec, well bounded while in a critical region.
  //
  // Each of the following two method is split into a fast path and a
  // slow path. JNICritical_lock is only grabbed in the slow path.
  // _needs_gc is initially false and every java thread will go
  // through the fast path, which simply increments or decrements the
  // current thread's critical count.  When GC happens at a safepoint,
  // GC_locker::is_active() is checked. Since there is no safepoint in
  // the fast path of lock_critical() and unlock_critical(), there is
  // no race condition between the fast path and GC. After _needs_gc
  // is set at a safepoint, every thread will go through the slow path
  // after the safepoint.  Since after a safepoint, each of the
  // following two methods is either entered from the method entry and
  // falls into the slow path, or is resumed from the safepoints in
  // the method, which only exist in the slow path. So when _needs_gc
  // is set, the slow path is always taken, till _needs_gc is cleared.
  static void lock_critical(JavaThread* thread); // 线程可能因为各种原因需要lock_critical，比如，jni需要读取一个值等等
  static void unlock_critical(JavaThread* thread); // unlock_critical和lock_critical配对，使用

  static address needs_gc_address() { return (address) &_needs_gc; }
};


// A No_GC_Verifier object can be placed in methods where one assumes that
// no garbage collection will occur. The destructor will verify this property
// unless the constructor is called with argument false (not verifygc).
//
// The check will only be done in debug mode and if verifygc true.

class No_GC_Verifier: public StackObj {
 friend class Pause_No_GC_Verifier;

 protected:
  bool _verifygc;
  unsigned int _old_invocations;

 public:
#ifdef ASSERT
  No_GC_Verifier(bool verifygc = true);
  ~No_GC_Verifier();
#else
  No_GC_Verifier(bool verifygc = true) {}
  ~No_GC_Verifier() {}
#endif
};

// A Pause_No_GC_Verifier is used to temporarily pause the behavior
// of a No_GC_Verifier object. If we are not in debug mode or if the
// No_GC_Verifier object has a _verifygc value of false, then there
// is nothing to do.

class Pause_No_GC_Verifier: public StackObj {
 private:
  No_GC_Verifier * _ngcv;

 public:
#ifdef ASSERT
  Pause_No_GC_Verifier(No_GC_Verifier * ngcv);
  ~Pause_No_GC_Verifier();
#else
  Pause_No_GC_Verifier(No_GC_Verifier * ngcv) {}
  ~Pause_No_GC_Verifier() {}
#endif
};


// A No_Safepoint_Verifier object will throw an assertion failure if
// the current thread passes a possible safepoint while this object is
// instantiated. A safepoint, will either be: an oop allocation, blocking
// on a Mutex or JavaLock, or executing a VM operation.
//
// If StrictSafepointChecks is turned off, it degrades into a No_GC_Verifier
//
class No_Safepoint_Verifier : public No_GC_Verifier {
 friend class Pause_No_Safepoint_Verifier;

 private:
  bool _activated;
  Thread *_thread;
 public:
#ifdef ASSERT
  No_Safepoint_Verifier(bool activated = true, bool verifygc = true ) :
    No_GC_Verifier(verifygc),
    _activated(activated) {
    _thread = Thread::current();
    if (_activated) {
      _thread->_allow_allocation_count++;
      _thread->_allow_safepoint_count++;
    }
  }

  ~No_Safepoint_Verifier() {
    if (_activated) {
      _thread->_allow_allocation_count--;
      _thread->_allow_safepoint_count--;
    }
  }
#else
  No_Safepoint_Verifier(bool activated = true, bool verifygc = true) : No_GC_Verifier(verifygc){}
  ~No_Safepoint_Verifier() {}
#endif
};

// A Pause_No_Safepoint_Verifier is used to temporarily pause the
// behavior of a No_Safepoint_Verifier object. If we are not in debug
// mode then there is nothing to do. If the No_Safepoint_Verifier
// object has an _activated value of false, then there is nothing to
// do for safepoint and allocation checking, but there may still be
// something to do for the underlying No_GC_Verifier object.

class Pause_No_Safepoint_Verifier : public Pause_No_GC_Verifier {
 private:
  No_Safepoint_Verifier * _nsv;

 public:
#ifdef ASSERT
  Pause_No_Safepoint_Verifier(No_Safepoint_Verifier * nsv)
    : Pause_No_GC_Verifier(nsv) {

    _nsv = nsv;
    if (_nsv->_activated) {
      _nsv->_thread->_allow_allocation_count--;
      _nsv->_thread->_allow_safepoint_count--;
    }
  }

  ~Pause_No_Safepoint_Verifier() {
    if (_nsv->_activated) {
      _nsv->_thread->_allow_allocation_count++;
      _nsv->_thread->_allow_safepoint_count++;
    }
  }
#else
  Pause_No_Safepoint_Verifier(No_Safepoint_Verifier * nsv)
    : Pause_No_GC_Verifier(nsv) {}
  ~Pause_No_Safepoint_Verifier() {}
#endif
};

// A SkipGCALot object is used to elide the usual effect of gc-a-lot
// over a section of execution by a thread. Currently, it's used only to
// prevent re-entrant calls to GC.
class SkipGCALot : public StackObj {
  private:
   bool _saved;
   Thread* _t;

  public:
#ifdef ASSERT
    SkipGCALot(Thread* t) : _t(t) {
      _saved = _t->skip_gcalot();
      _t->set_skip_gcalot(true);
    }

    ~SkipGCALot() {
      assert(_t->skip_gcalot(), "Save-restore protocol invariant");
      _t->set_skip_gcalot(_saved);
    }
#else
    SkipGCALot(Thread* t) { }
    ~SkipGCALot() { }
#endif
};

// JRT_LEAF currently can be called from either _thread_in_Java or
// _thread_in_native mode. In _thread_in_native, it is ok
// for another thread to trigger GC. The rest of the JRT_LEAF
// rules apply.
class JRT_Leaf_Verifier : public No_Safepoint_Verifier {
  static bool should_verify_GC();
 public:
#ifdef ASSERT
  JRT_Leaf_Verifier();
  ~JRT_Leaf_Verifier();
#else
  JRT_Leaf_Verifier() {}
  ~JRT_Leaf_Verifier() {}
#endif
};

// A No_Alloc_Verifier object can be placed in methods where one assumes that
// no allocation will occur. The destructor will verify this property
// unless the constructor is called with argument false (not activated).
//
// The check will only be done in debug mode and if activated.
// Note: this only makes sense at safepoints (otherwise, other threads may
// allocate concurrently.)

class No_Alloc_Verifier : public StackObj {
 private:
  bool  _activated;

 public:
#ifdef ASSERT
  No_Alloc_Verifier(bool activated = true) {
    _activated = activated;
    if (_activated) Thread::current()->_allow_allocation_count++;
  }

  ~No_Alloc_Verifier() {
    if (_activated) Thread::current()->_allow_allocation_count--;
  }
#else
  No_Alloc_Verifier(bool activated = true) {}
  ~No_Alloc_Verifier() {}
#endif
};

#endif // SHARE_VM_MEMORY_GCLOCKER_HPP
