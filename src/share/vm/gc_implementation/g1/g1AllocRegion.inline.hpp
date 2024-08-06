/*
 * Copyright (c) 2011, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCREGION_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCREGION_INLINE_HPP

#include "gc_implementation/g1/g1AllocRegion.hpp"
#include "gc_implementation/g1/heapRegion.inline.hpp"

/**
 * 跟par_allocate的区别是，par_allocate是线程安全的，自己负责线程同步，而allocate()则需要调用者负责线程安全
 * @param alloc_region
 * @param word_size
 * @param bot_updates
 * @return
 */
inline HeapWord* G1AllocRegion::allocate(HeapRegion* alloc_region,
                                         size_t word_size,
                                         bool bot_updates) {
  assert(alloc_region != NULL, err_msg("pre-condition"));

  if (!bot_updates) {
      /**
       * 搜索 HeapWord* HeapRegion::allocate_no_bot_updates
       */
    return alloc_region->allocate_no_bot_updates(word_size);
  } else {
      /**
       * HeapRegion是G1OffsetTableContigSpace的子类，
       * 这里实际调用的是 G1OffsetTableContigSpace::allocate
       */
    return alloc_region->allocate(word_size);
  }
}

/**
 *
 * 这个方法是线程安全的，在alloc_region外面进行一次线程安全的分配
 * 调用者： G1AllocRegion::fill_up_remaining_space， 和 方法 G1AllocRegion::attempt_allocation
 * @param alloc_region 当前需要进行对象分配的region
 * @param word_size
 * @param bot_updates
 * @return
 */
inline HeapWord* G1AllocRegion::par_allocate(HeapRegion* alloc_region,
                                             size_t word_size,
                                             bool bot_updates) {
  assert(alloc_region != NULL, err_msg("pre-condition"));
  assert(!alloc_region->is_empty(), err_msg("pre-condition"));
  // 对于不需要bot_updates，不需要对内存加Heap_lock锁，而如果需要进行bot_updates，需要对内存加Heap_lock锁
  if (!bot_updates) { // 调用 HeapRegion::par_allocate_no_bot_updates(),在heapRegion.inline.hpp中
    return alloc_region->par_allocate_no_bot_updates(word_size); // 进行无锁的内存分配
  } else {
      /**
       * HeapRegion是G1OffsetTableContigSpace的子类，
       * 这里实际调用的是 G1OffsetTableContigSpace::par_allocate
       */
    return alloc_region->par_allocate(word_size); // 进行有锁的内存分配，即先对Heap_lock加锁
  }
}

/**
 * 第一级分配：应该在不持有锁的情况下调用。
 * 调用者  G1AllocRegion::attempt_allocation_locked，
 *        G1CollectedHeap::attempt_allocation_slow，
 *        G1CollectedHeap::attempt_allocation
 *        OldGCAllocRegion::release
 * 它将尝试在活动区域之外进行无锁分配，如果无法分配则返回 NULL
 * 与G1CollectedHeap::attempt_allocation()区分
 *
 * @param word_size
 * @param bot_updates
 * @return
 */
inline HeapWord* G1AllocRegion::attempt_allocation(size_t word_size,
                                                   bool bot_updates) {
  assert(bot_updates == _bot_updates, ar_ext_msg(this, "pre-condition"));

  HeapRegion* alloc_region = _alloc_region; // 当前正在使用中的region
  assert(alloc_region != NULL, ar_ext_msg(this, "not initialized properly"));

  /**
   * 查看 G1AllocRegion::par_allocate,通过线程安全的方式进行分配，不会进行任何GC
   * 可以看到，这是尝试在这个Region内部分配一个大小为word_size的区域，这个word_size可能是一个object，可能是一个plab
   */

  HeapWord* result = par_allocate(alloc_region, word_size, bot_updates);
  if (result != NULL) {
    trace("alloc", word_size, result);
    return result;
  }
  trace("alloc failed", word_size);
  return NULL;
}

/**
 *
 * 二级分配：调用者自己负责获取Heap_lock，查看G1CollectedHeap::attempt_allocation_slow可以看到调用该方法的时候已经上了Heap_Lock
 * 它将尝试首先在活动区域之外进行无锁分配，或者，如果无法分配，它将尝试用新的区域替换活动分配区域。
 * 我们要求调用者在调用此函数之前获取适当的锁，以便更容易使其符合其锁定协议。
 *
 * 这里可能是给G1AllocRegion的三个子类在分配region
   G1AllocRegion
	-- MutatorAllocRegion
	-- SurvivorGCAllocRegion
	-- OldGCAllocRegion
   G1Alloctor对象持有G1AllocRegion的三个子类的各自的实例
   搜索 ->attempt_allocation_locked 可以看到三个不同的子类所代表的三个不同区域对这个方法的调用
 * @param word_size
 * @param bot_updates
 * @return
 */
inline HeapWord* G1AllocRegion::attempt_allocation_locked(size_t word_size,
                                                          bool bot_updates) {
  // First we have to do the allocation, assuming we're holding the
  // appropriate lock, in case another thread changed the region while
  // we were waiting to get the lock.
  // 搜索G1AllocRegion::attempt_allocation 查看方法的具体实现
  HeapWord* result = attempt_allocation(word_size, bot_updates); // 如果bot_updates==false，那么就尝试进行不上锁分配内存，第一级分配
  if (result != NULL) {
    return result;
  }
  // 不上锁分配内存失败，清空当前的region
     /**
      * 这里调用的是 G1AllocRegion::retire，代表的含义是卸载掉当前这个区域的对应的active region，因为将会分配一个新的region
      * 搜索  G1AllocRegion::retire 查看具体实现
     *      需要跟 G1ParGCAllocBuffer::retire 查看具体实现， G1ParGCAllocBuffer::retire 将当前的这个G1ParGCAllocBuffer从G1ParGCAllocator中卸载掉
     */
  retire(true /* fill_up */);
  /**
   * 对应了G1AllocRegion::new_alloc_region_and_allocate
   * 创建一个新的region，然后在新的Region上分配内存
   *    如果分配成功(result != NULL)，那么说明新的region创建成功，对象也分配成功
   *    如果分配失败(result == NULL), 那么说明新的region肯定也创建失败
   *    不可能region创建成功但是对象分配失败，也不可能region创建失败但是对象分配成功
   */
  result = new_alloc_region_and_allocate(word_size, false /* force */);
  if (result != NULL) { // 分配不成功，可以肯定，region也没有创建成功
    trace("alloc locked (second attempt)", word_size, result);
    return result;
  }
  trace("alloc locked failed", word_size); // 分配失败
  return NULL;
}

/**
 *
 * @param word_size
 * @param bot_updates
 * @return
 */
inline HeapWord* G1AllocRegion::attempt_allocation_force(size_t word_size,
                                                         bool bot_updates) {
  assert(bot_updates == _bot_updates, ar_ext_msg(this, "pre-condition"));
  assert(_alloc_region != NULL, ar_ext_msg(this, "not initialized properly"));

  trace("forcing alloc");
  HeapWord* result = new_alloc_region_and_allocate(word_size, true /* force */);
  if (result != NULL) {
    trace("alloc forced", word_size, result);
    return result;
  }
  trace("alloc forced failed", word_size);
  return NULL;
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCREGION_INLINE_HPP
