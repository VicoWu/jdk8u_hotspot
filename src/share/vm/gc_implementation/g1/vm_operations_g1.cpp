/*
 * Copyright (c) 2001, 2015, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/concurrentMarkThread.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1Log.hpp"
#include "gc_implementation/g1/vm_operations_g1.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/g1/vm_operations_g1.hpp"
#include "runtime/interfaceSupport.hpp"


/**
 * 一个VM_G1CollectForAllocation对象代表了一个由于对象分配失败导致的一次垃圾回收操作尝试
 * 由于一次分配的失败，会创建一个 VM_G1CollectForAllocation对象，这次回收是由于分配失败导致的
 * @param gc_count_before
 * @param word_size
 */
VM_G1CollectForAllocation::VM_G1CollectForAllocation(uint gc_count_before,
                                                     size_t word_size)
  : VM_G1OperationWithAllocRequest(gc_count_before, word_size,
                                   GCCause::_allocation_failure) { // 发起gc请求的原因是"Allocation failure"
  guarantee(word_size != 0, "An allocation should always be requested with this operation.");
}

/**
 * 由于分配失败导致的gc，进行并发full gc的操作，所以需要跟VM_G1IncCollectionPause::doit()区别开
 */
void VM_G1CollectForAllocation::doit() {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  GCCauseSetter x(g1h, _gc_cause);
  // 调用G1CollectedHeap::satisfy_failed_allocation, 由于是VM_G1CollectForAllocation，因此当前调用线程必须是VMThread
  _result = g1h->satisfy_failed_allocation(_word_size, allocation_context(), &_pause_succeeded);
  assert(_result == NULL || _pause_succeeded,
         "if we get back a result, the pause should have succeeded");
}

void VM_G1CollectFull::doit() {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  GCCauseSetter x(g1h, _gc_cause);
  g1h->do_full_collection(false /* clear_all_soft_refs */);
}

VM_G1IncCollectionPause::VM_G1IncCollectionPause(uint           gc_count_before,
                                                 size_t         word_size,
                                                 bool           should_initiate_conc_mark,
                                                 double         target_pause_time_ms,
                                                 GCCause::Cause gc_cause)
  : VM_G1OperationWithAllocRequest(gc_count_before, word_size, gc_cause),
    _should_initiate_conc_mark(should_initiate_conc_mark),
    _target_pause_time_ms(target_pause_time_ms),
    _should_retry_gc(false),
    _old_marking_cycles_completed_before(0) {
  guarantee(target_pause_time_ms > 0.0,
            err_msg("target_pause_time_ms = %1.6lf should be positive",
                    target_pause_time_ms));
  _gc_cause = gc_cause;
}

/**
 * prologue的意思是序言
 * 区别VM_G1IncCollectionPause::doit_epilogue()
 * @return
 */
bool VM_G1IncCollectionPause::doit_prologue() {
  /**
   * 其实是调用 VM_GC_Operation::doit_prologue()，做的事情主要是判断在构造VM_G1IncCollectionPause的事后到现在
   * 准备执行的时候，是否已经进行过其他的gc，从而避免重复gc操作
   */
  bool res = VM_G1OperationWithAllocRequest::doit_prologue();
  /**
   * 父类告知不需要进行重复gc，那么就返回false，但是返回的时候将VM_G1IncCollectionPause的_should_retry_gc设置为true，
   *    这样G1CollectedHeap::collect方法中的循环就会继续尝试判断，否则，就不尝试进行gc重试，返回false
   * 父类告知需要进行gc，就返回true，接着会调用doit
   *
   */
  if (!res) { // 父类的VM_GC_Operation::doit_prologue()返回false，自己也会返回false，但是需要对_should_retry_gc进行一些设置
    if (_should_initiate_conc_mark) { // 用户构造VM_G1IncCollectionPause的时候，制定应该初始化_should_initiate_conc_mark
      // The prologue can fail for a couple of reasons. The first is that another GC
      // got scheduled and prevented the scheduling of the initial mark GC. The
      // second is that the GC locker may be active and the heap can't be expanded.
      // In both cases we want to retry the GC so that the initial mark pause is
      // actually scheduled. In the second case, however, we should stall until
      // until the GC locker is no longer active and then retry the initial mark GC.
      /**
       * doit_prologue()可能会因为几个原因而失败，返回false。
       *    第一个是另一个GC被调度并且阻止了初始标记GC的调度。
       *    第二个是GC locker可能处于活动状态并且堆无法扩展。
       * 在这两种情况下，我们都希望重试 GC，以便实际安排初始标记暂停。
       * 然而，在第二种情况下，我们应该停止直到 GC 锁不再处于活动状态，然后重试初始标记 GC。
       */
       /**
        * _should_retry_gc设置为true，以便initial-mark pause能够被调度。
        * 在collect()方法中，在构建并调用VM_G1IncCollectionPause了以后VMThread::execute(&op)，会判断should_retry_gc()
        */
      _should_retry_gc = true;
    }
  }
  return res;
}
/**
 * 在 G1CollectedHeap::collect 中调用
 * 这里会在stw的环境中尝试进行分配，如果分配失败，就在stw的环境下进行一次回收，然后再尝试进行分配
 * VM_G1IncCollectionPause是一个VM_Operation，因此是在VMThread中执行
 * 并发full gc的操作类，所以需要跟VM_G1CollectForAllocation::doit()区别开
 * 这个方法属于g1CollectedHeap的下层，即被比如g1CollectedHeap::collect调用，调用者通过_pause_succeeded判断这次的转移是否成功，从而决定是否进行重试
 */
void VM_G1IncCollectionPause::doit() { // 进行增量收集
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  // 这里的意思是，如果_should_initiate_conc_mark = true，那么should_do_concurrent_full_gc()肯定是true
  assert(!_should_initiate_conc_mark || g1h->should_do_concurrent_full_gc(_gc_cause),
      "only a GC locker, a System.gc(), stats update, whitebox, or a hum allocation induced GC should start a cycle");

  // 我们搜索VM_G1IncCollectionPause op,
  // 在do_collection_pause()方法中，word_size!=0,说明是一次分配失败导致的gc，并且调用attempt_allocation_at_safepoint的时候，已经STW了
  if (_word_size > 0) { // 如果_word_size > 0，那么先尝试进行一次safepoint的STW分配
    // An allocation has been requested. So, try to do that first.
    /**
     * 由于VM_G1IncCollectionPause是VM_Operation，因此肯定是在safepoint中执行的
     * 区别 do_collection_pause_at_safepoint。
     * attempt_allocation_at_safepoint是尝试在VM_G1IncCollectionPause中进行分配，
     * do_collection_pause_at_safepoint是尝试在VM_G1IncCollectionPause中进行回收和初始标记
     */
    _result = g1h->attempt_allocation_at_safepoint(_word_size, allocation_context(),
                                     false /* expect_null_cur_alloc_region */);
    if (_result != NULL) {
      /**
       * 如果在回收以前就成功进行了分配，那么我们还是认为这个pause是成功的
       */
      _pause_succeeded = true;
      return; // 如果是由于分配导致的gc，那么在尝试分配成功以后，直接返回，不再进行gc
    }
  }
  // 分配失败
  GCCauseSetter x(g1h, _gc_cause);
    /**
     * 我们搜索VM_G1IncCollectionPause op,可以看到构造VM_G1IncCollectionPause时并设置_should_initiate_conc_mark=true的情况
     * 如果设置了需要调度并发标记,那么，只要当前不在一个cycle中，就强制设置_initiate_conc_mark_if_possible=true
     */
  if (_should_initiate_conc_mark) {
    // It's safer to read old_marking_cycles_completed() here, given
    // that no one else will be updating it concurrently. Since we'll
    // only need it if we're initiating a marking cycle, no point in
    // setting it earlier.
    _old_marking_cycles_completed_before = g1h->old_marking_cycles_completed();

    // At this point we are supposed to start a concurrent cycle. We
    // will do so if one is not already in progress.
    /**
   * G1CollectorPolicy的成员方法
   * 如果我们不处于一个并发标记的cycle中，那么设置G1CollectorPolicy的成员变量_initiate_conc_mark_if_possible为true，返回true
   * 否则，如果当前已经处于一个并发标记cycle中，返回false
     */
    bool res = g1h->g1_policy()->force_initial_mark_if_outside_cycle(_gc_cause);

    // The above routine returns true if we were able to force the
    // next GC pause to be an initial mark; it returns false if a
    // marking cycle is already in progress.
    //
    // If a marking cycle is already in progress just return and skip the
    // pause below - if the reason for requesting this initial mark pause
    // was due to a System.gc() then the requesting thread should block in
    // doit_epilogue() until the marking cycle is complete.
    //
    // If this initial mark pause was requested as part of a humongous
    // allocation then we know that the marking cycle must just have
    // been started by another thread (possibly also allocating a humongous
    // object) as there was no active marking cycle when the requesting
    // thread checked before calling collect() in
    // attempt_allocation_humongous(). Retrying the GC, in this case,
    // will cause the requesting thread to spin inside collect() until the
    // just started marking cycle is complete - which may be a while. So
    // we do NOT retry the GC.
    /**
      如果上面的代码成功地将_initiate_conc_mark_if_possible设置为true，那么res的值为 true,这时候会继续下面的暂停
      如果标记周期已在进行中，则返回 false。在返回false的情况下，不会进行下面的标记暂停，但是这时候会通过_should_retry_gc告诉上面的调用者是否进行重试

      如果标记周期已经在进行中即res=false，只需返回并跳过下面的暂停
      如果请求此初始标记暂停的原因是由于 System.gc()，则请求线程应阻塞在 doit_epilogue() 中，直到标记周期完成。

      如果这个初始标记暂停是作为巨大分配的一部分请求的，那么我们知道标记周期一定是由另一个线程（可能也分配了一个巨大对象）启动的，
      因为当请求线程在调用collect()之前检查时没有活动的标记周期 try_allocation_humongous() 中的collect()。
      在这种情况下，重试GC将导致请求线程在collect()内部旋转，直到刚刚开始的标记周期完成——这可能需要一段时间。
      所以我们不会重试 GC。
   */
    if (!res) {  // 当前正处于一个cycle中，那么将跳过下面的转移暂停evacuation pause
      assert(_word_size == 0, "Concurrent Full GC/Humongous Object IM shouldn't be allocating");
      if (_gc_cause != GCCause::_g1_humongous_allocation) { // 基于大对象分配所进行的收集，不进行重试
        _should_retry_gc = true;
      }
      return; // 直接返回，不再尝试下面的gc，这时候，_pause_succeeded=false，并没有被置为true
    }
  }

  /**
   * 调用实例方法 G1CollectedHeap::do_collection_pause_at_safepoint
   * 执行到这里，说明_should_initiate_conc_mark==false，或者_should_initiate_conc_mark=true，并且通过检查，发现当前的确不处于一个并发标记的cycle，
   * 尝试进行增量的回收暂停。执行线程必须是vm_thread
   *      attempt_allocation_at_safepoint是尝试在VM_G1IncCollectionPause中进行分配，
   *      do_collection_pause_at_safepoint是尝试在VM_G1IncCollectionPause中进行回收和初始标记
   **/
  _pause_succeeded =
    g1h->do_collection_pause_at_safepoint(_target_pause_time_ms); // 在这个方法里面，将最终决定是否进行初始标记
  /**
   * 如果成功地进行了一次回收暂停，并且当前正在申请分配空间，那么，立刻进行一次分配
   * 如果回收本身是失败的 ，那么根本不会再尝试进行分配
   */
  if (_pause_succeeded && _word_size > 0) { //
    // An allocation had been requested.
    _result = g1h->attempt_allocation_at_safepoint(_word_size, allocation_context(),
                                      true /* expect_null_cur_alloc_region */);
  } else {
    assert(_result == NULL, "invariant");
    //  查看，可以看到do_collection_pause_at_safepoint()中
    //  只有GC_locker::check_active_before_gc()为false的时候才会让_pause_succeeded = false
    if (!_pause_succeeded) { // 分配不成功，可能是GCLocker 导致的，那么我们需要等GC_locker结束以后再进行
      // Another possible reason reason for the pause to not be successful
      // is that, again, the GC locker is active (and has become active
      // since the prologue was executed). In this case we should retry
      // the pause after waiting for the GC locker to become inactive.
      _should_retry_gc = true; // 应该重试
    }
  }
}

/**
 * 区别VM_G1IncCollectionPause::doit_prologue()
 */
void VM_G1IncCollectionPause::doit_epilogue() {
  VM_G1OperationWithAllocRequest::doit_epilogue();

  // If the pause was initiated by a System.gc() and
  // +ExplicitGCInvokesConcurrent, we have to wait here for the cycle
  // that just started (or maybe one that was already in progress) to
  // finish.
  if (_gc_cause == GCCause::_java_lang_system_gc &&
      _should_initiate_conc_mark) { // 如果当前gc的原因是用户调用System.gc，并且_should_initiate_conc_mark=true
    assert(ExplicitGCInvokesConcurrent,
           "the only way to be here is if ExplicitGCInvokesConcurrent is set");

    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    // In the doit() method we saved g1h->old_marking_cycles_completed()
    // in the _old_marking_cycles_completed_before field. We have to
    // wait until we observe that g1h->old_marking_cycles_completed()
    // has increased by at least one. This can happen if a) we started
    // a cycle and it completes, b) a cycle already in progress
    // completes, or c) a Full GC happens.

    // If the condition has already been reached, there's no point in
    // actually taking the lock and doing the wait.
    //
    if (g1h->old_marking_cycles_completed() <=
                                          _old_marking_cycles_completed_before) {
      // The following is largely copied from CMS
      // 为什么一定是Java Thread? 只有Java Thread才会执行doit_epilogue和doit_prologure
      Thread* thr = Thread::current(); // 需要搜索提交任务VM_G1IncCollectionPause的线程，就知道提交线程是什么线程
      assert(thr->is_Java_thread(), "invariant"); // 触发System.gc()的一定是用户线程
      JavaThread* jt = (JavaThread*)thr;
      ThreadToNativeFromVM native(jt);

      MutexLockerEx x(FullGCCount_lock, Mutex::_no_safepoint_check_flag);
      while (g1h->old_marking_cycles_completed() <=
                                          _old_marking_cycles_completed_before) {
          // block waiting g1h->old_marking_cycles_completed() 增加
          // 在increment_old_marking_cycles_completed()中，会在锁FullGCCount_lock上执行通知
          // 即一个cycle或者一个full gc执行完毕以后，会调用increment_old_marking_cycles_completed()，然后通知在这上面等待的线程
        FullGCCount_lock->wait(Mutex::_no_safepoint_check_flag);// 放弃锁，并等待
      }
    }
  }
}
/**
 * 获取java_lang_ref_Reference类的静态对象锁，该锁用于处理软引用
 */
void VM_CGC_Operation::acquire_pending_list_lock() {
  assert(_needs_pll, "don't call this otherwise");
  // The caller may block while communicating
  // with the SLT thread in order to acquire/release the PLL.
  SurrogateLockerThread* slt = ConcurrentMarkThread::slt();
  if (slt != NULL) {
    slt->manipulatePLL(SurrogateLockerThread::acquirePLL);
  } else {
    SurrogateLockerThread::report_missing_slt();
  }
}

void VM_CGC_Operation::release_and_notify_pending_list_lock() {
  assert(_needs_pll, "don't call this otherwise");
  // The caller may block while communicating
  // with the SLT thread in order to acquire/release the PLL.
  ConcurrentMarkThread::slt()->
    manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}

void VM_CGC_Operation::doit() {
  TraceCPUTime tcpu(G1Log::finer(), true, gclog_or_tty);
  GCTraceTime t(_printGCMessage, G1Log::fine(), true, G1CollectedHeap::heap()->gc_timer_cm(), G1CollectedHeap::heap()->concurrent_mark()->concurrent_gc_id());
  SharedHeap* sh = SharedHeap::heap();
  // This could go away if CollectedHeap gave access to _gc_is_active...
  if (sh != NULL) {
    IsGCActiveMark x;
    _cl->do_void();
  } else {
    _cl->do_void();
  }
}

bool VM_CGC_Operation::doit_prologue() {
  // Note the relative order of the locks must match that in
  // VM_GC_Operation::doit_prologue() or deadlocks can occur
  if (_needs_pll) {
    acquire_pending_list_lock();
  }

  Heap_lock->lock();
  SharedHeap::heap()->_thread_holds_heap_lock_for_gc = true;
  return true;
}

void VM_CGC_Operation::doit_epilogue() {
  // Note the relative order of the unlocks must match that in
  // VM_GC_Operation::doit_epilogue()
  SharedHeap::heap()->_thread_holds_heap_lock_for_gc = false;
  Heap_lock->unlock();
  if (_needs_pll) {
    release_and_notify_pending_list_lock();
  }
}
