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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1COLLECTEDHEAP_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1COLLECTEDHEAP_INLINE_HPP

#include "gc_implementation/g1/concurrentMark.hpp"
#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/g1AllocRegion.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/g1/heapRegionManager.inline.hpp"
#include "gc_implementation/g1/heapRegionSet.inline.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "utilities/taskqueue.hpp"
/**
 * 根据对应的InCSetState，获取对应的PLABStats对应
 * 在 G1CollectedHeap::alloc_buffer_stats中被调用
 * @param dest
 * @return
 */
PLABStats* G1CollectedHeap::alloc_buffer_stats(InCSetState dest) {
  switch (dest.value()) {
    case InCSetState::Young:
      return &_survivor_plab_stats; // 返回Young的PLABStats对象,搜索 G1CollectedHeap::G1CollectedHeap构造方法，查看_survivor_plab_stats的构造
    case InCSetState::Old:
      return &_old_plab_stats; // 返回Old的PLABStats对象,搜索 G1CollectedHeap::G1CollectedHeap构造方法，查看_survivor_plab_stats的构造
    default:
      ShouldNotReachHere();
      return NULL; // Keep some compilers happy
  }
}

/**
 * 搜索 G1ParGCAllocator::allocate_direct_or_new_plab 查看调用者
 * 根据当前的dest状态，分配应该创建的PLAB的大小
 * 查看G1CollectedHeap::G1CollectedHeap，可以看到分别对于young和old的gclab_word_size
 */
size_t G1CollectedHeap::desired_plab_sz(InCSetState dest) {
    /**
     * 根据目标区域的状态InCSetState, 获取对应区域的PLATStats对象，然后获取这个区域的plab的大小，以字为单位
     */
  size_t gclab_word_size = alloc_buffer_stats(dest)->desired_plab_sz();
  // Prevent humongous PLAB sizes for two reasons:
  // * PLABs are allocated using a similar paths as oops, but should
  //   never be in a humongous region
  // * Allowing humongous PLABs needlessly churns the region free lists
  // gclab_word_size始终不应该大于大对象的word_size
  return MIN2(_humongous_object_threshold_in_words, gclab_word_size);
}

/**
 * 根据对象的目标状态，选择在那一块儿区域进行对象分配，然后直接在对应内存中进行对象分配(而不是通过plab分配)
 * @param dest
 * @param word_size
 * @param context
 * @return
 */
HeapWord* G1CollectedHeap::par_allocate_during_gc(InCSetState dest,
                                                  size_t word_size,
                                                  AllocationContext_t context) {
  switch (dest.value()) {
    case InCSetState::Young: // Young状态，那么一定是在survivor区域进行分配
      return survivor_attempt_allocation(word_size, context);
    case InCSetState::Old: // Old状态，那么一定是尝试在老年代进行分配
      return old_attempt_allocation(word_size, context);
    default:
      ShouldNotReachHere();
      return NULL; // Keep some compilers happy
  }
}

// Inline functions for G1CollectedHeap

inline AllocationContextStats& G1CollectedHeap::allocation_context_stats() {
  return _allocation_context_stats;
}

// Return the region with the given index. It assumes the index is valid.
inline HeapRegion* G1CollectedHeap::region_at(uint index) const { return _hrm.at(index); }

inline uint G1CollectedHeap::addr_to_region(HeapWord* addr) const {
  assert(is_in_reserved(addr),
         err_msg("Cannot calculate region index for address " PTR_FORMAT " that is outside of the heap [" PTR_FORMAT ", " PTR_FORMAT ")",
                 p2i(addr), p2i(_reserved.start()), p2i(_reserved.end())));
  return (uint)(pointer_delta(addr, _reserved.start(), sizeof(uint8_t)) >> HeapRegion::LogOfHRGrainBytes);
}

/**
 * 将Region的index获取对应的Region的地址，一个HeapWord指针
 */
inline HeapWord* G1CollectedHeap::bottom_addr_for_region(uint index) const {
  return _hrm.reserved().start() + index * HeapRegion::GrainWords;
}

template <class T>
inline HeapRegion* G1CollectedHeap::heap_region_containing_raw(const T addr) const {
  assert(addr != NULL, "invariant");
  assert(is_in_g1_reserved((const void*) addr),
      err_msg("Address " PTR_FORMAT " is outside of the heap ranging from [" PTR_FORMAT " to " PTR_FORMAT ")",
          p2i((void*)addr), p2i(g1_reserved().start()), p2i(g1_reserved().end())));
  /**
   * 调用 HeapRegionManager的addr_to_region方法，有可能返回null
   * 搜索 HeapRegion* HeapRegionManager::addr_to_region
   */
  return _hrm.addr_to_region((HeapWord*) addr);
}

template <class T>
inline HeapRegion* G1CollectedHeap::heap_region_containing(const T addr) const {
  HeapRegion* hr = heap_region_containing_raw(addr); // 搜索 HeapRegion* G1CollectedHeap::heap_region_containing_raw
  if (hr->continuesHumongous()) {
    return hr->humongous_start_region();
  }
  return hr;
}

inline void G1CollectedHeap::reset_gc_time_stamp() {
  _gc_time_stamp = 0;
  OrderAccess::fence();
  // Clear the cached CSet starting regions and time stamps.
  // Their validity is dependent on the GC timestamp.
  clear_cset_start_regions();
}

inline void G1CollectedHeap::increment_gc_time_stamp() {
  ++_gc_time_stamp;
  OrderAccess::fence();
}

inline void G1CollectedHeap::old_set_remove(HeapRegion* hr) {
  _old_set.remove(hr);
}

inline bool G1CollectedHeap::obj_in_cs(oop obj) {
  HeapRegion* r = _hrm.addr_to_region((HeapWord*) obj);
  return r != NULL && r->in_collection_set();
}

/**
 * in book
 * 这个方法不会负责大对象的分配，大对象的分配由方法attempt_allocation_humongous()负责
 * 由于取出的是mutator_alloc_region，因此可见这个方法只负责给用户层面进行内存分配
 * 注意将该方法与 G1AllocRegion::attempt_allocation()区分，两个方法同名，但是处于不同的层次
 * 从G1CollectedHeap::mem_allocate()可以看到，方法G1CollectedHeap::attempt_allocation()的是TLAB分配失败后才会尝试调用的
 * @param word_size
 * @param gc_count_before_ret
 * @param gclocker_retry_count_ret
 * @return
 */
inline HeapWord* G1CollectedHeap::attempt_allocation(size_t word_size,
                                                     uint* gc_count_before_ret,
                                                     uint* gclocker_retry_count_ret) {
    // 不可以处于安全点，在Heap_lock上不可以枷锁
  assert_heap_not_locked_and_not_at_safepoint();
  // 大对象不由这个方法负责
  assert(!isHumongous(word_size), "attempt_allocation() should not "
         "be called for humongous allocation requests");  // 大对象不应该通过attempt_allocation来进行分配

  // 进行一次非TLAB的内存分配
  AllocationContext_t context = AllocationContext::current();
  // 这里的_allocator实现是G1DefaultAllocator，因此多态地调用了G1DefaultAllocator->mutator_alloc_region()
  // mutator_alloc_region是专门用来给用户线程分配内存的方法，即在eden区分配内存的方法
  // 在GC过程中，还会在其他区域比如survivor区域分配，也会在old区域分配，分别使用的方式是survivor_gc_alloc_region(), old_gc_alloc_region()
  HeapWord* result = _allocator
          ->mutator_alloc_region(context) // 返回MutatorAllocRegion对象
                  // 调用 MutatorAllocRegion::attempt_allocation(),实际上是调用父类方法G1AllocRegion::attempt_allocation()，
                  // 由于是young 区，因此bot_update是false
          ->attempt_allocation(word_size, false /* bot_updates */);
  if (result == NULL) { //  尝试分配但是失败，因此调用G1CollectedHeap::attempt_allocation_slow()进行慢分配
    // 这里会通过上锁的方式调用 G1CollectedHeap::attempt_allocation_slow()进行分配，如果分配失败，方法中会直接进行gc
    result = attempt_allocation_slow(word_size, // 尝试进行一次full gc然后再接着进行分配
                                     context,
                                     gc_count_before_ret,
                                     gclocker_retry_count_ret);
  }
  // 分配结束。无论成功与否，无论上锁与否，执行到这里，Heap_lock必须已经释放了
  assert_heap_not_locked();
  if (result != NULL) {
    dirty_young_block(result, word_size);
  }
  return result;
}

/**
 * in book
 * 调用者 G1CollectedHeap::par_allocate_during_gc
 * 是在尝试了PLAB或者TLAB以后直接在堆中进行分配
 * @param word_size
 * @param context
 * @return
 */
inline HeapWord* G1CollectedHeap::survivor_attempt_allocation(size_t word_size,
                                                              AllocationContext_t context) {
  assert(!isHumongous(word_size),
         "we should not be seeing humongous-size allocations in this path");

  /**
   *
   */
  HeapWord* result = _allocator
          ->survivor_gc_alloc_region(context) // 返回对应的SurvivorGCAllocRegion对象,是G1AllocRegion的子对象
          ->attempt_allocation(word_size,false /* bot_updates */); // 搜索 G1AllocRegion::attempt_allocation 查看具体实现
  if (result == NULL) {
    MutexLockerEx x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    /**
     * 由于是在年轻代进行分配，因此不需要update bot
     */
    result = _allocator->survivor_gc_alloc_region(context)->attempt_allocation_locked(word_size,
                                                                                      false /* bot_updates */);
  }
  if (result != NULL) {
    dirty_young_block(result, word_size);
  }
  return result;
}

/**
 * 调用者 G1CollectedHeap::par_allocate_during_gc
 * @param word_size
 * @param context
 * @return
 */
inline HeapWord* G1CollectedHeap::old_attempt_allocation(size_t word_size,
                                                         AllocationContext_t context) {
  assert(!isHumongous(word_size),
         "we should not be seeing humongous-size allocations in this path");
  /**
   * 返回的是OldGCAllocRegion对象，而OldGCAllocRegion类是G1AllocRegion 的子类，
   *    因此这里实际上调用的是inline HeapWord* G1AllocRegion::attempt_allocation方法
   */
  HeapWord* result = _allocator->old_gc_alloc_region(context)->attempt_allocation(word_size,
                                                                                  true /* bot_updates */);
  if (result == NULL) {
    MutexLockerEx x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    result = _allocator->old_gc_alloc_region(context)->attempt_allocation_locked(word_size,
                                                                                 true /* bot_updates */);
  }
  return result;
}

// It dirties the cards that cover the block so that so that the post
// write barrier never queues anything when updating objects on this
// block. It is assumed (and in fact we assert) that the block
// belongs to a young region.
inline void
G1CollectedHeap::dirty_young_block(HeapWord* start, size_t word_size) {
  assert_heap_not_locked();

  // Assign the containing region to containing_hr so that we don't
  // have to keep calling heap_region_containing_raw() in the
  // asserts below.
  DEBUG_ONLY(HeapRegion* containing_hr = heap_region_containing_raw(start);)
  assert(word_size > 0, "pre-condition");
  assert(containing_hr->is_in(start), "it should contain start");
  assert(containing_hr->is_young(), "it should be young");
  assert(!containing_hr->isHumongous(), "it should not be humongous");

  HeapWord* end = start + word_size;
  assert(containing_hr->is_in(end - 1), "it should also contain end - 1");

  MemRegion mr(start, end);
  g1_barrier_set()->g1_mark_as_young(mr);
}

inline RefToScanQueue* G1CollectedHeap::task_queue(int i) const {
  return _task_queues->queue(i);
}

inline bool G1CollectedHeap::isMarkedPrev(oop obj) const {
  return _cm->prevMarkBitMap()->isMarked((HeapWord *)obj);
}

inline bool G1CollectedHeap::isMarkedNext(oop obj) const {
  return _cm->nextMarkBitMap()->isMarked((HeapWord *)obj);
}

// This is a fast test on whether a reference points into the
// collection set or not. Assume that the reference
// points into the heap.
inline bool G1CollectedHeap::is_in_cset(oop obj) {
  bool ret = _in_cset_fast_test.is_in_cset((HeapWord*)obj);
  // let's make sure the result is consistent with what the slower
  // test returns
  assert( ret || !obj_in_cs(obj), "sanity");
  assert(!ret ||  obj_in_cs(obj), "sanity");
  return ret;
}

bool G1CollectedHeap::is_in_cset_or_humongous(const oop obj) {
  return _in_cset_fast_test.is_in_cset_or_humongous((HeapWord*)obj);
}

InCSetState G1CollectedHeap::in_cset_state(const oop obj) {
  return _in_cset_fast_test.at((HeapWord*)obj);
}


void G1CollectedHeap::register_humongous_region_with_in_cset_fast_test(uint index) {
  _in_cset_fast_test.set_humongous(index); // void set_humongous(uintptr_t index) {
}

#ifndef PRODUCT
// Support for G1EvacuationFailureALot

/**
 * 根据传入的参数逐个检查是否在指定的GC类型期间启用了EFL策略。
 * 如果传入的参数指示某个阶段需要启用EFL策略，就将相应的EFL标志位设置为true。最后，函数返回res，表示是否启用了EFL策略。
 */
inline bool
G1CollectedHeap::evacuation_failure_alot_for_gc_type(bool gcs_are_young,
                                                     bool during_initial_mark,
                                                     bool during_marking) {
  bool res = false;
  if (during_marking) {
    res |= G1EvacuationFailureALotDuringConcMark;
  }
  if (during_initial_mark) {
    res |= G1EvacuationFailureALotDuringInitialMark;
  }
  /**
   *  下面的是一个if/else clause
   */
  if (gcs_are_young) {
    res |= G1EvacuationFailureALotDuringYoungGC;
  } else {
    // GCs are mixed
    res |= G1EvacuationFailureALotDuringMixedGC;
  }
  return res;
}

/**
 * 调用者是 G1CollectedHeap::do_collection_pause_at_safepoint -> G1CollectedHeap::evacuate_collection_set
 * 所以这个方法只是在young/mix gc调用开始的时候会调用，用来根据当前总的gc次数设置_evacuation_failure_alot_for_current_gc
 *   而在方法 G1CollectedHeap::evacuation_should_fail() 中会判断是否应该fail掉这个evacuation
 */
inline void
G1CollectedHeap::set_evacuation_failure_alot_for_current_gc() {
  if (G1EvacuationFailureALot) { // 如果当前jvm设置了G1EvacuationFailureALot
    // Note we can't assert that _evacuation_failure_alot_for_current_gc
    // is clear here. It may have been set during a previous GC but that GC
    // did not copy enough objects (i.e. G1EvacuationFailureALotCount) to
    // trigger an evacuation failure and clear the flags and and counts.

    // Check if we have gone over the interval.
    /**
     * 搜 increment_total_collections 查看_tocal_collections的增加过程,在进行young/mix gc或者full gc的时候都会调用这个方法
     */
    const size_t gc_num = total_collections(); // 统计整个gc次数(包括young gc和full gc)
    /**
     * _evacuation_failure_alot_gc_number记录的是本地full gc开始的时候的total_collections()
     * 因此elapsed_gcs代表这个interval之间发生的gc的次数
     */

    const size_t elapsed_gcs = gc_num - _evacuation_failure_alot_gc_number;
    /**
     * 判断在这个interval中间发生的gc次数是否大于配置的 G1EvacuationFailureALotInterval
     */
    _evacuation_failure_alot_for_current_gc = (elapsed_gcs >= G1EvacuationFailureALotInterval);

    // Now check if G1EvacuationFailureALot is enabled for the current GC type.
    const bool gcs_are_young = g1_policy()->gcs_are_young();
    const bool during_im = g1_policy()->during_initial_mark_pause();
    const bool during_marking = mark_in_progress();
    /**
     * 最终的_evacuation_failure_alot_for_current_gc 还取决于evacuation_failure_alot_for_gc_type的计算值
     * 即根据当前的标记状态(初始标记还是并发标记)，以及gc类型(young还是mix),和用户配置的各种状态下是否需要enable EFL，来决定是否可以对当前的gc enable EFL
     */
    _evacuation_failure_alot_for_current_gc &=
      evacuation_failure_alot_for_gc_type(gcs_are_young,
                                          during_im,
                                          during_marking);
  }
}

/**
 *  在 copy_to_survivor_space 中调用
 * @return
 */
inline bool G1CollectedHeap::evacuation_should_fail() {
  if (!G1EvacuationFailureALot || !_evacuation_failure_alot_for_current_gc) {
    return false;
  }
  // G1EvacuationFailureALot is in effect for current GC
  // Access to _evacuation_failure_alot_count is not atomic;
  // the value does not have to be exact.
  /**
   * 执行到这里，意味着G1EvacuationFailureALot = true，并且 _evacuation_failure_alot_for_current_gc = true
   * 即用户配置了着G1EvacuationFailureALot，同时_evacuation_failure_alot_for_current_gc意味着G1EvacuationFailureALot在本轮gc中应该生效
   *
   */
  if (++_evacuation_failure_alot_count < G1EvacuationFailureALotCount) {
    return false; // 次数还没达到，先不进行fail
  }
  /**
   * 次数达到，重置_evacuation_failure_alot_count，返回true
   */
  _evacuation_failure_alot_count = 0;
  return true;
}

inline void G1CollectedHeap::reset_evacuation_should_fail() {
  if (G1EvacuationFailureALot) {
    _evacuation_failure_alot_gc_number = total_collections();
    _evacuation_failure_alot_count = 0;
    _evacuation_failure_alot_for_current_gc = false;
  }
}
#endif  // #ifndef PRODUCT

inline bool G1CollectedHeap::is_in_young(const oop obj) {
  if (obj == NULL) {
    return false;
  }
  return heap_region_containing(obj)->is_young();
}

// We don't need barriers for initializing stores to objects
// in the young gen: for the SATB pre-barrier, there is no
// pre-value that needs to be remembered; for the remembered-set
// update logging post-barrier, we don't maintain remembered set
// information for young gen objects.
inline bool G1CollectedHeap::can_elide_initializing_store_barrier(oop new_obj) {
  return is_in_young(new_obj);
}

inline bool G1CollectedHeap::is_obj_dead(const oop obj) const {
  if (obj == NULL) {
    return false;
  }
  return is_obj_dead(obj, heap_region_containing(obj));
}

inline bool G1CollectedHeap::is_obj_ill(const oop obj) const {
  if (obj == NULL) {
    return false;
  }
  return is_obj_ill(obj, heap_region_containing(obj));
}

// 将这个HeapRegion标记位巨型对象回收候选的HeapRegion,
// 通过方法 is_humongous_reclaim_candidate()查询对应HeapRegion是否已经被设置为巨型对象回收候选HeapRegion
inline void G1CollectedHeap::set_humongous_reclaim_candidate(uint region, bool value) {
  assert(_hrm.at(region)->startsHumongous(), "Must start a humongous object");
  _humongous_reclaim_candidates.set_candidate(region, value);
}

// 查询对应HeapRegion是否已经被设置为巨型对象回收候选HeapRegion
inline bool G1CollectedHeap::is_humongous_reclaim_candidate(uint region) {
  assert(_hrm.at(region)->startsHumongous(), "Must start a humongous object");
  return _humongous_reclaim_candidates.is_candidate(region);
}

inline void G1CollectedHeap::set_humongous_is_live(oop obj) {
  uint region = addr_to_region((HeapWord*)obj);
  // Clear the flag in the humongous_reclaim_candidates table.  Also
  // reset the entry in the _in_cset_fast_test table so that subsequent references
  // to the same humongous object do not go into the slow path again.
  // This is racy, as multiple threads may at the same time enter here, but this
  // is benign.
  // During collection we only ever clear the "candidate" flag, and only ever clear the
  // entry in the in_cset_fast_table.
  // We only ever evaluate the contents of these tables (in the VM thread) after
  // having synchronized the worker threads with the VM thread, or in the same
  // thread (i.e. within the VM thread).
  if (is_humongous_reclaim_candidate(region)) {
    set_humongous_reclaim_candidate(region, false);
    _in_cset_fast_test.clear_humongous(region);
  }
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1COLLECTEDHEAP_INLINE_HPP
