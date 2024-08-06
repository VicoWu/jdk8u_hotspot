/*
 * Copyright (c) 2005, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoader.hpp"
#include "classfile/javaClasses.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/oopFactory.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceRefKlass.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/interfaceSupport.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/preserveException.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#endif // INCLUDE_ALL_GCS

#ifndef USDT2
HS_DTRACE_PROBE_DECL1(hotspot, gc__begin, bool);
HS_DTRACE_PROBE_DECL(hotspot, gc__end);
#endif /* !USDT2 */

// The same dtrace probe can't be inserted in two different files, so we
// have to call it here, so it's only in one file.  Can't create new probes
// for the other file anymore.   The dtrace probes have to remain stable.
void VM_GC_Operation::notify_gc_begin(bool full) {
#ifndef USDT2
  HS_DTRACE_PROBE1(hotspot, gc__begin, full);
  HS_DTRACE_WORKAROUND_TAIL_CALL_BUG();
#else /* USDT2 */
  HOTSPOT_GC_BEGIN(
                   full);
#endif /* USDT2 */
}

void VM_GC_Operation::notify_gc_end() {
#ifndef USDT2
  HS_DTRACE_PROBE(hotspot, gc__end);
  HS_DTRACE_WORKAROUND_TAIL_CALL_BUG();
#else /* USDT2 */
  HOTSPOT_GC_END(
);
#endif /* USDT2 */
}

void VM_GC_Operation::acquire_pending_list_lock() {
  // we may enter this with pending exception set
  InstanceRefKlass::acquire_pending_list_lock(&_pending_list_basic_lock);
}


void VM_GC_Operation::release_and_notify_pending_list_lock() {

  InstanceRefKlass::release_and_notify_pending_list_lock(&_pending_list_basic_lock);
}

// Allocations may fail in several threads at about the same time,
// resulting in multiple gc requests.  We only want to do one of them.
// In case a GC locker is active and the need for a GC is already signalled,
// we want to skip this GC attempt altogether, without doing a futile
// safepoint operation. 避免无意义的安全点操作
// 这里skip的原因是，构造VM_GC_Operation的时候的gc数量不等于实时的gc的数量，说明中间已经进行过gc，没必要重复进行无意义的gc
bool VM_GC_Operation::skip_operation() const {
  bool skip = (_gc_count_before != Universe::heap()->total_collections()); // 如果获取PLL以前统计的gc数量(即构造这个VM_GC_Operation对象时的GC数量)已经不等于当前实时的gc数量，说明中间经历了多次gc
  if (_full && skip) { // 尽管整个gc的数量不一致(skip = true)，但是，目前这个VM_GC_Operation是用来进行full gc，那么，只要full gc的数量不一致，skip依然可以为false
    skip = (_full_gc_count_before != Universe::heap()->total_full_collections()); // full gc 的数量统计也不对
  }
  // skip 是false，说明构造VM_GC_Operation的时候整个gc的数量等于目前的gc数量，或者，尽管整体数量不等，但是当前进行的是full gc并且full gc的数量等于目前实时的full gc的数量
  // is_active_and_needs_gc()的内在含义是，当前有线程处在临界区，并且内存是需要gc的状态，当线程离开临界区的时候，肯定会触发一次gc请求(查看代码jni_unlock())
  // 中间的确没有进行过其他gc，并且当前有线程在临界区并且内存状态需要gc(需要gc的意思是，有回收尝试进行，发现is_active=true，因此设置needs_gc=true并放弃)，并且已经进行了signal，并且GCLocker是活跃的
  if (!skip && GC_locker::is_active_and_needs_gc()) {
      // 堆可以扩展，skip == false；堆无法扩展，skip == true
      skip = Universe::heap()->is_maximal_no_gc(); // 如果可用region的数量等于0，即堆内存已经无法扩展达到最大状态，那么就skip，如果可用的region数量不等于0，即还有可用内存，那么就需要执行本次gc
    // 如果skip为true，_gc_cause不可以是GCCause::_gc_locker，搜索Universe::heap()->collect(GCCause::_gc_locker)，
    // 在GC_Locker::jni_unlock中，会发起一个gc,有可能是并发gc，也有可能是G1IncCollectionPause等
    /**
    * skip==true代表堆已经无法扩展，GC_Locker处于活跃状态，并且GC_Locker发起了一次请求
    */
    assert(!(skip && (_gc_cause == GCCause::_gc_locker)), // 从jni_unlock()可以看到，只有当is_active为false并且needs_gc()为true的时候，_gc_cause才会为GCCause::_gc_locker
           "GC_locker cannot be active when initiating GC");
    /**
     * !(skip && (_gc_cause == GCCause::_gc_locker))：对上述条件取否定。即，如果需要跳过垃圾回收，
     * 且是由 GC locker 触发的，那么断言会失败，因为这意味着在 GC locker 活动的情况下仍然试图触发垃圾回收。

    "GC_locker cannot be active when initiating GC"：断言失败时输出的错误消息，说明触发垃圾回收时，不允许 GC locker 处于活动状态。

        这个断言的目的是确保在 GC locker 处于活动状态时，不会触发垃圾回收，因为 GC locker 本身可能会在一些关键区域执行代码，而在这些区域触发垃圾回收可能导致不一致的状态。
        因此，这个断言确保了在垃圾回收被触发时，不会同时存在 GC locker 的活动。如果条件不满足，会触发断言错误。在生产环境中，这样的断言通常用于调试目的，以便尽早发现潜在问题。
     */

  }
  return skip;
}

/**
 * 这个doit_prologue会在子类的doit_prologue中被率先调用，比如
 * @return
 */
bool VM_GC_Operation::doit_prologue() {
    // 必须是Java Thread
  assert(Thread::current()->is_Java_thread(), "just checking");
  assert(((_gc_cause != GCCause::_no_gc) &&
          (_gc_cause != GCCause::_no_cause_specified)), "Illegal GCCause"); // 必须有gc cause

  // To be able to handle a GC the VM initialization needs to be completed.
  if (!is_init_completed()) { // 在初始化完成以前就出发了这个操作，这可能是由于newSize太小导致
    vm_exit_during_initialization(
      err_msg("GC triggered before VM initialization completed. Try increasing "
              "NewSize, current value " UINTX_FORMAT "%s.",
              byte_size_in_proper_unit(NewSize),
              proper_unit_for_byte_size(NewSize)));
  }
  //  确保获取了Reference的Pending List Lock
  acquire_pending_list_lock();
  // If the GC count has changed someone beat us to the collection
  // Get the Heap_lock after the pending_list_lock.
  Heap_lock->lock(); // 锁堆内存

  // Check invocations
  // skip本次执行，判断的主要标准是构造VM_GC_Operation对象的时候传入的回收次数信息，
  // 和当前实时的回收次数信息是否一致，如果不一致，说明中间已经进行过gc，因此没必要再重复进行gc
  if (skip_operation()) {
    // skip collection
    Heap_lock->unlock(); // 释放Heap_Lock
    release_and_notify_pending_list_lock(); // 释放PLL
    _prologue_succeeded = false; // 返回false
  } else { //继续执行
    _prologue_succeeded = true; // 返回true
    SharedHeap* sh = SharedHeap::heap();
    if (sh != NULL) sh->_thread_holds_heap_lock_for_gc = true;
  }
  return _prologue_succeeded;
}


void VM_GC_Operation::doit_epilogue() {
  assert(Thread::current()->is_Java_thread(), "just checking");
  // Release the Heap_lock first.
  SharedHeap* sh = SharedHeap::heap();
  if (sh != NULL) sh->_thread_holds_heap_lock_for_gc = false;
  Heap_lock->unlock();
  release_and_notify_pending_list_lock();
}

bool VM_GC_HeapInspection::doit_prologue() {
  if (Universe::heap()->supports_heap_inspection()) {
    return VM_GC_Operation::doit_prologue();
  } else {
    return false;
  }
}

bool VM_GC_HeapInspection::skip_operation() const {
  assert(Universe::heap()->supports_heap_inspection(), "huh?");
  return false;
}

bool VM_GC_HeapInspection::collect() {
  if (GC_locker::is_active()) {
    return false;
  }
  Universe::heap()->collect_as_vm_thread(GCCause::_heap_inspection);
  return true;
}

void VM_GC_HeapInspection::doit() {
  HandleMark hm;
  Universe::heap()->ensure_parsability(false); // must happen, even if collection does
                                               // not happen (e.g. due to GC_locker)
                                               // or _full_gc being false
  if (_full_gc) {
    if (!collect()) {
      // The collection attempt was skipped because the gc locker is held.
      // The following dump may then be a tad misleading to someone expecting
      // only live objects to show up in the dump (see CR 6944195). Just issue
      // a suitable warning in that case and do not attempt to do a collection.
      // The latter is a subtle point, because even a failed attempt
      // to GC will, in fact, induce one in the future, which we
      // probably want to avoid in this case because the GC that we may
      // be about to attempt holds value for us only
      // if it happens now and not if it happens in the eventual
      // future.
      warning("GC locker is held; pre-dump GC was skipped");
    }
  }
  HeapInspection inspect(_csv_format, _print_help, _print_class_stats,
                         _columns);
  inspect.heap_inspection(_out);
}


void VM_GenCollectForAllocation::doit() {
  SvcGCMarker sgcm(SvcGCMarker::MINOR);

  GenCollectedHeap* gch = GenCollectedHeap::heap();
  GCCauseSetter gccs(gch, _gc_cause);
  _result = gch->satisfy_failed_allocation(_word_size, _tlab);
  assert(gch->is_in_reserved_or_null(_result), "result not in heap");

  if (_result == NULL && GC_locker::is_active_and_needs_gc()) {
    set_gc_locked();
  }
}

static bool is_full_gc(int max_level) {
  // Return true if max_level is all generations
  return (max_level == (GenCollectedHeap::heap()->n_gens() - 1));
}

VM_GenCollectFull::VM_GenCollectFull(uint gc_count_before,
                                     uint full_gc_count_before,
                                     GCCause::Cause gc_cause,
                                     int max_level) :
  VM_GC_Operation(gc_count_before, gc_cause, full_gc_count_before,
                  is_full_gc(max_level) /* full */),
  _max_level(max_level) { }

void VM_GenCollectFull::doit() {
  SvcGCMarker sgcm(SvcGCMarker::FULL);

  GenCollectedHeap* gch = GenCollectedHeap::heap();
  GCCauseSetter gccs(gch, _gc_cause);
  gch->do_full_collection(gch->must_clear_all_soft_refs(), _max_level);
}

VM_CollectForMetadataAllocation::VM_CollectForMetadataAllocation(ClassLoaderData* loader_data,
                                                                 size_t size,
                                                                 Metaspace::MetadataType mdtype,
                                                                 uint gc_count_before,
                                                                 uint full_gc_count_before,
                                                                 GCCause::Cause gc_cause)
    : VM_GC_Operation(gc_count_before, gc_cause, full_gc_count_before, true),
      _loader_data(loader_data), _size(size), _mdtype(mdtype), _result(NULL) {
  assert(_size != 0, "An allocation should always be requested with this operation.");
  AllocTracer::send_allocation_requiring_gc_event(_size * HeapWordSize, GCId::peek());
}

// Returns true iff concurrent GCs unloads metadata.
/**
 * 尝试初始化一个并发标记周期，即调用一次带有初始标记的回收暂停
 * @return
 */
bool VM_CollectForMetadataAllocation::initiate_concurrent_GC() {
#if INCLUDE_ALL_GCS
  if (UseConcMarkSweepGC && CMSClassUnloadingEnabled) {
    MetaspaceGC::set_should_concurrent_collect(true);
    return true;
  }

  /**
   *  在 metadata分配失败的时候，如果当前的G1GC enable了类卸载，那么，我们需要请求一次带有初始标记的回收暂停，以及在并发标记的清理阶段进行无效类的卸载操作
   */
  if (UseG1GC && ClassUnloadingWithConcurrentMark) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    /**
     * 设置初始标记请求，这样在回收暂停的时候就会进行一次初始标记并启动一个并发标记周期
     */
    g1h->g1_policy()->set_initiate_conc_mark_if_possible();

    GCCauseSetter x(g1h, _gc_cause);

    // At this point we are supposed to start a concurrent cycle. We
    // will do so if one is not already in progress.
    /**
     *  如果我们不处于一个并发标记的cycle中，那么设置G1CollectorPolicy的成员变量_initiate_conc_mark_if_possible为true，
     *  返回true，表示当前可以进行一次全新的标记周期，即开始进行初始标记过程
     *  否则，如果当前已经处于一个并发标记cycle中，返回false
     */
    bool should_start = g1h->g1_policy()->force_initial_mark_if_outside_cycle(_gc_cause);

    if (should_start) { // 当前不在任何一次concurrent mark cycle中，已经设置了_initiate_conc_mark_if_possible
      double pause_target = g1h->g1_policy()->max_pause_time_ms();
      /**
       * 调用 do_collection_pause_at_safepoint，触发一次带有初始标记的回收暂停
       */
      g1h->do_collection_pause_at_safepoint(pause_target);
    }
    return true;
  }
#endif

  return false;
}

static void log_metaspace_alloc_failure_for_concurrent_GC() {
  if (Verbose && PrintGCDetails) {
    if (UseConcMarkSweepGC) {
      gclog_or_tty->print_cr("\nCMS full GC for Metaspace");
    } else if (UseG1GC) {
      gclog_or_tty->print_cr("\nG1 full GC for Metaspace");
    }
  }
}

void VM_CollectForMetadataAllocation::doit() {
  SvcGCMarker sgcm(SvcGCMarker::FULL);

  CollectedHeap* heap = Universe::heap();
  GCCauseSetter gccs(heap, _gc_cause);

  // Check again if the space is available.  Another thread
  // may have similarly failed a metadata allocation and induced
  // a GC that freed space for the allocation.
  if (!MetadataAllocationFailALot) {
    _result = _loader_data->metaspace_non_null()->allocate(_size, _mdtype);
    if (_result != NULL) {
      return;
    }
  }
  /**
   * 在这里会触发一次并发标记请求
   * initiate_concurrent_GC返回true，说明已经通过一次stw 触发了一次回收，因此尝试进行扩展和分配
   */
  if (initiate_concurrent_GC()) {
    // For CMS and G1 expand since the collection is going to be concurrent.
    _result = _loader_data->metaspace_non_null()->expand_and_allocate(_size, _mdtype);
    if (_result != NULL) {
      return;
    }

    log_metaspace_alloc_failure_for_concurrent_GC();
  }

  // Don't clear the soft refs yet.
  /**
   * 在完成了初始标记以后，尝试进行一次full gc，这是一次并行的full gc，与用户线程同时执行，不会清理软引用
   */
  heap->collect_as_vm_thread(GCCause::_metadata_GC_threshold);
  // After a GC try to allocate without expanding.  Could fail
  // and expansion will be tried below.
  /**
   * 在进行了full gc以后，在不尝试扩展元数据的情况下再次进行分配尝试
   */
  _result = _loader_data->metaspace_non_null()->allocate(_size, _mdtype);
  if (_result != NULL) { // full gc以后分配成功
    return; // 分配成功，返回
  }

  // If still failing, allow the Metaspace to expand.
  // See delta_capacity_until_GC() for explanation of the
  // amount of the expansion.
  // This should work unless there really is no more space
  // or a MaxMetaspaceSize has been specified on the command line.
  /**
   * 如果仍然失败，请允许扩展元空间。 有关扩展数量的说明，请参阅 delta_capacity_until_GC()。
   * 除非确实没有更多空间或者已在命令行上指定了 MaxMetaspaceSize，否则这应该有效。
   */
  _result = _loader_data->metaspace_non_null()->expand_and_allocate(_size, _mdtype);
  if (_result != NULL) {
    return;
  }

  // If expansion failed, do a last-ditch collection and try allocating
  // again.  A last-ditch collection will clear softrefs.  This
  // behavior is similar to the last-ditch collection done for perm
  // gen when it was full and a collection for failed allocation
  // did not free perm gen space.
  /**
   * 如果扩展失败，请进行最后一搏的收集并尝试再次分配。
   * 最后的收集将清除软引用。 此行为类似于在永久代已满时进行的最后一次收集，并且分配失败以后的收集行为并没有释放永久代空间。
   */
  heap->collect_as_vm_thread(GCCause::_last_ditch_collection);
  _result = _loader_data->metaspace_non_null()->allocate(_size, _mdtype);
  if (_result != NULL) {
    return; // 分配成功，返回
  }

  if (Verbose && PrintGCDetails) {
    gclog_or_tty->print_cr("\nAfter Metaspace GC failed to allocate size "
                           SIZE_FORMAT, _size);
  }
  // 如果当前发现处于临界区，并且已经设置了_needs_gc变量，那么设置gc lock，调用者发现设置了GC lock，那么就可以尝试进行重试，而不是直接放弃
  if (GC_locker::is_active_and_needs_gc()) {
    set_gc_locked();
  }
}

VM_CollectForAllocation::VM_CollectForAllocation(size_t word_size, uint gc_count_before, GCCause::Cause cause)
    : VM_GC_Operation(gc_count_before, cause), _result(NULL), _word_size(word_size) {
  // Only report if operation was really caused by an allocation.
  if (_word_size != 0) {
    AllocTracer::send_allocation_requiring_gc_event(_word_size * HeapWordSize, GCId::peek());
  }
}
