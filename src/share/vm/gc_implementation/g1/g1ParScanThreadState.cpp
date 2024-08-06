/*
 * Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1ParScanThreadState.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.pcgc.inline.hpp"
#include "runtime/prefetch.inline.hpp"

G1ParScanThreadState::G1ParScanThreadState(G1CollectedHeap* g1h, uint queue_num, ReferenceProcessor* rp)
  : _g1h(g1h),
    _refs(g1h->task_queue(queue_num)),
    _dcq(&g1h->dirty_card_queue_set()),
    _ct_bs(g1h->g1_barrier_set()),
    _g1_rem(g1h->g1_rem_set()),
    _hash_seed(17), _queue_num(queue_num),
    _term_attempts(0),
    _tenuring_threshold(g1h->g1_policy()->tenuring_threshold()),
    _age_table(false), _scanner(g1h, rp),
    _strong_roots_time(0), _term_time(0) {
    /**
     * 这个scanner用于在完成了一个对象的evacuation以后，对对象进行一个递归的扫描
     * 搜索 obj->oop_iterate_backwards(&_scanner);
     */
  _scanner.set_par_scan_thread_state(this);
  // we allocate G1YoungSurvRateNumRegions plus one entries, since
  // we "sacrifice" entry 0 to keep track of surviving bytes for
  // non-young regions (where the age is -1)
  // We also add a few elements at the beginning and at the end in
  // an attempt to eliminate cache contention
  uint real_length = 1 + _g1h->g1_policy()->young_cset_region_length();
  uint array_length = PADDING_ELEM_NUM +
                      real_length +
                      PADDING_ELEM_NUM;
  _surviving_young_words_base = NEW_C_HEAP_ARRAY(size_t, array_length, mtGC);
  if (_surviving_young_words_base == NULL)
    vm_exit_out_of_memory(array_length * sizeof(size_t), OOM_MALLOC_ERROR,
                          "Not enough space for young surv histo.");
  _surviving_young_words = _surviving_young_words_base + PADDING_ELEM_NUM;
  memset(_surviving_young_words, 0, (size_t) real_length * sizeof(size_t));

  _g1_par_allocator = G1ParGCAllocator::create_allocator(_g1h);

  _dest[InCSetState::NotInCSet]    = InCSetState::NotInCSet;
  // The dest for Young is used when the objects are aged enough to
  // need to be moved to the next space.
  _dest[InCSetState::Young]        = InCSetState::Old;
  _dest[InCSetState::Old]          = InCSetState::Old;

  _start = os::elapsedTime();
}

G1ParScanThreadState::~G1ParScanThreadState() {
  _g1_par_allocator->retire_alloc_buffers();
  delete _g1_par_allocator;
  FREE_C_HEAP_ARRAY(size_t, _surviving_young_words_base, mtGC);
}

void
G1ParScanThreadState::print_termination_stats_hdr(outputStream* const st)
{
  st->print_raw_cr("GC Termination Stats");
  st->print_raw_cr("     elapsed  --strong roots-- -------termination-------"
                   " ------waste (KiB)------");
  st->print_raw_cr("thr     ms        ms      %        ms      %    attempts"
                   "  total   alloc    undo");
  st->print_raw_cr("--- --------- --------- ------ --------- ------ --------"
                   " ------- ------- -------");
}

void
G1ParScanThreadState::print_termination_stats(int i,
                                              outputStream* const st) const
{
  const double elapsed_ms = elapsed_time() * 1000.0;
  const double s_roots_ms = strong_roots_time() * 1000.0;
  const double term_ms    = term_time() * 1000.0;
  const size_t alloc_buffer_waste = _g1_par_allocator->alloc_buffer_waste();
  const size_t undo_waste         = _g1_par_allocator->undo_waste();
  st->print_cr("%3d %9.2f %9.2f %6.2f "
               "%9.2f %6.2f " SIZE_FORMAT_W(8) " "
               SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7),
               i, elapsed_ms, s_roots_ms, s_roots_ms * 100 / elapsed_ms,
               term_ms, term_ms * 100 / elapsed_ms, term_attempts(),
               (alloc_buffer_waste + undo_waste) * HeapWordSize / K,
               alloc_buffer_waste * HeapWordSize / K,
               undo_waste * HeapWordSize / K);
}

#ifdef ASSERT
bool G1ParScanThreadState::verify_ref(narrowOop* ref) const {
  assert(ref != NULL, "invariant");
  assert(UseCompressedOops, "sanity");
  assert(!has_partial_array_mask(ref), err_msg("ref=" PTR_FORMAT, p2i(ref)));
  oop p = oopDesc::load_decode_heap_oop(ref);
  assert(_g1h->is_in_g1_reserved(p),
         err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  return true;
}

bool G1ParScanThreadState::verify_ref(oop* ref) const {
  assert(ref != NULL, "invariant");
  if (has_partial_array_mask(ref)) {
    // Must be in the collection set--it's already been copied.
    oop p = clear_partial_array_mask(ref);
    assert(_g1h->obj_in_cs(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  } else {
    oop p = oopDesc::load_decode_heap_oop(ref);
    assert(_g1h->is_in_g1_reserved(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  }
  return true;
}

bool G1ParScanThreadState::verify_task(StarTask ref) const {
  if (ref.is_narrow()) {
    return verify_ref((narrowOop*) ref);
  } else {
    return verify_ref((oop*) ref);
  }
}
#endif // ASSERT

/**
 * 调用方法为 G1ParEvacuateFollowersClosure::do_void()
 * 查看 G1ParPushHeapRSClosure::do_oop_nv 可以看到往_refs中插入元素的过程
 * _refs中的元素是在根扫描的时候插入进来的
 */
void G1ParScanThreadState::trim_queue() {
  assert(_evac_failure_cl != NULL, "not set");

  StarTask ref;
  do { // 不断循环，直到_refs中的ref被全部处理完
    // Drain the overflow stack first, so other threads can steal.
    while (_refs->pop_overflow(ref)) { //不断取出队列中的每一个对象引用，放入到ref中，直到_ref为空
      if (!_refs->try_push_to_taskqueue(ref)) { // 把ref 给 push到这个_ref对应的taskqueue中
          /**
           * 对这个对象引用进行处理，会对ref对应的对象进行转移操作，
           * 搜索 G1ParScanThreadState::dispatch_reference -> G1ParScanThreadState::deal_with_reference
           */
        dispatch_reference(ref); //
      }
    }

    while (_refs->pop_local(ref)) {
      dispatch_reference(ref); // 对这个对象引用进行处理，会对ref对应的对象进行转移操作
    }
  } while (!_refs->is_empty());
}

/**
 * 调用者是 G1ParScanThreadState::copy_to_survivor_space
 */
HeapWord* G1ParScanThreadState::allocate_in_next_plab(InCSetState const state,
                                                      InCSetState* dest,
                                                      size_t word_sz,
                                                      AllocationContext_t const context) {
  assert(state.is_in_cset_or_humongous(), err_msg("Unexpected state: " CSETSTATE_FORMAT, state.value()));
  assert(dest->is_in_cset_or_humongous(), err_msg("Unexpected dest: " CSETSTATE_FORMAT, dest->value()));

  // Right now we only have two types of regions (young / old) so
  // let's keep the logic here simple. We can generalize it when necessary.
  if (dest->is_young()) {
      // 如果目标区域是Young，那么就尝试在Old区域
    HeapWord* const obj_ptr = _g1_par_allocator->allocate(InCSetState::Old,
                                                          word_sz, context);
    if (obj_ptr == NULL) {
      return NULL;
    }
    // Make sure that we won't attempt to copy any other objects out
    // of a survivor region (given that apparently we cannot allocate
    // any new ones) to avoid coming into this slow path.
    /**
     * 将 _tenuring_threshold 设置为 0，以确保不会尝试将任何其他对象从survivor区域，从而避免进入慢速路径。
     * 因为代码走到这里，说明年轻代已经无法容纳这个对象了，那么后面的对象就不需要再尝试在年轻代区域分配了，直接在老年代区域分配
     */
    _tenuring_threshold = 0;
    dest->set_old(); // 将当前的目标状态改值为Old状态
    return obj_ptr;
  } else { // 除了InCSetState==young区可以在下一个区域进行分配，其他情况无法实现这个功能
    assert(dest->is_old(), err_msg("Unexpected dest: " CSETSTATE_FORMAT, dest->value()));
    // no other space to try.
    return NULL;
  }
}

/**
 * 根据对象的当前年龄，当前InCSetState状态，获取对象的目标InCSetState状态
 */
InCSetState G1ParScanThreadState::next_state(InCSetState const state, markOop const m, uint& age) {
  if (state.is_young()) { // 如果对象目前在年轻代，则需要根据对象年龄判断对象是否需要晋升到老年代
    age = !m->has_displaced_mark_helper() ? m->age()
                                          : m->displaced_mark_helper()->age();
    /**
     * 如果对象的年龄小于MaxTenuringThreshold，那么可以保持住状态，否则就需要晋升
     */
    if (age < _tenuring_threshold) {
      return state;
    }
  }
  /**
   * 返回这个state的目标state
   * 搜索构造方法 G1ParScanThreadState::G1ParScanThreadState 可以看到_dest数组的构造
   */
  return dest(state); // 或者这个state的目标state
}
/**
 * 在方法  G1ParCopyClosure<barrier, do_mark_object>::do_oop_work 和
 *  方法 template <class T> void G1ParScanThreadState::do_oop_evac中被调用
 */
oop G1ParScanThreadState::copy_to_survivor_space(InCSetState const state,
                                                 oop const old, //需要移动的对象，oop是一个typedef oopDesc*   oop;
                                                 markOop const old_mark) {
  const size_t word_sz = old->size(); // 获取对象的大小
  HeapRegion* const from_region = _g1h->heap_region_containing_raw(old);
  // +1 to make the -1 indexes valid...
  const int young_index = from_region->young_index_in_cset()+1;
  assert( (from_region->is_young() && young_index >  0) ||
         (!from_region->is_young() && young_index == 0), "invariant" );
  const AllocationContext_t context = from_region->allocation_context();

  uint age = 0;
  InCSetState dest_state = next_state(state, old_mark, age); // 根据对象当前的状态，年龄，确定对象的下一个状态
    /**
     * 根据对象的下一个状态尝试在plab上进行相应分配
     */
  HeapWord* obj_ptr = _g1_par_allocator->plab_allocate(dest_state, word_sz, context); //

  // PLAB allocations should succeed most of the time, so we'll
  // normally check against NULL once and that's it.
  if (obj_ptr == NULL) { // 在plab上分配失败
      /**
       * 查看方法实现 G1ParGCAllocator::allocate_direct_or_new_plab
       * 尝试通过创建新的plab或者直接将对象分配在对应区域的堆内存
       */
    obj_ptr = _g1_par_allocator->allocate_direct_or_new_plab(dest_state, word_sz, context);
    if (obj_ptr == NULL) {
        // 尝试在当前的dest_state的下一个state对应的region中分配
      obj_ptr = allocate_in_next_plab(state, &dest_state, word_sz, context);
      if (obj_ptr == NULL) { // 最终失败，进行转移失败的相关处理
        // This will either forward-to-self, or detect that someone else has
        // installed a forwarding pointer.
        /**
         * 搜索 G1CollectedHeap::handle_evacuation_failure_par
         */
        return _g1h->handle_evacuation_failure_par(this, old); // 处理转移失败的情况
      }
    }
  }

  assert(obj_ptr != NULL, "when we get here, allocation should have succeeded");
#ifndef PRODUCT
  // Should this evacuation fail?
  if (_g1h->evacuation_should_fail()) { // 这里返回true，说明evacuation失败了
    // Doing this after all the allocation attempts also tests the
    // undo_allocation() method too.
    _g1_par_allocator->undo_allocation(dest_state, obj_ptr, word_sz, context);
    /**
     * 搜索 G1CollectedHeap::handle_evacuation_failure_par
     */
    return _g1h->handle_evacuation_failure_par(this, old);
  }
#endif // !PRODUCT
  // 在这里，说明evacuation成功了
  // We're going to allocate linearly, so might as well prefetch ahead.
  Prefetch::write(obj_ptr, PrefetchCopyIntervalInBytes);

  const oop obj = oop(obj_ptr);
  const oop forward_ptr = old->forward_to_atomic(obj);
  if (forward_ptr == NULL) {
    Copy::aligned_disjoint_words((HeapWord*) old, obj_ptr, word_sz);

    if (dest_state.is_young()) {
      if (age < markOopDesc::max_age) {
        age++;
      }
      if (old_mark->has_displaced_mark_helper()) {
        // In this case, we have to install the mark word first,
        // otherwise obj looks to be forwarded (the old mark word,
        // which contains the forward pointer, was copied)
        obj->set_mark(old_mark);
        markOop new_mark = old_mark->displaced_mark_helper()->set_age(age);
        old_mark->set_displaced_mark_helper(new_mark);
      } else {
        obj->set_mark(old_mark->set_age(age));
      }
      age_table()->add(age, word_sz);
    } else {
        /**
         * 如果不是年轻代对象，对对象进行标记
         * 这个标记位是设置在对象的header中的，而不是并发标记中的标记位图标记
         */
      obj->set_mark(old_mark);
    }
    /**
     * 字符串去重操作
     */
    if (G1StringDedup::is_enabled()) {
      const bool is_from_young = state.is_young();
      const bool is_to_young = dest_state.is_young();
      assert(is_from_young == _g1h->heap_region_containing_raw(old)->is_young(),
             "sanity");
      assert(is_to_young == _g1h->heap_region_containing_raw(obj)->is_young(),
             "sanity");
      G1StringDedup::enqueue_from_evacuation(is_from_young,
                                             is_to_young,
                                             queue_num(),
                                             obj);
    }

    size_t* const surv_young_words = surviving_young_words();
    surv_young_words[young_index] += word_sz;

    if (obj->is_objArray() && arrayOop(obj)->length() >= ParGCArrayScanChunk) {
      // We keep track of the next start index in the length field of
      // the to-space object. The actual length can be found in the
      // length field of the from-space object.
      arrayOop(obj)->set_length(0);
      oop* old_p = set_partial_array_mask(old);
      push_on_queue(old_p);
    } else {
        // 获取对象移动到的目标region
      HeapRegion* const to_region = _g1h->heap_region_containing_raw(obj_ptr);
      _scanner.set_region(to_region);  // 设置这个目标region到scanner中
      /**
       * 搜索 inline int oopDesc::oop_iterate_backwards(OopClosureType* blk)  -> InstanceKlass::oop_oop_iterate_backwards##nv_suffix
       *  查看这个宏定义的调用栈
       * 把obj的每一个Field对象都通过scanner，如果对象的field也在cset中，那么就需要放入任务栈中递归扫描
       * 搜索 G1ParScanClosure::do_oop_nv
       */
      obj->oop_iterate_backwards(&_scanner);
    }
    return obj;
  } else {
    _g1_par_allocator->undo_allocation(dest_state, obj_ptr, word_sz, context);
    return forward_ptr;
  }
}
