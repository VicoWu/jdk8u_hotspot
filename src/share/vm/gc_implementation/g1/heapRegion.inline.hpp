/*
 * Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_HEAPREGION_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_HEAPREGION_INLINE_HPP

#include "gc_implementation/g1/g1BlockOffsetTable.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/heapRegion.hpp"
#include "memory/space.hpp"
#include "runtime/atomic.inline.hpp"

// This version requires locking.
/**
 * 有锁版本的内存分配，需要在外部加锁
 * 调用者是 G1OffsetTableContigSpace::allocate
 *
 */

inline HeapWord* G1OffsetTableContigSpace::allocate_impl(size_t size,
                                                HeapWord* const end_value) {
  HeapWord* obj = top();
  if (pointer_delta(end_value, obj) >= size) {
    HeapWord* new_top = obj + size;
    set_top(new_top);
    assert(is_aligned(obj) && is_aligned(new_top), "checking alignment");
    return obj;
  } else {
    return NULL;
  }
}

// This version is lock-free.
// 这里的lock free的意思是，通过反复尝试分配
inline HeapWord* G1OffsetTableContigSpace::par_allocate_impl(size_t size,
                                                    HeapWord* const end_value) {
  do {
    HeapWord* obj = top();
      // 当前空间'看起来'足够，尝试进行分配(注意，这里是无锁尝试，尽管现在空间足够，但是依然可能由于多线程竞争，导致原子比较交换失败)
    if (pointer_delta(end_value, obj) >= size) {
      HeapWord* new_top = obj + size;
      // 通过原子比较交换，将top的值更新为new_top
      HeapWord* result = (HeapWord*)Atomic::cmpxchg_ptr(new_top, top_addr(), obj);
      // result can be one of two:
      //  the old top value: the exchange succeeded
      //  otherwise: the new value of the top is returned.
      if (result == obj) { // 如果比较交换成功，那么返回对象地址肯定是不变的
        assert(is_aligned(obj) && is_aligned(new_top), "checking alignment");
        return obj;
      }
    } else { // 地址不够
      return NULL;
    }
  } while (true);
}

/**
 * 这个内存分配必须在外部加锁，在内存中分配一个大小为size的内存区域
 * 查看调用方 TenuredGeneration::par_promote
 * @param size
 * @return
 */
inline HeapWord* G1OffsetTableContigSpace::allocate(size_t size) {
  HeapWord* res = allocate_impl(size, end()); // 进行内存分配，返回内存分配的字地址
  if (res != NULL) { //如果分配成功，那么需要更新和维护对应的BOT, 搜索 G1BlockOffsetArrayContigSpace::alloc_block查看具体的调用者
      /**
       * _offsets的类型是 G1BlockOffsetArrayContigSpace,
       * 查看方法实现 G1BlockOffsetArrayContigSpace::alloc_block
       */
      _offsets.alloc_block(res, size);
}
  return res;
}

// Because of the requirement of keeping "_offsets" up to date with the
// allocations, we sequentialize these with a lock.  Therefore, best if
// this is used for larger LAB allocations only.
/**
 * 这个方法的调用者是HeapWord* G1AllocRegion::par_allocate，可以看到，这个方法要求维护块偏移表
 * @param size
 * @return
 */
inline HeapWord* G1OffsetTableContigSpace::par_allocate(size_t size) {
  MutexLocker x(&_par_alloc_lock); // 必须对内存加锁
  return allocate(size);
}

/**
 * 在方法 oops_on_card_seq_iterate_careful 中被调用
 * 输入一个地址，返回这个地址对应的块的首地址
 * @param p
 * @return
 */
inline HeapWord* G1OffsetTableContigSpace::block_start(const void* p) {
    /**
     * _offsets 是 G1BlockOffsetArrayContigSpace
     * 实际上调用的是 G1BlockOffsetTable::block_start
     *      继承链路是 G1BlockOffsetArrayContigSpace -> G1BlockOffsetArray -> G1BlockOffsetTable
     *      在block_start中，又调用了block_start_unsafe，
     *          这里的block_start_unsafe应该是G1BlockOffsetArrayContigSpace::block_start_unsafe
     */
  return _offsets.block_start(p);
}

inline HeapWord*
G1OffsetTableContigSpace::block_start_const(const void* p) const {
    /**
     * 查看G1BlockOffsetArrayContigSpace的block_start_const方法
     * 实际上调用的是G1BlockOffsetTable::block_start_const
     */
  return _offsets.block_start_const(p);
}

/**
 * 判断给定地址 p 所在的块是否为对象块
 * @param p
 * @return
 */
inline bool
HeapRegion::block_is_obj(const HeapWord* p) const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  /**
   * 如果我们打开了标记期间的类卸载，那么只有对象不是死对象，才会认为是obj
   * 死对象的判断标准是：
   *    对象在上次标记的时候还没有分配 **并且** 在这轮标记的时候也没有被标记
   */
  if (ClassUnloadingWithConcurrentMark) {
    return !g1h->is_obj_dead(oop(p), this);
  }
  /**
   * 如果没有启动并发标记时候类卸载，那么只要这个地址小于当前的HeapRegion的top，就肯定是对象地址
   */
  return p < top();
}

inline size_t
HeapRegion::block_size(const HeapWord *addr) const {
  if (addr == top()) { // 如果这个地址刚好等于top()，即后面已经没有元素了，那么后面的空间就是一个大的block
    return pointer_delta(end(), addr);
  }

  if (block_is_obj(addr)) {
    return oop(addr)->size(); // 如果这个地址是一个对象，那么这个对象的大小就是block的大小
  }

  assert(ClassUnloadingWithConcurrentMark,
      err_msg("All blocks should be objects if G1 Class Unloading isn't used. "
              "HR: [" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT ") "
              "addr: " PTR_FORMAT,
              p2i(bottom()), p2i(top()), p2i(end()), p2i(addr)));

  // 在这里，block_is_obj返回false，这有可能是一些死的对象有一些死的类(由于打开了ClassUnloadingWithConcurrentMark)
  // Old regions' dead objects may have dead classes
  // We need to find the next live object in some other
  // manner than getting the oop size
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  HeapWord* next = g1h->concurrent_mark()->prevMarkBitMap()->
      getNextMarkedWordAddress(addr, prev_top_at_mark_start()); // 方法实现搜索 CMBitMapRO::getNextMarkedWordAddress

  assert(next > addr, "must get the next live object");
  return pointer_delta(next, addr);
}

/**
 * 进行无锁方式的内存分配
 * 调用者是 HeapWord* G1AllocRegion::par_allocate
 * 这个方法必须是用来在年轻代进行对象分配(比如用户的mutator线程发起的分配)，因此才可以允许不更新bot
 */
inline HeapWord* HeapRegion::par_allocate_no_bot_updates(size_t word_size) {
  // 只有在young区（eden 或者 survivor）可以跳过bot update
  assert(is_young(), "we can only skip BOT updates on young regions");
  /**
   * HeapRegion是 G1OffsetTableContigSpace的子类，这里其实调用的是 G1OffsetTableContigSpace::par_allocate_impl
   */

  return par_allocate_impl(word_size, end()); // 进行无锁内存分配
}

/**
 * 进行有锁方式的内存分配
 */
inline HeapWord* HeapRegion::allocate_no_bot_updates(size_t word_size) {
  // 只有在young区可以跳过bot update
  assert(is_young(), "we can only skip BOT updates on young regions");
  return allocate_impl(word_size, end());
}

/**
 *
 * 在 NoteStartOfMarkHRClosure 中被调用，发生在初始标记开始的时候，
 *      设置_next_top_at_mark_start的地址为当前的分配位置,随后，top就一直往前移动，但是_next_top_at_mark_start保持不变
 */
inline void HeapRegion::note_start_of_marking() {
  _next_marked_bytes = 0;
  _next_top_at_mark_start = top();
}

/**
 * 在 G1NoteEndOfConcMarkClosure的do_heap 方法中调用，发生在并发标记结束以后，用来交换ptams 和 ntams
 */
inline void HeapRegion::note_end_of_marking() {
  _prev_top_at_mark_start = _next_top_at_mark_start;
  _prev_marked_bytes = _next_marked_bytes;
  _next_marked_bytes = 0;

  assert(_prev_marked_bytes <=
         (size_t) pointer_delta(prev_top_at_mark_start(), bottom()) *
         HeapWordSize, "invariant");
}

/**
 通知该区域它将在 GC 期间用作目标空间，并且我们即将开始将对象复制到其中。
 * 在方法 G1CollectedHeap::new_gc_alloc_region 和 G1Allocator::reuse_retained_old_region 中调用，即刚刚分配一个region的时候
 * 因此只用于survivor 和 old region开始进行拷贝时候的调用
 * 当这个region刚刚被分配出来，准备用来存放数据的时候调用
 * @param during_initial_mark
 */
inline void HeapRegion::note_start_of_copying(bool during_initial_mark) {
  if (is_survivor()) { // 如果是survivor区域，那么_next_top_at_mark_start等于bottom
    // This is how we always allocate survivors.
    assert(_next_top_at_mark_start == bottom(), "invariant");
  } else { // 如果是old区域
    if (during_initial_mark) {
      // During initial-mark we'll explicitly mark any objects on old
      // regions that are pointed to by roots. Given that explicit
      // marks only make sense under NTAMS it'd be nice if we could
      // check that condition if we wanted to. Given that we don't
      // know where the top of this region will end up, we simply set
      // NTAMS to the end of the region so all marks will be below
      // NTAMS. We'll set it to the actual top when we retire this region.
      /**
       * 在initial-mark期间，我们将明确标记根指向的旧区域上的任何对象。
       * 鉴于显式标记仅在 NTAMS 下才有意义，如果我们愿意的话，如果我们能够检查该条件，那就太好了。
       * 鉴于当前开始进行复制，我们不知道该区域的顶部将在哪里结束，我们只需将 NTAMS 设置为该区域的末尾，以便所有标记都将低于 NTAMS。
       * 当我们退出该区域时，我们会将其设置为实际顶部，即更新为NTAMS当前的真实的top。
       */
      _next_top_at_mark_start = end();
    } else { // 当前不是在初始标记阶段，但是可能处于并发标记阶段
      // We could have re-used this old region as to-space over a
      // couple of GCs since the start of the concurrent marking
      // cycle. This means that [bottom,NTAMS) will contain objects
      // copied up to and including initial-mark and [NTAMS, top)
      // will contain objects copied during the concurrent marking cycle.
      assert(top() >= _next_top_at_mark_start, "invariant");
    }
  }
}

/**
 * 通知这个region 它已经不会再在GC操作过程中作为一个目标region进行数据拷贝，并且我们将不再将对象复制到其中。
 * 在方法 G1CollectedHeap::retire_gc_alloc_region中调用，因此只用于survivor和old region的拷贝结束时的调用
 * @param during_initial_mark
 */
inline void HeapRegion::note_end_of_copying(bool during_initial_mark) {
    /**
     * 如果是survivor区域，由于对象根本不会在这里直接分配，都是从eden区域转移过来的，
     *  因此survivor区域的_next_top_at_mark_start等于bottom()是一个恒定值
     */
  if (is_survivor()) { //
    // This is how we always allocate survivors.
    assert(_next_top_at_mark_start == bottom(), "invariant");
  } else { //  对于old区域
    if (during_initial_mark) {
      // See the comment for note_start_of_copying() for the details
      // on this.
      assert(_next_top_at_mark_start == end(), "pre-condition");
      /**
       * 对于old区域，当前复制结束了，top的位置已经知道了，因此可以将_next_top_at_mark_start更新为当前的top的位置
       */
      _next_top_at_mark_start = top();
    } else { // 当前不是在初始标记阶段，但是可能处于并发标记阶段
      // See the comment for note_start_of_copying() for the details
      // on this.
      assert(top() >= _next_top_at_mark_start, "invariant");
    }
  }
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_HEAPREGION_INLINE_HPP
