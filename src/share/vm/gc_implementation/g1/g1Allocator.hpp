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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCATOR_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCATOR_HPP

#include "gc_implementation/g1/g1AllocationContext.hpp"
#include "gc_implementation/g1/g1AllocRegion.hpp"
#include "gc_implementation/g1/g1InCSetState.hpp"
#include "gc_implementation/shared/parGCAllocBuffer.hpp"

// Base class for G1 allocators.
class G1Allocator : public CHeapObj<mtGC> {
  friend class VMStructs;
protected:
  G1CollectedHeap* _g1h;

  // Outside of GC pauses, the number of bytes used in all regions other
  // than the current allocation region.
  size_t _summary_bytes_used;

public:
   G1Allocator(G1CollectedHeap* heap) :
     _g1h(heap), _summary_bytes_used(0) { }

   static G1Allocator* create_allocator(G1CollectedHeap* g1h);

   virtual void init_mutator_alloc_region() = 0;
   virtual void release_mutator_alloc_region() = 0;

   virtual void init_gc_alloc_regions(EvacuationInfo& evacuation_info) = 0;
   virtual void release_gc_alloc_regions(uint no_of_gc_workers, EvacuationInfo& evacuation_info) = 0;
   virtual void abandon_gc_alloc_regions() = 0;
   // 为用户线程分配eden区域的region
   virtual MutatorAllocRegion*    mutator_alloc_region(AllocationContext_t context) = 0; // 纯虚函数声明，其默认实现都在G1DefaultAllocator中
   // 分配survivor区域的region，这个肯定是gc线程触发的
   virtual SurvivorGCAllocRegion* survivor_gc_alloc_region(AllocationContext_t context) = 0;// 纯虚函数声明，其默认实现都在G1DefaultAllocator中
   // 分配old 区域的region
   virtual OldGCAllocRegion*      old_gc_alloc_region(AllocationContext_t context) = 0;// 纯虚函数声明，其默认实现都在G1DefaultAllocator中
   virtual size_t                 used() = 0;
   virtual bool                   is_retained_old_region(HeapRegion* hr) = 0;

   void                           reuse_retained_old_region(EvacuationInfo& evacuation_info,
                                                            OldGCAllocRegion* old,
                                                            HeapRegion** retained);

   size_t used_unlocked() const {
     return _summary_bytes_used;
   }

   void increase_used(size_t bytes) {
     _summary_bytes_used += bytes;
   }

   void decrease_used(size_t bytes) {
     assert(_summary_bytes_used >= bytes,
            err_msg("invariant: _summary_bytes_used: " SIZE_FORMAT " should be >= bytes: " SIZE_FORMAT,
                _summary_bytes_used, bytes));
     _summary_bytes_used -= bytes;
   }

   void set_used(size_t bytes) {
     _summary_bytes_used = bytes;
   }

   virtual HeapRegion* new_heap_region(uint hrs_index,
                                       G1BlockOffsetSharedArray* sharedOffsetArray,
                                       MemRegion mr) {
     return new HeapRegion(hrs_index, sharedOffsetArray, mr);
   }
};

// The default allocator for G1.
class G1DefaultAllocator : public G1Allocator {
protected:
  // Alloc region used to satisfy mutator allocation requests.
  MutatorAllocRegion _mutator_alloc_region;

  // Alloc region used to satisfy allocation requests by the GC for
  // survivor objects.
  SurvivorGCAllocRegion _survivor_gc_alloc_region;

  // Alloc region used to satisfy allocation requests by the GC for
  // old objects.
  OldGCAllocRegion _old_gc_alloc_region;

  HeapRegion* _retained_old_gc_alloc_region;
public:
  G1DefaultAllocator(G1CollectedHeap* heap) : G1Allocator(heap), _retained_old_gc_alloc_region(NULL) { } // 在构造G1DefaultAllocator的时候，传入了G1CollectedHeap对内存实现

  virtual void init_mutator_alloc_region(); // 见g1Allocator.cpp中方法的实现
  virtual void release_mutator_alloc_region();// 见g1Allocator.cpp中方法的实现

  virtual void init_gc_alloc_regions(EvacuationInfo& evacuation_info);// 见g1Allocator.cpp中方法的实现
  virtual void release_gc_alloc_regions(uint no_of_gc_workers, EvacuationInfo& evacuation_info);// 见g1Allocator.cpp中方法的实现
  virtual void abandon_gc_alloc_regions();

  virtual bool is_retained_old_region(HeapRegion* hr) { // 见g1Allocator.cpp中方法的实现
    return _retained_old_gc_alloc_region == hr;
  }

  /**
   * 可以看到，这个方法只是返回当前的_mutator_alloc_region，并不是去分配一个region
   * 而_mutator_alloc_region的值是在init_mutator_alloc_region()方法中进行初始化的，一个MutatorAllocRegion对象维护了当前region的信息
   * @param context
   * @return
   */
  virtual MutatorAllocRegion* mutator_alloc_region(AllocationContext_t context) {
    return &_mutator_alloc_region;
  }

  virtual SurvivorGCAllocRegion* survivor_gc_alloc_region(AllocationContext_t context) {
    return &_survivor_gc_alloc_region;
  }

  virtual OldGCAllocRegion* old_gc_alloc_region(AllocationContext_t context) {
    return &_old_gc_alloc_region;
  }

  virtual size_t used() {
    assert(Heap_lock->owner() != NULL,
           "Should be owned on this thread's behalf.");
    size_t result = _summary_bytes_used;

    // Read only once in case it is set to NULL concurrently
    HeapRegion* hr = mutator_alloc_region(AllocationContext::current())->get();
    if (hr != NULL) {
      result += hr->used();
    }
    return result;
  }
};

class G1ParGCAllocBuffer: public ParGCAllocBuffer {
private:
  bool _retired;

public:
    /**
     * 构造方法实现 G1ParGCAllocBuffer::G1ParGCAllocBuffer
     * @param gclab_word_size
     */
  G1ParGCAllocBuffer(size_t gclab_word_size);  // 声明这个G1ParGCAllocBuffer的构造函数
  virtual ~G1ParGCAllocBuffer() {
    guarantee(_retired, "Allocation buffer has not been retired");
  }

  virtual void set_buf(HeapWord* buf) {
    ParGCAllocBuffer::set_buf(buf);
    _retired = false;
  }

  /**
   * 这是方法 G1ParGCAllocBuffer::retire
   * 调用者是 G1ParGCAllocator::allocate_direct_or_new_plab
   */
  virtual void retire(bool end_of_gc, bool retain) {
    if (_retired) {
      return;
    }
    /**
     * 调用父类的 void ParGCAllocBuffer::retire 方法
     * 由于是父类的方法，虽然写着ParGCAllocBuffer::retire，其实是调用的实例方法
     */
    ParGCAllocBuffer::retire(end_of_gc, retain);
    _retired = true;
  }
};

class G1ParGCAllocator : public CHeapObj<mtGC> {
  friend class G1ParScanThreadState;
protected:
  G1CollectedHeap* _g1h;

  // The survivor alignment in effect in bytes.
  // == 0 : don't align survivors
  // != 0 : align survivors to that alignment
  // These values were chosen to favor the non-alignment case since some
  // architectures have a special compare against zero instructions.
  const uint _survivor_alignment_bytes;

  size_t _alloc_buffer_waste;
  size_t _undo_waste;

  /**
   * 搜索 G1ParGCAllocator::allocate_direct_or_new_plab 查看调用者
   * 用来统计这个G1ParGCAllocator所维护的所有的G1ParGCAllocBuffer的总的浪费空间，
   *    比如我们在创建一个新的plab的时候，会把当前即将丢弃的plab的剩余空间加进来
   * @param waste
   */
  void add_to_alloc_buffer_waste(size_t waste) { _alloc_buffer_waste += waste; }
  void add_to_undo_waste(size_t waste)         { _undo_waste += waste; }

  virtual void retire_alloc_buffers() = 0;
  virtual G1ParGCAllocBuffer* alloc_buffer(InCSetState dest, AllocationContext_t context) = 0;

  // Calculate the survivor space object alignment in bytes. Returns that or 0 if
  // there are no restrictions on survivor alignment.
  static uint calc_survivor_alignment_bytes() {
    assert(SurvivorAlignmentInBytes >= ObjectAlignmentInBytes, "sanity");
    if (SurvivorAlignmentInBytes == ObjectAlignmentInBytes) {
      // No need to align objects in the survivors differently, return 0
      // which means "survivor alignment is not used".
      return 0;
    } else {
      assert(SurvivorAlignmentInBytes > 0, "sanity");
      return SurvivorAlignmentInBytes;
    }
  }

public:
  G1ParGCAllocator(G1CollectedHeap* g1h) :
    _g1h(g1h), _survivor_alignment_bytes(calc_survivor_alignment_bytes()),
    _alloc_buffer_waste(0), _undo_waste(0) {
  }

  static G1ParGCAllocator* create_allocator(G1CollectedHeap* g1h);

  size_t alloc_buffer_waste() { return _alloc_buffer_waste; }
  size_t undo_waste() {return _undo_waste; }

  // Allocate word_sz words in dest, either directly into the regions or by
  // allocating a new PLAB. Returns the address of the allocated memory, NULL if
  // not successful.
  /**
   * 查看方法实现 G1ParGCAllocator::allocate_direct_or_new_plab
   * 在 G1ParScanThreadState::copy_to_survivor_space中被调用
   */
  HeapWord* allocate_direct_or_new_plab(InCSetState dest,
                                        size_t word_sz,
                                        AllocationContext_t context);

  // Allocate word_sz words in the PLAB of dest.  Returns the address of the
  // allocated memory, NULL if not successful.
  /**
   *  这个方法的全称是 G1ParGCAllocator::plab_allocate
   * TLAB（Thread Local Allocation Buffer）：
        TLAB 是针对单个线程的，每个线程都有自己的 TLAB。
        TLAB 是在 Java 虚拟机堆内存中为每个线程分配的一块小空间，用于线程本地的对象分配。
        当一个线程需要分配对象时，首先尝试在自己的 TLAB 上进行分配。如果 TLAB 空间不足，则需要重新申请一个 TLAB。
        TLAB 的使用可以减少线程之间的竞争，提高对象分配的效率。
     PLAB（Parallel Local Allocation Buffer）：
        PLAB 是针对并行垃圾回收算法的，在并行垃圾回收中，每个线程都有自己的 PLAB。
        PLAB 也是用于线程本地的对象分配，但是与 TLAB 不同的是，PLAB 是为并行垃圾回收中的线程分配的。
        PLAB 的作用类似于 TLAB，都是为了减少线程之间的竞争，提高对象分配的效率。
   * @param dest
   * @param word_sz
   * @param context
   * @return
   */
  HeapWord* plab_allocate(InCSetState dest,
                          size_t word_sz,
                          AllocationContext_t context) {
      /**
       * 搜索 virtual G1ParGCAllocBuffer* alloc_buffer
       * 根据对象状态，取出对应状态的对象所应该分配的目标区域的G1ParGCAllocBuffer对象，该对象负责在这个区域中进行内存分配
       */
    G1ParGCAllocBuffer* buffer = alloc_buffer(dest, context);
    if (_survivor_alignment_bytes == 0) {
      return buffer->allocate(word_sz); // ParGCAllocBuffer::allocate
    } else {
        /**
         * 方法的实现 搜索  ParGCAllocBuffer::allocate_aligned
         */
      return buffer->allocate_aligned(word_sz, _survivor_alignment_bytes);
    }
  }

  /**
   * 调用者是 G1ParGCAllocator::allocate_in_next_plab
   * @param dest
   * @param word_sz
   * @param context
   * @return
   */
  HeapWord* allocate(InCSetState dest, size_t word_sz,
                     AllocationContext_t context) {
      /**
       * 尝试在dest(Young或者old)区域进行分配
       */
    HeapWord* const obj = plab_allocate(dest, word_sz, context);
    if (obj != NULL) {
      return obj; // 分配成功，返回对象
    }
    // 分配失败，尝试为dest区域创建新的plab，或者直接将对象分配在堆内存中
    return allocate_direct_or_new_plab(dest, word_sz, context);
  }

  void undo_allocation(InCSetState dest, HeapWord* obj, size_t word_sz, AllocationContext_t context) {
    if (alloc_buffer(dest, context)->contains(obj)) {
      assert(alloc_buffer(dest, context)->contains(obj + word_sz - 1),
             "should contain whole object");
      alloc_buffer(dest, context)->undo_allocation(obj, word_sz);
    } else {
      CollectedHeap::fill_with_object(obj, word_sz);
      add_to_undo_waste(word_sz);
    }
  }
};

class G1DefaultParGCAllocator : public G1ParGCAllocator {
    // 这三个变量在构造函数 G1DefaultParGCAllocator::G1DefaultParGCAllocator 中初始化
  G1ParGCAllocBuffer  _surviving_alloc_buffer;
  G1ParGCAllocBuffer  _tenured_alloc_buffer;
  G1ParGCAllocBuffer* _alloc_buffers[InCSetState::Num];

  /**
   * 声明了构造函数，构造函数调用是G1DefaultParGCAllocator::G1DefaultParGCAllocator
   */
public:
  G1DefaultParGCAllocator(G1CollectedHeap* g1h);

  /**
   * 这是 成员方法 G1DefaultParGCAllocator::alloc_buffer
   * G1DefaultParGCAllocator维护了一个存放G1ParGCAllocBuffer*指针的数组_alloc_buffers，
   * 数组的每一个元素是一个G1ParGCAllocBuffer指针，对应了某一个InCSetState状态的G1ParGCAllocBuffer对象
   */
  virtual G1ParGCAllocBuffer* alloc_buffer(InCSetState dest, AllocationContext_t context) {
    assert(dest.is_valid(),
           err_msg("Allocation buffer index out-of-bounds: " CSETSTATE_FORMAT, dest.value()));
    assert(_alloc_buffers[dest.value()] != NULL,
           err_msg("Allocation buffer is NULL: " CSETSTATE_FORMAT, dest.value()));
    return _alloc_buffers[dest.value()];
  }

  virtual void retire_alloc_buffers() ;
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1ALLOCATOR_HPP
