/*
 * Copyright (c) 1999, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/genCollectedHeap.hpp"
#include "memory/resourceArea.hpp"
#include "memory/threadLocalAllocBuffer.inline.hpp"
#include "memory/universe.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/copy.hpp"

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC

// Thread-Local Edens support

// static member initialization
size_t           ThreadLocalAllocBuffer::_max_size       = 0;
unsigned         ThreadLocalAllocBuffer::_target_refills = 0;
GlobalTLABStats* ThreadLocalAllocBuffer::_global_stats   = NULL;

void ThreadLocalAllocBuffer::clear_before_allocation() {
  _slow_refill_waste += (unsigned)remaining();
  make_parsable(true);   // also retire the TLAB
}

void ThreadLocalAllocBuffer::accumulate_statistics_before_gc() {
  global_stats()->initialize();

  for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
    thread->tlab().accumulate_statistics();
    thread->tlab().initialize_statistics();
  }

  // Publish new stats if some allocation occurred.
  if (global_stats()->allocation() != 0) {
    global_stats()->publish();
    if (PrintTLAB) {
      global_stats()->print();
    }
  }
}

void ThreadLocalAllocBuffer::accumulate_statistics() {
  Thread* thread = myThread();
  size_t capacity = Universe::heap()->tlab_capacity(thread);
  size_t used     = Universe::heap()->tlab_used(thread);

  _gc_waste += (unsigned)remaining();
  size_t total_allocated = thread->allocated_bytes();
  size_t allocated_since_last_gc = total_allocated - _allocated_before_last_gc;
  _allocated_before_last_gc = total_allocated;

  if (PrintTLAB && (_number_of_refills > 0 || Verbose)) {
    print_stats("gc");
  }

  if (_number_of_refills > 0) {
    // Update allocation history if a reasonable amount of eden was allocated.
    bool update_allocation_history = used > 0.5 * capacity;

    if (update_allocation_history) {
      // Average the fraction of eden allocated in a tlab by this
      // thread for use in the next resize operation.
      // _gc_waste is not subtracted because it's included in
      // "used".
      // The result can be larger than 1.0 due to direct to old allocations.
      // These allocations should ideally not be counted but since it is not possible
      // to filter them out here we just cap the fraction to be at most 1.0.
      double alloc_frac = MIN2(1.0, (double) allocated_since_last_gc / used);
      _allocation_fraction.sample(alloc_frac);
    }
    global_stats()->update_allocating_threads();
    global_stats()->update_number_of_refills(_number_of_refills);
    global_stats()->update_allocation(_number_of_refills * desired_size());
    global_stats()->update_gc_waste(_gc_waste);
    global_stats()->update_slow_refill_waste(_slow_refill_waste);
    global_stats()->update_fast_refill_waste(_fast_refill_waste);

  } else {
    assert(_number_of_refills == 0 && _fast_refill_waste == 0 &&
           _slow_refill_waste == 0 && _gc_waste          == 0,
           "tlab stats == 0");
  }
  global_stats()->update_slow_allocations(_slow_allocations);
}

// Fills the current tlab with a dummy filler array to create
// an illusion of a contiguous Eden and optionally retires the tlab.
// Waste accounting should be done in caller as appropriate; see,
// for example, clear_before_allocation().
void ThreadLocalAllocBuffer::make_parsable(bool retire) {
  if (end() != NULL) {
    invariants(); //检查当前TLAB的一些不应该改变的性质，比如，top的位置等
    if (retire) {
      myThread()->incr_allocated_bytes(used_bytes()); //将已分配字节累加到当前线程的已分配自己统计值中
    }

    /**
     * 用虚拟填充数组填充当前的 tab，以创建连续eden空间的幻觉，并可选择退出该
     */
    // 调用静态方法fill_with_object, 将剩余部分的数据填充进去无用数据，
    CollectedHeap::fill_with_object(top(), hard_end(), retire);

    if (retire || ZeroTLAB) {  // "Reset" the TLAB
      set_start(NULL);
      set_top(NULL);
      set_pf_top(NULL);
      set_end(NULL);
    }
  }
  assert(!(retire || ZeroTLAB)  ||
         (start() == NULL && end() == NULL && top() == NULL),
         "TLAB must be reset");
}
/**
 * 重新计算所有的线程的空间大小，这是发生在一次gc完成，通过可分配给tlab的总空间的大小，平均到每一个线程和tlab的分配次数以后的均值
 */
void ThreadLocalAllocBuffer::resize_all_tlabs() {
  if (ResizeTLAB) {
    for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
      thread->tlab().resize();
    }
  }
}

/**
 * 在一次内存回收以后，会重新计算每一个tlab的大小
 */
void ThreadLocalAllocBuffer::resize() {
  // Compute the next tlab size using expected allocation amount
  assert(ResizeTLAB, "Should not call this otherwise");
  /**
   * 重新大致计算tlab的大小，这发生在一次垃圾回收结束以后，这时候年轻代的堆内存已经发生了变化，因此需要重新估计一个tlab区域的大小
   * 注意，_allocation_fraction是一个成员变量，每一个_allocation_fraction都采样的是自己的内存占比，不是一个全局变量
   *
   */
  size_t alloc = (size_t)(_allocation_fraction.average() *
                          (Universe::heap()->tlab_capacity(myThread()) / HeapWordSize));
  /**
   * 总共分配给tlab的空间大小，处于tlab的分配次数，就是平均下来一个tlab的大小
   * 这里的_target_refills是全局的目标填充次数，即所有线程的目标填充次数
   */
  size_t new_size = alloc / _target_refills;

  new_size = MIN2(MAX2(new_size, min_size()), max_size());
  /**
   * 将size进行对齐，必须是8字节的整数倍
   */
  size_t aligned_new_size = align_object_size(new_size);

  if (PrintTLAB && Verbose) {
    gclog_or_tty->print("TLAB new size: thread: " INTPTR_FORMAT " [id: %2d]"
                        " refills %d  alloc: %8.6f desired_size: " SIZE_FORMAT " -> " SIZE_FORMAT "\n",
                        myThread(), myThread()->osthread()->thread_id(),
                        _target_refills, _allocation_fraction.average(), desired_size(), aligned_new_size);
  }
  /**
   * 设置新的tlab的大小
   */
  set_desired_size(aligned_new_size);
  /**
   * 设置新的浪费空间的最大大小
   */
  set_refill_waste_limit(initial_refill_waste_limit());
}

void ThreadLocalAllocBuffer::initialize_statistics() {
    _number_of_refills = 0;
    _fast_refill_waste = 0;
    _slow_refill_waste = 0;
    _gc_waste          = 0;
    _slow_allocations  = 0;
}

void ThreadLocalAllocBuffer::fill(HeapWord* start,
                                  HeapWord* top,
                                  size_t    new_size) {
  _number_of_refills++;
  if (PrintTLAB && Verbose) {
    print_stats("fill");
  }
  assert(top <= start + new_size - alignment_reserve(), "size too small");
  initialize(start, top, start + new_size - alignment_reserve());

  // Reset amount of internal fragmentation
  set_refill_waste_limit(initial_refill_waste_limit());
}

void ThreadLocalAllocBuffer::initialize(HeapWord* start,
                                        HeapWord* top,
                                        HeapWord* end) {
  set_start(start);
  set_top(top);
  set_pf_top(top);
  set_end(end);
  invariants();
}

/**
 * ThreadLocalAllocBuffer的成员方法，初始化这个ThreadLocalAllocBuffer对象
 */
void ThreadLocalAllocBuffer::initialize() {
  initialize(NULL,                    // start
             NULL,                    // top
             NULL);                   // end

  set_desired_size(initial_desired_size());

  // Following check is needed because at startup the main
  // thread is initialized before the heap is.  The initialization for
  // this thread is redone in startup_initialization below.
  if (Universe::heap() != NULL) {
      /**
       * 搜索 G1CollectedHeap::tlab_capacity
       * 返回总共可以分配给TLAB的空间大小，这里以字大小为单位，即，所有的Young Region中Eden Space的大小总和
       */
    size_t capacity   = Universe::heap()->tlab_capacity(myThread()) / HeapWordSize;
    /**
     * 每次的预期大小，乘以总的分配次数就是实际分配的大小，
     * target_refills是所有线程的目标填充次数之和
     * 因此alloc_frac表示年轻代中实际分配给当前线程的tlab的大小占整个年轻代(除去survivor区域)的比例
     */
    double alloc_frac = desired_size() * target_refills() / (double) capacity;
    _allocation_fraction.sample(alloc_frac);
  }
  /**
   * 根据设置的TLABRefillWasteFraction，设置最大允许浪费的空间大小
   */
  set_refill_waste_limit(initial_refill_waste_limit());

  initialize_statistics();
}

/**
 *  这是ThreadLocalAllocBuffer的静态方法
 */
void ThreadLocalAllocBuffer::startup_initialization() {

  // Assuming each thread's active tlab is, on average,
  // 1/2 full at a GC
  /**
   * 在垃圾回收时，假设每个线程的活跃TLAB平均只有一半被使用,即浪费了50%的空间
   * TLABWasteTargetPercent是一个1 ~ 100的整数值，表示所有的tlba总共可以浪费的eden的空间比值，默认是1，表示所有的TLAB总共可以浪费eden的1%的空间
   * 这样，由于我们假设每次回收的时候所有的tlab都是一半存放、一半满(平均值)，即50%的浪费，由于TLABWasteTargetPercent=1表示每个tlab可以浪费掉整个eden的1%，
   * 此时_target_refills=50，那么填充50次以后，堆内存的浪费比例是50%
   */
  _target_refills = 100 / (2 * TLABWasteTargetPercent);
  /**
   * 目标回填次数最少是1
   */
  _target_refills = MAX2(_target_refills, (unsigned)1U);
  /**
   *  创建一个全局TLAB统计信息的对象。
   */
  _global_stats = new GlobalTLABStats();

  // During jvm startup, the main thread is initialized
  // before the heap is initialized.  So reinitialize it now.
  guarantee(Thread::current()->is_Java_thread(), "tlab initialization thread not Java thread");
  Thread::current()->tlab().initialize(); // 初始化当前线程的tlab

  if (PrintTLAB && Verbose) {
    gclog_or_tty->print("TLAB min: " SIZE_FORMAT " initial: " SIZE_FORMAT " max: " SIZE_FORMAT "\n",
                        min_size(), Thread::current()->tlab().initial_desired_size(), max_size());
  }
}

/**
 * ThreadLocalAllocBuffer的实例方法，用来初始化这个tlab的预期的大小
 * @return
 */
size_t ThreadLocalAllocBuffer::initial_desired_size() {
  size_t init_sz = 0;
  /**
   * 如果显式设置了非0的TLABSize(默认是0)，那么直接除以HeapWordSize计算得到这个TLAB的字大小
   */
  if (TLABSize > 0) {
    init_sz = TLABSize / HeapWordSize;
  } else if (global_stats() != NULL) { // 没有显示设置TLABSize
    // Initial size is a function of the average number of allocating threads.
    /**
     * 正在并行进行分配的线程的平均数量
     * 如果只有一个并行线程在分配，那么就是除以全局的target_refills，如果有两个，除以 2 * target_refills()
     */
    unsigned nof_threads = global_stats()->allocating_threads_avg();
    /**
     * 计算平均一个线程的预期大小
     * 搜索 G1CollectedHeap::tlab_capacity(Thread* ignored)
     * Universe::heap()->tlab_capacity(myThread()) / HeapWordSize返回的是整个堆内存年轻代(不包括survivor)可以分配给tlab的空间大小，以字为单位
     * nof_threads * target_refills() 是所有线程总共refill的次数之和
     * 因此，（Universe::heap()->tlab_capacity(myThread()) / HeapWordSize）/ nof_threads * target_refills() 就代表每一个线程所分配的一个tlab的大小
     * 这里的计算逻辑是，用整个堆内存的所有eden space的region的字大小总容量，除以线程总数，再除以 target_refills()
     */
    init_sz  = (Universe::heap()->tlab_capacity(myThread()) / HeapWordSize) /
                      (nof_threads * target_refills());
    init_sz = align_object_size(init_sz); // 大小需要进行字对齐
  }
  init_sz = MIN2(MAX2(init_sz, min_size()), max_size()); // 最小、最大的区间约束
  return init_sz;
}

void ThreadLocalAllocBuffer::print_stats(const char* tag) {
  Thread* thrd = myThread();
  size_t waste = _gc_waste + _slow_refill_waste + _fast_refill_waste;
  size_t alloc = _number_of_refills * _desired_size;
  double waste_percent = alloc == 0 ? 0.0 :
                      100.0 * waste / alloc;
  size_t tlab_used  = Universe::heap()->tlab_used(thrd);
  gclog_or_tty->print("TLAB: %s thread: " INTPTR_FORMAT " [id: %2d]"
                      " desired_size: " SIZE_FORMAT "KB"
                      " slow allocs: %d  refill waste: " SIZE_FORMAT "B"
                      " alloc:%8.5f %8.0fKB refills: %d waste %4.1f%% gc: %dB"
                      " slow: %dB fast: %dB\n",
                      tag, thrd, thrd->osthread()->thread_id(),
                      _desired_size / (K / HeapWordSize),
                      _slow_allocations, _refill_waste_limit * HeapWordSize,
                      _allocation_fraction.average(),
                      _allocation_fraction.average() * tlab_used / K,
                      _number_of_refills, waste_percent,
                      _gc_waste * HeapWordSize,
                      _slow_refill_waste * HeapWordSize,
                      _fast_refill_waste * HeapWordSize);
}

void ThreadLocalAllocBuffer::verify() {
  HeapWord* p = start();
  HeapWord* t = top();
  HeapWord* prev_p = NULL;
  while (p < t) {
    oop(p)->verify();
    prev_p = p;
    p += oop(p)->size();
  }
  guarantee(p == top(), "end of last object must match end of space");
}

Thread* ThreadLocalAllocBuffer::myThread() {
  return (Thread*)(((char *)this) +
                   in_bytes(start_offset()) -
                   in_bytes(Thread::tlab_start_offset()));
}


GlobalTLABStats::GlobalTLABStats() :
  _allocating_threads_avg(TLABAllocationWeight) {

  initialize();

  _allocating_threads_avg.sample(1); // One allocating thread at startup

  if (UsePerfData) {

    EXCEPTION_MARK;
    ResourceMark rm;

    char* cname = PerfDataManager::counter_name("tlab", "allocThreads");
    _perf_allocating_threads =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_None, CHECK);

    cname = PerfDataManager::counter_name("tlab", "fills");
    _perf_total_refills =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_None, CHECK);

    cname = PerfDataManager::counter_name("tlab", "maxFills");
    _perf_max_refills =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_None, CHECK);

    cname = PerfDataManager::counter_name("tlab", "alloc");
    _perf_allocation =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "gcWaste");
    _perf_gc_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "maxGcWaste");
    _perf_max_gc_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "slowWaste");
    _perf_slow_refill_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "maxSlowWaste");
    _perf_max_slow_refill_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "fastWaste");
    _perf_fast_refill_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "maxFastWaste");
    _perf_max_fast_refill_waste =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, CHECK);

    cname = PerfDataManager::counter_name("tlab", "slowAlloc");
    _perf_slow_allocations =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_None, CHECK);

    cname = PerfDataManager::counter_name("tlab", "maxSlowAlloc");
    _perf_max_slow_allocations =
      PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_None, CHECK);
  }
}

void GlobalTLABStats::initialize() {
  // Clear counters summarizing info from all threads
  _allocating_threads      = 0;
  _total_refills           = 0;
  _max_refills             = 0;
  _total_allocation        = 0;
  _total_gc_waste          = 0;
  _max_gc_waste            = 0;
  _total_slow_refill_waste = 0;
  _max_slow_refill_waste   = 0;
  _total_fast_refill_waste = 0;
  _max_fast_refill_waste   = 0;
  _total_slow_allocations  = 0;
  _max_slow_allocations    = 0;
}

void GlobalTLABStats::publish() {
  _allocating_threads_avg.sample(_allocating_threads);
  if (UsePerfData) {
    _perf_allocating_threads   ->set_value(_allocating_threads);
    _perf_total_refills        ->set_value(_total_refills);
    _perf_max_refills          ->set_value(_max_refills);
    _perf_allocation           ->set_value(_total_allocation);
    _perf_gc_waste             ->set_value(_total_gc_waste);
    _perf_max_gc_waste         ->set_value(_max_gc_waste);
    _perf_slow_refill_waste    ->set_value(_total_slow_refill_waste);
    _perf_max_slow_refill_waste->set_value(_max_slow_refill_waste);
    _perf_fast_refill_waste    ->set_value(_total_fast_refill_waste);
    _perf_max_fast_refill_waste->set_value(_max_fast_refill_waste);
    _perf_slow_allocations     ->set_value(_total_slow_allocations);
    _perf_max_slow_allocations ->set_value(_max_slow_allocations);
  }
}

void GlobalTLABStats::print() {
  size_t waste = _total_gc_waste + _total_slow_refill_waste + _total_fast_refill_waste;
  double waste_percent = _total_allocation == 0 ? 0.0 :
                         100.0 * waste / _total_allocation;
  gclog_or_tty->print("TLAB totals: thrds: %d  refills: %d max: %d"
                      " slow allocs: %d max %d waste: %4.1f%%"
                      " gc: " SIZE_FORMAT "B max: " SIZE_FORMAT "B"
                      " slow: " SIZE_FORMAT "B max: " SIZE_FORMAT "B"
                      " fast: " SIZE_FORMAT "B max: " SIZE_FORMAT "B\n",
                      _allocating_threads,
                      _total_refills, _max_refills,
                      _total_slow_allocations, _max_slow_allocations,
                      waste_percent,
                      _total_gc_waste * HeapWordSize,
                      _max_gc_waste * HeapWordSize,
                      _total_slow_refill_waste * HeapWordSize,
                      _max_slow_refill_waste * HeapWordSize,
                      _total_fast_refill_waste * HeapWordSize,
                      _max_fast_refill_waste * HeapWordSize);
}
