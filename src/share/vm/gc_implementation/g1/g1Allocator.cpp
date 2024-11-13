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
#include "gc_implementation/g1/g1Allocator.hpp"
#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/heapRegion.inline.hpp"
#include "gc_implementation/g1/heapRegionSet.inline.hpp"

/**
 *  在一次回收暂停结束以后，会调用这个初始化操作
 */
void G1DefaultAllocator::init_mutator_alloc_region() {
  assert(_mutator_alloc_region.get() == NULL, "pre-condition");
  _mutator_alloc_region.init(); // 查看 G1AllocRegion::init
}

/**
 * 这里只是初始化mutator_region，而初始化gc和old region是放在init_gc_alloc_regions中的
 */
void G1DefaultAllocator::release_mutator_alloc_region() {
  _mutator_alloc_region.release();
  assert(_mutator_alloc_region.get() == NULL, "post-condition");
}

void G1Allocator::reuse_retained_old_region(EvacuationInfo& evacuation_info,
                                            OldGCAllocRegion* old,
                                            HeapRegion** retained_old) {
  HeapRegion* retained_region = *retained_old;
  *retained_old = NULL;

  // We will discard the current GC alloc region if:
  // a) it's in the collection set (it can happen!),
  // b) it's already full (no point in using it),
  // c) it's empty (this means that it was emptied during
  // a cleanup and it should be on the free list now), or
  // d) it's humongous (this means that it was emptied
  // during a cleanup and was added to the free list, but
  // has been subsequently used to allocate a humongous
  // object that may be less than the region size).
  if (retained_region != NULL &&
      !retained_region->in_collection_set() &&
      !(retained_region->top() == retained_region->end()) &&
      !retained_region->is_empty() &&
      !retained_region->isHumongous()) {
    retained_region->record_timestamp(); // 记录这个HeapRegion的timestamp为GC timestamp
    // The retained region was added to the old region set when it was
    // retired. We have to remove it now, since we don't allow regions
    // we allocate to in the region sets. We'll re-add it later, when
    // it's retired again.
    _g1h->_old_set.remove(retained_region);
    bool during_im = _g1h->g1_policy()->during_initial_mark_pause();
    retained_region->note_start_of_copying(during_im);
    old->set(retained_region);
    _g1h->_hr_printer.reuse(retained_region);
    evacuation_info.set_alloc_regions_used_before(retained_region->used());
  }
}

void G1DefaultAllocator::init_gc_alloc_regions(EvacuationInfo& evacuation_info) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  _survivor_gc_alloc_region.init();
  _old_gc_alloc_region.init();
  reuse_retained_old_region(evacuation_info,
                            &_old_gc_alloc_region,
                            &_retained_old_gc_alloc_region);
}

void G1DefaultAllocator::release_gc_alloc_regions(uint no_of_gc_workers, EvacuationInfo& evacuation_info) {
  AllocationContext_t context = AllocationContext::current();
  evacuation_info.set_allocation_regions(survivor_gc_alloc_region(context)->count() +
                                         old_gc_alloc_region(context)->count());
  survivor_gc_alloc_region(context)->release();
  // If we have an old GC alloc region to release, we'll save it in
  // _retained_old_gc_alloc_region. If we don't
  // _retained_old_gc_alloc_region will become NULL. This is what we
  // want either way so no reason to check explicitly for either
  // condition.
  _retained_old_gc_alloc_region = old_gc_alloc_region(context)->release();
  if (_retained_old_gc_alloc_region != NULL) {
    _retained_old_gc_alloc_region->record_retained_region();
  }

  if (ResizePLAB) {
    _g1h->_survivor_plab_stats.adjust_desired_plab_sz(no_of_gc_workers);
    _g1h->_old_plab_stats.adjust_desired_plab_sz(no_of_gc_workers);
  }
}

void G1DefaultAllocator::abandon_gc_alloc_regions() {
  assert(survivor_gc_alloc_region(AllocationContext::current())->get() == NULL, "pre-condition");
  assert(old_gc_alloc_region(AllocationContext::current())->get() == NULL, "pre-condition");
  _retained_old_gc_alloc_region = NULL;
}

/**
 * G1ParGCAllocBuffer的构造方法，其实是调用父类方法ParGCAllocBuffer
 */
G1ParGCAllocBuffer::G1ParGCAllocBuffer(size_t gclab_word_size) :
  ParGCAllocBuffer(gclab_word_size), _retired(true) { }

  /**
   * 查看调用方法 G1ParScanThreadState::copy_to_survivor_space
   * @return
   */
HeapWord* G1ParGCAllocator::allocate_direct_or_new_plab(InCSetState dest,
                                                        size_t word_sz,
                                                        AllocationContext_t context) {
  size_t gclab_word_size = _g1h->desired_plab_sz(dest);// 获取目标区域的gclab的大小
      /**
       * PLAB中的参数ParallelGCBufferWastePct与TLAB中TLABRefillWasteFraction的含义类似
       * 只有当已经尝试在当前的plab中分配并且失败，才会调用allocate_direct_or_new_plab()，这说明调用allocate_direct_or_new_plab的时候，
       *        当前plab的剩余空间已经小到无法容纳当前对象，因此，这时候就需要根据对象大小来判断下一步是创建一个新的plab，还是将对象直接分配在堆内存中。
       *            如果对象很小，即word_sz * 100 < gclab_word_size * ParallelGCBufferWastePct，那么就尝试创建一个新的plab，
       *                因为jvm不希望直接将很小的对象直接分配在堆内存中。
       *            而如果对象不是特别小，那就直接将对象分配在堆内存中，省去了创建一个新的plab的开销。。。
       */
  if (word_sz * 100 < gclab_word_size * ParallelGCBufferWastePct) {
      /**
       * 如果需要分配的空间足够小，那么我们不需要在heap中直接分配，还是希望通过plab进行分配
       * 获取 G1ParGCAllocator 维护的这个dest对应的 G1ParGCAllocBuffer 对象
       * 具体实现 搜索 virtual G1ParGCAllocBuffer* alloc_buffer
       */
    G1ParGCAllocBuffer* alloc_buf = alloc_buffer(dest, context); //
    /**
     *  由于需要创建一个新的plab，因此将当前plab的剩余空间(即浪费掉的空间)加入到G1ParGCAllocator的_alloc_buffer_waste的统计值中
     */
    add_to_alloc_buffer_waste(alloc_buf->words_remaining());
    /**
     * 将当前的这个 G1ParGCAllocBuffer从G1ParGCAllocator中卸载掉
     * 搜索 G1ParGCAllocBuffer::retire 查看具体实现
     * 需要跟 G1AllocRegion::retire 进行区分
     */
    alloc_buf->retire(false /* end_of_gc */, false /* retain */);
    /**
     * 在堆内存中直接分配一个plab，这个plab的大小是gclab_word_size
     * 返回分配的地址的字地址
     */
    HeapWord* buf = _g1h->par_allocate_during_gc(dest, gclab_word_size, context);
    if (buf == NULL) {
      return NULL; // Let caller handle allocation failure.
    }
    // Otherwise.
    alloc_buf->set_word_size(gclab_word_size);
    alloc_buf->set_buf(buf);
    /**
     * 在新分配的 G1ParGCAllocBuffer 中分配word_sz个字的空间
     * G1ParGCAllocBuffer没有重载父类ParGCAllocBuffer中的该方法
     * 具体实现搜索 ParGCAllocBuffer::allocate
     * 从当前的buffer中取出大小为word_sz大小的buffer
     */
    HeapWord* const obj = alloc_buf->allocate(word_sz);
    assert(obj != NULL, "buffer was definitely big enough...");
    return obj;
  } else {
      /**
       * 如果要分配的内存大小不小于当前 GCLAB 的大小乘以并行 GC 缓冲浪费百分比的值,
       *    从堆中分配所需大小的内存，并将分配的对象指针返回给调用方
       */
    return _g1h->par_allocate_during_gc(dest, word_sz, context);
  }
}

/**
 * G1DefaultParGCAllocator的构造方法
 * @param g1h
 */
G1DefaultParGCAllocator::G1DefaultParGCAllocator(G1CollectedHeap* g1h) :
  G1ParGCAllocator(g1h), // 调用父类的构造函数
  /**
   * 搜索 G1CollectedHeap::desired_plab_sz(InCSetState dest)
   */
  _surviving_alloc_buffer(g1h->desired_plab_sz(InCSetState::Young)), // 初始化_surviving_alloc_buffer
  _tenured_alloc_buffer(g1h->desired_plab_sz(InCSetState::Old)) { // 初始化_tenured_alloc_buffer
  for (uint state = 0; state < InCSetState::Num; state++) {
    _alloc_buffers[state] = NULL; // 对于每一个InCSetState，初始化_alloc_buffers中的对应位置
  }
  // 当前处于Young的，应该移动到survivor 区域
  _alloc_buffers[InCSetState::Young] = &_surviving_alloc_buffer;
  // 当前处于old的，应该移动到tenured区域
  _alloc_buffers[InCSetState::Old]  = &_tenured_alloc_buffer;
}

void G1DefaultParGCAllocator::retire_alloc_buffers() {
  for (uint state = 0; state < InCSetState::Num; state++) {
    G1ParGCAllocBuffer* const buf = _alloc_buffers[state];
    if (buf != NULL) {
      add_to_alloc_buffer_waste(buf->words_remaining());
      buf->flush_stats_and_retire(_g1h->alloc_buffer_stats(state),
                                  true /* end_of_gc */,
                                  false /* retain */);
    }
  }
}
