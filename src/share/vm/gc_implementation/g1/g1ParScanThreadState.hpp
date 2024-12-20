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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1PARSCANTHREADSTATE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1PARSCANTHREADSTATE_HPP

#include "gc_implementation/g1/dirtyCardQueue.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1OopClosures.hpp"
#include "gc_implementation/g1/g1RemSet.hpp"
#include "gc_implementation/shared/ageTable.hpp"
#include "memory/allocation.hpp"
#include "oops/oop.hpp"

class HeapRegion;
class outputStream;

class G1ParScanThreadState : public StackObj {
 private:
  G1CollectedHeap* _g1h;
  /** typedef OverflowTaskQueue<StarTask, mtGC>         RefToScanQueue;
   *  这是一个 RefToScanQueue 对象，本质上是一个TaskQueue对象
   *  搜索 push_on_queue 可以看到往这里添加引用的过程，搜索 G1ParScanThreadState::trim_queue，可以看到从queue中弹出引用进行处理的过程
   */
  RefToScanQueue*  _refs;
  DirtyCardQueue   _dcq; // 来自G1CollectedHeap的全局dcqs
  G1SATBCardTableModRefBS* _ct_bs;
  G1RemSet* _g1_rem;

  G1ParGCAllocator* _g1_par_allocator;

  ageTable          _age_table;
  InCSetState       _dest[InCSetState::Num];
  // Local tenuring threshold.
  uint              _tenuring_threshold; // 初始化的时候(查看构造方法 G1ParScanThreadState::G1ParScanThreadState)，是根据Policy设置了晋升的年龄值
    /**
   * 这个scanner用于在完成了一个对象的evacuation以后，对对象进行一个递归的扫描
   * 搜索 obj->oop_iterate_backwards(&_scanner);
   */
  G1ParScanClosure  _scanner; //

  size_t            _alloc_buffer_waste;
  size_t            _undo_waste;

  OopsInHeapRegionClosure*      _evac_failure_cl;

  int  _hash_seed;
  uint _queue_num;

  size_t _term_attempts;

  double _start;
  double _start_strong_roots;
  double _strong_roots_time;
  double _start_term;
  double _term_time;

  // Map from young-age-index (0 == not young, 1 is youngest) to
  // surviving words. base is what we get back from the malloc call
  size_t* _surviving_young_words_base;
  // this points into the array, as we use the first few entries for padding
  size_t* _surviving_young_words;

#define PADDING_ELEM_NUM (DEFAULT_CACHE_LINE_SIZE / sizeof(size_t))

  void   add_to_alloc_buffer_waste(size_t waste) { _alloc_buffer_waste += waste; }
  void   add_to_undo_waste(size_t waste)         { _undo_waste += waste; }

  DirtyCardQueue& dirty_card_queue()             { return _dcq;  }
  G1SATBCardTableModRefBS* ctbs()                { return _ct_bs; }

  InCSetState dest(InCSetState original) const {
    assert(original.is_valid(),
           err_msg("Original state invalid: " CSETSTATE_FORMAT, original.value()));
    assert(_dest[original.value()].is_valid_gen(),
           err_msg("Dest state is invalid: " CSETSTATE_FORMAT, _dest[original.value()].value()));
    return _dest[original.value()];
  }

 public:
  G1ParScanThreadState(G1CollectedHeap* g1h, uint queue_num, ReferenceProcessor* rp);
  ~G1ParScanThreadState();

  ageTable*         age_table()       { return &_age_table;       }

#ifdef ASSERT
  bool queue_is_empty() const { return _refs->is_empty(); }

  bool verify_ref(narrowOop* ref) const;
  bool verify_ref(oop* ref) const;
  bool verify_task(StarTask ref) const;
#endif // ASSERT

  /**
   * 这个方法是类G1ParScanThreadState的成员方法
   * 搜索 G1ParPushHeapRSClosure::do_oop_nv 方法 可以看到基于cset进行转移的时候，使用这个closure往pss的_ref队列中添加引用的过程
   *    比如 G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss)就将G1ParScanThreadState作为自己的成员
   *
   * 搜索 G1ParScanClosure::do_oop_nv(T* p) 方法，可以看到在 转移暂停的 copy_to_survivor 的过程中往这个pss的_ref中添加元素的过程，
   *    即当我们完成了一个obj的拷贝，那么这个obj的所有的reference都需要递归进行处理
   *
   * 搜索 G1ParScanThreadState::trim_queue() 可以看到从_refs中取出元素处理的过程，
   *    其实就是在根扫描完成以后，进行转移的时候通过G1ParEvacuateFollowersClosure调用的
   * @tparam T
   * @param ref
   */
  template <class T> void push_on_queue(T* ref) {
    assert(verify_ref(ref), "sanity");
    _refs->push(ref);
  }

  /**
   * in book
   * 这是 G1ParScanThreadState::update_rs
   * 注意，一个G1ParTask会有一个G1ParScanThreadState对象
   * 搜索 _par_scan_state->update_rs 和 G1ParScanThreadState::do_oop_evac 查看调用位置
   * p 代表了对象的原始地址，
   * @tparam T
   * @param from field所在的region
   * @param p 刚刚发生了对象移动的field指针，即指向一个field的指针，现在这个field指向的对象已经发生了移动。
   * @param tid
   */
  template <class T> void update_rs(HeapRegion* from, T* p, int tid) {
    // If the new value of the field points to the same region or
    // is the to-space, we don't need to include it in the Rset updates.
    /**
     * oopDesc::load_decode_heap_oop(p) 是取出指针p所指向的地址上存放的值，这个指是对应的回收集合中的对象的指针
     * 因此,from代表field所在的HeapRegion，oopDesc::load_decode_heap_oop(p)代表field所指向的对象的地址，两个地址不可以相同，不然更新卡片没有任何意义
     * 当且仅当对象本身不在from 中，并且，from不是survivor region的时候，才需要处理rset
     */
    if (!from->is_in_reserved(oopDesc::load_decode_heap_oop(p)) && !from->is_survivor()) {
      size_t card_index = ctbs()->index_for(p); // 对应的field的卡片的卡片索引, 搜索 size_t index_for(void* p) 查看具体实现
      // If the card hasn't been added to the buffer, do it.
      /**
       * 检查这个field的卡片是否还需要后续的处理。比如，如果卡片已经被标记为延迟处理，或者卡片已经被其他线程claim了，说明其它GC线程已经将其添加到DCQS了，这里不需要重复添加
       * 如果卡片当前是clean的，那么说明当前线程是第一个处理它的， 因此，当前线程会处理它，将其加入到全局的_dirty_card_queue_set中
       */
      if (ctbs()->mark_card_deferred(card_index)) {
          /**
           * dirty_card_queue() 返回的是当前的G1ParScanThreadState的DCQ，所有的G1ParScanThreadState对象的dcq都是共享于G1CollectedHeap的_dirty_card_queue_set
           * 现在看到，迁移导致的脏卡片只是用来在redirty(数据恢复)的时候使用
           * 将源对象地址存入到这个回收线程本地的转移专用记忆集合日志中，后续会有Refine线程来进行处理
           * 所以，脏卡片队列中的脏卡片可能来自用户代码中的引用更新，也可能来自gc线程的转移操作
           * ctbs()->byte_for_index(card_index) 返回了这个卡片索引在卡片数组中的真实偏移量
           */
        dirty_card_queue().enqueue((jbyte*)ctbs()->byte_for_index(card_index));
      }
   }
  }

  /**
   * 搜索  G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, rp);
   * @param evac_failure_cl
   */
  void set_evac_failure_closure(OopsInHeapRegionClosure* evac_failure_cl) {
    _evac_failure_cl = evac_failure_cl;
  }

  OopsInHeapRegionClosure* evac_failure_closure() { return _evac_failure_cl; }

  int* hash_seed() { return &_hash_seed; }
  uint queue_num() { return _queue_num; }

  size_t term_attempts() const  { return _term_attempts; }
  void note_term_attempt() { _term_attempts++; }

  void start_strong_roots() {
    _start_strong_roots = os::elapsedTime();
  }
  void end_strong_roots() {
    _strong_roots_time += (os::elapsedTime() - _start_strong_roots);
  }
  double strong_roots_time() const { return _strong_roots_time; }

  void start_term_time() {
    note_term_attempt();
    _start_term = os::elapsedTime();
  }
  void end_term_time() {
    _term_time += (os::elapsedTime() - _start_term);
  }
  double term_time() const { return _term_time; }

  double elapsed_time() const {
    return os::elapsedTime() - _start;
  }

  static void print_termination_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_termination_stats(int i, outputStream* const st = gclog_or_tty) const;

  size_t* surviving_young_words() {
    // We add on to hide entry 0 which accumulates surviving words for
    // age -1 regions (i.e. non-young ones)
    return _surviving_young_words;
  }

 private:
  #define G1_PARTIAL_ARRAY_MASK 0x2

  inline bool has_partial_array_mask(oop* ref) const {
    return ((uintptr_t)ref & G1_PARTIAL_ARRAY_MASK) == G1_PARTIAL_ARRAY_MASK;
  }

  // We never encode partial array oops as narrowOop*, so return false immediately.
  // This allows the compiler to create optimized code when popping references from
  // the work queue.
  inline bool has_partial_array_mask(narrowOop* ref) const {
    assert(((uintptr_t)ref & G1_PARTIAL_ARRAY_MASK) != G1_PARTIAL_ARRAY_MASK, "Partial array oop reference encoded as narrowOop*");
    return false;
  }

  // Only implement set_partial_array_mask() for regular oops, not for narrowOops.
  // We always encode partial arrays as regular oop, to allow the
  // specialization for has_partial_array_mask() for narrowOops above.
  // This means that unintentional use of this method with narrowOops are caught
  // by the compiler.
  inline oop* set_partial_array_mask(oop obj) const {
    assert(((uintptr_t)(void *)obj & G1_PARTIAL_ARRAY_MASK) == 0, "Information loss!");
    return (oop*) ((uintptr_t)(void *)obj | G1_PARTIAL_ARRAY_MASK);
  }

  inline oop clear_partial_array_mask(oop* ref) const {
    return cast_to_oop((intptr_t)ref & ~G1_PARTIAL_ARRAY_MASK);
  }

  inline void do_oop_partial_array(oop* p);

  // This method is applied to the fields of the objects that have just been copied.
  /**
   * 搜索 template <class T> void G1ParScanThreadState::do_oop_evac 查看具体实现，这是一个内联方法
   * @tparam T
   * @param p
   * @param from
   */
  template <class T> inline void do_oop_evac(T* p, HeapRegion* from);

  template <class T> inline void deal_with_reference(T* ref_to_scan);

  inline void dispatch_reference(StarTask ref);

  // Tries to allocate word_sz in the PLAB of the next "generation" after trying to
  // allocate into dest. State is the original (source) cset state for the object
  // that is allocated for.
  // Returns a non-NULL pointer if successful, and updates dest if required.
  HeapWord* allocate_in_next_plab(InCSetState const state,
                                  InCSetState* dest,
                                  size_t word_sz,
                                  AllocationContext_t const context);

  inline InCSetState next_state(InCSetState const state, markOop const m, uint& age);
 public: // 方法实现，搜索 G1ParScanThreadState::copy_to_survivor_space
  oop copy_to_survivor_space(InCSetState const state, oop const obj, markOop const old_mark);

  void trim_queue();

  inline void steal_and_trim_queue(RefToScanQueueSet *task_queues);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1PARSCANTHREADSTATE_HPP
