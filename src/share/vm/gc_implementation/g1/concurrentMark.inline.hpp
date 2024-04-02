/*
 * Copyright (c) 2001, 2014, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTMARK_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTMARK_INLINE_HPP

#include "gc_implementation/g1/concurrentMark.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"

// Utility routine to set an exclusive range of cards on the given
// card liveness bitmap
inline void ConcurrentMark::set_card_bitmap_range(BitMap* card_bm,
                                                  BitMap::idx_t start_idx,
                                                  BitMap::idx_t end_idx,
                                                  bool is_par) {

  // Set the exclusive bit range [start_idx, end_idx).
  assert((end_idx - start_idx) > 0, "at least one card");
  assert(end_idx <= card_bm->size(), "sanity");

  // Silently clip the end index
  end_idx = MIN2(end_idx, card_bm->size());

  // For small ranges use a simple loop; otherwise use set_range or
  // use par_at_put_range (if parallel). The range is made up of the
  // cards that are spanned by an object/mem region so 8 cards will
  // allow up to object sizes up to 4K to be handled using the loop.
  if ((end_idx - start_idx) <= 8) {
    for (BitMap::idx_t i = start_idx; i < end_idx; i += 1) {
      if (is_par) {
        card_bm->par_set_bit(i);
      } else {
        card_bm->set_bit(i);
      }
    }
  } else {
    // Note BitMap::par_at_put_range() and BitMap::set_range() are exclusive.
    if (is_par) {
      card_bm->par_at_put_range(start_idx, end_idx, true);
    } else {
      card_bm->set_range(start_idx, end_idx);
    }
  }
}

// Returns the index in the liveness accounting card bitmap
// for the given address
inline BitMap::idx_t ConcurrentMark::card_bitmap_index_for(HeapWord* addr) {
  // Below, the term "card num" means the result of shifting an address
  // by the card shift -- address 0 corresponds to card number 0.  One
  // must subtract the card num of the bottom of the heap to obtain a
  // card table index.
  intptr_t card_num = intptr_t(uintptr_t(addr) >> CardTableModRefBS::card_shift);
  return card_num - heap_bottom_card_num();
}

// Counts the given memory region in the given task/worker
// counting data structures.
inline void ConcurrentMark::count_region(MemRegion mr, HeapRegion* hr,
                                         size_t* marked_bytes_array,
                                         BitMap* task_card_bm) {
  G1CollectedHeap* g1h = _g1h;
  CardTableModRefBS* ct_bs = g1h->g1_barrier_set();

  HeapWord* start = mr.start();
  HeapWord* end = mr.end();
  size_t region_size_bytes = mr.byte_size();
  uint index = hr->hrm_index();

  assert(!hr->continuesHumongous(), "should not be HC region");
  assert(hr == g1h->heap_region_containing(start), "sanity");
  assert(hr == g1h->heap_region_containing(mr.last()), "sanity");
  assert(marked_bytes_array != NULL, "pre-condition");
  assert(task_card_bm != NULL, "pre-condition");

  // Add to the task local marked bytes for this region.
  marked_bytes_array[index] += region_size_bytes;

  BitMap::idx_t start_idx = card_bitmap_index_for(start);
  BitMap::idx_t end_idx = card_bitmap_index_for(end);

  // Note: if we're looking at the last region in heap - end
  // could be actually just beyond the end of the heap; end_idx
  // will then correspond to a (non-existent) card that is also
  // just beyond the heap.
  if (g1h->is_in_g1_reserved(end) && !ct_bs->is_card_aligned(end)) {
    // end of region is not card aligned - incremement to cover
    // all the cards spanned by the region.
    end_idx += 1;
  }
  // The card bitmap is task/worker specific => no need to use
  // the 'par' BitMap routines.
  // Set bits in the exclusive bit range [start_idx, end_idx).
  set_card_bitmap_range(task_card_bm, start_idx, end_idx, false /* is_par */);
}

// Counts the given memory region in the task/worker counting
// data structures for the given worker id.
/**
 * 调用者是 inline bool ConcurrentMark::par_mark_and_count
 * 更新这个worker的卡片数组
 * @param mr
 * @param hr
 * @param worker_id
 */
inline void ConcurrentMark::count_region(MemRegion mr,
                                         HeapRegion* hr,
                                         uint worker_id) {
  size_t* marked_bytes_array = count_marked_bytes_array_for(worker_id);
  BitMap* task_card_bm = count_card_bitmap_for(worker_id); // 获取 这个worker对应的标记数组 _count_card_bitmaps
  count_region(mr, hr, marked_bytes_array, task_card_bm);
}

// Counts the given object in the given task/worker counting data structures.
inline void ConcurrentMark::count_object(oop obj,
                                         HeapRegion* hr,
                                         size_t* marked_bytes_array,
                                         BitMap* task_card_bm) {
  MemRegion mr((HeapWord*)obj, obj->size());
  count_region(mr, hr, marked_bytes_array, task_card_bm);
}

// Attempts to mark the given object and, if successful, counts
// the object in the given task/worker counting structures.
/**
 * 这是ConcurrentMark的实例方法，这个方法仅仅是将对象在标记位图上进行标记，不会通过将对象入栈的方式递归扫描对象的field，
 *   将对象入栈以递归扫描是在它的调用者 make_reference_grey 上负责的
 * 尝试标记给定对象，如果成功，则在给定任务/工作人员计数结构中对对象进行计数
 * 如果置位成功，返回true，如果发现其他线程已经对这个位图的这个位置进行了置位，返回false
 * @param obj
 * @param hr
 * @param marked_bytes_array
 * @param task_card_bm
 * @return
 */
inline bool ConcurrentMark::par_mark_and_count(oop obj,
                                               HeapRegion* hr,
                                               size_t* marked_bytes_array,
                                               BitMap* task_card_bm) {
  HeapWord* addr = (HeapWord*)obj;
  if (_nextMarkBitMap->parMark(addr)) { // 置位成功，返回true，这里可以看到，_nextMarkBitMap是ConcurrentMark的成员变量，而ConcurrentMark全局只有一个
    // Update the task specific count data for the object.
    count_object(obj, hr, marked_bytes_array, task_card_bm);
    return true;
  }
  // 置位失败，说明其他线程已经对这个位置进行了置位，返回false
  return false;
}

// Attempts to mark the given object and, if successful, counts
// the object in the task/worker counting structures for the
// given worker id.
/**
 * 调用者是 inline void ConcurrentMark::grayRoot
 * @param obj
 * @param word_size
 * @param hr
 * @param worker_id
 * @return
 */
inline bool ConcurrentMark::par_mark_and_count(oop obj,
                                               size_t word_size,
                                               HeapRegion* hr,
                                               uint worker_id) {
  HeapWord* addr = (HeapWord*)obj;
  if (_nextMarkBitMap->parMark(addr)) { // 如果并发标记成功，并发标记是通过CAS操作来进行并发操作的
    MemRegion mr(addr, word_size);
    /**
     * 更新卡片位图的相关统计信息
     */
    count_region(mr, hr, worker_id); // 搜索 inline void ConcurrentMark::count_region
    return true;
  }
  return false;
}

/**
 * 调用者是 void CMTask::do_marking_step
 * 在这个位图对应的mr区域范围内apply对应的closure
 * mr代表了HeapRegion的一块区域，这里会将mr的左右边界通过方法 heapWordToOffset
 *   映射为对应的bitmap的左右边界，然后在这个bitmap的左右边界上apply对应的closure
 * 当所有的位置都do_bit成功，则返回成功，有一个失败，就终止并返回失败
 * @param cl
 * @param mr
 * @return
 */
inline bool CMBitMapRO::iterate(BitMapClosure* cl, MemRegion mr) {
  HeapWord* start_addr = MAX2(startWord(), mr.start());
  HeapWord* end_addr = MIN2(endWord(), mr.end());

  if (end_addr > start_addr) {
    // Right-open interval [start-offset, end-offset).
    BitMap::idx_t start_offset = heapWordToOffset(start_addr); // 将内存地址转换成 位图 偏移量
    BitMap::idx_t end_offset = heapWordToOffset(end_addr);// 将内存地址转换成 位图 偏移量

    start_offset = _bm.get_next_one_offset(start_offset, end_offset);
    while (start_offset < end_offset) {
        /**
         * 这里调用 cl->do_bit, 说明 start_offset的这个位置已经标记过了
         */
      if (!cl->do_bit(start_offset)) { //
        return false;
      }
      HeapWord* next_addr = MIN2(nextObject(offsetToHeapWord(start_offset)), end_addr); // 下一个对象的地址
      BitMap::idx_t next_offset = heapWordToOffset(next_addr);
      start_offset = _bm.get_next_one_offset(next_offset, end_offset); // 更新start_offset，不断往前进
    }
  }
  return true;
}

inline bool CMBitMapRO::iterate(BitMapClosure* cl) {
  MemRegion mr(startWord(), sizeInWords());
  return iterate(cl, mr);
}

#define check_mark(addr)                                                       \
  assert(_bmStartWord <= (addr) && (addr) < (_bmStartWord + _bmWordSize),      \
         "outside underlying space?");                                         \
  assert(G1CollectedHeap::heap()->is_in_exact(addr),                           \
         err_msg("Trying to access not available bitmap " PTR_FORMAT           \
                 " corresponding to " PTR_FORMAT " (%u)",                      \
                 p2i(this), p2i(addr), G1CollectedHeap::heap()->addr_to_region(addr)));

inline void CMBitMap::mark(HeapWord* addr) {
  check_mark(addr);
  _bm.set_bit(heapWordToOffset(addr));
}

inline void CMBitMap::clear(HeapWord* addr) {
  check_mark(addr);
  _bm.clear_bit(heapWordToOffset(addr));
}

inline bool CMBitMap::parMark(HeapWord* addr) {
  check_mark(addr);
  /**
   * 搜索 size_t heapWordToOffset(const HeapWord* addr)
   * _bm 定义在  class CMBitMapRO VALUE_OBJ_CLASS_SPEC中， CMBitMapRO是CMBitMap 的父类
   * 搜索 BitMap::par_set_bit(idx_t bit)查看 par_set_bit
   * 可以看到，当有其他
   */
  return _bm.par_set_bit(heapWordToOffset(addr));
}

inline bool CMBitMap::parClear(HeapWord* addr) {
  check_mark(addr);
  return _bm.par_clear_bit(heapWordToOffset(addr));
}

#undef check_mark

inline void CMTask::push(oop obj) {
  HeapWord* objAddr = (HeapWord*) obj;
  assert(_g1h->is_in_g1_reserved(objAddr), "invariant");
  assert(!_g1h->is_on_master_free_list(
              _g1h->heap_region_containing((HeapWord*) objAddr)), "invariant");
  assert(!_g1h->is_obj_ill(obj), "invariant");
  assert(_nextMarkBitMap->isMarked(objAddr), "invariant");

  if (_cm->verbose_high()) {
    gclog_or_tty->print_cr("[%u] pushing " PTR_FORMAT, _worker_id, p2i((void*) obj));
  }

  if (!_task_queue->push(obj)) {
    // The local task queue looks full. We need to push some entries
    // to the global stack.

    if (_cm->verbose_medium()) {
      gclog_or_tty->print_cr("[%u] task queue overflow, "
                             "moving entries to the global stack",
                             _worker_id);
    }
    move_entries_to_global_stack();

    // this should succeed since, even if we overflow the global
    // stack, we should have definitely removed some entries from the
    // local queue. So, there must be space on it.
    bool success = _task_queue->push(obj);
    assert(success, "invariant");
  }

  statsOnly( int tmp_size = _task_queue->size();
             if (tmp_size > _local_max_size) {
               _local_max_size = tmp_size;
             }
             ++_local_pushes );
}
/**
 * 测试 obj 是否已被标记位图扫描给超过了，即已经被全局_finger 超过了，即是否已经在全局_finger的下面了，在这种已经被超过的情况下，需要将其推送到标记堆栈上。
 */
inline bool CMTask::is_below_finger(oop obj, HeapWord* global_finger) const {
  // If obj is above the global finger, then the mark bitmap scan
  // will find it later, and no push is needed.  Similarly, if we have
  // a current region and obj is between the local finger and the
  // end of the current region, then no push is needed.  The tradeoff
  // of checking both vs only checking the global finger is that the
  // local check will be more accurate and so result in fewer pushes,
  // but may also be a little slower.
  /**
   * 如果obj在全局_finger的上面，那么稍后mark bitmap扫描就会找到它，不需要push。
   * 类似地，如果我们有一个当前区域并且 obj 位于本地_finger和当前区域的end之间，则不需要push，这个是本地检查。
   * 检查两者与仅检查全局_finger的权衡在于，本地检查会更准确，因此会导致更少的push，但也可能会慢一些。
   */
  HeapWord* objAddr = (HeapWord*)obj;
  if (_finger != NULL) {
    // We have a current region.

    // Finger and region values are all NULL or all non-NULL.  We
    // use _finger to check since we immediately use its value.
    assert(_curr_region != NULL, "invariant");
    assert(_region_limit != NULL, "invariant");
    assert(_region_limit <= global_finger, "invariant");

    // True if obj is less than the local finger, or is between
    // the region limit and the global finger.
    if (objAddr < _finger) { // 如果当前的objAddr在局部_finger的下面，返回true
      return true;
    } else if (objAddr < _region_limit) { // objAddr位于region_limit的下面，返回false
      return false;
    } // Else check global finger.
  }
  // Check global finger.
  return objAddr < global_finger; // 如果objAddr位于region_limit和global_finger之间，返回true
}

/**
 * 对对象进行标记和计数，这是CMTask对象的实例方法
 * 搜索 class CMSATBBufferClosure: public SATBBufferClosure 以及
 *     inline void CMTask::deal_with_reference中 查看调用
 * @param obj
 * @param hr
 */
inline void CMTask::make_reference_grey(oop obj, HeapRegion* hr) {
    /**
     * 返回true，说明成功标记了该对象，返回false，说明这个对象已经被其他对象标记
     * 对对象的标记与全局_finger 无关
     * 这里par_mark_and_count方法是全局的_cm的成员方法，而不是当前的CMTask的成员方法
     */

  if (_cm->par_mark_and_count(obj, hr, _marked_bytes_array, _card_bm)) {
    //如果成功标记了对象
    if (_cm->verbose_high()) {
      gclog_or_tty->print_cr("[%u] marked object " PTR_FORMAT,
                             _worker_id, p2i(obj));
    }

    // No OrderAccess:store_load() is needed. It is implicit in the
    // CAS done in CMBitMap::parMark() call in the routine above.
    HeapWord* global_finger = _cm->finger(); // 读取全局的finger

    // We only need to push a newly grey object on the mark
    // stack if it is in a section of memory the mark bitmap
    // scan has already examined.  Mark bitmap scanning
    // maintains progress "fingers" for determining that.
    //
    // Notice that the global finger might be moving forward
    // concurrently. This is not a problem. In the worst case, we
    // mark the object while it is above the global finger and, by
    // the time we read the global finger, it has moved forward
    // past this object. In this case, the object will probably
    // be visited when a task is scanning the region and will also
    // be pushed on the stack. So, some duplicate work, but no
    // correctness problems.
    /**
     *  查看 ConcurrentMark::claim_region方法查看全局_finger的移动过程
     *  这里的意思是，只有位于全局_finger的下面，或者位于局部的region_limit和全局_finger之间的对象才需要push到标记栈，而位于全局_finger后面的对象，早晚会被扫到，没必要push
     *  我们只需将 那些标记位图扫描刚刚检查的区域(处于_finger前面的位置)上的灰色对象 推送到标记堆栈上。 标记位图扫描操作，维护了一个进度 _finger 以确定这个位置
     *  请注意，全局_finger可能同时向前移动，但是这不是问题。
     *  在最坏的情况下，我们在对象位于全局_finger上方时刚好对其进行标记，并且当我们读取全局_finger时，这个全局_finger已经向前移过该对象。
     *      在这种情况下，当任务扫描该区域时，该对象可能会被访问，并且也会被推送到堆栈上。 因此，有些重复的工作，但没有正确性问题。
     */
    if (is_below_finger(obj, global_finger)) { // 只有位于全局_finger以下的灰色对象才会被放入到stack中
      if (obj->is_typeArray()) { //如果对象是array
        // Immediately process arrays of primitive types, rather
        // than pushing on the mark stack.  This keeps us from
        // adding humongous objects to the mark stack that might
        // be reclaimed before the entry is processed - see
        // selection of candidates for eager reclaim of humongous
        // objects.  The cost of the additional type test is
        // mitigated by avoiding a trip through the mark stack,
        // by only doing a bookkeeping update and avoiding the
        // actual scan of the object - a typeArray contains no
        // references, and the metadata is built-in.
        /**
         * 搜索 查看具体实现 inline void CMTask::process_grey_object
         * 这里scan=false，因此不会在obj上去apply G1CMOopClosure，这是因为对象是基本对象primitive，不需要递归扫描了
         */
        process_grey_object<false>(obj); //
      } else { // 如果对象不是array
        if (_cm->verbose_high()) {
          gclog_or_tty->print_cr("[%u] below a finger (local: " PTR_FORMAT
                                 ", global: " PTR_FORMAT ") pushing "
                                 PTR_FORMAT " on mark stack",
                                 _worker_id, p2i(_finger),
                                 p2i(global_finger), p2i(obj));
        }
        push(obj); // 将对象推入当前的 CMTask的本地标记栈 _task_queue，因此将会得到递归处理
      }
    }
  }
}

/**
 * 在inline void G1CMOopClosure::do_oop_nv中被调用
 * @param obj
 */
inline void CMTask::deal_with_reference(oop obj) {
  if (_cm->verbose_high()) {
    gclog_or_tty->print_cr("[%u] we're dealing with reference = " PTR_FORMAT,
                           _worker_id, p2i((void*) obj));
  }

  increment_refs_reached();

  HeapWord* objAddr = (HeapWord*) obj;
  assert(obj->is_oop_or_null(true /* ignore mark word */), "Error");
  if (_g1h->is_in_g1_reserved(objAddr)) {
    assert(obj != NULL, "null check is implicit");
    if (!_nextMarkBitMap->isMarked(objAddr)) {
      // Only get the containing region if the object is not marked on the
      // bitmap (otherwise, it's a waste of time since we won't do
      // anything with it).
      HeapRegion* hr = _g1h->heap_region_containing_raw(obj);
      if (!hr->obj_allocated_since_next_marking(obj)) { // 这个object不可以是TAMS以后新分配的对象
        make_reference_grey(obj, hr);
      }
    }
  }
}

inline void ConcurrentMark::markPrev(oop p) {
  assert(!_prevMarkBitMap->isMarked((HeapWord*) p), "sanity");
  // Note we are overriding the read-only view of the prev map here, via
  // the cast.
  ((CMBitMap*)_prevMarkBitMap)->mark((HeapWord*) p);
}

/**
 * 标记根对象
 * @param obj 对象的地址
 * @param word_size 对象的大小
 * @param worker_id 当前线程的id
 * @param hr 对象所在的HeapRegion，如果为null，也可以从对象地址中进行推断
 */
inline void ConcurrentMark::grayRoot(oop obj, size_t word_size,
                                     uint worker_id, HeapRegion* hr) {
  assert(obj != NULL, "pre-condition");
  HeapWord* addr = (HeapWord*) obj;
  if (hr == NULL) {
    hr = _g1h->heap_region_containing_raw(addr); // 完全可以根据对象地址查找出对应的HeapRegion
  } else {
    assert(hr->is_in(addr), "pre-condition");
  }
  assert(hr != NULL, "sanity");
  // Given that we're looking for a region that contains an object
  // header it's impossible to get back a HC region.
  assert(!hr->continuesHumongous(), "sanity");

  // We cannot assert that word_size == obj->size() given that obj
  // might not be in a consistent state (another thread might be in
  // the process of copying it). So the best thing we can do is to
  // assert that word_size is under an upper bound which is its
  // containing region's capacity.
  assert(word_size * HeapWordSize <= hr->capacity(),
         err_msg("size: " SIZE_FORMAT " capacity: " SIZE_FORMAT " " HR_FORMAT,
                 word_size * HeapWordSize, hr->capacity(),
                 HR_FORMAT_PARAMS(hr)));
  // 只有位于TAMS以前的对象才有标记的必要，位于TAMS后面的对象在本轮标记中默认会被认为是可达对象，不会被回收
  if (addr < hr->next_top_at_mark_start()) { // 如果对象的地址位于TAMS前面
    if (!_nextMarkBitMap->isMarked(addr)) { // 如果对象还没有被标记
      par_mark_and_count(obj, word_size, hr, worker_id); // 标记对象
    }
  }
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTMARK_INLINE_HPP
