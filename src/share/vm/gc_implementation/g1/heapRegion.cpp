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

#include "precompiled.hpp"
#include "code/nmethod.hpp"
#include "gc_implementation/g1/g1BlockOffsetTable.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/heapRegion.inline.hpp"
#include "gc_implementation/g1/heapRegionBounds.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "gc_implementation/g1/heapRegionManager.inline.hpp"
#include "gc_implementation/shared/liveRange.hpp"
#include "memory/genOopClosures.inline.hpp"
#include "memory/iterator.hpp"
#include "memory/space.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/orderAccess.inline.hpp"

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC

int    HeapRegion::LogOfHRGrainBytes = 0;
int    HeapRegion::LogOfHRGrainWords = 0;
size_t HeapRegion::GrainBytes        = 0;
size_t HeapRegion::GrainWords        = 0;
size_t HeapRegion::CardsPerRegion    = 0;

/**
 * 搜搜 HeapRegionDCTOC cl
 * 在 方法 ScanRSClosure::scanCard 中构造
 * @param g1
 * @param hr
 * @param cl
 * @param precision
 */
HeapRegionDCTOC::HeapRegionDCTOC(G1CollectedHeap* g1,
                                 HeapRegion* hr,
                                 G1ParPushHeapRSClosure* cl,
                                 CardTableModRefBS::PrecisionStyle precision) :
  DirtyCardToOopClosure(hr, cl, precision, NULL),
  _hr(hr), _rs_scan(cl), _g1(g1) { }

FilterOutOfRegionClosure::FilterOutOfRegionClosure(HeapRegion* r,
                                                   OopClosure* oc) :
  _r_bottom(r->bottom()), _r_end(r->end()), _oc(oc) { }

  /**
   * class HeapRegionDCTOC : public DirtyCardToOopClosure
   * 这个方法根据上一轮并发标记的结果，在遍历对象的时候进行了过滤
   * @param mr
   * @param bottom
   * @param top
   */
void HeapRegionDCTOC::walk_mem_region(MemRegion mr,
                                      HeapWord* bottom,
                                      HeapWord* top) {
  G1CollectedHeap* g1h = _g1;
  size_t oop_size;
  HeapWord* cur = bottom;

  // Start filtering what we add to the remembered set. If the object is
  // not considered dead, either because it is marked (in the mark bitmap)
  // or it was allocated after marking finished, then we add it. Otherwise
  // we can safely ignore the object.
  /**
   * 开始过滤我们添加到RSet中的内容。
   * 如果该对象不被视为死亡，无论是
   *    因为它已被标记（在标记位图中）
   *      还是
   *    因为在标记完成后被分配，
   * 如果对象未被标记为死亡，则调用 oop_iterate() 方法使用 G1ParPushHeapRSClosure 遍历对象，并将其添加到 RSet 中；否则，直接跳过该对象。
   */
  if (!g1h->is_obj_dead(oop(cur), _hr)) {
      /**
       * typedef class oopDesc*                            oop;
       * 搜索 inline int oopDesc::oop_iterate
       */
    oop_size = oop(cur)->oop_iterate(_rs_scan, mr);
  } else {
    oop_size = _hr->block_size(cur);
  }

  cur += oop_size; // 更新当前指针
  // 检查是否还有更多的对象需要遍历
  if (cur < top) {
    oop cur_oop = oop(cur);// 将当前地址转换为对象指针。
    oop_size = _hr->block_size(cur);// 获取当前对象的大小
    HeapWord* next_obj = cur + oop_size; // 下一个对象的位置
    while (next_obj < top) { // 水平遍历这个MemRegion中的所有对象，但是在处理每一个对象的时候，又是递归的
      // Keep filtering the remembered set.
      if (!g1h->is_obj_dead(cur_oop, _hr)) {
        // Bottom lies entirely below top, so we can call the
        // non-memRegion version of oop_iterate below.
        /**
         * typedef class oopDesc*                            oop;
         * 搜索 inline int oopDesc::oop_iterate，会在这个对象的每一个field上apply对应的_rs_scan
         * 由于oop_iterate是一个递归的过程，所以这里看起来是一个递归遍历
         * _rs_scan 是 G1ParPushHeapRSClosure
         */
        cur_oop->oop_iterate(_rs_scan); // 循环遍历剩余的对象
      }
      cur = next_obj;
      cur_oop = oop(cur);
      oop_size = _hr->block_size(cur);
      next_obj = cur + oop_size;
    }

    // Last object. Need to do dead-obj filtering here too.
    if (!g1h->is_obj_dead(oop(cur), _hr)) {
      oop(cur)->oop_iterate(_rs_scan, mr);
    }
  }
}

size_t HeapRegion::max_region_size() {
  return HeapRegionBounds::max_size();
}

void HeapRegion::setup_heap_region_size(size_t initial_heap_size, size_t max_heap_size) {
  uintx region_size = G1HeapRegionSize;
  if (FLAG_IS_DEFAULT(G1HeapRegionSize)) {
    size_t average_heap_size = (initial_heap_size + max_heap_size) / 2;
    region_size = MAX2(average_heap_size / HeapRegionBounds::target_number(),
                       (uintx) HeapRegionBounds::min_size());
  }

  int region_size_log = log2_long((jlong) region_size);
  // Recalculate the region size to make sure it's a power of
  // 2. This means that region_size is the largest power of 2 that's
  // <= what we've calculated so far.
  region_size = ((uintx)1 << region_size_log);

  // Now make sure that we don't go over or under our limits.
  if (region_size < HeapRegionBounds::min_size()) {
    region_size = HeapRegionBounds::min_size();
  } else if (region_size > HeapRegionBounds::max_size()) {
    region_size = HeapRegionBounds::max_size();
  }

  // And recalculate the log.
  region_size_log = log2_long((jlong) region_size);

  // Now, set up the globals.
  guarantee(LogOfHRGrainBytes == 0, "we should only set it once");
  LogOfHRGrainBytes = region_size_log;

  guarantee(LogOfHRGrainWords == 0, "we should only set it once");
  LogOfHRGrainWords = LogOfHRGrainBytes - LogHeapWordSize;

  guarantee(GrainBytes == 0, "we should only set it once");
  // The cast to int is safe, given that we've bounded region_size by
  // MIN_REGION_SIZE and MAX_REGION_SIZE.
  GrainBytes = (size_t)region_size; // 设置一个region的大小,字节为单位

  guarantee(GrainWords == 0, "we should only set it once");
  GrainWords = GrainBytes >> LogHeapWordSize; // 一个region的大小，字为单位
  guarantee((size_t) 1 << LogOfHRGrainWords == GrainWords, "sanity");

  guarantee(CardsPerRegion == 0, "we should only set it once");
  CardsPerRegion = GrainBytes >> CardTableModRefBS::card_shift; // 一个Region中的卡片数量，是用Region的大小右移9位得到，这说明一个卡片对应了512B的内存区域
}

void HeapRegion::reset_after_compaction() {
  G1OffsetTableContigSpace::reset_after_compaction();
  // After a compaction the mark bitmap is invalid, so we must
  // treat all objects as being inside the unmarked area.
  zero_marked_bytes();
  init_top_at_mark_start();
}

void HeapRegion::hr_clear(bool par, bool clear_space, bool locked) {
  assert(_humongous_start_region == NULL,
         "we should have already filtered out humongous regions");
  assert(_end == _orig_end,
         "we should have already filtered out humongous regions");

  _in_collection_set = false;

  set_allocation_context(AllocationContext::system());
  set_young_index_in_cset(-1);
  uninstall_surv_rate_group();
  set_free();
  reset_pre_dummy_top();

  if (!par) {
    // If this is parallel, this will be done later.
    HeapRegionRemSet* hrrs = rem_set();
    if (locked) {
      hrrs->clear_locked();
    } else {
      hrrs->clear();
    }
    _claimed = InitialClaimValue;
  }
  zero_marked_bytes();

  _offsets.resize(HeapRegion::GrainWords);
  init_top_at_mark_start();
  if (clear_space) clear(SpaceDecorator::Mangle);
}

void HeapRegion::par_clear() {
  assert(used() == 0, "the region should have been already cleared");
  assert(capacity() == HeapRegion::GrainBytes, "should be back to normal");
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->clear();
  CardTableModRefBS* ct_bs =
                   (CardTableModRefBS*)G1CollectedHeap::heap()->barrier_set();
  ct_bs->clear(MemRegion(bottom(), end()));
}

void HeapRegion::calc_gc_efficiency() {
  // GC efficiency is the ratio of how much space would be
  // reclaimed over how long we predict it would take to reclaim it.
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1CollectorPolicy* g1p = g1h->g1_policy();

  // Retrieve a prediction of the elapsed time for this region for
  // a mixed gc because the region will only be evacuated during a
  // mixed gc.
  double region_elapsed_time_ms =
    g1p->predict_region_elapsed_time_ms(this, false /* for_young_gc */);
  _gc_efficiency = (double) reclaimable_bytes() / region_elapsed_time_ms;
}

void HeapRegion::set_startsHumongous(HeapWord* new_top, HeapWord* new_end) {
  assert(!isHumongous(), "sanity / pre-condition");
  assert(end() == _orig_end,
         "Should be normal before the humongous object allocation");
  assert(top() == bottom(), "should be empty");
  assert(bottom() <= new_top && new_top <= new_end, "pre-condition");

  _type.set_starts_humongous();
  _humongous_start_region = this;

  set_end(new_end);
  _offsets.set_for_starts_humongous(new_top);
}

void HeapRegion::set_continuesHumongous(HeapRegion* first_hr) {
  assert(!isHumongous(), "sanity / pre-condition");
  assert(end() == _orig_end,
         "Should be normal before the humongous object allocation");
  assert(top() == bottom(), "should be empty");
  assert(first_hr->startsHumongous(), "pre-condition");

  _type.set_continues_humongous();
  _humongous_start_region = first_hr;
}

void HeapRegion::clear_humongous() {
  assert(isHumongous(), "pre-condition");

  if (startsHumongous()) {
    assert(top() <= end(), "pre-condition");
    set_end(_orig_end);
    if (top() > end()) {
      // at least one "continues humongous" region after it
      set_top(end());
    }
  } else {
    // continues humongous
    assert(end() == _orig_end, "sanity");
  }

  assert(capacity() == HeapRegion::GrainBytes, "pre-condition");
  _humongous_start_region = NULL;
}

bool HeapRegion::claimHeapRegion(jint claimValue) {
  jint current = _claimed;
  if (current != claimValue) {
    jint res = Atomic::cmpxchg(claimValue, &_claimed, current);
    if (res == current) {
      return true;
    }
  }
  return false;
}

HeapRegion::HeapRegion(uint hrm_index,
                       G1BlockOffsetSharedArray* sharedOffsetArray,
                       MemRegion mr) :
    G1OffsetTableContigSpace(sharedOffsetArray, mr), // HeapRegion是G1OffsetTableContigSpace的子类
    _hrm_index(hrm_index),
    _allocation_context(AllocationContext::system()),
    _humongous_start_region(NULL),
    _in_collection_set(false),
    _next_in_special_set(NULL), _orig_end(NULL),
    _claimed(InitialClaimValue), _evacuation_failed(false),
    _prev_marked_bytes(0), _next_marked_bytes(0), _gc_efficiency(0.0),
    _next_young_region(NULL),
    _next_dirty_cards_region(NULL), _next(NULL), _prev(NULL),
#ifdef ASSERT
    _containing_set(NULL),
#endif // ASSERT
     _young_index_in_cset(-1), _surv_rate_group(NULL), _age_index(-1),
    _rem_set(NULL), _recorded_rs_length(0), _predicted_elapsed_time_ms(0),
    _predicted_bytes_to_copy(0)
{
  _rem_set = new HeapRegionRemSet(sharedOffsetArray, this);
  assert(HeapRegionRemSet::num_par_rem_sets() > 0, "Invariant.");

  initialize(mr);
}

/**
 * 调用者 HeapRegionManager::make_regions_available
 * @param mr
 * @param clear_space
 * @param mangle_space
 */
void HeapRegion::initialize(MemRegion mr, bool clear_space, bool mangle_space) {
  assert(_rem_set->is_empty(), "Remembered set must be empty");

  G1OffsetTableContigSpace::initialize(mr, clear_space, mangle_space);

  _orig_end = mr.end();
  hr_clear(false /*par*/, false /*clear_space*/);
  set_top(bottom()); // 刚初始化的时候，top就是bottom，从最低地址开始写入
  record_timestamp(); // 记录这个Region的timestamp为GC的timestamp(如果当前的确是在GC Pause中)
}

CompactibleSpace* HeapRegion::next_compaction_space() const {
  return G1CollectedHeap::heap()->next_compaction_region(this);
}

void HeapRegion::note_self_forwarding_removal_start(bool during_initial_mark,
                                                    bool during_conc_mark) {
  // We always recreate the prev marking info and we'll explicitly
  // mark all objects we find to be self-forwarded on the prev
  // bitmap. So all objects need to be below PTAMS.
  _prev_marked_bytes = 0;

  if (during_initial_mark) {
    // During initial-mark, we'll also explicitly mark all objects
    // we find to be self-forwarded on the next bitmap. So all
    // objects need to be below NTAMS.
    _next_top_at_mark_start = top();
    _next_marked_bytes = 0;
  } else if (during_conc_mark) {
    // During concurrent mark, all objects in the CSet (including
    // the ones we find to be self-forwarded) are implicitly live.
    // So all objects need to be above NTAMS.
    _next_top_at_mark_start = bottom();
    _next_marked_bytes = 0;
  }
}

void HeapRegion::note_self_forwarding_removal_end(bool during_initial_mark,
                                                  bool during_conc_mark,
                                                  size_t marked_bytes) {
  assert(0 <= marked_bytes && marked_bytes <= used(),
         err_msg("marked: " SIZE_FORMAT " used: " SIZE_FORMAT,
                 marked_bytes, used()));
  _prev_top_at_mark_start = top();
  _prev_marked_bytes = marked_bytes;
}

HeapWord*
HeapRegion::object_iterate_mem_careful(MemRegion mr,
                                                 ObjectClosure* cl) {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  // We used to use "block_start_careful" here.  But we're actually happy
  // to update the BOT while we do this...
  HeapWord* cur = block_start(mr.start());
  mr = mr.intersection(used_region());
  if (mr.is_empty()) return NULL;
  // Otherwise, find the obj that extends onto mr.start().

  assert(cur <= mr.start()
         && (oop(cur)->klass_or_null() == NULL ||
             cur + oop(cur)->size() > mr.start()),
         "postcondition of block_start");
  oop obj;
  while (cur < mr.end()) {
    obj = oop(cur);
    if (obj->klass_or_null() == NULL) {
      // Ran into an unparseable point.
      return cur;
    } else if (!g1h->is_obj_dead(obj)) {
      cl->do_object(obj);
    }
    if (cl->abort()) return cur;
    // The check above must occur before the operation below, since an
    // abort might invalidate the "size" operation.
    cur += block_size(cur);
  }
  return NULL;
}

/**
 * 在 G1RemSet::refine_card 中被调用，返回一个地址HeapWord* stop_point
 * @param mr
 * @param cl
 * @param filter_young
 * @param card_ptr
 * @return
 */
HeapWord* HeapRegion::oops_on_card_seq_iterate_careful(MemRegion mr,
                                 FilterOutOfRegionClosure* cl, //  FilterOutOfRegionClosure
                                 bool filter_young,
                                 jbyte* card_ptr) {
  // Currently, we should only have to clean the card if filter_young
  // is true and vice versa.
  // 当 filter_young = true，card_ptr必须不为空
  if (filter_young) {
    assert(card_ptr != NULL, "pre-condition");
  } else {
      // 当 filter_young = false，card_ptr必须为空
    assert(card_ptr == NULL, "pre-condition");
  }
  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  // If we're within a stop-world GC, then we might look at a card in a
  // GC alloc region that extends onto a GC LAB, which may not be
  // parseable.  Stop such at the "scan_top" of the region.
  if (g1h->is_gc_active()) {
    mr = mr.intersection(MemRegion(bottom(), scan_top())); //  将传入的HeapRegion同 [bottom(), scan_top()]进行交集操作
  } else {
    mr = mr.intersection(used_region());
  }
  if (mr.is_empty()) return NULL; // 没有交叉区域，比如，当前的HeapRegion就是当前正在进行的GC的GC Alloc Region，那么是不接受扫描的
  // Otherwise, find the obj that extends onto mr.start().

  // The intersection of the incoming mr (for the card) and the
  // allocated part of the region is non-empty. This implies that
  // we have actually allocated into this region. The code in
  // G1CollectedHeap.cpp that allocates a new region sets the
  // is_young tag on the region before allocating. Thus we
  // safely know if this region is young.
  if (is_young() && filter_young) {
    return NULL;
  }

  assert(!is_young(), "check value of filter_young");

  // We can only clean the card here, after we make the decision that
  // the card is not young. And we only clean the card if we have been
  // asked to (i.e., card_ptr != NULL).
  if (card_ptr != NULL) {
    *card_ptr = CardTableModRefBS::clean_card_val(); // 把卡片的值变成一张干净的卡片
    // We must complete this write before we do any of the reads below.
    OrderAccess::storeload();
  }

  // Cache the boundaries of the memory region in some const locals
  HeapWord* const start = mr.start(); // MemRegion的起始位置,以 字(word)为单位
  HeapWord* const end = mr.end(); // MemRegion的终止位置，以 字(word)为单位

  // We used to use "block_start_careful" here.  But we're actually happy
  // to update the BOT while we do this...
  /**
   * 调用父类的 G1OffsetTableContigSpace::block_start
   * 根据指定的地址，寻找包含了这个start字地址的block的首地址
   * 最终调用的是 G1BlockOffsetArray::block_at_or_preceding，这里就用到了块偏移数组的数据进行快速的查找
   */
  HeapWord* cur = block_start(start);
  assert(cur <= start, "Postcondition");

  oop obj;

  /**
   * 当前，我们已经有了对应的MemRegion（简称mr）的首尾地址，下面两个循环，就是去搜索和遍历落到这个区间的所有的obj
   *    第一个循环做的事情就是找到一个cur地址，使得addr刚好位于 [cur, cur + block_size(cur)]区间
   *    第二个循环从这个cur开始遍历，逐个对象去apply对应的closure
   */
  HeapWord* next = cur;
  /**
   * 从包含这个start地址的block地址（这个block地址肯定小于start）开始往后搜索，每次往后挪动一个block_size(cur)【查看block_size的定义】
   * ，直到 cur + block_size(cur) 已经超过了start
   */
  do {
    cur = next;
    obj = oop(cur);
    if (obj->klass_or_null() == NULL) {
      // Ran into an unparseable point.
      return cur;
    }
    // Otherwise...
    next = cur + block_size(cur); // 一个对象一个对象地往后移动，一直
  } while (next <= start); // 一直从start对应的block_start的位置遍历到start的位置

  // If we finish the above loop...We have a parseable object that
  // begins on or before the start of the memory region, and ends
  // inside or spans the entire region.
  assert(cur <= start, "Loop postcondition");
  assert(obj->klass_or_null() != NULL, "Loop postcondition");
  /**
   * 执行到这里，cur + block_size(cur) 一定是 > start的值，即我们找到了这样一个[cur, cur + block_size(cur)]区间，使得addr处于这个区间内
   * 然后接着往下开始遍历，一直遍历到  cur >= end，或者遇到一个对象
   * 对中间的每一个地址，在上面apply对应的closure
   */

  do {
    obj = oop(cur); // 将HeapWord* 转换成 oopDesc *，即转换成一个对象指针
    assert((cur + block_size(cur)) > (HeapWord*)obj, "Loop invariant");
    // 搜索 Klass* oopDesc::klass_or_null()
    if (obj->klass_or_null() == NULL) { // 如果是KLass，则返回对应的KLass*，否则返回null
      // 遇到一个无法解析的指针，不是对象指针，退出，因为显然，遇到这样的对象是无法直到它的block size，因此无法往后继续的
      return cur; // 返回cur 作为stop_at
    }

    // Advance the current pointer. "obj" still points to the object to iterate.
    cur = cur + block_size(cur);

    if (!g1h->is_obj_dead(obj)) {
      // Non-objArrays are sometimes marked imprecise at the object start. We
      // always need to iterate over them in full.
      // We only iterate over object arrays in full if they are completely contained
      // in the memory region.
      // 如果这个对象不是一个array，或者，尽管是一个array，但是这个array是在start和end之间，那么对这个obj apply FilterOutOfRegionClosure
      if (!obj->is_objArray() || (((HeapWord*)obj) >= start && cur <= end)) {
        obj->oop_iterate(cl); // 搜索 inline int oopDesc::oop_iterate, 实际上会 apply FilterOutOfRegionClosure::do_oop_nv
      } else {
        obj->oop_iterate(cl, mr); // 如果这个对象是obj Array，并且 (obj < start 或者  cur > end)
      }
    }
  } while (cur < end); // 一直遍历到 cur >= end

  return NULL; // 返回空地址，代表已经完成了
}

// Code roots support

void HeapRegion::add_strong_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_strong_code_root(nm);
}

void HeapRegion::add_strong_code_root_locked(nmethod* nm) {
  assert_locked_or_safepoint(CodeCache_lock);
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_strong_code_root_locked(nm);
}

void HeapRegion::remove_strong_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->remove_strong_code_root(nm);
}

void HeapRegion::strong_code_roots_do(CodeBlobClosure* blk) const {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->strong_code_roots_do(blk);
}

class VerifyStrongCodeRootOopClosure: public OopClosure {
  const HeapRegion* _hr;
  nmethod* _nm;
  bool _failures;
  bool _has_oops_in_region;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);

      // Note: not all the oops embedded in the nmethod are in the
      // current region. We only look at those which are.
      if (_hr->is_in(obj)) {
        // Object is in the region. Check that its less than top
        if (_hr->top() <= (HeapWord*)obj) {
          // Object is above top
          gclog_or_tty->print_cr("Object " PTR_FORMAT " in region "
                                 "[" PTR_FORMAT ", " PTR_FORMAT ") is above "
                                 "top " PTR_FORMAT,
                                 (void *)obj, _hr->bottom(), _hr->end(), _hr->top());
          _failures = true;
          return;
        }
        // Nmethod has at least one oop in the current region
        _has_oops_in_region = true;
      }
    }
  }

public:
  VerifyStrongCodeRootOopClosure(const HeapRegion* hr, nmethod* nm):
    _hr(hr), _failures(false), _has_oops_in_region(false) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }

  bool failures()           { return _failures; }
  bool has_oops_in_region() { return _has_oops_in_region; }
};

class VerifyStrongCodeRootCodeBlobClosure: public CodeBlobClosure {
  const HeapRegion* _hr;
  bool _failures;
public:
  VerifyStrongCodeRootCodeBlobClosure(const HeapRegion* hr) :
    _hr(hr), _failures(false) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = (cb == NULL) ? NULL : cb->as_nmethod_or_null();
    if (nm != NULL) {
      // Verify that the nemthod is live
      if (!nm->is_alive()) {
        gclog_or_tty->print_cr("region [" PTR_FORMAT "," PTR_FORMAT "] has dead nmethod "
                               PTR_FORMAT " in its strong code roots",
                               _hr->bottom(), _hr->end(), nm);
        _failures = true;
      } else {
        VerifyStrongCodeRootOopClosure oop_cl(_hr, nm);
        nm->oops_do(&oop_cl);
        if (!oop_cl.has_oops_in_region()) {
          gclog_or_tty->print_cr("region [" PTR_FORMAT "," PTR_FORMAT "] has nmethod "
                                 PTR_FORMAT " in its strong code roots "
                                 "with no pointers into region",
                                 _hr->bottom(), _hr->end(), nm);
          _failures = true;
        } else if (oop_cl.failures()) {
          gclog_or_tty->print_cr("region [" PTR_FORMAT "," PTR_FORMAT "] has other "
                                 "failures for nmethod " PTR_FORMAT,
                                 _hr->bottom(), _hr->end(), nm);
          _failures = true;
        }
      }
    }
  }

  bool failures()       { return _failures; }
};

void HeapRegion::verify_strong_code_roots(VerifyOption vo, bool* failures) const {
  if (!G1VerifyHeapRegionCodeRoots) {
    // We're not verifying code roots.
    return;
  }
  if (vo == VerifyOption_G1UseMarkWord) {
    // Marking verification during a full GC is performed after class
    // unloading, code cache unloading, etc so the strong code roots
    // attached to each heap region are in an inconsistent state. They won't
    // be consistent until the strong code roots are rebuilt after the
    // actual GC. Skip verifying the strong code roots in this particular
    // time.
    assert(VerifyDuringGC, "only way to get here");
    return;
  }

  HeapRegionRemSet* hrrs = rem_set();
  size_t strong_code_roots_length = hrrs->strong_code_roots_list_length();

  // if this region is empty then there should be no entries
  // on its strong code root list
  if (is_empty()) {
    if (strong_code_roots_length > 0) {
      gclog_or_tty->print_cr("region [" PTR_FORMAT "," PTR_FORMAT "] is empty "
                             "but has " SIZE_FORMAT " code root entries",
                             bottom(), end(), strong_code_roots_length);
      *failures = true;
    }
    return;
  }

  if (continuesHumongous()) {
    if (strong_code_roots_length > 0) {
      gclog_or_tty->print_cr("region " HR_FORMAT " is a continuation of a humongous "
                             "region but has " SIZE_FORMAT " code root entries",
                             HR_FORMAT_PARAMS(this), strong_code_roots_length);
      *failures = true;
    }
    return;
  }

  VerifyStrongCodeRootCodeBlobClosure cb_cl(this);
  strong_code_roots_do(&cb_cl);

  if (cb_cl.failures()) {
    *failures = true;
  }
}

void HeapRegion::print() const { print_on(gclog_or_tty); }
void HeapRegion::print_on(outputStream* st) const {
  st->print("AC%4u", allocation_context());
  st->print(" %2s", get_short_type_str());
  if (in_collection_set())
    st->print(" CS");
  else
    st->print("   ");
  st->print(" TS %5d", _gc_time_stamp);
  st->print(" PTAMS " PTR_FORMAT " NTAMS " PTR_FORMAT,
            prev_top_at_mark_start(), next_top_at_mark_start());
  G1OffsetTableContigSpace::print_on(st);
}

class G1VerificationClosure : public OopClosure {
protected:
  G1CollectedHeap* _g1h;
  CardTableModRefBS* _bs;
  oop _containing_obj;
  bool _failures;
  int _n_failures;
  VerifyOption _vo;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  G1VerificationClosure(G1CollectedHeap* g1h, VerifyOption vo) :
    _g1h(g1h), _bs(NULL), _containing_obj(NULL),
    _failures(false), _n_failures(0), _vo(vo)
  {
    BarrierSet* bs = _g1h->barrier_set();
    if (bs->is_a(BarrierSet::CardTableModRef))
      _bs = (CardTableModRefBS*)bs;
  }

  void set_containing_obj(oop obj) {
    _containing_obj = obj;
  }

  bool failures() { return _failures; }
  int n_failures() { return _n_failures; }

  void print_object(outputStream* out, oop obj) {
#ifdef PRODUCT
    Klass* k = obj->klass();
    const char* class_name = InstanceKlass::cast(k)->external_name();
    out->print_cr("class name %s", class_name);
#else // PRODUCT
    obj->print_on(out);
#endif // PRODUCT
  }
};

class VerifyLiveClosure : public G1VerificationClosure {
public:
  VerifyLiveClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_liveness(p);
  }

  template <class T>
  void verify_liveness(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p); // heap_oop 是一个 oopDesc*
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      bool failed = false;
      if (!_g1h->is_in_closed_subset(obj) || _g1h->is_obj_dead_cond(obj, _vo)) {
        MutexLockerEx x(ParGCRareEvent_lock,
          Mutex::_no_safepoint_check_flag);

        if (!_failures) {
          gclog_or_tty->cr();
          gclog_or_tty->print_cr("----------");
        }
        if (!_g1h->is_in_closed_subset(obj)) {
          HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p);
          gclog_or_tty->print_cr("Field " PTR_FORMAT
                                 " of live obj " PTR_FORMAT " in region "
                                 "[" PTR_FORMAT ", " PTR_FORMAT ")",
                                 p, (void*) _containing_obj,
                                 from->bottom(), from->end());
          print_object(gclog_or_tty, _containing_obj);
          gclog_or_tty->print_cr("points to obj " PTR_FORMAT " not in the heap",
                                 (void*) obj);
        } else {
          HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p);
          HeapRegion* to   = _g1h->heap_region_containing((HeapWord*)obj);
          gclog_or_tty->print_cr("Field " PTR_FORMAT
                                 " of live obj " PTR_FORMAT " in region "
                                 "[" PTR_FORMAT ", " PTR_FORMAT ")",
                                 p, (void*) _containing_obj,
                                 from->bottom(), from->end());
          print_object(gclog_or_tty, _containing_obj);
          gclog_or_tty->print_cr("points to dead obj " PTR_FORMAT " in region "
                                 "[" PTR_FORMAT ", " PTR_FORMAT ")",
                                 (void*) obj, to->bottom(), to->end());
          print_object(gclog_or_tty, obj);
        }
        gclog_or_tty->print_cr("----------");
        gclog_or_tty->flush();
        _failures = true;
        failed = true;
        _n_failures++;
      }
    }
  }
};

class VerifyRemSetClosure : public G1VerificationClosure {
public:
  VerifyRemSetClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_remembered_set(p);
  }

  template <class T>
  //  VerifyRemSetClosure::verify_remembered_set
  void verify_remembered_set(T* p) { // 对这个oop进行验证

      // 静态方法 搜索 inline oop       oopDesc::load_heap_oop(oop* p) {return *p};
    T heap_oop = oopDesc::load_heap_oop(p); // oopDesc*
    if (!oopDesc::is_null(heap_oop)) {
        // 静态方法 inline oop oopDesc::decode_heap_oop_not_null(oop v) { return v; }
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop); // 搜索 inline oop oopDesc::decode_heap_oop_not_null
      bool failed = false;
      // *p = obj
      //   inline HeapRegion* heap_region_containing(const T addr) const;
      HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p); // 获取包含指针 p 所在内存地址的堆区域 from
      // *p
      HeapRegion* to   = _g1h->heap_region_containing(obj); // 获取包含对象 obj 所在内存地址的堆区域 to，搜索 HeapRegion* G1CollectedHeap::heap_region_containing
      if (from != NULL && to != NULL &&
          from != to && // from和to在不同的Region
          !to->isHumongous()) { // 不是指向一个巨型对象
            // const jbyte* byte_for_const(const void* p)
            jbyte cv_obj = *_bs->byte_for_const(_containing_obj);// 这个对象对应的卡表调用byte_for_const以后，返回char*，取值
            jbyte cv_field = *_bs->byte_for_const(p); // 调用byte_for_const以后，返回char*，取值
        const jbyte dirty = CardTableModRefBS::dirty_card_val(); // static int dirty_card_val()

        bool is_bad = !(from->is_young() // from来自于young region，肯定没问题
                        || to->rem_set()->contains_reference(p) // from虽然不是来自于young，但是to region的RSet含有指针p
                        // 如果并不强制在检查以前flush log buffer
                        || !G1HRRSFlushLogBuffersOnVerify && // buffers were not flushed
                            (_containing_obj->is_objArray() ?  // _containing_obj是当前其所有的field被apply VerifyRemSetClosure的那个对象
                                cv_field == dirty // 如果是数组，那么field对应的卡片必须标记为脏卡片
                             : cv_obj == dirty || cv_field == dirty));  // 如果是普通对象，那么cv_obj或者cv_field有一个是脏的
        if (is_bad) {
          MutexLockerEx x(ParGCRareEvent_lock,
                          Mutex::_no_safepoint_check_flag);

          if (!_failures) {
            gclog_or_tty->cr();
            gclog_or_tty->print_cr("----------");
          }
          gclog_or_tty->print_cr("Missing rem set entry:");
          gclog_or_tty->print_cr("Field " PTR_FORMAT " "
                                 "of obj " PTR_FORMAT ", "
                                 "in region " HR_FORMAT,
                                 p, (void*) _containing_obj,
                                 HR_FORMAT_PARAMS(from));
          _containing_obj->print_on(gclog_or_tty);
          gclog_or_tty->print_cr("points to obj " PTR_FORMAT " "
                                 "in region " HR_FORMAT,
                                 (void*) obj,
                                 HR_FORMAT_PARAMS(to));
          if (obj->is_oop()) {
            obj->print_on(gclog_or_tty);
          }
          gclog_or_tty->print_cr("Obj head CTE = %d, field CTE = %d.",
                        cv_obj, cv_field);
          gclog_or_tty->print_cr("----------");
          gclog_or_tty->flush();
          _failures = true;
          if (!failed) _n_failures++;
        }
      }
    }
  }
};

// This really ought to be commoned up into OffsetTableContigSpace somehow.
// We would need a mechanism to make that code skip dead objects.

void HeapRegion::verify(VerifyOption vo,
                        bool* failures) const {
  G1CollectedHeap* g1 = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyLiveClosure vl_cl(g1, vo);
  VerifyRemSetClosure vr_cl(g1, vo);
  bool is_humongous = isHumongous();
  bool do_bot_verify = !is_young();
  size_t object_num = 0;
  while (p < top()) {
    oop obj = oop(p);
    size_t obj_size = block_size(p);
    object_num += 1;

    if (is_humongous != g1->isHumongous(obj_size) &&
        !g1->is_obj_dead(obj, this)) { // Dead objects may have bigger block_size since they span several objects.
      gclog_or_tty->print_cr("obj " PTR_FORMAT " is of %shumongous size ("
                             SIZE_FORMAT " words) in a %shumongous region",
                             p, g1->isHumongous(obj_size) ? "" : "non-",
                             obj_size, is_humongous ? "" : "non-");
       *failures = true;
       return;
    }

    // If it returns false, verify_for_object() will output the
    // appropriate message.
    if (do_bot_verify &&
        !g1->is_obj_dead(obj, this) &&
        !_offsets.verify_for_object(p, obj_size)) {
      *failures = true;
      return;
    }

    if (!g1->is_obj_dead_cond(obj, this, vo)) {
      if (obj->is_oop()) {
        Klass* klass = obj->klass();
        bool is_metaspace_object = Metaspace::contains(klass) ||
                                   (vo == VerifyOption_G1UsePrevMarking &&
                                   ClassLoaderDataGraph::unload_list_contains(klass));
        if (!is_metaspace_object) {
          gclog_or_tty->print_cr("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                 "not metadata", klass, (void *)obj);
          *failures = true;
          return;
        } else if (!klass->is_klass()) {
          gclog_or_tty->print_cr("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                 "not a klass", klass, (void *)obj);
          *failures = true;
          return;
        } else {
          vl_cl.set_containing_obj(obj);
          if (!g1->full_collection() || G1VerifyRSetsDuringFullGC) {
            // verify liveness and rem_set
            vr_cl.set_containing_obj(obj);
            G1Mux2Closure mux(&vl_cl, &vr_cl);
            obj->oop_iterate_no_header(&mux);

            if (vr_cl.failures()) {
              *failures = true;
            }
            if (G1MaxVerifyFailures >= 0 &&
              vr_cl.n_failures() >= G1MaxVerifyFailures) {
              return;
            }
          } else {
            // verify only liveness
            obj->oop_iterate_no_header(&vl_cl);
          }
          if (vl_cl.failures()) {
            *failures = true;
          }
          if (G1MaxVerifyFailures >= 0 &&
              vl_cl.n_failures() >= G1MaxVerifyFailures) {
            return;
          }
        }
      } else {
        gclog_or_tty->print_cr(PTR_FORMAT " not an oop", (void *)obj);
        *failures = true;
        return;
      }
    }
    prev_p = p;
    p += obj_size;
  }

  if (p != top()) {
    gclog_or_tty->print_cr("end of last object " PTR_FORMAT " "
                           "does not match top " PTR_FORMAT, p, top());
    *failures = true;
    return;
  }

  HeapWord* the_end = end();
  assert(p == top(), "it should still hold");
  // Do some extra BOT consistency checking for addresses in the
  // range [top, end). BOT look-ups in this range should yield
  // top. No point in doing that if top == end (there's nothing there).
  if (p < the_end) {
    // Look up top
    HeapWord* addr_1 = p;
    HeapWord* b_start_1 = _offsets.block_start_const(addr_1);
    if (b_start_1 != p) {
      gclog_or_tty->print_cr("BOT look up for top: " PTR_FORMAT " "
                             " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                             addr_1, b_start_1, p);
      *failures = true;
      return;
    }

    // Look up top + 1
    HeapWord* addr_2 = p + 1;
    if (addr_2 < the_end) {
      HeapWord* b_start_2 = _offsets.block_start_const(addr_2);
      if (b_start_2 != p) {
        gclog_or_tty->print_cr("BOT look up for top + 1: " PTR_FORMAT " "
                               " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                               addr_2, b_start_2, p);
        *failures = true;
        return;
      }
    }

    // Look up an address between top and end
    size_t diff = pointer_delta(the_end, p) / 2;
    HeapWord* addr_3 = p + diff;
    if (addr_3 < the_end) {
      HeapWord* b_start_3 = _offsets.block_start_const(addr_3);
      if (b_start_3 != p) {
        gclog_or_tty->print_cr("BOT look up for top + diff: " PTR_FORMAT " "
                               " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                               addr_3, b_start_3, p);
        *failures = true;
        return;
      }
    }

    // Loook up end - 1
    HeapWord* addr_4 = the_end - 1;
    HeapWord* b_start_4 = _offsets.block_start_const(addr_4);
    if (b_start_4 != p) {
      gclog_or_tty->print_cr("BOT look up for end - 1: " PTR_FORMAT " "
                             " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                             addr_4, b_start_4, p);
      *failures = true;
      return;
    }
  }

  if (is_humongous && object_num > 1) {
    gclog_or_tty->print_cr("region [" PTR_FORMAT "," PTR_FORMAT "] is humongous "
                           "but has " SIZE_FORMAT ", objects",
                           bottom(), end(), object_num);
    *failures = true;
    return;
  }

  verify_strong_code_roots(vo, failures);
}

void HeapRegion::verify() const {
  bool dummy = false;
  verify(VerifyOption_G1UsePrevMarking, /* failures */ &dummy);
}

void HeapRegion::verify_rem_set(VerifyOption vo, bool* failures) const {
  G1CollectedHeap* g1 = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyRemSetClosure vr_cl(g1, vo);
  while (p < top()) {
    oop obj = oop(p); // obj 是一个 oopDesc*
    size_t obj_size = block_size(p);
    /**
     *  * 给定该对象以及该对象所属的区域，确定该对象是否已死亡。 一个对象已死亡，当且仅当
   *    a) 这个对象是在prev ntams以前分配的，
   *       **并且**
   *    b) 这个对象在上次的标记位图中没有被标记
     */
    if (!g1->is_obj_dead_cond(obj, this, vo)) { // 如果不是死亡对象
      if (obj->is_oop()) {
        vr_cl.set_containing_obj(obj);
        // 在这个对象上运行 VerifyRemSetClosure::verify_remembered_set 以验证RSet,
        // 其实是遍历这个对象所有的Filed，然后逐个Filed验证RSet
        obj->oop_iterate_no_header(&vr_cl);

        if (vr_cl.failures()) {
          *failures = true;
        }
        if (G1MaxVerifyFailures >= 0 &&
          vr_cl.n_failures() >= G1MaxVerifyFailures) {
          return;
        }
      } else {
        gclog_or_tty->print_cr(PTR_FORMAT " not an oop", p2i(obj));
        *failures = true;
        return;
      }
    }

    prev_p = p;
    p += obj_size;
  }
}

void HeapRegion::verify_rem_set() const {
  bool failures = false;
  // 搜索 void HeapRegion::verify_rem_set(VerifyOption vo, bool* failures)
  verify_rem_set(VerifyOption_G1UsePrevMarking, &failures);
  guarantee(!failures, "HeapRegion RemSet verification failed");
}

// G1OffsetTableContigSpace code; copied from space.cpp.  Hope this can go
// away eventually.

void G1OffsetTableContigSpace::clear(bool mangle_space) {
  set_top(bottom());
  _scan_top = bottom(); // 整个region目前不可扫描
  CompactibleSpace::clear(mangle_space);
  reset_bot();
}

void G1OffsetTableContigSpace::set_bottom(HeapWord* new_bottom) {
  Space::set_bottom(new_bottom);
  _offsets.set_bottom(new_bottom);
}

void G1OffsetTableContigSpace::set_end(HeapWord* new_end) {
  Space::set_end(new_end);
  _offsets.resize(new_end - bottom());
}

void G1OffsetTableContigSpace::print() const {
  print_short();
  gclog_or_tty->print_cr(" [" INTPTR_FORMAT ", " INTPTR_FORMAT ", "
                INTPTR_FORMAT ", " INTPTR_FORMAT ")",
                bottom(), top(), _offsets.threshold(), end());
}

HeapWord* G1OffsetTableContigSpace::initialize_threshold() {
  return _offsets.initialize_threshold();
}

HeapWord* G1OffsetTableContigSpace::cross_threshold(HeapWord* start,
                                                    HeapWord* end) {
  _offsets.alloc_block(start, end);
  return _offsets.threshold();
}

/**
 * in book
 * @return
 */
HeapWord* G1OffsetTableContigSpace::scan_top() const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  HeapWord* local_top = top();
  OrderAccess::loadload();
  const unsigned local_time_stamp = _gc_time_stamp;
  assert(local_time_stamp <= g1h->get_gc_time_stamp(), "invariant");
  // local_time_stamp < g1h->get_gc_time_stamp() = true，说明当前肯定处于GC Pause中(否则 gc_time_stamp() = 0),
  // 并且，当前的这个HeapRegion肯定不是当前GC所分配的Region
  if (local_time_stamp < g1h->get_gc_time_stamp()) {
    return local_top;
  } else { // 这个HeapRegion是当前GC alloc Region，那么，只可以最多扫描到_scan_top的位置(对于本轮的gc alloc region，_scan_top就是bottom)，高于这个位置
    return _scan_top;
  }
}
/**
 * 记录当前的GC版本号到这个在GC期间使用的HeapRegion中。
 * 这样，通过一个HeapRegion的本地版本号与当前GC的版本号做比较，就可以知道当前HeapRegion是不是在当前GC中分配的还是以前的GC分配的。
 */
void G1OffsetTableContigSpace::record_timestamp() {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  unsigned curr_gc_time_stamp = g1h->get_gc_time_stamp();
  //  由于每次GC结束的时候都会将_gc_time_stamp重置为0，因此如果_gc_time_stamp < curr_gc_time_stamp，说明当前肯定处于GC pause当中
  if (_gc_time_stamp < curr_gc_time_stamp) {
    // Setting the time stamp here tells concurrent readers to look at
    // scan_top to know the maximum allowed address to look at.

    // scan_top should be bottom for all regions except for the
    // retained old alloc region which should have scan_top == top
    HeapWord* st = _scan_top;
    guarantee(st == _bottom || st == _top, "invariant");

    _gc_time_stamp = curr_gc_time_stamp; // 这个Region是在当前GC中分配的，因此将当前GC的版本号记录到这个HeapRegion中
  }
}

void G1OffsetTableContigSpace::record_retained_region() {
  // scan_top is the maximum address where it's safe for the next gc to
  // scan this region.
  _scan_top = top(); // 更新可扫描区域的最高地址为当前HeapRegion的_top，即当前内存分配的最新地址(注意不是_end)
}

void G1OffsetTableContigSpace::safe_object_iterate(ObjectClosure* blk) {
  object_iterate(blk);
}

void G1OffsetTableContigSpace::object_iterate(ObjectClosure* blk) {
  HeapWord* p = bottom();
  while (p < top()) {
    if (block_is_obj(p)) {
      blk->do_object(oop(p));
    }
    p += block_size(p);
  }
}

#define block_is_always_obj(q) true
void G1OffsetTableContigSpace::prepare_for_compaction(CompactPoint* cp) {
  SCAN_AND_FORWARD(cp, top, block_is_always_obj, block_size);
}
#undef block_is_always_obj

/**
 * 构造函数 G1OffsetTableContigSpace::G1OffsetTableContigSpace，
 * 在这里构造了成员变量G1OffsetTableContigSpace
 * G1OffsetTableContigSpace 是HeapRegion的父类，因此搜索 HeapRegion::HeapRegion 的构造函数查看该构造函数的调用
 */
G1OffsetTableContigSpace::G1OffsetTableContigSpace(G1BlockOffsetSharedArray* sharedOffsetArray,
                         MemRegion mr) :
  _offsets(sharedOffsetArray, mr), //  构造了一个G1BlockOffsetArrayContigSpace对象
  _par_alloc_lock(Mutex::leaf, "OffsetTableContigSpace par alloc lock", true),
  _gc_time_stamp(0)
{
    /**
     * 设置这个G1BlockOffsetArrayContigSpace(G1BlockOffsetArrayContigSpace -> G1BlockOffsetArray -> G1BlockOffsetTable)
     *      所属的G1OffsetTableContigSpace(G1OffsetTableContigSpace -> HeapRegion) ，
     * 搜索 G1BlockOffsetArray::set_space 查看方法实现
     */
  _offsets.set_space(this);
}

void G1OffsetTableContigSpace::initialize(MemRegion mr, bool clear_space, bool mangle_space) {
  CompactibleSpace::initialize(mr, clear_space, mangle_space);
  _top = bottom();
  _scan_top = bottom();
  set_saved_mark_word(NULL);
  reset_bot();
}

