/*
 * Copyright (c) 1999, 2014, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_INLINE_HPP
#define SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_INLINE_HPP

#include "gc_interface/collectedHeap.hpp"
#include "memory/threadLocalAllocBuffer.hpp"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"

inline HeapWord* ThreadLocalAllocBuffer::allocate(size_t size) {
  invariants(); // 检查当前的TLAB是否符合一些不变式，就是top的位置是否位于start和end之间
  HeapWord* obj = top();
  if (pointer_delta(end(), obj) >= size) { // 剩余空间大于对象大小
    // successful thread-local allocation
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    /**
     * Copy::fill_to_words(obj + hdr_size, size - hdr_size, badHeapWordVal);: 使用 Copy::fill_to_words 将从 obj + hdr_size 开始、
     * 长度为 size - hdr_size 的空间填充为 badHeapWordVal
     * 这个过程破坏了分配的空间，以防止其他并发的垃圾收集线程误将其视为有效的对象。
     * 这是一种安全机制，确保在调试模式下能更容易地检测到对已释放空间的引用。
     */
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(obj + hdr_size, size - hdr_size, badHeapWordVal);
#endif // ASSERT
    // This addition is safe because we know that top is
    // at least size below end, so the add can't wrap.
    set_top(obj + size); // 更新top

    invariants(); // 再次检查不变式
    return obj;
  }
  return NULL;
}

/**
 * 根据这个待分配对象的大小，计算即将创建的新的tlab的大小
 * @param obj_size
 * @return
 */
inline size_t ThreadLocalAllocBuffer::compute_size(size_t obj_size) {

    /**
     * 搜索 inline intptr_t align_object_size
     * 将一个给定的大小size按照指定的对齐方式 MinObjAlignment 向上对齐到最接近的边界,默认MinObjAlignment=8Byte
     */
  const size_t aligned_obj_size = align_object_size(obj_size);

  // Compute the size for the new TLAB.
  // The "last" tlab may be smaller to reduce fragmentation.
  // unsafe_max_tlab_alloc is just a hint.
  /**
   * 计算了当前线程在堆上分配内存的最大空间，以字（Word）为单位
   */
  const size_t available_size = Universe::heap()->unsafe_max_tlab_alloc(myThread()) /
                                                  HeapWordSize; // heap内存剩余的word 大小
  /**
   * 可用空间和期望大小加上对齐后的对象大小的较小值。这是为了确保新的TLAB能够容纳期望大小的对象，并且不超过可用空间的限制。
   */
  size_t new_tlab_size = MIN2(available_size, desired_size() + aligned_obj_size); // 期望大小和可用大小的较小值

  // Make sure there's enough room for object and filler int[].
  const size_t obj_plus_filler_size = aligned_obj_size + alignment_reserve();
  if (new_tlab_size < obj_plus_filler_size) {
    // If there isn't enough room for the allocation, return failure.
    if (PrintTLAB && Verbose) {
      gclog_or_tty->print_cr("ThreadLocalAllocBuffer::compute_size(" SIZE_FORMAT ")"
                    " returns failure",
                    obj_size);
    }
    return 0;// 分配失败
  }
  if (PrintTLAB && Verbose) {
    gclog_or_tty->print_cr("ThreadLocalAllocBuffer::compute_size(" SIZE_FORMAT ")"
                  " returns " SIZE_FORMAT,
                  obj_size, new_tlab_size);
  }
  return new_tlab_size;
}


void ThreadLocalAllocBuffer::record_slow_allocation(size_t obj_size) {
  // Raise size required to bypass TLAB next time. Why? Else there's
  // a risk that a thread that repeatedly allocates objects of one
  // size will get stuck on this slow path.

  /**
   * 这里的意思是，如果我们不把refile_waste_limit进行欺骗性增大，那么很有可能JVM在不断创建同等大小的对象，然后一直走slow这条调用线路，导致性能下降。
   * 因此，在这里，我们欺骗性的增加refile_waste_limit的大小，这样，如果JVM不断创建同等大小的对象，最后，就不会再走这条调用线路。
   */
  set_refill_waste_limit(refill_waste_limit() + refill_waste_limit_increment());

  _slow_allocations++;

  if (PrintTLAB && Verbose) {
    Thread* thrd = myThread();
    gclog_or_tty->print("TLAB: %s thread: " INTPTR_FORMAT " [id: %2d]"
                        " obj: " SIZE_FORMAT
                        " free: " SIZE_FORMAT
                        " waste: " SIZE_FORMAT "\n",
                        "slow", p2i(thrd), thrd->osthread()->thread_id(),
                        obj_size, free(), refill_waste_limit());
  }
}

#endif // SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_INLINE_HPP
