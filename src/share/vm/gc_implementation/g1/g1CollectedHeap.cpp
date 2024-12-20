/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
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

#if !defined(__clang_major__) && defined(__GNUC__)
#define ATTRIBUTE_PRINTF(x,y) // FIXME, formats are a mess.
#endif

#include "precompiled.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "gc_implementation/g1/bufferingOopClosure.hpp"
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/concurrentMarkThread.inline.hpp"
#include "gc_implementation/g1/g1AllocRegion.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1ErgoVerbose.hpp"
#include "gc_implementation/g1/g1EvacFailure.hpp"
#include "gc_implementation/g1/g1GCPhaseTimes.hpp"
#include "gc_implementation/g1/g1Log.hpp"
#include "gc_implementation/g1/g1MarkSweep.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1ParScanThreadState.inline.hpp"
#include "gc_implementation/g1/g1RegionToSpaceMapper.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/g1RootProcessor.hpp"
#include "gc_implementation/g1/g1StringDedup.hpp"
#include "gc_implementation/g1/g1YCTypes.hpp"
#include "gc_implementation/g1/heapRegion.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "gc_implementation/g1/heapRegionSet.inline.hpp"
#include "gc_implementation/g1/vm_operations_g1.hpp"
#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "memory/allocation.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/generationSpec.hpp"
#include "memory/iterator.hpp"
#include "memory/referenceProcessor.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.pcgc.inline.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "runtime/vmThread.hpp"

size_t G1CollectedHeap::_humongous_object_threshold_in_words = 0;

// turn it on so that the contents of the young list (scan-only /
// to-be-collected) are printed at "strategic" points before / during
// / after the collection --- this is useful for debugging
#define YOUNG_LIST_VERBOSE 0
// CURRENT STATUS
// This file is under construction.  Search for "FIXME".

// INVARIANTS/NOTES
//
// All allocation activity covered by the G1CollectedHeap interface is
// serialized by acquiring the HeapLock.  This happens in mem_allocate
// and allocate_new_tlab, which are the "entry" points to the
// allocation code from the rest of the JVM.  (Note that this does not
// apply to TLAB allocation, which is not part of this interface: it
// is done by clients of this interface.)

// Local to this file.

/**
 * in book
 */
class RefineCardTableEntryClosure: public CardTableEntryClosure {
  bool _concurrent;
public:
  RefineCardTableEntryClosure() : _concurrent(true) { }

  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
      // 对卡片进行refine处理
    bool oops_into_cset = G1CollectedHeap::heap()->g1_rem_set()->refine_card(card_ptr, worker_i, false);
    // This path is executed by the concurrent refine or mutator threads,
    // concurrently, and so we do not care if card_ptr contains references
    // that point into the collection set.
    assert(!oops_into_cset, "should be");

    if (_concurrent && SuspendibleThreadSet::should_yield()) {
      // Caller will actually yield.
      return false;
    }
    // Otherwise, we finished successfully; return true.
    return true;
  }

  void set_concurrent(bool b) { _concurrent = b; }
};


class ClearLoggedCardTableEntryClosure: public CardTableEntryClosure {
  size_t _num_processed;
  CardTableModRefBS* _ctbs;
  int _histo[256];

 public:
  ClearLoggedCardTableEntryClosure() :
    _num_processed(0), _ctbs(G1CollectedHeap::heap()->g1_barrier_set())
  {
    for (int i = 0; i < 256; i++) _histo[i] = 0;
  }

  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    unsigned char* ujb = (unsigned char*)card_ptr;
    int ind = (int)(*ujb);
    _histo[ind]++;

    *card_ptr = (jbyte)CardTableModRefBS::clean_card_val();
    _num_processed++;

    return true;
  }

  size_t num_processed() { return _num_processed; }

  void print_histo() {
    gclog_or_tty->print_cr("Card table value histogram:");
    for (int i = 0; i < 256; i++) {
      if (_histo[i] != 0) {
        gclog_or_tty->print_cr("  %d: %d", i, _histo[i]);
      }
    }
  }
};

// in book
class RedirtyLoggedCardTableEntryClosure : public CardTableEntryClosure {
 private:
  size_t _num_processed;

 public:
  RedirtyLoggedCardTableEntryClosure() : CardTableEntryClosure(), _num_processed(0) { }

  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    *card_ptr = CardTableModRefBS::dirty_card_val(); // 将dirty值复制给对应的卡片
    _num_processed++;
    return true;
  }

  size_t num_processed() const { return _num_processed; }
};

YoungList::YoungList(G1CollectedHeap* g1h) :
    _g1h(g1h), _head(NULL), _length(0), _last_sampled_rs_lengths(0),
    _survivor_head(NULL), _survivor_tail(NULL), _survivor_length(0) {
  guarantee(check_list_empty(false), "just making sure...");
}

/**
 * 查看 G1CollectedHeap::new_mutator_alloc_region，因此只发生在用户层面需要创建一个新的region的时候
 */
void YoungList::push_region(HeapRegion *hr) {
  assert(!hr->is_young(), "should not already be young");
  assert(hr->get_next_young_region() == NULL, "cause it should!")
  /**
   * 将当前region的_next_young_region设置为当前的_head
   */
  hr->set_next_young_region(_head);
  _head = hr;

  _g1h->g1_policy()->set_region_eden(hr, (int) _length); // 这肯定是一个eden region
  ++_length; // 为什么在add_survivor_region中添加的时候必须要递增++_length
}

/**
 * 向YoungList中添加一个新的Survivor HeapRegion
 */
void YoungList::add_survivor_region(HeapRegion* hr) {
  assert(hr->is_survivor(), "should be flagged as survivor region");
  assert(hr->get_next_young_region() == NULL, "cause it should!");
  // 这个新插入的Region的下一个young region是_survivor_head
  hr->set_next_young_region(_survivor_head);
  if (_survivor_head == NULL) { //如果当前还没有任何一个survivor region
    _survivor_tail = hr; // 设置tail
  }
  _survivor_head = hr; // 当前新push进来的HeapRegion作为Survivor Region链表的头指针
  ++_survivor_length;
}

void YoungList::empty_list(HeapRegion* list) {
  while (list != NULL) {
    HeapRegion* next = list->get_next_young_region();
    list->set_next_young_region(NULL); // 从young list中剥离
    list->uninstall_surv_rate_group();
    // This is called before a Full GC and all the non-empty /
    // non-humongous regions at the end of the Full GC will end up as
    // old anyway.
    list->set_old(); // 将这个Region的类型设置为old
    list = next; // next指针指向下一个Young HeapRegion，查看下一个Young HeapRegion
  }
}

void YoungList::empty_list() {
  assert(check_list_well_formed(), "young list should be well formed");

  empty_list(_head); // void YoungList::empty_list(HeapRegion* list) 将以_head为链表的YoungList中的
  _head = NULL; // _head置为空
  _length = 0; // YoungList长度清空

  empty_list(_survivor_head);// void YoungList::empty_list(HeapRegion* list) 将以_survivor_head为链表的YoungList中的所有HeapRegion执行清理
  _survivor_head = NULL; // survivor的head、tail都置为空
  _survivor_tail = NULL;
  _survivor_length = 0;

  _last_sampled_rs_lengths = 0;

  assert(check_list_empty(false), "just making sure...");
}

bool YoungList::check_list_well_formed() {
  bool ret = true;

  uint length = 0;
  HeapRegion* curr = _head;
  HeapRegion* last = NULL;
  while (curr != NULL) {
    if (!curr->is_young()) {
      gclog_or_tty->print_cr("### YOUNG REGION " PTR_FORMAT "-" PTR_FORMAT " "
                             "incorrectly tagged (y: %d, surv: %d)",
                             curr->bottom(), curr->end(),
                             curr->is_young(), curr->is_survivor());
      ret = false;
    }
    ++length;
    last = curr;
    curr = curr->get_next_young_region();
  }
  ret = ret && (length == _length);

  if (!ret) {
    gclog_or_tty->print_cr("### YOUNG LIST seems not well formed!");
    gclog_or_tty->print_cr("###   list has %u entries, _length is %u",
                           length, _length);
  }

  return ret;
}

bool YoungList::check_list_empty(bool check_sample) {
  bool ret = true;

  if (_length != 0) {
    gclog_or_tty->print_cr("### YOUNG LIST should have 0 length, not %u",
                  _length);
    ret = false;
  }
  if (check_sample && _last_sampled_rs_lengths != 0) {
    gclog_or_tty->print_cr("### YOUNG LIST has non-zero last sampled RS lengths");
    ret = false;
  }
  if (_head != NULL) {
    gclog_or_tty->print_cr("### YOUNG LIST does not have a NULL head");
    ret = false;
  }
  if (!ret) {
    gclog_or_tty->print_cr("### YOUNG LIST does not seem empty");
  }

  return ret;
}

void
YoungList::rs_length_sampling_init() {
  _sampled_rs_lengths = 0;
  _curr               = _head;
}

bool
YoungList::rs_length_sampling_more() {
  return _curr != NULL;
}

/**
 * 对当前region _curr 进行采样，并更新游标 _curr 到下一个region
 */
void
YoungList::rs_length_sampling_next() {
  assert( _curr != NULL, "invariant" );
  /**
   * 或者当前这个Region的rem_set的已经占用的长度，这里的长度是三个粒度表中包含的所有的card的数量
   * 实际上是调用OtherRegionsTable::occupied()
   */
  size_t rs_length = _curr->rem_set()->occupied();
  // 将当前HeapRegion的RSet的长度累加到_sampled_rs_lengthsz中
  _sampled_rs_lengths += rs_length;

  // The current region may not yet have been added to the
  // incremental collection set (it gets added when it is
  // retired as the current allocation region).
  if (_curr->in_collection_set()) { // 当前的Region在回收集合Collection Set中
    // Update the collection set policy information for this region
    // 如果这个Region在回收集合中，那么就更新当前region的
    _g1h->g1_policy()->update_incremental_cset_info(_curr, rs_length);
  }

  _curr = _curr->get_next_young_region();// 更新游标指针到下一个region
  if (_curr == NULL) { // 已经是最后一个region了，那么将统计的_sampled_rs_lengths的值更新到_last_sampled_rs_lengths中
    _last_sampled_rs_lengths = _sampled_rs_lengths;
    // gclog_or_tty->print_cr("last sampled RS lengths = %d", _last_sampled_rs_lengths);
  }
}

void
YoungList::reset_auxilary_lists() {
  guarantee( is_empty(), "young list should be empty" );
  assert(check_list_well_formed(), "young list should be well formed");

  // Add survivor regions to SurvRateGroup.
  _g1h->g1_policy()->note_start_adding_survivor_regions();
  _g1h->g1_policy()->finished_recalculating_age_indexes(true /* is_survivors */);

  int young_index_in_cset = 0;
  for (HeapRegion* curr = _survivor_head;
       curr != NULL;
       curr = curr->get_next_young_region()) {
    _g1h->g1_policy()->set_region_survivor(curr, young_index_in_cset);

    // The region is a non-empty survivor so let's add it to
    // the incremental collection set for the next evacuation
    // pause.
    /**
     * 这个region是一个非空的region，因此将这个region添加到增量回收集合中，这样在下次回收暂停的时候进行回收
     */
    _g1h->g1_policy()->add_region_to_incremental_cset_rhs(curr);
    young_index_in_cset += 1;
  }
  assert((uint) young_index_in_cset == _survivor_length, "post-condition");
  _g1h->g1_policy()->note_stop_adding_survivor_regions();

  _head   = _survivor_head;
  _length = _survivor_length;
  if (_survivor_head != NULL) {
    assert(_survivor_tail != NULL, "cause it shouldn't be");
    assert(_survivor_length > 0, "invariant");
    _survivor_tail->set_next_young_region(NULL);
  }

  // Don't clear the survivor list handles until the start of
  // the next evacuation pause - we need it in order to re-tag
  // the survivor regions from this evacuation pause as 'young'
  // at the start of the next.

  _g1h->g1_policy()->finished_recalculating_age_indexes(false /* is_survivors */);

  assert(check_list_well_formed(), "young list should be well formed");
}

void YoungList::print() {
  HeapRegion* lists[] = {_head,   _survivor_head};
  const char* names[] = {"YOUNG", "SURVIVOR"};

  for (uint list = 0; list < ARRAY_SIZE(lists); ++list) {
    gclog_or_tty->print_cr("%s LIST CONTENTS", names[list]);
    HeapRegion *curr = lists[list];
    if (curr == NULL)
      gclog_or_tty->print_cr("  empty");
    while (curr != NULL) {
      gclog_or_tty->print_cr("  " HR_FORMAT ", P: " PTR_FORMAT ", N: " PTR_FORMAT ", age: %4d",
                             HR_FORMAT_PARAMS(curr),
                             curr->prev_top_at_mark_start(),
                             curr->next_top_at_mark_start(),
                             curr->age_in_surv_rate_group_cond());
      curr = curr->get_next_young_region();
    }
  }

  gclog_or_tty->cr();
}

void G1RegionMappingChangedListener::reset_from_card_cache(uint start_idx, size_t num_regions) {
  OtherRegionsTable::invalidate(start_idx, num_regions);
}

void G1RegionMappingChangedListener::on_commit(uint start_idx, size_t num_regions, bool zero_filled) {
  // The from card cache is not the memory that is actually committed. So we cannot
  // take advantage of the zero_filled parameter.
  reset_from_card_cache(start_idx, num_regions);
}

/**
 * 这个方法涉及到了两个原子交换操作
 * 第一个原子交换，用来让自己的_dirty_cards_region_addr指针指向自己，这个操作用来获取 将这个Region推送到脏卡片列表的权利，
 *      因为如果要将Region推送到脏卡片列表，需要next == null
 * 第二个原子交换，用来将当前的Region设置为头结点  _dirty_cards_region_list
 * @param hr
 */
void G1CollectedHeap::push_dirty_cards_region(HeapRegion* hr)
{
  // Claim the right to put the region on the dirty cards region list
  // by installing a self pointer.
  HeapRegion* next = hr->get_next_dirty_cards_region();
  // 当前堆区域还没有被添加到脏卡片区域列表中。
  if (next == NULL) {
      // 当前堆区域的指针设置为下一个脏卡片区域的指针，如果操作成功，则将当前堆区域添加到脏卡片区域列表中。
    HeapRegion* res = (HeapRegion*)
      //  这个原子交换操作将hr->next_dirty_cards_region_addr() 处的指针与 NULL 进行比较，如果相等，则将其替换为 hr。该操作返回原来的值。
      Atomic::cmpxchg_ptr(hr, hr->next_dirty_cards_region_addr(),
                          NULL);
    if (res == NULL) {
      HeapRegion* head;
      do {
        // Put the region to the dirty cards region list.
        head = _dirty_cards_region_list;
        // 尝试将当前堆区域添加到脏卡片区域列表的头部，返回
        // 比较 _dirty_cards_region_list 的头部指针与 head，如果相等，则将其替换为 hr，并返回原来的值
        next = (HeapRegion*)
          Atomic::cmpxchg_ptr(hr, &_dirty_cards_region_list, head);
        if (next == head) { // 添加成功，即当前列表的头部没有被其他线程修改，替换成功，此时头部已经是hr了
          assert(hr->get_next_dirty_cards_region() == hr,
                 "hr->get_next_dirty_cards_region() != hr");
          if (next == NULL) {
            // The last region in the list points to itself.
            hr->set_next_dirty_cards_region(hr); // 列表中最后一个HeapRegion的_next_dirty_cards_region指向自己
          } else {
            hr->set_next_dirty_cards_region(next);// 列表中最后一个HeapRegion的_next_dirty_cards_region指向next
          }
        }
      } while (next != head); // 替换不成功
    }
  }
}

HeapRegion* G1CollectedHeap::pop_dirty_cards_region()
{
  HeapRegion* head;
  HeapRegion* hr;
  do {
    head = _dirty_cards_region_list;
    if (head == NULL) {
      return NULL;
    }
    HeapRegion* new_head = head->get_next_dirty_cards_region();
    if (head == new_head) {
      // The last region.
      new_head = NULL;
    }
    hr = (HeapRegion*)Atomic::cmpxchg_ptr(new_head, &_dirty_cards_region_list,
                                          head);
  } while (hr != head);
  assert(hr != NULL, "invariant");
  hr->set_next_dirty_cards_region(NULL);
  return hr;
}

#ifdef ASSERT
// A region is added to the collection set as it is retired
// so an address p can point to a region which will be in the
// collection set but has not yet been retired.  This method
// therefore is only accurate during a GC pause after all
// regions have been retired.  It is used for debugging
// to check if an nmethod has references to objects that can
// be move during a partial collection.  Though it can be
// inaccurate, it is sufficient for G1 because the conservative
// implementation of is_scavengable() for G1 will indicate that
// all nmethods must be scanned during a partial collection.
bool G1CollectedHeap::is_in_partial_collection(const void* p) {
  if (p == NULL) {
    return false;
  }
  return heap_region_containing(p)->in_collection_set();
}
#endif

// Returns true if the reference points to an object that
// can move in an incremental collection.
bool G1CollectedHeap::is_scavengable(const void* p) {
  HeapRegion* hr = heap_region_containing(p);
  return !hr->isHumongous();
}
// 检查卡表日志
void G1CollectedHeap::check_ct_logs_at_safepoint() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  CardTableModRefBS* ct_bs = g1_barrier_set();

  // Count the dirty cards at the start.
  CountNonCleanMemRegionClosure count1(this);
  ct_bs->mod_card_iterate(&count1);
  int orig_count = count1.n();

  // First clear the logged cards.
  ClearLoggedCardTableEntryClosure clear;
  dcqs.apply_closure_to_all_completed_buffers(&clear);
  dcqs.iterate_closure_all_threads(&clear, false);
  clear.print_histo();

  // Now ensure that there's no dirty cards.
  CountNonCleanMemRegionClosure count2(this);
  ct_bs->mod_card_iterate(&count2);
  if (count2.n() != 0) {
    gclog_or_tty->print_cr("Card table has %d entries; %d originally",
                           count2.n(), orig_count);
  }
  guarantee(count2.n() == 0, "Card table should be clean.");

  RedirtyLoggedCardTableEntryClosure redirty;
  dcqs.apply_closure_to_all_completed_buffers(&redirty);
  dcqs.iterate_closure_all_threads(&redirty, false);
  gclog_or_tty->print_cr("Log entries = %d, dirty cards = %d.",
                         clear.num_processed(), orig_count);
  guarantee(redirty.num_processed() == clear.num_processed(),
            err_msg("Redirtied " SIZE_FORMAT " cards, bug cleared " SIZE_FORMAT,
                    redirty.num_processed(), clear.num_processed()));

  CountNonCleanMemRegionClosure count3(this);
  ct_bs->mod_card_iterate(&count3);
  if (count3.n() != orig_count) {
    gclog_or_tty->print_cr("Should have restored them all: orig = %d, final = %d.",
                           orig_count, count3.n());
    guarantee(count3.n() >= orig_count, "Should have restored them all.");
  }
}

// Private class members.

G1CollectedHeap* G1CollectedHeap::_g1h;

// Private methods.

HeapRegion*
G1CollectedHeap::new_region_try_secondary_free_list(bool is_old) {
    // 获取次要空闲列表的互斥锁
  MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
  // 只要 二级空闲列表不为空或者还有空闲区域到来时持续执行
  while (!_secondary_free_list.is_empty() || free_regions_coming()) {
    if (!_secondary_free_list.is_empty()) {
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "secondary_free_list has %u entries",
                               _secondary_free_list.length());
      }
      // It looks as if there are free regions available on the
      // secondary_free_list. Let's move them to the free_list and try
      // again to allocate from it.
      append_secondary_free_list(); // 看起来 secondary_free_list 上似乎有可用的空闲区域。 让我们将它们移至 free_list 并再次尝试从中分配。

      assert(_hrm.num_free_regions() > 0, "if the secondary_free_list was not "
             "empty we should have moved at least one entry to the free_list");
      // 搜索 HeapRegion* allocate_free_region
      HeapRegion* res = _hrm.allocate_free_region(is_old); // 从一级空闲列表中取出一个Region
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "allocated " HR_FORMAT " from secondary_free_list",
                               HR_FORMAT_PARAMS(res));
      }
      return res;
    }

    // Wait here until we get notified either when (a) there are no
    // more free regions coming or (b) some regions have been moved on
    // the secondary_free_list.
    // 在此等待，直到 (a) 没有更多空闲区域到来或 (b) 某些区域已移至 secondary_free_list 时我们收到通知。
    // 在CM的cleanup阶段结束的时候，会调用reset_free_regions_coming，进而通知SecondaryFreeList_lock
    SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag);
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                           "could not allocate from secondary_free_list");
  }
  return NULL;
}

/**
 * 这个方法用来给一个大小为word_size的大对象分配空间而创建一个新的region
 * @param word_size
 * @param is_old
 * @param do_expand
 * @return
 */
HeapRegion* G1CollectedHeap::new_region(size_t word_size, bool is_old, bool do_expand) {
  assert(!isHumongous(word_size) || word_size <= HeapRegion::GrainWords,
         "the only time we use this to allocate a humongous region is "
         "when we are allocating a single humongous region");

  HeapRegion* res;
  // 如果打开了 G1StressConcRegionFreeing 并且二级region空闲列表不为空，从二级region空闲列表中分配成功则返回
  if (G1StressConcRegionFreeing) {
      /**
       * 二级region空闲列表来自于并发标记的清理阶段
       * ConcurrentMark::completeCleanup
       */
    if (!_secondary_free_list.is_empty()) { // 二级Region空闲列表不是空的
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "forced to look at the secondary_free_list");
      }
      res = new_region_try_secondary_free_list(is_old); // 从二级Region空闲列表中取出region
      if (res != NULL) {
        return res;
      }
    }
  }
  /**
   *  否则，继续尝试分配
   */
  // 从主空闲列表中 分配一个新的堆区域
  res = _hrm.allocate_free_region(is_old);

  if (res == NULL) {
    if (G1ConcRegionFreeingVerbose) {
      gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                             "res == NULL, trying the secondary_free_list");
    }
    // 在主空闲列表中分配失败，尝试二级空闲列表
    res = new_region_try_secondary_free_list(is_old);
  }
  //
  /**
   * 一级和二级分配都失败，那么看看是否需要进行扩展
   * 我们搜索  new_gc_alloc_region，humongous_obj_allocate，new_mutator_alloc_region，发现只有new_gc_alloc_region()会扩展
   */
  if (res == NULL && do_expand && _expand_heap_after_alloc_failure) {
    // Currently, only attempts to allocate GC alloc regions set
    // do_expand to true. So, we should only reach here during a
    // safepoint. If this assumption changes we might have to
    // reconsider the use of _expand_heap_after_alloc_failure.
    /**
     * 进行堆内存扩展的时候必须要求处于safepoint
     */
    assert(SafepointSynchronize::is_at_safepoint(), "invariant");

    ergo_verbose1(ErgoHeapSizing,
                  "attempt heap expansion",
                  ergo_format_reason("region allocation request failed")
                  ergo_format_byte("allocation request"),
                  word_size * HeapWordSize);
    /**
     * 方法实现 搜索 bool G1CollectedHeap::expand
     */
    if (expand(word_size * HeapWordSize)) {
      // Given that expand() succeeded in expanding the heap, and we
      // always expand the heap by an amount aligned to the heap
      // region size, the free list should in theory not be empty.
      // In either case allocate_free_region() will check for NULL.
      res = _hrm.allocate_free_region(is_old);
    } else {
      _expand_heap_after_alloc_failure = false;
    }
  }
  return res;
}

HeapWord*
G1CollectedHeap::humongous_obj_allocate_initialize_regions(uint first,
                                                           uint num_regions,
                                                           size_t word_size,
                                                           AllocationContext_t context) {
  assert(first != G1_NO_HRM_INDEX, "pre-condition");
  assert(isHumongous(word_size), "word_size should be humongous");
  assert(num_regions * HeapRegion::GrainWords >= word_size, "pre-condition");

  // Index of last region in the series + 1.
  uint last = first + num_regions;

  // We need to initialize the region(s) we just discovered. This is
  // a bit tricky given that it can happen concurrently with
  // refinement threads refining cards on these regions and
  // potentially wanting to refine the BOT as they are scanning
  // those cards (this can happen shortly after a cleanup; see CR
  // 6991377). So we have to set up the region(s) carefully and in
  // a specific order.

  // The word size sum of all the regions we will allocate.
  size_t word_size_sum = (size_t) num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // This will be the "starts humongous" region.
  HeapRegion* first_hr = region_at(first);
  // The header of the new object will be placed at the bottom of
  // the first region.
  HeapWord* new_obj = first_hr->bottom();
  // This will be the new end of the first region in the series that
  // should also match the end of the last region in the series.
  HeapWord* new_end = new_obj + word_size_sum;
  // This will be the new top of the first region that will reflect
  // this allocation.
  HeapWord* new_top = new_obj + word_size;

  // First, we need to zero the header of the space that we will be
  // allocating. When we update top further down, some refinement
  // threads might try to scan the region. By zeroing the header we
  // ensure that any thread that will try to scan the region will
  // come across the zero klass word and bail out.
  //
  // NOTE: It would not have been correct to have used
  // CollectedHeap::fill_with_object() and make the space look like
  // an int array. The thread that is doing the allocation will
  // later update the object header to a potentially different array
  // type and, for a very short period of time, the klass and length
  // fields will be inconsistent. This could cause a refinement
  // thread to calculate the object size incorrectly.
  Copy::fill_to_words(new_obj, oopDesc::header_size(), 0);

  // We will set up the first region as "starts humongous". This
  // will also update the BOT covering all the regions to reflect
  // that there is a single object that starts at the bottom of the
  // first region.
  first_hr->set_startsHumongous(new_top, new_end);
  first_hr->set_allocation_context(context);
  // Then, if there are any, we will set up the "continues
  // humongous" regions.
  HeapRegion* hr = NULL;
  for (uint i = first + 1; i < last; ++i) {
    hr = region_at(i);
    hr->set_continuesHumongous(first_hr);
    hr->set_allocation_context(context);
  }
  // If we have "continues humongous" regions (hr != NULL), then the
  // end of the last one should match new_end.
  assert(hr == NULL || hr->end() == new_end, "sanity");

  // Up to this point no concurrent thread would have been able to
  // do any scanning on any region in this series. All the top
  // fields still point to bottom, so the intersection between
  // [bottom,top] and [card_start,card_end] will be empty. Before we
  // update the top fields, we'll do a storestore to make sure that
  // no thread sees the update to top before the zeroing of the
  // object header and the BOT initialization.
  OrderAccess::storestore();

  // Now that the BOT and the object header have been initialized,
  // we can update top of the "starts humongous" region.
  assert(first_hr->bottom() < new_top && new_top <= first_hr->end(),
         "new_top should be in this region");
  first_hr->set_top(new_top);
  if (_hr_printer.is_active()) {
    HeapWord* bottom = first_hr->bottom();
    HeapWord* end = first_hr->orig_end();
    if ((first + 1) == last) {
      // the series has a single humongous region
      _hr_printer.alloc(G1HRPrinter::SingleHumongous, first_hr, new_top);
    } else {
      // the series has more than one humongous regions
      _hr_printer.alloc(G1HRPrinter::StartsHumongous, first_hr, end);
    }
  }

  // Now, we will update the top fields of the "continues humongous"
  // regions. The reason we need to do this is that, otherwise,
  // these regions would look empty and this will confuse parts of
  // G1. For example, the code that looks for a consecutive number
  // of empty regions will consider them empty and try to
  // re-allocate them. We can extend is_empty() to also include
  // !continuesHumongous(), but it is easier to just update the top
  // fields here. The way we set top for all regions (i.e., top ==
  // end for all regions but the last one, top == new_top for the
  // last one) is actually used when we will free up the humongous
  // region in free_humongous_region().
  hr = NULL;
  for (uint i = first + 1; i < last; ++i) {
    hr = region_at(i);
    if ((i + 1) == last) {
      // last continues humongous region
      assert(hr->bottom() < new_top && new_top <= hr->end(),
             "new_top should fall on this region");
      hr->set_top(new_top);
      _hr_printer.alloc(G1HRPrinter::ContinuesHumongous, hr, new_top);
    } else {
      // not last one
      assert(new_top > hr->end(), "new_top should be above this region");
      hr->set_top(hr->end());
      _hr_printer.alloc(G1HRPrinter::ContinuesHumongous, hr, hr->end());
    }
  }
  // If we have continues humongous regions (hr != NULL), then the
  // end of the last one should match new_end and its top should
  // match new_top.
  assert(hr == NULL ||
         (hr->end() == new_end && hr->top() == new_top), "sanity");
  check_bitmaps("Humongous Region Allocation", first_hr);

  assert(first_hr->used() == word_size * HeapWordSize, "invariant");
  _allocator->increase_used(first_hr->used());
  _humongous_set.add(first_hr);

  return new_obj;
}

// If could fit into free regions w/o expansion, try.
// Otherwise, if can expand, do so.
// Otherwise, if using ex regions might help, try with ex given back.
// 安全点进行大对象分配
HeapWord* G1CollectedHeap::humongous_obj_allocate(size_t word_size, AllocationContext_t context) {
    /**
     * 或者已经拿到了HeapLock，或者当前处于safepoint
     */
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);

  verify_region_sets_optional();

  uint first = G1_NO_HRM_INDEX;
  // 将对象大小对齐到Region的倍数，并将其除以区域的大小，以确定所涉及的Region数量
  uint obj_regions = (uint)(align_size_up_(word_size, HeapRegion::GrainWords) / HeapRegion::GrainWords);
  /**
   * 根据要分配的巨型对象所涉及的区域数量来选择不同的分配路径
   */
  if (obj_regions == 1) {
    // Only one region to allocate, try to use a fast path by directly allocating
    // from the free lists. Do not try to expand here, we will potentially do that
    // later.
    /**
     * 如果只涉及一个区域，尝试从空闲列表直接分配一个新的Region
     * 如果涉及多个区域，则根据当前的堆状态来选择分配路径
     * 搜索 G1CollectedHeap::new_region
     */
    HeapRegion* hr = new_region(word_size, true /* is_old */, false /* do_expand */);
    if (hr != NULL) {
      first = hr->hrm_index();
    }
  } else {
    /**
     * 在 cleanupComplete() 运行时，我们无法分配跨越多个Region的超大区域，因为我们发现的一些空区域可能尚未添加到空闲列表中。
     * 我们无法直接知道它们在哪个列表中，以便我们可以删除它们。只有在我们需要分配多个区域以满足当前超大分配请求时，我们才需要这样做。
     * 如果我们只分配一个区域，我们使用单区域区域分配代码（见上文），该代码可能已经等待来自二级空闲列表的区域。
     */
    wait_while_free_regions_coming();
    append_secondary_free_list_if_not_empty_with_lock();

    // Policy: Try only empty regions (i.e. already committed first). Maybe we
    // are lucky enough to find some.
    /**
     * 尝试找到已经commit的空的region
     */
    first = _hrm.find_contiguous_only_empty(obj_regions);
    if (first != G1_NO_HRM_INDEX) {
        // 找到了一个连续的Region
      _hrm.allocate_free_regions_starting_at(first, obj_regions); // 把这一片连续的Region从_free_list中remove掉
    }
  }

  if (first == G1_NO_HRM_INDEX) { // 没有找到这样的连续的region
    // Policy: We could not find enough regions for the humongous object in the
    // free list. Look through the heap to find a mix of free and uncommitted regions.
    // If so, try expansion.
    /**
     * 我们无法在已经commit的region里面找到合适分配的区域。
     * 因此，我们查找整个heap去查找空闲以及未提交的region混合在一起的region
     */
    first = _hrm.find_contiguous_empty_or_unavailable(obj_regions);
    if (first != G1_NO_HRM_INDEX) { // 成功找到
      // We found something. Make sure these regions are committed, i.e. expand
      // the heap. Alternatively we could do a defragmentation GC.
      ergo_verbose1(ErgoHeapSizing,
                    "attempt heap expansion",
                    ergo_format_reason("humongous allocation request failed")
                    ergo_format_byte("allocation request"),
                    word_size * HeapWordSize);

      _hrm.expand_at(first, obj_regions); // 对region进行扩展，对于那些unavailable的region进行提交，准备使用
      g1_policy()->record_new_heap_size(num_regions());

#ifdef ASSERT
      for (uint i = first; i < first + obj_regions; ++i) {
        HeapRegion* hr = region_at(i);
        assert(hr->is_free(), "sanity");
        assert(hr->is_empty(), "sanity");
        assert(is_on_master_free_list(hr), "sanity");
      }
#endif
    // 扩展以后再次尝试进行分配，即将这些Region从_free_list中删除
      _hrm.allocate_free_regions_starting_at(first, obj_regions);
    } else {
      // Policy: Potentially trigger a defragmentation GC.
    }
  }

  HeapWord* result = NULL;
  if (first != G1_NO_HRM_INDEX) { // 分配成功了，大对象分配条件下的Region的初始化
    result = humongous_obj_allocate_initialize_regions(first, obj_regions,
                                                       word_size, context);
    assert(result != NULL, "it should always return a valid result");

    // A successful humongous object allocation changes the used space
    // information of the old generation so we need to recalculate the
    // sizes and update the jstat counters here.
    g1mm()->update_sizes(); //修改内存空间的使用信息
  }

  verify_region_sets_optional();

  return result;
}

HeapWord* G1CollectedHeap::allocate_new_tlab(size_t word_size) {
  assert_heap_not_locked_and_not_at_safepoint();
  assert(!isHumongous(word_size), "we do not allow humongous TLABs");

  uint dummy_gc_count_before;
  uint dummy_gclocker_retry_count = 0;
  /**
   * 这里调用的是 G1CollectedHeap层面的attempt_allocation， 即 搜索 HeapWord* G1CollectedHeap::attempt_allocation
   */
  return attempt_allocation(word_size, &dummy_gc_count_before, &dummy_gclocker_retry_count);
}

/**
 * 这个方法在 CollectedHeap::common_mem_allocate_noinit() 中被调用。我们在common_mem_allocate_noinit()方法中可以看到，
 *      在尝试过TLAB分配失败以后，会通过该方法G1CollectedHeap::mem_allocate()对象分配到普通内存中
 * @param word_size
 * @param gc_overhead_limit_was_exceeded
 * @return
 */
HeapWord*
G1CollectedHeap::mem_allocate(size_t word_size,
                              bool*  gc_overhead_limit_was_exceeded) {
  assert_heap_not_locked_and_not_at_safepoint(); //必须不能持有Heap_lock，必须不可以在安全点

  // Loop until the allocation is satisfied, or unsatisfied after GC.
  // 不断循环，直到分配成功，或者，在GC以后依然分配不成功
  for (uint try_count = 1, gclocker_retry_count = 0; /* we'll return */; try_count += 1) {
    uint gc_count_before;

    HeapWord* result = NULL;
    if (!isHumongous(word_size)) {
        /**
         * 调用的是方法 HeapWord* G1CollectedHeap::attempt_allocation
         * 注意，在下层的 G1AllocRegion::attempt_allocation，但是是下层了
         */
      result = attempt_allocation(word_size, &gc_count_before, &gclocker_retry_count);
    } else {
        // 调用方法 G1CollectedHeap::attempt_allocation_humongous
      result = attempt_allocation_humongous(word_size, &gc_count_before, &gclocker_retry_count);
    }
    if (result != NULL) {
      return result; // 如果分配成功了，就返回分配成功的结果
    }

    // 执行到这里，说明分配失败，因此，需要进行gc的尝试
    // Create the garbage collection operation...
    VM_G1CollectForAllocation op(gc_count_before, word_size);
    op.set_allocation_context(AllocationContext::current());

    // ...and get the VM thread to execute it.
    VMThread::execute(&op); // VMThread::execute(&op); 其实是调用对应的op类的doit()方法，就好像线程池的每一个线程的run()方法

    if (op.prologue_succeeded() && op.pause_succeeded()) {
      // If the operation was successful we'll return the result even
      // if it is NULL. If the allocation attempt failed immediately
      // after a Full GC, it's unlikely we'll be able to allocate now.
      HeapWord* result = op.result();
      if (result != NULL && !isHumongous(word_size)) {
        // Allocations that take place on VM operations do not do any
        // card dirtying and we have to do it here. We only have to do
        // this for non-humongous allocations, though.
        dirty_young_block(result, word_size);
      }
      return result;
    } else {
      if (gclocker_retry_count > GCLockerRetryAllocationCount) {
        return NULL; // GC次数超过上限
      }
      assert(op.result() == NULL,
             "the result should be NULL if the VM op did not succeed");
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::mem_allocate retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

/**
 *  这个方法也专门处理 非大对象 的分配，大对象的分配由方法attempt_allocation_humongous()负责
 *  这个方法的调用者是方法 CollectedHeap::common_mem_allocate_noinit() ->
 *                          G1CollectedHeap::mem_allocate() ->
 *                              G1CollectedHeap::attempt_allocation() ->
 *                                  G1CollectedHeap::attempt_allocation_slow()
 *      我们把方法attempt_allocation叫做非大对象分配的第一层，只有当第一层分配失败，才会尝试第二层的attempt_allocation_slow()
 *
 *  这个方法的结构与attempt_allocation_humongous()有很多相似之处。 这两者没有合并为一个的原因是，
 *  这种方法需要几个“如果分配不是很大，则执行此操作，否则执行此操作”的条件路径，这会模糊其流程。
 *  事实上，该代码的早期版本确实使用了更难遵循的统一方法，因此，它存在难以追踪的微妙错误。
 *  因此，将这两个方法分开可以使每个方法更具可读性。 尽可能保持这两者同步是件好事。
 * @param word_size
 * @param context
 * @param gc_count_before_ret
 * @param gclocker_retry_count_ret
 * @return
 */
HeapWord* G1CollectedHeap::attempt_allocation_slow(size_t word_size,
                                                   AllocationContext_t context,
                                                   uint* gc_count_before_ret,
                                                   uint* gclocker_retry_count_ret) {
  // Make sure you read the note in attempt_allocation_humongous().

  assert_heap_not_locked_and_not_at_safepoint();
  assert(!isHumongous(word_size), "attempt_allocation_slow() should not "
         "be called for humongous allocation requests");

  // We should only get here after the first-level allocation attempt
  // (attempt_allocation()) failed to allocate.

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.

  /**
   * 我们将循环直到 a) 我们成功地执行分配或
   *              b)我们成功地调度了一个无法执行分配的回收
   *   只有在b的情况下我们才会返回null
   */
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;

    {
      MutexLockerEx x(Heap_lock); // 给堆内存上锁
      // 给mutator线程分配region，因此是分配在eden区，而不是给survivor或者old 分配region。
      // 详见G1DefaultAllocator
      result = _allocator // G1DefaultAllocator
              // MutatorAllocRegion,是G1AllocRegion 的子类，返回的MutatorAllocRegion维护了当前eden区域正在使用的region的信息
              // ，搜索 MutatorAllocRegion* mutator_alloc_region
              ->mutator_alloc_region(context) // 返回负责维护eden区域的region管理的G1AllocRegion的实现类的对象MutatorAllocRegion的地址
              // 进行二级分配，调用MutatorAllocRegion的attempt_allocation_locked()方法，
              // 实际上是调用其父类的G1AllocRegion::attempt_allocation_locked方法
              ->attempt_allocation_locked(word_size, false /* bot_updates */);//  这个方法在年轻代分配region因此不需要更新bot
      if (result != NULL) {
        return result; // 二级分配，终于分配成功
      }
      // 二级分配失败.
      // mutator_alloc_region(context)->get()方法其实是G1AllocRegion中的HeapRegion* get()方法，
      // 返回的是一个HeapRegion，即当前的MutatorAllocRegion对象所维护的active region
      // 由于不可能出现分配失败但是当前的HeapRegion又不是空(有Region的情况下不可能分配失败)，因此这里有对应的assert
      // If we reach here, attempt_allocation_locked() above failed to
      // allocate a new region. So the mutator alloc region should be NULL.
      assert(_allocator->mutator_alloc_region(context)->get() == NULL, "only way to get here");
      // 当前处于临界区状态，因此当前不能立刻进行gc，但是已经有其它地方请求了GC，一旦离开临界区就会被触发，因此不需要我们自己进行重复的gc
      if (GC_locker::is_active_and_needs_gc()) {
        if (g1_policy()->can_expand_young_list()) { // 还可以扩展 young 区region的数量
          // No need for an ergo verbose message here,
          // can_expand_young_list() does this when it returns true.
          // 进行基于Young区Region扩展的分配
          result = _allocator->mutator_alloc_region(context)->attempt_allocation_force(word_size,
                                                                                       false /* bot_updates */);
          if (result != NULL) {
            return result;
          }
        }
        should_try_gc = false; // 不应该主动进行gc
      } else {
        // The GCLocker may not be active but the GCLocker initiated
        // GC may not yet have been performed (GCLocker::needs_gc()
        // returns true). In this case we do not try this GC and
        // wait until the GCLocker initiated GC is performed, and
        // then retry the allocation.
        if (GC_locker::needs_gc()) { //GCLocker并不是活跃的，但是却有还未执行的gc，在这种情况下，我们放弃这个gc，因为期望那个还没有执行的gc进行执行
          should_try_gc = false;
        } else {
          // Read the GC count while still holding the Heap_lock.
          gc_count_before = total_collections(); // 没有GCLocker,同时也没有needs_gc的请求，因此，我们需要自己进行gc
          should_try_gc = true;
        }
      }
    }
    // 内存扩展也失败了，我们判断了当前的状态，的确应该尝试进行一次gc，因此调用do_collection_pause进行一次gc
    if (should_try_gc) {
      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded,
                                   GCCause::_g1_inc_collection_pause);
      if (result != NULL) {
        assert(succeeded, "only way to get back a non-NULL result");
        return result; // 成功，直接返回
      }
      /**
       * 这里的意思是，没有成功分配到内存，但是GC是成功的，那么没有必要再尝试了
       */
      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
    } else {
      if (*gclocker_retry_count_ret > GCLockerRetryAllocationCount) {
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
      // The GCLocker is either active or the GCLocker initiated
      // GC has not yet been performed. Stall until it is and
      // then retry the allocation.
      GC_locker::stall_until_clear(); // 一直等待，直到完成evacuation pause
      (*gclocker_retry_count_ret) += 1;
    }

    // We can reach here if we were unsuccessful in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space. We do the
    // first attempt (without holding the Heap_lock) here and the
    // follow-on attempt will be at the start of the next loop
    // iteration (after taking the Heap_lock).
    /**
     * MutatorAllocRegion 是 G1AllocRegion 的 子类, 这里调用的是父类 G1AllocRegion的attempt_allocation方法
     * 搜索 G1AllocRegion::attempt_allocation
     */
    result = _allocator->mutator_alloc_region(context)->attempt_allocation(word_size,
                                                                           false /* bot_updates */);
    if (result != NULL) {
      return result;
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::attempt_allocation_slow() "
              "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

/**
 * 给大对象分配内存。这里是在非安全点进行大对象分配
 * 在这个方法内部，会尝试进行young gc，不会进行full gc
 * 给小对象分配内存是在方法attempt_allocation_slow()中
 * @param word_size
 * @param gc_count_before_ret
 * @param gclocker_retry_count_ret
 * @return
 */
HeapWord* G1CollectedHeap::attempt_allocation_humongous(size_t word_size,
                                                        uint* gc_count_before_ret,
                                                        uint* gclocker_retry_count_ret) {
  // The structure of this method has a lot of similarities to
  // attempt_allocation_slow(). The reason these two were not merged
  // into a single one is that such a method would require several "if
  // allocation is not humongous do this, otherwise do that"
  // conditional paths which would obscure its flow. In fact, an early
  // version of this code did use a unified method which was harder to
  // follow and, as a result, it had subtle bugs that were hard to
  // track down. So keeping these two methods separate allows each to
  // be more readable. It will be good to keep these two in sync as
  // much as possible.

  assert_heap_not_locked_and_not_at_safepoint();
  assert(isHumongous(word_size), "attempt_allocation_humongous() "
         "should only be called for humongous allocations");

  // Humongous objects can exhaust the heap quickly, so we should check if we
  // need to start a marking cycle at each humongous object allocation. We do
  // the check before we do the actual allocation. The reason for doing it
  // before the allocation is that we avoid having to keep track of the newly
  // allocated memory while we do a GC.
  // 每一次大对象的分配以前，都需要检查如果大对象分配了以后是否导致需要进行并发标记。
  // 之所以在分配以前就检查，是为了避免我们在进行gc的时候还需要track这个新分配对象的内存
  if (g1_policy()->need_to_start_conc_mark("concurrent humongous allocation",
                                           word_size)) { //  如果当前没有处在一个标记周期
    // 当前没有marking cycle
    collect(GCCause::_g1_humongous_allocation); // 进行回收，这个回收的原因是大对象的分配
  }

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;

    {
        /**
         * 获取整个的Heap_lock
         */
      MutexLockerEx x(Heap_lock); // 开始持有Heap_lock， 所有的锁都定义在gcLocker.cpp中

      // Given that humongous objects are not allocated in young
      // regions, we'll first try to do the allocation without doing a
      // collection hoping that there's enough space in the heap.
      // 搜索 HeapWord* G1CollectedHeap::humongous_obj_allocate
      result = humongous_obj_allocate(word_size, AllocationContext::current());
      if (result != NULL) {
        return result; // 分配成功，直接返回
      }
      // GC_locker主要对跟GC相关的操作进行并发控制操作，因此涉及到critical region（临界区，不要将临界区和安全点混淆）
      /**
       *    GC_locker::check_active_before_gc() 会将_needs_gc设置为true,
       *    在GC_locker::jni_unlock即退出安全区的时候中，如果发现needs_gc=true，会直接进行gc，然后设置_needs_gc=false
       */
      if (GC_locker::is_active_and_needs_gc()) {
        should_try_gc = false; // 当前处于安全区，并且needs_gc，那么当退出安全区的时候，自然会触发gc，不需要我们处理
      } else {
         // The GCLocker may not be active but the GCLocker initiated
        // GC may not yet have been performed (GCLocker::needs_gc()
        // returns true). In this case we do not try this GC and
        // wait until the GCLocker initiated GC is performed, and
        // then retry the allocation.
        /**
         * 这时候 is_active 是 false，但是由gclocker触发的gc还没有进行，因此 needs_gc 还是等于true
         * 这时候我们在下面会通过stall_until_clear()方法一直等待needs_gc为false
         */
        if (GC_locker::needs_gc()) {
          should_try_gc = false;
        } else {
          // Read the GC count while still holding the Heap_lock.
          gc_count_before = total_collections();
          should_try_gc = true;
        }
      }
    }
    // 分配失败，并且应该尝试进行gc
    if (should_try_gc) {
      // If we failed to allocate the humongous object, we should try to
      // do a collection pause (if we're allowed) in case it reclaims
      // enough space for the allocation to succeed after the pause.
      // 大对象分配失败，我们应该尝试一次转移操作(pause)，这样有可能一次短暂的转移，可以给我们带来可用空间
      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded,
                                   GCCause::_g1_humongous_allocation);
      if (result != NULL) { // 分配成功，这个分配成功可能进行了evacuation pause，可能没进行，总之分配成功了，返回分配结果
        assert(succeeded, "only way to get back a non-NULL result");
        return result;
      }
      /**
       * 分配失败，但是我们其实成功进行了evacuation pause，但是分配失败了
       */
      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
      // 执行到这里，就会重试
    } else {
        // 不应该try gc
      if (*gclocker_retry_count_ret > GCLockerRetryAllocationCount) {
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = total_collections();
        return NULL;
      }
      // The GCLocker is either active or the GCLocker initiated
      // GC has not yet been performed. Stall until it is and
      // then retry the allocation.
      GC_locker::stall_until_clear(); // 一直等到needs_gc为false，然后重新进行一次分配
      (*gclocker_retry_count_ret) += 1;
    }

    // We can reach here if we were unsuccessful in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space.  Give a
    // warning if we seem to be looping forever.

    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::attempt_allocation_humongous() "
              "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

/**
 * 必须处于safepoint才能调用这个方法，即这个方法调用是STW的,调用线程必须是STW的, 方
 *  法是在VM_G1IncCollectionPause这个VM_Operation中执行的，肯定是STW的
**/
HeapWord* G1CollectedHeap::attempt_allocation_at_safepoint(size_t word_size,
                                                           AllocationContext_t context,
                                                           bool expect_null_mutator_alloc_region) {
  // 虚拟机处于safepoint，并且当前线程不是java现成，而是vm线程，比如，gc线程
  assert_at_safepoint(true /* should_be_vm_thread */);
  assert(_allocator->mutator_alloc_region(context)->get() == NULL ||
                                             !expect_null_mutator_alloc_region,
         "the current alloc region was unexpectedly found to be non-NULL");
  // 不是大对象
  if (!isHumongous(word_size)) {
    return _allocator->mutator_alloc_region(context)->attempt_allocation_locked(word_size,
                                                      false /* bot_updates */);
  } else {
    HeapWord* result = humongous_obj_allocate(word_size, context);
    if (result != NULL && g1_policy()->need_to_start_conc_mark("STW humongous allocation")) {
      g1_policy()->set_initiate_conc_mark_if_possible(); //如果当前的内存状态需要进行一次新的并发标记，那么就设置_initiate_conc_mark_if_possible为true
    }
    return result;
  }

  ShouldNotReachHere();
}

class PostMCRemSetClearClosure: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  ModRefBarrierSet* _mr_bs;
public:
  PostMCRemSetClearClosure(G1CollectedHeap* g1h, ModRefBarrierSet* mr_bs) :
    _g1h(g1h), _mr_bs(mr_bs) {}

  bool doHeapRegion(HeapRegion* r) {
    HeapRegionRemSet* hrrs = r->rem_set();

    if (r->continuesHumongous()) {
      // We'll assert that the strong code root list and RSet is empty
      assert(hrrs->strong_code_roots_list_length() == 0, "sanity");
      assert(hrrs->occupied() == 0, "RSet should be empty");
      return false;
    }

    _g1h->reset_gc_time_stamps(r);
    hrrs->clear();
    // You might think here that we could clear just the cards
    // corresponding to the used region.  But no: if we leave a dirty card
    // in a region we might allocate into, then it would prevent that card
    // from being enqueued, and cause it to be missed.
    // Re: the performance cost: we shouldn't be doing full GC anyway!
    _mr_bs->clear(MemRegion(r->bottom(), r->end()));

    return false;
  }
};

void G1CollectedHeap::clear_rsets_post_compaction() {
  PostMCRemSetClearClosure rs_clear(this, g1_barrier_set());
  heap_region_iterate(&rs_clear);
}

class RebuildRSOutOfRegionClosure: public HeapRegionClosure {
  G1CollectedHeap*   _g1h;
  UpdateRSOopClosure _cl;
  int                _worker_i;
public:
  RebuildRSOutOfRegionClosure(G1CollectedHeap* g1, int worker_i = 0) :
    _cl(g1->g1_rem_set(), worker_i),
    _worker_i(worker_i),
    _g1h(g1)
  { }

  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      _cl.set_from(r);
      r->oop_iterate(&_cl);
    }
    return false;
  }
};

class ParRebuildRSTask: public AbstractGangTask {
  G1CollectedHeap* _g1;
public:
  ParRebuildRSTask(G1CollectedHeap* g1)
    : AbstractGangTask("ParRebuildRSTask"),
      _g1(g1)
  { }

  void work(uint worker_id) {
    RebuildRSOutOfRegionClosure rebuild_rs(_g1, worker_id);
    _g1->heap_region_par_iterate_chunked(&rebuild_rs, worker_id,
                                          _g1->workers()->active_workers(),
                                         HeapRegion::RebuildRSClaimValue);
  }
};

class PostCompactionPrinterClosure: public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  bool doHeapRegion(HeapRegion* hr) {
    assert(!hr->is_young(), "not expecting to find young regions");
    if (hr->is_free()) {
      // We only generate output for non-empty regions.
    } else if (hr->startsHumongous()) {
      if (hr->region_num() == 1) {
        // single humongous region
        _hr_printer->post_compaction(hr, G1HRPrinter::SingleHumongous);
      } else {
        _hr_printer->post_compaction(hr, G1HRPrinter::StartsHumongous);
      }
    } else if (hr->continuesHumongous()) {
      _hr_printer->post_compaction(hr, G1HRPrinter::ContinuesHumongous);
    } else if (hr->is_old()) {
      _hr_printer->post_compaction(hr, G1HRPrinter::Old);
    } else {
      ShouldNotReachHere();
    }
    return false;
  }

  PostCompactionPrinterClosure(G1HRPrinter* hr_printer)
    : _hr_printer(hr_printer) { }
};

void G1CollectedHeap::print_hrm_post_compaction() {
  PostCompactionPrinterClosure cl(hr_printer());
  heap_region_iterate(&cl);
}

/**
 * full gc
 * 这个方法要求调用线程必须是vm_thread，即必须处在一个VMOperation中执行
 * 这个方法主要是在 G1CollectedHeap::satisfy_failed_allocation()(在VM_G1CollectForAllocation这个VM_Operation中) 中
 *    和G1CollectedHeap::do_full_collection（在VM_G1CollectFull这个VM_Operation中调用） 中调用的
 * 在 G1CollectedHeap::satisfy_failed_allocation()中，explicit_gc都为false，表示这次gc的触发并不是一个规律的显式调度触发，而是由于分配失败导致的，因此wordsize不等于0;
 *    因此，在gc完成以后重新resize内存的时候，会考虑这个wordsize(但是看resize_if_necessary_after_full_collection()代码，虽然传入参数wordsize，却似乎没有用到wordsize)，
 *    两次尝试调用satisfy_failed_allocation()中，clear_all_soft_refs第一次先为false，第二次则为true
 * 在G1CollectedHeap::do_full_collection()中，explicit_gc都为true,表示这是为了gc而gc，而不是某次分配失败导致的gc
 *
 * 从代码来看，do_collection()就是进行的STW的full gc，
 *  而 G1CollectedHeap::do_collection_pause_at_safepoint 并不是full gc，虽然也是在safepoint执行的
 *  这是full gc
 **/
bool G1CollectedHeap::do_collection(bool explicit_gc,
                                    bool clear_all_soft_refs,
                                    size_t word_size) {
  // 必须处于安全点
  assert_at_safepoint(true /* should_be_vm_thread */);
  // 当前有线程处于临界区，直接放弃gc，但是放弃的时候一定会将_needs_gc设置为true，从而在临界区状态结束以后，会重新立刻调度gc
  if (GC_locker::check_active_before_gc()) {
    return false;
  }
  // full gc是使用G1MarkSweep进行的
  STWGCTimer* gc_timer = G1MarkSweep::gc_timer();
  gc_timer->register_gc_start();

  SerialOldTracer* gc_tracer = G1MarkSweep::gc_tracer();
  gc_tracer->report_gc_start(gc_cause(), gc_timer->gc_start());

  SvcGCMarker sgcm(SvcGCMarker::FULL);
  ResourceMark rm;

  print_heap_before_gc();
  trace_heap_before_gc(gc_tracer);

  size_t metadata_prev_used = MetaspaceAux::used_bytes();

  verify_region_sets_optional();

  const bool do_clear_all_soft_refs = clear_all_soft_refs ||
                           collector_policy()->should_clear_all_soft_refs();

  ClearedAllSoftRefs casr(do_clear_all_soft_refs, collector_policy());

  {
    IsGCActiveMark x;

    // Timing
    /**
     * GC的原因是GCCause::_java_lang_system_gc的时候，explicit_gc必须是true
     */
    assert(gc_cause() != GCCause::_java_lang_system_gc || explicit_gc, "invariant");
    TraceCPUTime tcpu(G1Log::finer(), true, gclog_or_tty);

    {
      GCTraceTime t(GCCauseString("Full GC", gc_cause()), G1Log::fine(), true, NULL, gc_tracer->gc_id());
      TraceCollectorStats tcs(g1mm()->full_collection_counters());
      TraceMemoryManagerStats tms(true /* fullGC */, gc_cause());

      double start = os::elapsedTime();
      g1_policy()->record_full_collection_start(); // 记录 full gc 开始的时间

      // Note: When we have a more flexible GC logging framework that
      // allows us to add optional attributes to a GC log record we
      // could consider timing and reporting how long we wait in the
      // following two methods.
      wait_while_free_regions_coming();
      // If we start the compaction before the CM threads finish
      // scanning the root regions we might trip them over as we'll
      // be moving objects / updating references. So let's wait until
      // they are done. By telling them to abort, they should complete
      // early.
      _cm->root_regions()->abort();
      _cm->root_regions()->wait_until_scan_finished();
      append_secondary_free_list_if_not_empty_with_lock();

      gc_prologue(true);
      increment_total_collections(true /* full gc */); // full gc计数器加1
        /**
         * full gc和初始标记暂停的时候，都会增加_old_marking_cycles_started的值，标记初始标记和full gc的次数
         */
      increment_old_marking_cycles_started();

      assert(used() == recalculate_used(), "Should be equal");

      verify_before_gc();

      check_bitmaps("Full GC Start");
      pre_full_gc_dump(gc_timer);

      COMPILER2_PRESENT(DerivedPointerTable::clear());

      // Disable discovery and empty the discovered lists
      // for the CM ref processor.
      ref_processor_cm()->disable_discovery();
      ref_processor_cm()->abandon_partial_discovery();
      ref_processor_cm()->verify_no_references_recorded();

      // Abandon current iterations of concurrent marking and concurrent
      // refinement, if any are in progress. We have to do this before
      // wait_until_scan_finished() below.
      concurrent_mark()->abort(); // 中断并发标记

      // Make sure we'll choose a new allocation region afterwards.
      _allocator->release_mutator_alloc_region();
      _allocator->abandon_gc_alloc_regions();
      g1_rem_set()->cleanupHRRS(); // 清理HeapRegionRemSet，查看代码，可以看到这里是清空SparsePRT::cleanup()，这里并不是删除任何元素，而是将SparsePRT中的_cur清空，然后让_cur和_next设置为一致指向_next

      // We should call this after we retire any currently active alloc
      // regions so that all the ALLOC / RETIRE events are generated
      // before the start GC event.
      _hr_printer.start_gc(true /* full */, (size_t) total_collections());

      // We may have added regions to the current incremental collection
      // set between the last GC or pause and now. We need to clear the
      // incremental collection set and then start rebuilding it afresh
      // after this full GC.
      abandon_collection_set(g1_policy()->inc_cset_head());
      g1_policy()->clear_incremental_cset();
      g1_policy()->stop_incremental_cset_building();
      // 在进行Full GC以前，先摧毁所有的RegionSet，比如，将所有的Old Region从_old_sets中删掉，而Young Region和巨型对象的Region不处理
      // 搜索 G1CollectedHeap::tear_down_region_sets
      tear_down_region_sets(false /* free_list_only */);
      g1_policy()->set_gcs_are_young(true);

      // See the comments in g1CollectedHeap.hpp and
      // G1CollectedHeap::ref_processing_init() about
      // how reference processing currently works in G1.

      // Temporarily make discovery by the STW ref processor single threaded (non-MT).
      ReferenceProcessorMTDiscoveryMutator stw_rp_disc_ser(ref_processor_stw(), false);

      // Temporarily clear the STW ref processor's _is_alive_non_header field.
      ReferenceProcessorIsAliveMutator stw_rp_is_alive_null(ref_processor_stw(), NULL);

      ref_processor_stw()->enable_discovery(true /*verify_disabled*/, true /*verify_no_refs*/);
      ref_processor_stw()->setup_policy(do_clear_all_soft_refs);

      // 在这里进行gc的操作
      {
        HandleMark hm;  // Discard invalid handles created during gc
        // 搜索 G1MarkSweep::invoke_at_safepoint
        G1MarkSweep::invoke_at_safepoint(ref_processor_stw(), do_clear_all_soft_refs);
      }

      assert(num_free_regions() == 0, "we should not have added any free regions");
        // 搜索 void G1CollectedHeap::rebuild_region_sets(bool free_list_only)
      rebuild_region_sets(false /* free_list_only */); // 重构RegionSet

      // Enqueue any discovered reference objects that have
      // not been removed from the discovered lists.
      ref_processor_stw()->enqueue_discovered_references();

      COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

      MemoryService::track_memory_usage();

      assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
      ref_processor_stw()->verify_no_references_recorded();

      // Delete metaspaces for unloaded class loaders and clean up loader_data graph
      ClassLoaderDataGraph::purge(); // 释放掉已经卸载的类
      MetaspaceAux::verify_metrics();

      // Note: since we've just done a full GC, concurrent
      // marking is no longer active. Therefore we need not
      // re-enable reference discovery for the CM ref processor.
      // That will be done at the start of the next marking cycle.
      assert(!ref_processor_cm()->discovery_enabled(), "Postcondition");
      ref_processor_cm()->verify_no_references_recorded();

      reset_gc_time_stamp();
      // Since everything potentially moved, we will clear all remembered
      // sets, and clear all cards.  Later we will rebuild remembered
      // sets. We will also reset the GC time stamps of the regions.
      clear_rsets_post_compaction();
      check_gc_time_stamps();

      // Resize the heap if necessary.
      resize_if_necessary_after_full_collection(explicit_gc ? 0 : word_size);

      if (_hr_printer.is_active()) {
        // We should do this after we potentially resize the heap so
        // that all the COMMIT / UNCOMMIT events are generated before
        // the end GC event.

        print_hrm_post_compaction();
        _hr_printer.end_gc(true /* full */, (size_t) total_collections());
      }

      G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
      if (hot_card_cache->use_cache()) {
        hot_card_cache->reset_card_counts();
        hot_card_cache->reset_hot_cache();
      }

      // Rebuild remembered sets of all regions.
      // 如果 ParallelGCThread > 0
      if (G1CollectedHeap::use_parallel_gc_threads()) { // 如果在FullGC的时候使用并行GC线程(注意不是并发)
          // 关于workers，搜索 FlexibleWorkGang* workers() const { return _workers; }
          // 搜索 SharedHeap::SharedHeap(CollectorPolicy* policy_) :,可以看到Workers就是ParallelGCThreads
        uint n_workers =
                //  搜索 int AdaptiveSizePolicy::calc_active_workers(
          AdaptiveSizePolicy::calc_active_workers(workers()->total_workers(),
                                                  workers()->active_workers(), // 搜索 FlexibleWorkGang(const char* name, uint workers，可以看到，active_workers默认是ParallelGCThreads
                                                  Threads::number_of_non_daemon_threads());
        assert(UseDynamicNumberOfGCThreads ||
               n_workers == workers()->total_workers(),
               "If not dynamic should be using all the  workers");
        workers()->set_active_workers(n_workers); // 根据需要修正 active workers，即实际运行的线程数量
        // Set parallel threads in the heap (_n_par_threads) only
        // before a parallel phase and always reset it to 0 after
        // the phase so that the number of parallel threads does
        // no get carried forward to a serial phase where there
        // may be code that is "possibly_parallel".
        set_par_threads(n_workers);

        ParRebuildRSTask rebuild_rs_task(this);
        assert(check_heap_region_claim_values(
               HeapRegion::InitialClaimValue), "sanity check");
        assert(UseDynamicNumberOfGCThreads ||
               workers()->active_workers() == workers()->total_workers(),
               "Unless dynamic should use total workers");
        // Use the most recent number of  active workers
        assert(workers()->active_workers() > 0,
               "Active workers not properly set");
        set_par_threads(workers()->active_workers());
        workers()->run_task(&rebuild_rs_task); //  在STW的状态下，并行进行RSet的重建
        set_par_threads(0);
        assert(check_heap_region_claim_values(
               HeapRegion::RebuildRSClaimValue), "sanity check");
        reset_heap_region_claim_values();
      } else {
        RebuildRSOutOfRegionClosure rebuild_rs(this);
        heap_region_iterate(&rebuild_rs);
      }

      // Rebuild the strong code root lists for each region
      rebuild_strong_code_roots();

      // Purge code root memory
      purge_code_root_memory();

      if (true) { // FIXME
        MetaspaceGC::compute_new_size();
      }

#ifdef TRACESPINNING
      ParallelTaskTerminator::print_termination_counts();
#endif

      // Discard all rset updates
      JavaThread::dirty_card_queue_set().abandon_logs();
      assert(dirty_card_queue_set().completed_buffers_num() == 0, "DCQS should be empty");

      _young_list->reset_sampled_info();
      // At this point there should be no regions in the
      // entire heap tagged as young.
      assert(check_young_list_empty(true /* check_heap */),
             "young list should be empty at this point");

      // Update the number of full collections that have been completed.
      increment_old_marking_cycles_completed(false /* concurrent */); // 更新已经完成的Full GC的数量，同时在锁 FullGCCount_lock 上通知，通知那些在锁FullGCCount_lock上等待的操作，比如，System.gc()

      _hrm.verify_optional();
      verify_region_sets_optional();

      verify_after_gc();

      // Clear the previous marking bitmap, if needed for bitmap verification.
      // Note we cannot do this when we clear the next marking bitmap in
      // ConcurrentMark::abort() above since VerifyDuringGC verifies the
      // objects marked during a full GC against the previous bitmap.
      // But we need to clear it before calling check_bitmaps below since
      // the full GC has compacted objects and updated TAMS but not updated
      // the prev bitmap.
      if (G1VerifyBitmaps) {
        ((CMBitMap*) concurrent_mark()->prevMarkBitMap())->clearAll();
      }
      check_bitmaps("Full GC End");

      // Start a new incremental collection set for the next pause
      assert(g1_policy()->collection_set() == NULL, "must be");
      g1_policy()->start_incremental_cset_building();

      clear_cset_fast_test();

      _allocator->init_mutator_alloc_region();

      double end = os::elapsedTime();
      g1_policy()->record_full_collection_end();

      if (G1Log::fine()) {
        g1_policy()->print_heap_transition();
      }

      // We must call G1MonitoringSupport::update_sizes() in the same scoping level
      // as an active TraceMemoryManagerStats object (i.e. before the destructor for the
      // TraceMemoryManagerStats is called) so that the G1 memory pools are updated
      // before any GC notifications are raised.
      g1mm()->update_sizes();
      /**
       * 做一些收尾工作，比如，可能会重新设置tlab的大小
       * 搜索 G1CollectedHeap::gc_epilogue
       */
      gc_epilogue(true);
    }

    if (G1Log::finer()) {
      g1_policy()->print_detailed_heap_transition(true /* full */);
    }

    print_heap_after_gc();
    trace_heap_after_gc(gc_tracer);

    post_full_gc_dump(gc_timer);

    gc_timer->register_gc_end();
    gc_tracer->report_gc_end(gc_timer->gc_end(), gc_timer->time_partitions());
  }

  return true;
}

/**
 * STW的full gc。
 * 在VM_G1CollectFull::doit()中被调用，调用的时候clear_all_soft_refs=false
 *
 * 与
 * @param clear_all_soft_refs
 */
void G1CollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  // do_collection() will return whether it succeeded in performing
  // the GC. Currently, there is no facility on the
  // do_full_collection() API to notify the caller than the collection
  // did not succeed (e.g., because it was locked out by the GC
  // locker). So, right now, we'll ignore the return value.
  /**
   * 搜索 G1CollectedHeap::do_collection
   * do_collection() 将返回是否成功执行 GC
   */
  bool dummy = do_collection(true,                /* explicit_gc */
                             clear_all_soft_refs,
                             0                    /* word_size */);
}

// This code is mostly copied from TenuredGeneration.
void
G1CollectedHeap::
resize_if_necessary_after_full_collection(size_t word_size) {
  // Include the current allocation, if any, and bytes that will be
  // pre-allocated to support collections, as "used".
  const size_t used_after_gc = used();
  const size_t capacity_after_gc = capacity();
  const size_t free_after_gc = capacity_after_gc - used_after_gc;

  // This is enforced in arguments.cpp.
  assert(MinHeapFreeRatio <= MaxHeapFreeRatio,
         "otherwise the code below doesn't make sense");

  // We don't have floating point command-line arguments
  const double minimum_free_percentage = (double) MinHeapFreeRatio / 100.0;
  const double maximum_used_percentage = 1.0 - minimum_free_percentage;
  const double maximum_free_percentage = (double) MaxHeapFreeRatio / 100.0;
  const double minimum_used_percentage = 1.0 - maximum_free_percentage;

  const size_t min_heap_size = collector_policy()->min_heap_byte_size();
  const size_t max_heap_size = collector_policy()->max_heap_byte_size();

  // We have to be careful here as these two calculations can overflow
  // 32-bit size_t's.
  double used_after_gc_d = (double) used_after_gc;
  double minimum_desired_capacity_d = used_after_gc_d / maximum_used_percentage;
  double maximum_desired_capacity_d = used_after_gc_d / minimum_used_percentage;

  // Let's make sure that they are both under the max heap size, which
  // by default will make them fit into a size_t.
  double desired_capacity_upper_bound = (double) max_heap_size;
  minimum_desired_capacity_d = MIN2(minimum_desired_capacity_d,
                                    desired_capacity_upper_bound);
  maximum_desired_capacity_d = MIN2(maximum_desired_capacity_d,
                                    desired_capacity_upper_bound);

  // We can now safely turn them into size_t's.
  size_t minimum_desired_capacity = (size_t) minimum_desired_capacity_d;
  size_t maximum_desired_capacity = (size_t) maximum_desired_capacity_d;

  // This assert only makes sense here, before we adjust them
  // with respect to the min and max heap size.
  assert(minimum_desired_capacity <= maximum_desired_capacity,
         err_msg("minimum_desired_capacity = " SIZE_FORMAT ", "
                 "maximum_desired_capacity = " SIZE_FORMAT,
                 minimum_desired_capacity, maximum_desired_capacity));

  // Should not be greater than the heap max size. No need to adjust
  // it with respect to the heap min size as it's a lower bound (i.e.,
  // we'll try to make the capacity larger than it, not smaller).
  minimum_desired_capacity = MIN2(minimum_desired_capacity, max_heap_size);
  // Should not be less than the heap min size. No need to adjust it
  // with respect to the heap max size as it's an upper bound (i.e.,
  // we'll try to make the capacity smaller than it, not greater).
  maximum_desired_capacity =  MAX2(maximum_desired_capacity, min_heap_size);

  if (capacity_after_gc < minimum_desired_capacity) {
    // Don't expand unless it's significant
    size_t expand_bytes = minimum_desired_capacity - capacity_after_gc;
    ergo_verbose4(ErgoHeapSizing,
                  "attempt heap expansion",
                  ergo_format_reason("capacity lower than "
                                     "min desired capacity after Full GC")
                  ergo_format_byte("capacity")
                  ergo_format_byte("occupancy")
                  ergo_format_byte_perc("min desired capacity"),
                  capacity_after_gc, used_after_gc,
                  minimum_desired_capacity, (double) MinHeapFreeRatio);
    expand(expand_bytes);

    // No expansion, now see if we want to shrink
  } else if (capacity_after_gc > maximum_desired_capacity) {
    // Capacity too large, compute shrinking size
    size_t shrink_bytes = capacity_after_gc - maximum_desired_capacity;
    ergo_verbose4(ErgoHeapSizing,
                  "attempt heap shrinking",
                  ergo_format_reason("capacity higher than "
                                     "max desired capacity after Full GC")
                  ergo_format_byte("capacity")
                  ergo_format_byte("occupancy")
                  ergo_format_byte_perc("max desired capacity"),
                  capacity_after_gc, used_after_gc,
                  maximum_desired_capacity, (double) MaxHeapFreeRatio);
    shrink(shrink_bytes);
  }
}


/**
 * 这个方法专门处理来自 VM_G1CollectForAllocation doit()操作的回调，由于是在VMOperation中调用的，因此调用线程是vm_thread，因此是STW的
 * 该函数会执行所有必要/可能的操作来满足失败的分配请求（包括垃圾收集、内存扩展等）
 * @param word_size
 * @param context
 * @param succeeded
 * @return
 */
HeapWord*
G1CollectedHeap::satisfy_failed_allocation(size_t word_size,
                                           AllocationContext_t context,
                                           bool* succeeded) {
  assert_at_safepoint(true /* should_be_vm_thread */); // 必须已经处于安全点，并且要求当前线程是vm_thread，而不是java_thread或者其他线程，因为这个方法肯定是在一个VMOpreation中执行的

  *succeeded = true;
  // Let's attempt the allocation first.
  /**
   * 尝试在安全点进行内存分配，这里的意思是，刚刚是由于普通的内存分配失败，因此触发VM_G1CollectForAllocation op，
   * 现在，我们已经处于安全点，我们现尝试在安全点分配一次，看看是否成功
   */

  HeapWord* result =
    attempt_allocation_at_safepoint(word_size,
                                    context,
                                    false /* expect_null_mutator_alloc_region */);
  if (result != NULL) { // 成功地在安全点进行分配
    assert(*succeeded, "sanity");
    return result;
  }

  // In a G1 heap, we're supposed to keep allocation from failing by
  // incremental pauses.  Therefore, at least for now, we'll favor
  // expansion over collection.  (This might change in the future if we can
  // do something smarter than full collection to satisfy a failed alloc.)
  /**
   * 在 G1 堆中，我们应该通过增量暂停来防止分配失败。 因此，至少现在，我们更倾向于扩展而不是收集。
   * （如果我们可以做一些比完整收集更聪明的事情来满足失败的分配，那么将来这可能会改变。）
   * 所以，从目前的代码来讲，一次分配失败导致的gc一定是full gc
   */
  result = expand_and_allocate(word_size, context); // 内存扩展，再尝试分配
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  // Expansion didn't work, we'll try to do a Full GC.
  // 直接进行STW的full gc。所以，由于allocation failure导致的gc其实是STW 的 full gc
  bool gc_succeeded = do_collection(false, /* explicit_gc */
                                    false, /* clear_all_soft_refs */
                                    word_size);
  if (!gc_succeeded) {
    *succeeded = false;
    return NULL;
  }

  // Retry the allocation
  result = attempt_allocation_at_safepoint(word_size,
                                           context,
                                           true /* expect_null_mutator_alloc_region */);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  // Then, try a Full GC that will collect all soft references.
  /**
   * 最后再尝试进行full gc，并且清除所有的软引用
   */
  gc_succeeded = do_collection(false, /* explicit_gc */
                               true,  /* clear_all_soft_refs */
                               word_size);
  if (!gc_succeeded) {
    *succeeded = false;
    return NULL;
  }

  // Retry the allocation once more
  // 再进行一次安全点的分配尝试
  result = attempt_allocation_at_safepoint(word_size,
                                           context,
                                           true /* expect_null_mutator_alloc_region */);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  assert(!collector_policy()->should_clear_all_soft_refs(),
         "Flag should have been handled and cleared prior to this point");

  // What else?  We might try synchronous finalization later.  If the total
  // space available is large enough for the allocation, then a more
  // complete compaction phase than we've tried so far might be
  // appropriate.
  assert(*succeeded, "sanity");
  return NULL;
}

// Attempting to expand the heap sufficiently
// to support an allocation of the given "word_size".  If
// successful, perform the allocation and return the address of the
// allocated block, or else "NULL".

HeapWord* G1CollectedHeap::expand_and_allocate(size_t word_size, AllocationContext_t context) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  verify_region_sets_optional();

  size_t expand_bytes = MAX2(word_size * HeapWordSize, MinHeapDeltaBytes);
  ergo_verbose1(ErgoHeapSizing,
                "attempt heap expansion",
                ergo_format_reason("allocation request failed")
                ergo_format_byte("allocation request"),
                word_size * HeapWordSize);
  if (expand(expand_bytes)) {
    _hrm.verify_optional();
    verify_region_sets_optional();
    return attempt_allocation_at_safepoint(word_size,
                                           context,
                                           false /* expect_null_mutator_alloc_region */);
  }
  return NULL;
}

/**
 * 堆内存扩展必须发生在safepoint中
 * @param expand_bytes 需要扩展的字节数
 * @return
 */
bool G1CollectedHeap::expand(size_t expand_bytes) {
  size_t aligned_expand_bytes = ReservedSpace::page_align_size_up(expand_bytes);
  aligned_expand_bytes = align_size_up(aligned_expand_bytes,
                                       HeapRegion::GrainBytes);
  ergo_verbose2(ErgoHeapSizing,
                "expand the heap",
                ergo_format_byte("requested expansion amount")
                ergo_format_byte("attempted expansion amount"),
                expand_bytes, aligned_expand_bytes);

  if (is_maximal_no_gc()) {
    ergo_verbose0(ErgoHeapSizing,
                      "did not expand the heap",
                      ergo_format_reason("heap already fully expanded"));
    return false;
  }
  /**
   * 计算需要扩展的Region的数量
   */
  uint regions_to_expand = (uint)(aligned_expand_bytes / HeapRegion::GrainBytes);
  assert(regions_to_expand > 0, "Must expand by at least one region");
  // 搜索 uint HeapRegionManager::expand_by(uint num_regions)
  uint expanded_by = _hrm.expand_by(regions_to_expand);

  if (expanded_by > 0) {
    size_t actual_expand_bytes = expanded_by * HeapRegion::GrainBytes;
    assert(actual_expand_bytes <= aligned_expand_bytes, "post-condition");
    g1_policy()->record_new_heap_size(num_regions());
  } else {
    ergo_verbose0(ErgoHeapSizing,
                  "did not expand the heap",
                  ergo_format_reason("heap expansion operation failed"));
    // The expansion of the virtual storage space was unsuccessful.
    // Let's see if it was because we ran out of swap.
    if (G1ExitOnExpansionFailure &&
        _hrm.available() >= regions_to_expand) {
      // We had head room...
      vm_exit_out_of_memory(aligned_expand_bytes, OOM_MMAP_ERROR, "G1 heap expansion");
    }
  }
  return regions_to_expand > 0;
}

void G1CollectedHeap::shrink_helper(size_t shrink_bytes) {
  size_t aligned_shrink_bytes =
    ReservedSpace::page_align_size_down(shrink_bytes);
  aligned_shrink_bytes = align_size_down(aligned_shrink_bytes,
                                         HeapRegion::GrainBytes);
  uint num_regions_to_remove = (uint)(shrink_bytes / HeapRegion::GrainBytes);

  uint num_regions_removed = _hrm.shrink_by(num_regions_to_remove);
  size_t shrunk_bytes = num_regions_removed * HeapRegion::GrainBytes;

  ergo_verbose3(ErgoHeapSizing,
                "shrink the heap",
                ergo_format_byte("requested shrinking amount")
                ergo_format_byte("aligned shrinking amount")
                ergo_format_byte("attempted shrinking amount"),
                shrink_bytes, aligned_shrink_bytes, shrunk_bytes);
  if (num_regions_removed > 0) {
    g1_policy()->record_new_heap_size(num_regions());
  } else {
    ergo_verbose0(ErgoHeapSizing,
                  "did not shrink the heap",
                  ergo_format_reason("heap shrinking operation failed"));
  }
}

void G1CollectedHeap::shrink(size_t shrink_bytes) {
  verify_region_sets_optional();

  // We should only reach here at the end of a Full GC which means we
  // should not not be holding to any GC alloc regions. The method
  // below will make sure of that and do any remaining clean up.
  _allocator->abandon_gc_alloc_regions();

  // Instead of tearing down / rebuilding the free lists here, we
  // could instead use the remove_all_pending() method on free_list to
  // remove only the ones that we need to remove.
  tear_down_region_sets(true /* free_list_only */); // 摧毁整个Region Set，但是仅限于解散Free List
  shrink_helper(shrink_bytes);
  rebuild_region_sets(true /* free_list_only */); // 摧毁整个Region Set，但是仅限于重构Free List

  _hrm.verify_optional();
  verify_region_sets_optional();
}

// Public methods.

#ifdef _MSC_VER // the use of 'this' below gets a warning, make it go away
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif // _MSC_VER


G1CollectedHeap::G1CollectedHeap(G1CollectorPolicy* policy_) :
  SharedHeap(policy_), // 首先调用父类SharedHeap:SharedHeap的构造函数，构造相应的FlexiableWorkGang并初始化下面的对应线程
  _g1_policy(policy_),
  _dirty_card_queue_set(false),
  _into_cset_dirty_card_queue_set(false),
  _is_alive_closure_cm(this),
  _is_alive_closure_stw(this),
  _ref_processor_cm(NULL),
  _ref_processor_stw(NULL),
  _bot_shared(NULL),
  _evac_failure_scan_stack(NULL),
  _mark_in_progress(false),
  _cg1r(NULL),
  _g1mm(NULL),
  _refine_cte_cl(NULL),
  _full_collection(false),
  _secondary_free_list("Secondary Free List", new SecondaryFreeRegionListMtSafeChecker()),
  _old_set("Old Set", false /* humongous */, new OldRegionSetMtSafeChecker()),
  _humongous_set("Master Humongous Set", true /* humongous */, new HumongousRegionSetMtSafeChecker()),
  _humongous_reclaim_candidates(),
  _has_humongous_reclaim_candidates(false),
  _free_regions_coming(false),
  _young_list(new YoungList(this)),
  _gc_time_stamp(0),
  _survivor_plab_stats(YoungPLABSize, PLABWeight),
  _old_plab_stats(OldPLABSize, PLABWeight),
  _expand_heap_after_alloc_failure(true), // 分配失败以后会尝试扩展堆内存
  _surviving_young_words(NULL),
  _old_marking_cycles_started(0),
  _old_marking_cycles_completed(0),
  _concurrent_cycle_started(false),
  _heap_summary_sent(false),
  _in_cset_fast_test(),
  _dirty_cards_region_list(NULL),
  _worker_cset_start_region(NULL),
  _worker_cset_start_region_time_stamp(NULL),
  _gc_timer_stw(new (ResourceObj::C_HEAP, mtGC) STWGCTimer()),
  _gc_timer_cm(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
  _gc_tracer_stw(new (ResourceObj::C_HEAP, mtGC) G1NewTracer()),
  _gc_tracer_cm(new (ResourceObj::C_HEAP, mtGC) G1OldTracer()) {

  _g1h = this;

  _allocator = G1Allocator::create_allocator(_g1h);
  _humongous_object_threshold_in_words = HeapRegion::GrainWords / 2;

  int n_queues = MAX2((int)ParallelGCThreads, 1);
  // typedef GenericTaskQueueSet<RefToScanQueue, mtGC> RefToScanQueueSet;
  _task_queues = new RefToScanQueueSet(n_queues); // 每一个GC线程会有一个独立的task queue，因此，RefToScanQueueSet中RefToScanQueue的大小是GC线程并发数决定的

  uint n_rem_sets = HeapRegionRemSet::num_par_rem_sets();
  assert(n_rem_sets > 0, "Invariant.");

  _worker_cset_start_region = NEW_C_HEAP_ARRAY(HeapRegion*, n_queues, mtGC);
  _worker_cset_start_region_time_stamp = NEW_C_HEAP_ARRAY(uint, n_queues, mtGC);
  _evacuation_failed_info_array = NEW_C_HEAP_ARRAY(EvacuationFailedInfo, n_queues, mtGC);
  // 构造 RefToScanQueue
  // typedef OverflowTaskQueue<StarTask, mtGC>         RefToScanQueue;
  // typedef GenericTaskQueueSet<RefToScanQueue, mtGC> RefToScanQueueSet;
  for (int i = 0; i < n_queues; i++) {
    RefToScanQueue* q = new RefToScanQueue();
    q->initialize();
    _task_queues->register_queue(i, q);
    ::new (&_evacuation_failed_info_array[i]) EvacuationFailedInfo();
  }
  clear_cset_start_regions();

  // Initialize the G1EvacuationFailureALot counters and flags.
  NOT_PRODUCT(reset_evacuation_should_fail();)

  guarantee(_task_queues != NULL, "task_queues allocation failure.");
}

/**
 * 创建辅助内存区域，主要是在G1CollectedHeap 中创建相关辅助内存，比如，卡表，热卡片的卡片计数器表等
 * @param description
 * @param size
 * @param translation_factor
 * @return
 */
G1RegionToSpaceMapper* G1CollectedHeap::create_aux_memory_mapper(const char* description,
                                                                 size_t size,
                                                                 size_t translation_factor) {
  size_t preferred_page_size = os::page_size_for_region_unaligned(size, 1);
  // Allocate a new reserved space, preferring to use large pages.
  ReservedSpace rs(size, preferred_page_size);
  G1RegionToSpaceMapper* result  = // 搜索 G1RegionToSpaceMapper* G1RegionToSpaceMapper::create_mapper 查看具体实现
    G1RegionToSpaceMapper::create_mapper(rs,
                                         size,
                                         rs.alignment(),
                                         HeapRegion::GrainBytes,
                                         translation_factor,
                                         mtGC);
  if (TracePageSizes) {
    gclog_or_tty->print_cr("G1 '%s': pg_sz=" SIZE_FORMAT " base=" PTR_FORMAT " size=" SIZE_FORMAT " alignment=" SIZE_FORMAT " reqsize=" SIZE_FORMAT,
                           description, preferred_page_size, p2i(rs.base()), rs.size(), rs.alignment(), size);
  }
  return result;
}
/**
 * 在这里会构造CocurrentG1Refine，进而在CocurrentG1Refine
 */
jint G1CollectedHeap::initialize() {
  CollectedHeap::pre_initialize();
  os::enable_vtime();

  G1Log::init();

  // Necessary to satisfy locking discipline assertions.

  MutexLocker x(Heap_lock);

  // We have to initialize the printer before committing the heap, as
  // it will be used then.
  _hr_printer.set_active(G1PrintHeapRegions);

  // While there are no constraints in the GC code that HeapWordSize
  // be any particular value, there are multiple other areas in the
  // system which believe this to be true (e.g. oop->object_size in some
  // cases incorrectly returns the size in wordSize units rather than
  // HeapWordSize).
  guarantee(HeapWordSize == wordSize, "HeapWordSize must equal wordSize");

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size(); // 最大堆内存大小
  size_t heap_alignment = collector_policy()->heap_alignment();

  // Ensure that the sizes are properly aligned.
  Universe::check_alignment(init_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(max_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(max_byte_size, heap_alignment, "g1 heap");

  _refine_cte_cl = new RefineCardTableEntryClosure();

  /**
   * 构造ConcurrentG1Refine，进而会负责构造 ConcurrentG1RefineThread线程池， 同时设置对应的DCQS的白绿黄红区域
   * 这里的 _refine_cte_cl 是 RefineCardTableEntryClosure
   */
  _cg1r = new ConcurrentG1Refine(this, _refine_cte_cl);

  // Reserve the maximum.

  // When compressed oops are enabled, the preferred heap base
  // is calculated by subtracting the requested size from the
  // 32Gb boundary and using the result as the base address for
  // heap reservation. If the requested size is not aligned to
  // HeapRegion::GrainBytes (i.e. the alignment that is passed
  // into the ReservedHeapSpace constructor) then the actual
  // base of the reserved heap may end up differing from the
  // address that was requested (i.e. the preferred heap base).
  // If this happens then we could end up using a non-optimal
  // compressed oops mode.
  // 搜索 Universe::reserve_heap
  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
                                                 heap_alignment);

  // It is important to do this in a way such that concurrent readers can't
  // temporarily think something is in the heap.  (I've actually seen this
  // happen in asserts: DLD.)
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*)(heap_rs.base() + heap_rs.size()));

  // Create the gen rem set (and barrier set) for the entire reserved region.
  // 创建整个Heap的RSet 搜索 CollectorPolicy::create_rem_set
  _rem_set = collector_policy()->create_rem_set(_reserved, 2);// CardTableRS，搜索 CollectorPolicy::create_rem_set
  //  搜索 CardTableRS::CardTableRS 的构造方法，看到_barrier_set是 G1SATBCardTableLoggingModRefBS
  set_barrier_set(rem_set()->bs()); // 设置BarrierSet的实现类，可以看到，BarrierSet的实现类来自于RSet
  if (!barrier_set()->is_a(BarrierSet::G1SATBCTLogging)) {
    vm_exit_during_initialization("G1 requires a G1SATBLoggingCardTableModRefBS");
    return JNI_ENOMEM;
  }
  // 搜索 G1SATBCardTableLoggingModRefBS* g1_barrier_set()
  // Also create a G1 rem set.
  _g1_rem_set = new G1RemSet(this, g1_barrier_set());

  // Carve out the G1 part of the heap.

  ReservedSpace g1_rs = heap_rs.first_part(max_byte_size); // ReservedSpace::first_part

  G1RegionToSpaceMapper* heap_storage =
    G1RegionToSpaceMapper::create_mapper(g1_rs,
                                         g1_rs.size(),
                                         UseLargePages ? os::large_page_size() : os::vm_page_size(),
                                         HeapRegion::GrainBytes,
                                         1,
                                         mtJavaHeap);
  heap_storage->set_mapping_changed_listener(&_listener);

  // Create storage for the BOT, card table, card counts table (hot card cache) and the bitmaps.
  // 这一片区域存放块偏移表
  G1RegionToSpaceMapper* bot_storage =
    create_aux_memory_mapper("Block offset table", // // 搜索 G1BlockOffsetSharedArray::compute_size
                             G1BlockOffsetSharedArray::compute_size(g1_rs.size() / HeapWordSize),
                             G1BlockOffsetSharedArray::N_bytes);

  // 这一片内存区域存放卡表
  ReservedSpace cardtable_rs(G1SATBCardTableLoggingModRefBS::compute_size(g1_rs.size() / HeapWordSize));
  G1RegionToSpaceMapper* cardtable_storage =
    create_aux_memory_mapper("Card table", // 搜索 G1SATBCardTableLoggingModRefBS::compute_size
                             G1SATBCardTableLoggingModRefBS::compute_size(g1_rs.size() / HeapWordSize),
                             G1BlockOffsetSharedArray::N_bytes);

  // 这一片内存区域存放热卡片的卡片计数器表
  G1RegionToSpaceMapper* card_counts_storage =
    create_aux_memory_mapper("Card counts table", // 搜索 G1BlockOffsetSharedArray::compute_size
                             G1BlockOffsetSharedArray::compute_size(g1_rs.size() / HeapWordSize),
                             G1BlockOffsetSharedArray::N_bytes);
  // 搜索 CMBitMap::compute_size
  size_t bitmap_size = CMBitMap::compute_size(g1_rs.size());
  G1RegionToSpaceMapper* prev_bitmap_storage =
    create_aux_memory_mapper("Prev Bitmap", bitmap_size, CMBitMap::mark_distance());
  G1RegionToSpaceMapper* next_bitmap_storage =
    create_aux_memory_mapper("Next Bitmap", bitmap_size, CMBitMap::mark_distance());

  _hrm.initialize(heap_storage, prev_bitmap_storage, next_bitmap_storage, bot_storage, cardtable_storage, card_counts_storage);
  g1_barrier_set()->initialize(cardtable_storage);
   // Do later initialization work for concurrent refinement.
  _cg1r->init(card_counts_storage);

  // 6843694 - ensure that the maximum region index can fit
  // in the remembered set structures.
  const uint max_region_idx = (1U << (sizeof(RegionIdx_t)*BitsPerByte-1)) - 1;
  guarantee((max_regions() - 1) <= max_region_idx, "too many regions");

  size_t max_cards_per_region = ((size_t)1 << (sizeof(CardIdx_t)*BitsPerByte-1)) - 1;
  guarantee(HeapRegion::CardsPerRegion > 0, "make sure it's initialized");
  guarantee(HeapRegion::CardsPerRegion < max_cards_per_region,
            "too many cards per region");

  FreeRegionList::set_unrealistically_long_length(max_regions() + 1);
  /**
   * 构造 G1BlockOffsetSharedArray对象，可以看到，整个heap只有一个G1BlockOffsetSharedArray对象
   */
  _bot_shared = new G1BlockOffsetSharedArray(_reserved, bot_storage);

  _g1h = this;

  {
    HeapWord* start = _hrm.reserved().start();
    HeapWord* end = _hrm.reserved().end();
    size_t granularity = HeapRegion::GrainBytes;

    _in_cset_fast_test.initialize(start, end, granularity);
    _humongous_reclaim_candidates.initialize(start, end, granularity);
  }

  // Create the ConcurrentMark data structure and thread.
  // (Must do this late, so that "max_regions" is defined.)
  _cm = new ConcurrentMark(this, prev_bitmap_storage, next_bitmap_storage);
  if (_cm == NULL || !_cm->completed_initialization()) {
    vm_shutdown_during_initialization("Could not create/initialize ConcurrentMark");
    return JNI_ENOMEM;
  }
  _cmThread = _cm->cmThread();

  // Initialize the from_card cache structure of HeapRegionRemSet.
  HeapRegionRemSet::init_heap(max_regions());

  // Now expand into the initial heap size.
  if (!expand(init_byte_size)) {
    vm_shutdown_during_initialization("Failed to allocate initial heap.");
    return JNI_ENOMEM;
  }

  // Perform any initialization actions delegated to the policy.
  g1_policy()->init();

  /**
   * 初始化全局的JavaThread的SATB Mark Queue Set
   */
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               G1SATBProcessCompletedThreshold,
                                               Shared_SATB_Q_lock);

  /**
   * 搜索 void DirtyCardQueueSet::initialize 查看初始化的过程
   * 初始化全局的JavaThread的DCQS
   */
  JavaThread::dirty_card_queue_set().initialize(_refine_cte_cl,
                                                DirtyCardQ_CBL_mon,
                                                DirtyCardQ_FL_lock,
                                                concurrent_g1_refine()->yellow_zone(),
                                                concurrent_g1_refine()->red_zone(),
                                                Shared_DirtyCardQ_lock);

  /**
   *  搜索 void DirtyCardQueueSet::initialize 查看初始化的过程
   *  在构造方法 G1CollectedHeap::G1CollectedHeap 中调用
   */
  dirty_card_queue_set().initialize(NULL, // Should never be called by the Java code; _mut_process_closure == null
                                    DirtyCardQ_CBL_mon,
                                    DirtyCardQ_FL_lock,
                                    -1, // never trigger processing
                                    -1, // no limit on length
                                    Shared_DirtyCardQ_lock,
                                    &JavaThread::dirty_card_queue_set());

  // Initialize the card queue set used to hold cards containing
  // references into the collection set.
  /**
   *  搜索 void DirtyCardQueueSet::initialize 查看初始化的过程
   *  在构造方法 G1CollectedHeap::G1CollectedHeap 中调用
   */
  _into_cset_dirty_card_queue_set.initialize(NULL, // Should never be called by the Java code
                                             DirtyCardQ_CBL_mon,
                                             DirtyCardQ_FL_lock,
                                             -1, // never trigger processing
                                             -1, // no limit on length
                                             Shared_DirtyCardQ_lock,
                                             &JavaThread::dirty_card_queue_set());

  // In case we're keeping closure specialization stats, initialize those
  // counts and that mechanism.
  SpecializationStats::clear();

  // Here we allocate the dummy HeapRegion that is required by the
  // G1AllocRegion class.
  HeapRegion* dummy_region = _hrm.get_dummy_region(); // 创建一个dummy region

  // We'll re-use the same region whether the alloc region will
  // require BOT updates or not and, if it doesn't, then a non-young
  // region will complain that it cannot support allocations without
  // BOT updates. So we'll tag the dummy region as eden to avoid that.
  dummy_region->set_eden();
  // Make sure it's full.
  dummy_region->set_top(dummy_region->end());
  G1AllocRegion::setup(this, dummy_region);

  _allocator->init_mutator_alloc_region();

  // Do create of the monitoring and management support so that
  // values in the heap have been properly initialized.
  _g1mm = new G1MonitoringSupport(this);

  G1StringDedup::initialize();

  return JNI_OK;
}

void G1CollectedHeap::stop() {
  // Stop all concurrent threads. We do this to make sure these threads
  // do not continue to execute and access resources (e.g. gclog_or_tty)
  // that are destroyed during shutdown.
  _cg1r->stop();
  _cmThread->stop();
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::stop();
  }
}

size_t G1CollectedHeap::conservative_max_heap_alignment() {
  return HeapRegion::max_region_size();
}

void G1CollectedHeap::ref_processing_init() {
  // Reference processing in G1 currently works as follows:
  //
  // * There are two reference processor instances. One is
  //   used to record and process discovered references
  //   during concurrent marking; the other is used to
  //   record and process references during STW pauses
  //   (both full and incremental).
  // * Both ref processors need to 'span' the entire heap as
  //   the regions in the collection set may be dotted around.
  //
  // * For the concurrent marking ref processor:
  //   * Reference discovery is enabled at initial marking.
  //   * Reference discovery is disabled and the discovered
  //     references processed etc during remarking.
  //   * Reference discovery is MT (see below).
  //   * Reference discovery requires a barrier (see below).
  //   * Reference processing may or may not be MT
  //     (depending on the value of ParallelRefProcEnabled
  //     and ParallelGCThreads).
  //   * A full GC disables reference discovery by the CM
  //     ref processor and abandons any entries on it's
  //     discovered lists.
  //
  // * For the STW processor:
  //   * Non MT discovery is enabled at the start of a full GC.
  //   * Processing and enqueueing during a full GC is non-MT.
  //   * During a full GC, references are processed after marking.
  //
  //   * Discovery (may or may not be MT) is enabled at the start
  //     of an incremental evacuation pause.
  //   * References are processed near the end of a STW evacuation pause.
  //   * For both types of GC:
  //     * Discovery is atomic - i.e. not concurrent.
  //     * Reference discovery will not need a barrier.

  SharedHeap::ref_processing_init();
  MemRegion mr = reserved_region();

  // Concurrent Mark ref processor
  _ref_processor_cm =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ParallelGCThreads > 1),
                                // mt processing
                           (int) ParallelGCThreads,
                                // degree of mt processing
                           (ParallelGCThreads > 1) || (ConcGCThreads > 1),
                                // mt discovery
                           (int) MAX2(ParallelGCThreads, ConcGCThreads),
                                // degree of mt discovery
                           false,
                                // Reference discovery is not atomic
                           &_is_alive_closure_cm);
                                // is alive closure
                                // (for efficiency/performance)

  // STW ref processor
  _ref_processor_stw =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ParallelGCThreads > 1),
                                // mt processing
                           MAX2((int)ParallelGCThreads, 1),
                                // degree of mt processing
                           (ParallelGCThreads > 1),
                                // mt discovery
                           MAX2((int)ParallelGCThreads, 1),
                                // degree of mt discovery
                           true,
                                // Reference discovery is atomic
                           &_is_alive_closure_stw);
                                // is alive closure
                                // (for efficiency/performance)
}

size_t G1CollectedHeap::capacity() const {
  return _hrm.length() * HeapRegion::GrainBytes;
}

void G1CollectedHeap::reset_gc_time_stamps(HeapRegion* hr) {
  assert(!hr->continuesHumongous(), "pre-condition");
  hr->reset_gc_time_stamp();
  if (hr->startsHumongous()) {
    uint first_index = hr->hrm_index() + 1;
    uint last_index = hr->last_hc_index();
    for (uint i = first_index; i < last_index; i += 1) {
      HeapRegion* chr = region_at(i);
      assert(chr->continuesHumongous(), "sanity");
      chr->reset_gc_time_stamp();
    }
  }
}

#ifndef PRODUCT
class CheckGCTimeStampsHRClosure : public HeapRegionClosure {
private:
  unsigned _gc_time_stamp;
  bool _failures;

public:
  CheckGCTimeStampsHRClosure(unsigned gc_time_stamp) :
    _gc_time_stamp(gc_time_stamp), _failures(false) { }

  virtual bool doHeapRegion(HeapRegion* hr) {
    unsigned region_gc_time_stamp = hr->get_gc_time_stamp();
    if (_gc_time_stamp != region_gc_time_stamp) {
      gclog_or_tty->print_cr("Region " HR_FORMAT " has GC time stamp = %d, "
                             "expected %d", HR_FORMAT_PARAMS(hr),
                             region_gc_time_stamp, _gc_time_stamp);
      _failures = true;
    }
    return false;
  }

  bool failures() { return _failures; }
};

void G1CollectedHeap::check_gc_time_stamps() {
  CheckGCTimeStampsHRClosure cl(_gc_time_stamp);
  heap_region_iterate(&cl);
  guarantee(!cl.failures(), "all GC time stamps should have been reset");
}
#endif // PRODUCT

/**
 * 调用位置 查看 G1RemSet::updateRS
 * RefineRecordRefsIntoCSCardTableEntryClosure 是 CardTableEntryClosure的子类，构造函数中包含了一个DCQ
 * @param cl 使用 RefineRecordRefsIntoCSCardTableEntryClosure 处理所有剩余的没有处理的DCQ
 * @param into_cset_dcq 在发生疏散失败（evacuation failure）时，这些卡片将被传递给管理 RSet 更新的 DirtyCardQueueSet。
 * @param concurrent
 * @param worker_i
 */
void G1CollectedHeap::iterate_dirty_card_closure(CardTableEntryClosure* cl, //这个RefineRecordRefsIntoCSCardTableEntryClosure中含有对into_cset_dcq的引用
                                                 DirtyCardQueue* into_cset_dcq,
                                                 bool concurrent,
                                                 uint worker_i) {
  // Clean cards in the hot card cache
  // 处理热表
  G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
  // refine所有的热卡片集合
  hot_card_cache->drain(worker_i, g1_rem_set(), into_cset_dcq);

  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set(); //获取属于JavaThread的全局静态的DCQS，这个DCQS收集了JavaThread的本地DCQ存放过来的脏卡片队列
  size_t n_completed_buffers = 0;
  // 在这个全局的DCQS上应用 RefineRecordRefsIntoCSCardTableEntryClosure
  /**
   * 查看具体实现 DirtyCardQueueSet::apply_closure_to_completed_buffer
   * 只要成功就不断循环，其实就是不断从DCQS中取出 _completed_buffers_head 链表的头结点来处理，直到处理完，返回false，循环退出
   * 查看 RefineRecordRefsIntoCSCardTableEntryClosure，其实就做了一件事情，把当前DCQS中的所有的void **buf中指向回收集合的条目添加到into_cset_dcq中去
   */
  while (dcqs.apply_closure_to_completed_buffer(cl, worker_i, 0, true)) {
    n_completed_buffers++; // 计数器加1
  }
  g1_policy()->phase_times()->record_thread_work_item(G1GCPhaseTimes::UpdateRS, worker_i, n_completed_buffers);
  dcqs.clear_n_completed_buffers();
  assert(!dcqs.completed_buffers_exist_dirty(), "Completed buffers exist!");
}


// Computes the sum of the storage used by the various regions.
size_t G1CollectedHeap::used() const {
  return _allocator->used();
}

size_t G1CollectedHeap::used_unlocked() const {
  return _allocator->used_unlocked();
}

class SumUsedClosure: public HeapRegionClosure {
  size_t _used;
public:
  SumUsedClosure() : _used(0) {}
  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      _used += r->used();
    }
    return false;
  }
  size_t result() { return _used; }
};

size_t G1CollectedHeap::recalculate_used() const {
  double recalculate_used_start = os::elapsedTime();

  SumUsedClosure blk;
  heap_region_iterate(&blk);

  g1_policy()->phase_times()->record_evac_fail_recalc_used_time((os::elapsedTime() - recalculate_used_start) * 1000.0);
  return blk.result();
}

// It decides whether an explicit GC should start a concurrent cycle
// instead of doing a STW GC. Currently, a concurrent cycle is
// explicitly started if:
// (a) cause == _gc_locker and +GCLockerInvokesConcurrent, or
// (b) cause == _java_lang_system_gc and +ExplicitGCInvokesConcurrent.
// (c) cause == _g1_humongous_allocation
// 查看 const char* GCCause::to_string(GCCause::Cause cause)获取各个cause的含义
bool G1CollectedHeap::should_do_concurrent_full_gc(GCCause::Cause cause) {
  switch (cause) {
      /**
       * 临界区线程清空导致的gc，gcInvoker触发的gc，根据配置，返回GCLockerInvokesConcurrent，默认false
       */
    case GCCause::_gc_locker:               return GCLockerInvokesConcurrent; //
    /**
     * 用户显式调用System.gc，根据配置，返回ExplicitGCInvokesConcurrent，默认false
     */
    case GCCause::_java_lang_system_gc:     return ExplicitGCInvokesConcurrent; //
    /**
     * 大对象分配失败，使用并发full gc进行处理
     */
    case GCCause::_g1_humongous_allocation: return true; //
    case GCCause::_update_allocation_context_stats_inc: return true;
    case GCCause::_wb_conc_mark:            return true;
    default:                                return false;
  }
}

#ifndef PRODUCT
void G1CollectedHeap::allocate_dummy_regions() {
  // Let's fill up most of the region
  size_t word_size = HeapRegion::GrainWords - 1024;
  // And as a result the region we'll allocate will be humongous.
  guarantee(isHumongous(word_size), "sanity");

  for (uintx i = 0; i < G1DummyRegionsPerGC; ++i) {
    // Let's use the existing mechanism for the allocation
    HeapWord* dummy_obj = humongous_obj_allocate(word_size,
                                                 AllocationContext::system());
    if (dummy_obj != NULL) {
      MemRegion mr(dummy_obj, word_size);
      CollectedHeap::fill_with_object(mr);
    } else {
      // If we can't allocate once, we probably cannot allocate
      // again. Let's get out of the loop.
      break;
    }
  }
}
#endif // !PRODUCT

void G1CollectedHeap::increment_old_marking_cycles_started() {
  assert(_old_marking_cycles_started == _old_marking_cycles_completed ||
    _old_marking_cycles_started == _old_marking_cycles_completed + 1,
    err_msg("Wrong marking cycle count (started: %d, completed: %d)",
    _old_marking_cycles_started, _old_marking_cycles_completed));

  _old_marking_cycles_started++;
}

void G1CollectedHeap::increment_old_marking_cycles_completed(bool concurrent) {
  MonitorLockerEx x(FullGCCount_lock, Mutex::_no_safepoint_check_flag);

  // We assume that if concurrent == true, then the caller is a
  // concurrent thread that was joined the Suspendible Thread
  // Set. If there's ever a cheap way to check this, we should add an
  // assert here.

  // Given that this method is called at the end of a Full GC or of a
  // concurrent cycle, and those can be nested (i.e., a Full GC can
  // interrupt a concurrent cycle), the number of full collections
  // completed should be either one (in the case where there was no
  // nesting) or two (when a Full GC interrupted a concurrent cycle)
  // behind the number of full collections started.

  // This is the case for the inner caller, i.e. a Full GC.
  // 如果是Full GC结束时的调用，concurrent = false
  // increment_old_marking_cycles_started() 会修改_old_marking_cycles_started的值
  assert(concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1) ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 2),
         err_msg("for inner caller (Full GC): _old_marking_cycles_started = %u "
                 "is inconsistent with _old_marking_cycles_completed = %u",
                 _old_marking_cycles_started, _old_marking_cycles_completed));

  // This is the case for the outer caller, i.e. the concurrent cycle.
  // 如果是并发标记结束时调用
  assert(!concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1),
         err_msg("for outer caller (concurrent cycle): "
                 "_old_marking_cycles_started = %u "
                 "is inconsistent with _old_marking_cycles_completed = %u",
                 _old_marking_cycles_started, _old_marking_cycles_completed));

  _old_marking_cycles_completed += 1;

  // We need to clear the "in_progress" flag in the CM thread before
  // we wake up any waiters (especially when ExplicitInvokesConcurrent
  // is set) so that if a waiter requests another System.gc() it doesn't
  // incorrectly see that a marking cycle is still in progress.
  if (concurrent) {
    _cmThread->clear_in_progress();
  }

  // This notify_all() will ensure that a thread that called
  // System.gc() with (with ExplicitGCInvokesConcurrent set or not)
  // and it's waiting for a full GC to finish will be woken up. It is
  // waiting in VM_G1IncCollectionPause::doit_epilogue().
  FullGCCount_lock->notify_all(); // 假如调用System.gc()，需要通过这把锁等待Full GC完成，因此在这里通知
}

void G1CollectedHeap::register_concurrent_cycle_start(const Ticks& start_time) {
  _concurrent_cycle_started = true;
  _gc_timer_cm->register_gc_start(start_time);

  _gc_tracer_cm->report_gc_start(gc_cause(), _gc_timer_cm->gc_start());
  trace_heap_before_gc(_gc_tracer_cm);
}

void G1CollectedHeap::register_concurrent_cycle_end() {
  if (_concurrent_cycle_started) {
    if (_cm->has_aborted()) {
      _gc_tracer_cm->report_concurrent_mode_failure();
    }

    _gc_timer_cm->register_gc_end();
    _gc_tracer_cm->report_gc_end(_gc_timer_cm->gc_end(), _gc_timer_cm->time_partitions());

    // Clear state variables to prepare for the next concurrent cycle.
    _concurrent_cycle_started = false;
    _heap_summary_sent = false;
  }
}

void G1CollectedHeap::trace_heap_after_concurrent_cycle() {
  if (_concurrent_cycle_started) {
    // This function can be called when:
    //  the cleanup pause is run
    //  the concurrent cycle is aborted before the cleanup pause.
    //  the concurrent cycle is aborted after the cleanup pause,
    //   but before the concurrent cycle end has been registered.
    // Make sure that we only send the heap information once.
    if (!_heap_summary_sent) {
      trace_heap_after_gc(_gc_tracer_cm);
      _heap_summary_sent = true;
    }
  }
}

G1YCType G1CollectedHeap::yc_type() {
  bool is_young = g1_policy()->gcs_are_young();
  bool is_initial_mark = g1_policy()->during_initial_mark_pause();
  bool is_during_mark = mark_in_progress();

  if (is_initial_mark) {
    return InitialMark;
  } else if (is_during_mark) {
    return DuringMark;
  } else if (is_young) {
    return Normal;
  } else {
    return Mixed;
  }
}

/**
 *  这个方法是最上层的触发垃圾回收的方法
 *
 *  在这个方法里，
 *     1. 如果，用户执行了 System.gc()，就会调用这个方法,参数中的 gc_cause是 GCCause::_java_lang_system_gc
 *     2. 同时，在分配对象的时候导致的分配失败，也会触发这个方法的执行，比如 attempt_allocation_humongous()中, 即分配大对象的时候，分配以前就会检查是否需要进行回收
 *     3. 同时, GCLocker在离开关键区的时候，也会经过条件判断，尝试调用 collect()
 *  从 satisfy_failed_allocation() 方法可以看到，一次分配失败导致的gc是不会调用到这里的，因为按照目前的设计，
 *      一次分配失败导致的gc的首先尝试是扩展内存，如果扩展内存都失败，直接full gc
 *  所以在进行回收的时候尽管是调用VMThread，但是很可能是用户线程在调用，但是进行回收的VMOperation是被VMThread执行的
 *  真正的gc操作是委托给对应的VM_GC_Operation的实现类，然后在不同的线程中同步或者异步执行，比如VM_G1IncCollectionPause，VM_G1CollectFull
 * @param cause
 **/
void G1CollectedHeap::collect(GCCause::Cause cause) {
  assert_heap_not_locked();

  uint gc_count_before;
  uint old_marking_count_before;
  uint full_gc_count_before;
  bool retry_gc;

  do {
    retry_gc = false; // 默认情况下，不进行重试

    {
      MutexLocker ml(Heap_lock); // gc的时候，加内存锁Heap_lock

      // Read the GC count while holding the Heap_lock
      gc_count_before = total_collections();
      full_gc_count_before = total_full_collections();
      old_marking_count_before = _old_marking_cycles_started;// 暂时保存当前的G1CollectedHeap中的_old_marking_cycles_started
    }
    /**
     * 根据gc cause的不同，确定是否需要进行需要进行并发标记
     * 如果是用户调用system.gc，或者是刚离开关键区，并且用户通过
     *      GCLockerInvokesConcurrent或者ExplicitGCInvokesConcurrent表示都希望在这种情况下触发初始标记，那么就构造一个需要进行初始标记的gc pause
     * 如果返回true，比如回收的原因是当前分配了大对象，那么就调度一次顺道进行初始标记的gc
     * 由于是并发的，所以必须有初始标记
     */
    if (should_do_concurrent_full_gc(cause)) {
      // Schedule an initial-mark evacuation pause that will start a
      // concurrent cycle. We're setting word_size to 0 which means that
      // we are not requesting a post-GC allocation.
      /**
       * 调度一个初始标记暂停，搜索 void VM_G1IncCollectionPause::doit()
       *  我们设置word_size=0，因为我们不需要在gc结束以后进行一次分配,因为这是由于GcLocker的临界区结束导致的
       */

      VM_G1IncCollectionPause op(gc_count_before,
                                 0,     /* word_size */
                                 true,  /* should_initiate_conc_mark */ // 调度并发标记
                                 g1_policy()->max_pause_time_ms(),
                                 cause);
      op.set_allocation_context(AllocationContext::current());
      // 调用 VM_G1IncCollectionPause::doit()
      VMThread::execute(&op); // 由于是vm thread，因此会stw，提交给VMThread去执行(但是背后的并行任务是由FlxiableWorkerGang线程池执行的)
      /**
       * 这一次的回收没有成功，查看void VM_G1IncCollectionPause::doit()方法
       */
      if (!op.pause_succeeded()) {
          /**
           * _old_marking_cycles_started 没有发生变化，搜索方法G1CollectedHeap的成员方法 increment_old_marking_cycles_started
           */
        if (old_marking_count_before == _old_marking_cycles_started) {
          retry_gc = op.should_retry_gc(); // 读取op的_should_retry标记，获取是否需要重试，在while循环中会判断retry_gc。对于大对象分配，不需要retry
        } else {
            /***
             *  _old_marking_cycles_started 已经发生了变化，说明刚刚发生了一次full gc,不用重试了
             */
          // A Full GC happened while we were trying to schedule the
          // initial-mark GC. No point in starting a new cycle given
          // that the whole heap was collected anyway.
        }

        if (retry_gc) { // 如果需要重试，必须等待关键区的线程清空并且没有在关键区的needs_gc请求
          if (GC_locker::is_active_and_needs_gc()) {
            GC_locker::stall_until_clear(); // 循环等待_needs_gc清空,即GC_locker结束以后自动执行了gc。搜索 _needs_gc = false
          }
        }
      }
      // 应该抛弃这一次GC_locker的request
      // 在jni_unlock方法中，会修改GCLocker._collection_count
    } else if (GC_locker::should_discard(cause, gc_count_before)) {
      // Return to be consistent with VMOp failure due to another
      // collection slipping in after our gc_count but before our
      // request is processed.  _gc_locker collections upgraded by
      // GCLockerInvokesConcurrent are handled above and never discarded.
      return;
    } else { // 进行并发的、单纯的不带有初始标记的gc，一般_gc_locker情况下，只要不是配置GCLockerInvokesConcurrent，
        // 就仅仅触发一次gc pause，即目标是进行资源的回收，不进行初始标记的任何处理
      if (cause == GCCause::_gc_locker || cause == GCCause::_wb_young_gc
          DEBUG_ONLY(|| cause == GCCause::_scavenge_alot)) {

        // Schedule a standard evacuation pause. We're setting word_size
        // to 0 which means that we are not requesting a post-GC allocation.
        // 启动一个增量并发gc暂停，并且不需要进行初始标记暂停
        VM_G1IncCollectionPause op(gc_count_before,
                                   0,     /* word_size */
                                   false, /* should_initiate_conc_mark */  // 不调度并发标记
                                   g1_policy()->max_pause_time_ms(),
                                   cause);
        VMThread::execute(&op); // 并发执行，这里是提交到线程池执行，而不是直接执行
      } else {  // 进行STW的full gc，这会发生在用户调用了system.gc()，并且我们没有配置-XX:+ExplicitGCInvokesConcurrent
        // Schedule a Full GC.
        VM_G1CollectFull op(gc_count_before, full_gc_count_before, cause); // 进行一次full gc,这个full gc是stw的
        VMThread::execute(&op);
      }
    }
  } while (retry_gc);
}

bool G1CollectedHeap::is_in(const void* p) const {
  if (_hrm.reserved().contains(p)) {
    // Given that we know that p is in the reserved space,
    // heap_region_containing_raw() should successfully
    // return the containing region.
    HeapRegion* hr = heap_region_containing_raw(p);
    return hr->is_in(p);
  } else {
    return false;
  }
}

#ifdef ASSERT
bool G1CollectedHeap::is_in_exact(const void* p) const {
  bool contains = reserved_region().contains(p);
  bool available = _hrm.is_available(addr_to_region((HeapWord*)p));
  if (contains && available) {
    return true;
  } else {
    return false;
  }
}
#endif

// Iteration functions.

// Applies an ExtendedOopClosure onto all references of objects within a HeapRegion.

class IterateOopClosureRegionClosure: public HeapRegionClosure {
  ExtendedOopClosure* _cl;
public:
  IterateOopClosureRegionClosure(ExtendedOopClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      r->oop_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::oop_iterate(ExtendedOopClosure* cl) {
  IterateOopClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

// Iterates an ObjectClosure over all objects within a HeapRegion.

class IterateObjectClosureRegionClosure: public HeapRegionClosure {
  ObjectClosure* _cl;
public:
  IterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    if (! r->continuesHumongous()) {
      r->object_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::object_iterate(ObjectClosure* cl) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

// Calls a SpaceClosure on a HeapRegion.

class SpaceClosureRegionClosure: public HeapRegionClosure {
  SpaceClosure* _cl;
public:
  SpaceClosureRegionClosure(SpaceClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    _cl->do_space(r);
    return false;
  }
};

void G1CollectedHeap::space_iterate(SpaceClosure* cl) {
  SpaceClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

/**
 * 调用者之一是 G1CollectedHeap::reset_heap_region_claim_values，用来遍历所有的HeapRegion
 *
 * @param cl
 */
void G1CollectedHeap::heap_region_iterate(HeapRegionClosure* cl) const {
  _hrm.iterate(cl);  //查看 void HeapRegionManager::iterate
}

void
G1CollectedHeap::heap_region_par_iterate_chunked(HeapRegionClosure* cl,
                                                 uint worker_id,
                                                 uint num_workers,
                                                 jint claim_value) const {
  _hrm.par_iterate(cl, worker_id, num_workers, claim_value);
}

class ResetClaimValuesClosure: public HeapRegionClosure {
public:
  bool doHeapRegion(HeapRegion* r) {
    r->set_claim_value(HeapRegion::InitialClaimValue);
    return false;
  }
};

void G1CollectedHeap::reset_heap_region_claim_values() {
  ResetClaimValuesClosure blk;
  heap_region_iterate(&blk);
}

void G1CollectedHeap::reset_cset_heap_region_claim_values() {
  ResetClaimValuesClosure blk;
  collection_set_iterate(&blk);
}

#ifdef ASSERT
// This checks whether all regions in the heap have the correct claim
// value. I also piggy-backed on this a check to ensure that the
// humongous_start_region() information on "continues humongous"
// regions is correct.

class CheckClaimValuesClosure : public HeapRegionClosure {
private:
  jint _claim_value;
  uint _failures;
  HeapRegion* _sh_region;

public:
  CheckClaimValuesClosure(jint claim_value) :
    _claim_value(claim_value), _failures(0), _sh_region(NULL) { }
  bool doHeapRegion(HeapRegion* r) {
    if (r->claim_value() != _claim_value) {
      gclog_or_tty->print_cr("Region " HR_FORMAT ", "
                             "claim value = %d, should be %d",
                             HR_FORMAT_PARAMS(r),
                             r->claim_value(), _claim_value);
      ++_failures;
    }
    if (!r->isHumongous()) {
      _sh_region = NULL;
    } else if (r->startsHumongous()) {
      _sh_region = r;
    } else if (r->continuesHumongous()) {
      if (r->humongous_start_region() != _sh_region) {
        gclog_or_tty->print_cr("Region " HR_FORMAT ", "
                               "HS = " PTR_FORMAT ", should be " PTR_FORMAT,
                               HR_FORMAT_PARAMS(r),
                               r->humongous_start_region(),
                               _sh_region);
        ++_failures;
      }
    }
    return false;
  }
  uint failures() { return _failures; }
};

bool G1CollectedHeap::check_heap_region_claim_values(jint claim_value) {
  CheckClaimValuesClosure cl(claim_value);
  heap_region_iterate(&cl);
  return cl.failures() == 0;
}

class CheckClaimValuesInCSetHRClosure: public HeapRegionClosure {
private:
  jint _claim_value;
  uint _failures;

public:
  CheckClaimValuesInCSetHRClosure(jint claim_value) :
    _claim_value(claim_value), _failures(0) { }

  uint failures() { return _failures; }

  bool doHeapRegion(HeapRegion* hr) {
    assert(hr->in_collection_set(), "how?");
    assert(!hr->isHumongous(), "H-region in CSet");
    if (hr->claim_value() != _claim_value) {
      gclog_or_tty->print_cr("CSet Region " HR_FORMAT ", "
                             "claim value = %d, should be %d",
                             HR_FORMAT_PARAMS(hr),
                             hr->claim_value(), _claim_value);
      _failures += 1;
    }
    return false;
  }
};

bool G1CollectedHeap::check_cset_heap_region_claim_values(jint claim_value) {
  CheckClaimValuesInCSetHRClosure cl(claim_value);
  collection_set_iterate(&cl);
  return cl.failures() == 0;
}
#endif // ASSERT

// Clear the cached CSet starting regions and (more importantly)
// the time stamps. Called when we reset the GC time stamp.
void G1CollectedHeap::clear_cset_start_regions() {
  assert(_worker_cset_start_region != NULL, "sanity");
  assert(_worker_cset_start_region_time_stamp != NULL, "sanity");

  int n_queues = MAX2((int)ParallelGCThreads, 1);
  for (int i = 0; i < n_queues; i++) {
    _worker_cset_start_region[i] = NULL;
    _worker_cset_start_region_time_stamp[i] = 0;
  }
}

// Given the id of a worker, obtain or calculate a suitable
// starting region for iterating over the current collection set.
/**
 * 查找一个worker_i负责的起始的HeapRegion
 * 调用者为 void G1RemSet::scanRS
 */
HeapRegion* G1CollectedHeap::start_cset_region_for_worker(uint worker_i) {
  assert(get_gc_time_stamp() > 0, "should have been updated by now");

  HeapRegion* result = NULL;
  unsigned gc_time_stamp = get_gc_time_stamp();
  // 如果发现本轮gc已经为这个worker_i计算过HeapRegion，那么就使用已经计算过的结果
  if (_worker_cset_start_region_time_stamp[worker_i] == gc_time_stamp) {
    // Cached starting region for current worker was set
    // during the current pause - so it's valid.
    // Note: the cached starting heap region may be NULL
    // (when the collection set is empty).
    result = _worker_cset_start_region[worker_i];
    assert(result == NULL || result->in_collection_set(), "sanity");
    return result;
  }

  // The cached entry was not valid so let's calculate
  // a suitable starting heap region for this worker.

  // We want the parallel threads to start their collection
  // set iteration at different collection set regions to
  // avoid contention.
  // If we have:
  //          n collection set regions
  //          p threads
  // Then thread t will start at region floor ((t * n) / p)
  /**
   * 如果缓存的起始区域无效，代码则计算一个合适的起始区域。为了避免线程竞争，希望并行线程从不同的区域开始遍历收集集合。为了实现这一点，代码根据以下公式计算每个线程的起始区域：
     如果有n个收集集合区域和p个线程，则线程t的起始区域为 floor((t * n) / p)
   */
  result = g1_policy()->collection_set();
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    uint cs_size = g1_policy()->cset_region_length(); // 整个region链表的长度，不是指单个Region的大小
    uint active_workers = workers()->active_workers();
    assert(UseDynamicNumberOfGCThreads ||
             active_workers == workers()->total_workers(),
             "Unless dynamic should use total workers");

    uint end_ind   = (cs_size * worker_i) / active_workers;
    uint start_ind = 0; // 起始位置

    /**
     * 如果为本轮gc已经计算过上一个线程(workder_i - 1)的起始区域，那么可以直接在链表中取出上一个线程的起始位置，
     *     当前worker的起始位置就可以从上一个线程的起始位置处开始以便利查找，而不用从回收集合链表头的位置查找。
     * 注意，第一个worker线程的index是0
     */
    if (worker_i > 0 &&
        _worker_cset_start_region_time_stamp[worker_i - 1] == gc_time_stamp) {
      // Previous workers starting region is valid
      // so let's iterate from there
      /**
       * 如果上一个线程已经分配了位置，
       */
      start_ind = (cs_size * (worker_i - 1)) / active_workers;
      OrderAccess::loadload(); // 读取-加载内存屏障，内存有序，比如，不可以先读入下面的代码，才读取上面的代码，否则会出现逻辑问题
      result = _worker_cset_start_region[worker_i - 1];
    }


    for (uint i = start_ind; i < end_ind; i++) {
      result = result->next_in_collection_set(); // 逐渐往后遍历，一直遍历到i == end_index的位置的region
    }
  }

  // Note: the calculated starting heap region may be NULL
  // (when the collection set is empty).
  assert(result == NULL || result->in_collection_set(), "sanity");
  assert(_worker_cset_start_region_time_stamp[worker_i] != gc_time_stamp,
         "should be updated only once per pause");
  _worker_cset_start_region[worker_i] = result;
  OrderAccess::storestore(); // 强制对这两个数组的写入操作的有序性
  _worker_cset_start_region_time_stamp[worker_i] = gc_time_stamp;
  return result;
}

void G1CollectedHeap::collection_set_iterate(HeapRegionClosure* cl) {
  HeapRegion* r = g1_policy()->collection_set();
  while (r != NULL) {
    HeapRegion* next = r->next_in_collection_set();
    if (cl->doHeapRegion(r)) {
      cl->incomplete();
      return;
    }
    r = next;
  }
}

/**
 * 调用者是    void G1RemSet::scanRS
 *       或者 G1ParRemoveSelfForwardPtrsTask::work()
 * cl是HeapRegionClosure的具体实现ScanRSClosure
 */
void G1CollectedHeap::collection_set_iterate_from(HeapRegion* r,
                                                  HeapRegionClosure *cl) {
  if (r == NULL) {
    // The CSet is empty so there's nothing to do.
    return;
  }

  assert(r->in_collection_set(),
         "Start region must be a member of the collection set.");
  HeapRegion* cur = r;
  while (cur != NULL) { // 从r开始遍历，一直遍历到回收集合链表的末尾(cur == null)
    HeapRegion* next = cur->next_in_collection_set();
    if (cl->doHeapRegion(cur) && false) { // 搜索 ScanRSClosure::doHeapRegion
      cl->incomplete();
      return;
    }
    cur = next;
  }
  cur = g1_policy()->collection_set(); // 回到 回收集合的头部，一直到遍历到r(cur == r)
  while (cur != r) {
    HeapRegion* next = cur->next_in_collection_set();
    if (cl->doHeapRegion(cur) && false) {
      cl->incomplete();
      return;
    }
    cur = next;
  }
}

HeapRegion* G1CollectedHeap::next_compaction_region(const HeapRegion* from) const {
  HeapRegion* result = _hrm.next_region_in_heap(from);
  while (result != NULL && result->isHumongous()) {
    result = _hrm.next_region_in_heap(result);
  }
  return result;
}

Space* G1CollectedHeap::space_containing(const void* addr) const {
  return heap_region_containing(addr);
}

HeapWord* G1CollectedHeap::block_start(const void* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_start(addr);
}

size_t G1CollectedHeap::block_size(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_size(addr);
}

bool G1CollectedHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_is_obj(addr);
}

bool G1CollectedHeap::supports_tlab_allocation() const {
  return true;
}

/**
 * 返回总共可以分配给TLAB的空间大小，以Byte为单位，即，所有的年轻代中eden space的大小总和
 */
size_t G1CollectedHeap::tlab_capacity(Thread* ignored) const {
    /**
     * GrainBytes是一个Region的字节数。搜索 GrainBytes = (size_t)region_size
     */
  return (_g1_policy->young_list_target_length() - young_list()->survivor_length()) * HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::tlab_used(Thread* ignored) const {
  return young_list()->eden_used_bytes();
}

// For G1 TLABs should not contain humongous objects, so the maximum TLAB size
// must be smaller than the humongous object limit.
size_t G1CollectedHeap::max_tlab_size() const {
  return align_size_down(_humongous_object_threshold_in_words - 1, MinObjAlignment);
}

size_t G1CollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  // Return the remaining space in the cur alloc region, but not less than
  // the min TLAB size.

  // Also, this value can be at most the humongous object threshold,
  // since we can't allow tlabs to grow big enough to accommodate
  // humongous objects.

  HeapRegion* hr = _allocator->mutator_alloc_region(AllocationContext::current())->get();
  size_t max_tlab = max_tlab_size() * wordSize;
  if (hr == NULL) {
    return max_tlab;
  } else {
    return MIN2(MAX2(hr->free(), (size_t) MinTLABSize), max_tlab);
  }
}

size_t G1CollectedHeap::max_capacity() const {
  return _hrm.reserved().byte_size();
}

jlong G1CollectedHeap::millis_since_last_gc() {
  // assert(false, "NYI");
  return 0;
}

void G1CollectedHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
  g1_rem_set()->prepare_for_verify();
}

bool G1CollectedHeap::allocated_since_marking(oop obj, HeapRegion* hr,
                                              VerifyOption vo) {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking:
    return hr->obj_allocated_since_prev_marking(obj);
  case VerifyOption_G1UseNextMarking:
    return hr->obj_allocated_since_next_marking(obj);
  case VerifyOption_G1UseMarkWord:
    return false;
  default:
    ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

HeapWord* G1CollectedHeap::top_at_mark_start(HeapRegion* hr, VerifyOption vo) {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return hr->prev_top_at_mark_start();
  case VerifyOption_G1UseNextMarking: return hr->next_top_at_mark_start();
  case VerifyOption_G1UseMarkWord:    return NULL;
  default:                            ShouldNotReachHere();
  }
  return NULL; // keep some compilers happy
}

bool G1CollectedHeap::is_marked(oop obj, VerifyOption vo) {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return isMarkedPrev(obj);
  case VerifyOption_G1UseNextMarking: return isMarkedNext(obj);
  case VerifyOption_G1UseMarkWord:    return obj->is_gc_marked();
  default:                            ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

const char* G1CollectedHeap::top_at_mark_start_str(VerifyOption vo) {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return "PTAMS";
  case VerifyOption_G1UseNextMarking: return "NTAMS";
  case VerifyOption_G1UseMarkWord:    return "NONE";
  default:                            ShouldNotReachHere();
  }
  return NULL; // keep some compilers happy
}

class VerifyRootsClosure: public OopClosure {
private:
  G1CollectedHeap* _g1h;
  VerifyOption     _vo;
  bool             _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyRootsClosure(VerifyOption vo) :
    _g1h(G1CollectedHeap::heap()),
    _vo(vo),
    _failures(false) { }

  bool failures() { return _failures; }

  template <class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      if (_g1h->is_obj_dead_cond(obj, _vo)) {
        gclog_or_tty->print_cr("Root location " PTR_FORMAT " "
                              "points to dead obj " PTR_FORMAT, p, (void*) obj);
        if (_vo == VerifyOption_G1UseMarkWord) {
          gclog_or_tty->print_cr("  Mark word: " PTR_FORMAT, (void*)(obj->mark()));
        }
        obj->print_on(gclog_or_tty);
        _failures = true;
      }
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }
};

class G1VerifyCodeRootOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  OopClosure* _root_cl;
  nmethod* _nm;
  VerifyOption _vo;
  bool _failures;

  template <class T> void do_oop_work(T* p) {
    // First verify that this root is live
    _root_cl->do_oop(p);

    if (!G1VerifyHeapRegionCodeRoots) {
      // We're not verifying the code roots attached to heap region.
      return;
    }

    // Don't check the code roots during marking verification in a full GC
    if (_vo == VerifyOption_G1UseMarkWord) {
      return;
    }

    // Now verify that the current nmethod (which contains p) is
    // in the code root list of the heap region containing the
    // object referenced by p.

    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);

      // Now fetch the region containing the object
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      HeapRegionRemSet* hrrs = hr->rem_set();
      // Verify that the strong code root list for this region
      // contains the nmethod
      if (!hrrs->strong_code_roots_list_contains(_nm)) {
        gclog_or_tty->print_cr("Code root location " PTR_FORMAT " "
                              "from nmethod " PTR_FORMAT " not in strong "
                              "code roots for region [" PTR_FORMAT "," PTR_FORMAT ")",
                              p, _nm, hr->bottom(), hr->end());
        _failures = true;
      }
    }
  }

public:
  G1VerifyCodeRootOopClosure(G1CollectedHeap* g1h, OopClosure* root_cl, VerifyOption vo):
    _g1h(g1h), _root_cl(root_cl), _vo(vo), _nm(NULL), _failures(false) {}

  void do_oop(oop* p) { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }

  void set_nmethod(nmethod* nm) { _nm = nm; }
  bool failures() { return _failures; }
};

class G1VerifyCodeRootBlobClosure: public CodeBlobClosure {
  G1VerifyCodeRootOopClosure* _oop_cl;

public:
  G1VerifyCodeRootBlobClosure(G1VerifyCodeRootOopClosure* oop_cl):
    _oop_cl(oop_cl) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = cb->as_nmethod_or_null();
    if (nm != NULL) {
      _oop_cl->set_nmethod(nm);
      nm->oops_do(_oop_cl);
    }
  }
};

class YoungRefCounterClosure : public OopClosure {
  G1CollectedHeap* _g1h;
  int              _count;
 public:
  YoungRefCounterClosure(G1CollectedHeap* g1h) : _g1h(g1h), _count(0) {}
  void do_oop(oop* p)       { if (_g1h->is_in_young(*p)) { _count++; } }
  void do_oop(narrowOop* p) { ShouldNotReachHere(); }

  int count() { return _count; }
  void reset_count() { _count = 0; };
};

class VerifyKlassClosure: public KlassClosure {
  YoungRefCounterClosure _young_ref_counter_closure;
  OopClosure *_oop_closure;
 public:
  VerifyKlassClosure(G1CollectedHeap* g1h, OopClosure* cl) : _young_ref_counter_closure(g1h), _oop_closure(cl) {}
  void do_klass(Klass* k) {
    k->oops_do(_oop_closure);

    _young_ref_counter_closure.reset_count();
    k->oops_do(&_young_ref_counter_closure);
    if (_young_ref_counter_closure.count() > 0) {
      guarantee(k->has_modified_oops(), err_msg("Klass %p, has young refs but is not dirty.", k));
    }
  }
};

class VerifyLivenessOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  VerifyOption _vo;
public:
  VerifyLivenessOopClosure(G1CollectedHeap* g1h, VerifyOption vo):
    _g1h(g1h), _vo(vo)
  { }
  void do_oop(narrowOop *p) { do_oop_work(p); }
  void do_oop(      oop *p) { do_oop_work(p); }

  template <class T> void do_oop_work(T *p) {
    oop obj = oopDesc::load_decode_heap_oop(p);
    guarantee(obj == NULL || !_g1h->is_obj_dead_cond(obj, _vo),
              "Dead object referenced by a not dead object");
  }
};

class VerifyObjsInRegionClosure: public ObjectClosure {
private:
  G1CollectedHeap* _g1h;
  size_t _live_bytes;
  HeapRegion *_hr;
  VerifyOption _vo;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyObjsInRegionClosure(HeapRegion *hr, VerifyOption vo)
    : _live_bytes(0), _hr(hr), _vo(vo) {
    _g1h = G1CollectedHeap::heap();
  }
  void do_object(oop o) {
    VerifyLivenessOopClosure isLive(_g1h, _vo);
    assert(o != NULL, "Huh?");
    if (!_g1h->is_obj_dead_cond(o, _vo)) {
      // If the object is alive according to the mark word,
      // then verify that the marking information agrees.
      // Note we can't verify the contra-positive of the
      // above: if the object is dead (according to the mark
      // word), it may not be marked, or may have been marked
      // but has since became dead, or may have been allocated
      // since the last marking.
      if (_vo == VerifyOption_G1UseMarkWord) {
        guarantee(!_g1h->is_obj_dead(o), "mark word and concurrent mark mismatch");
      }

      o->oop_iterate_no_header(&isLive);
      if (!_hr->obj_allocated_since_prev_marking(o)) {
        size_t obj_size = o->size();    // Make sure we don't overflow
        _live_bytes += (obj_size * HeapWordSize);
      }
    }
  }
  size_t live_bytes() { return _live_bytes; }
};

class PrintObjsInRegionClosure : public ObjectClosure {
  HeapRegion *_hr;
  G1CollectedHeap *_g1;
public:
  PrintObjsInRegionClosure(HeapRegion *hr) : _hr(hr) {
    _g1 = G1CollectedHeap::heap();
  };

  void do_object(oop o) {
    if (o != NULL) {
      HeapWord *start = (HeapWord *) o;
      size_t word_sz = o->size();
      gclog_or_tty->print("\nPrinting obj " PTR_FORMAT " of size " SIZE_FORMAT
                          " isMarkedPrev %d isMarkedNext %d isAllocSince %d\n",
                          (void*) o, word_sz,
                          _g1->isMarkedPrev(o),
                          _g1->isMarkedNext(o),
                          _hr->obj_allocated_since_prev_marking(o));
      HeapWord *end = start + word_sz;
      HeapWord *cur;
      int *val;
      for (cur = start; cur < end; cur++) {
        val = (int *) cur;
        gclog_or_tty->print("\t " PTR_FORMAT ":" PTR_FORMAT "\n", val, *val);
      }
    }
  }
};

class VerifyRegionClosure: public HeapRegionClosure {
private:
  bool             _par;
  VerifyOption     _vo;
  bool             _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyRegionClosure(bool par, VerifyOption vo)
    : _par(par),
      _vo(vo),
      _failures(false) {}

  bool failures() {
    return _failures;
  }

  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      bool failures = false;
      r->verify(_vo, &failures);
      if (failures) {
        _failures = true;
      } else {
        VerifyObjsInRegionClosure not_dead_yet_cl(r, _vo);
        r->object_iterate(&not_dead_yet_cl);
        if (_vo != VerifyOption_G1UseNextMarking) {
          if (r->max_live_bytes() < not_dead_yet_cl.live_bytes()) {
            gclog_or_tty->print_cr("[" PTR_FORMAT "," PTR_FORMAT "] "
                                   "max_live_bytes " SIZE_FORMAT " "
                                   "< calculated " SIZE_FORMAT,
                                   r->bottom(), r->end(),
                                   r->max_live_bytes(),
                                 not_dead_yet_cl.live_bytes());
            _failures = true;
          }
        } else {
          // When vo == UseNextMarking we cannot currently do a sanity
          // check on the live bytes as the calculation has not been
          // finalized yet.
        }
      }
    }
    return false; // stop the region iteration if we hit a failure
  }
};

// This is the task used for parallel verification of the heap regions

class G1ParVerifyTask: public AbstractGangTask {
private:
  G1CollectedHeap* _g1h;
  VerifyOption     _vo;
  bool             _failures;

public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  G1ParVerifyTask(G1CollectedHeap* g1h, VerifyOption vo) :
    AbstractGangTask("Parallel verify task"),
    _g1h(g1h),
    _vo(vo),
    _failures(false) { }

  bool failures() {
    return _failures;
  }

  void work(uint worker_id) {
    HandleMark hm;
    VerifyRegionClosure blk(true, _vo);
    _g1h->heap_region_par_iterate_chunked(&blk, worker_id,
                                          _g1h->workers()->active_workers(),
                                          HeapRegion::ParVerifyClaimValue);
    if (blk.failures()) {
      _failures = true;
    }
  }
};

void G1CollectedHeap::verify(bool silent, VerifyOption vo) {
  if (SafepointSynchronize::is_at_safepoint()) {
    assert(Thread::current()->is_VM_thread(),
           "Expected to be executed serially by the VM thread at this point");

    if (!silent) { gclog_or_tty->print("Roots "); }
    VerifyRootsClosure rootsCl(vo);
    VerifyKlassClosure klassCl(this, &rootsCl);
    CLDToKlassAndOopClosure cldCl(&klassCl, &rootsCl, false);

    // We apply the relevant closures to all the oops in the
    // system dictionary, class loader data graph, the string table
    // and the nmethods in the code cache.
    G1VerifyCodeRootOopClosure codeRootsCl(this, &rootsCl, vo);
    G1VerifyCodeRootBlobClosure blobsCl(&codeRootsCl);

    {
      G1RootProcessor root_processor(this);
      root_processor.process_all_roots(&rootsCl,
                                       &cldCl,
                                       &blobsCl);
    }

    bool failures = rootsCl.failures() || codeRootsCl.failures();

    if (vo != VerifyOption_G1UseMarkWord) {
      // If we're verifying during a full GC then the region sets
      // will have been torn down at the start of the GC. Therefore
      // verifying the region sets will fail. So we only verify
      // the region sets when not in a full GC.
      if (!silent) { gclog_or_tty->print("HeapRegionSets "); }
      verify_region_sets();
    }

    if (!silent) { gclog_or_tty->print("HeapRegions "); }
    if (GCParallelVerificationEnabled && ParallelGCThreads > 1) {
      assert(check_heap_region_claim_values(HeapRegion::InitialClaimValue),
             "sanity check");

      G1ParVerifyTask task(this, vo);
      assert(UseDynamicNumberOfGCThreads ||
        workers()->active_workers() == workers()->total_workers(),
        "If not dynamic should be using all the workers");
      int n_workers = workers()->active_workers();
      set_par_threads(n_workers);
      workers()->run_task(&task); // 并发进行校验工作
      set_par_threads(0);
      if (task.failures()) {
        failures = true;
      }

      // Checks that the expected amount of parallel work was done.
      // The implication is that n_workers is > 0.
      assert(check_heap_region_claim_values(HeapRegion::ParVerifyClaimValue),
             "sanity check");

      reset_heap_region_claim_values();

      assert(check_heap_region_claim_values(HeapRegion::InitialClaimValue),
             "sanity check");
    } else {
      VerifyRegionClosure blk(false, vo);
      heap_region_iterate(&blk);
      if (blk.failures()) {
        failures = true;
      }
    }
    if (!silent) gclog_or_tty->print("RemSet ");
    rem_set()->verify();

    if (G1StringDedup::is_enabled()) {
      if (!silent) gclog_or_tty->print("StrDedup ");
      G1StringDedup::verify();
    }

    if (failures) {
      gclog_or_tty->print_cr("Heap:");
      // It helps to have the per-region information in the output to
      // help us track down what went wrong. This is why we call
      // print_extended_on() instead of print_on().
      print_extended_on(gclog_or_tty);
      gclog_or_tty->cr();
#ifndef PRODUCT
      if (VerifyDuringGC && G1VerifyDuringGCPrintReachable) {
        concurrent_mark()->print_reachable("at-verification-failure",
                                           vo, false /* all */);
      }
#endif
      gclog_or_tty->flush();
    }
    guarantee(!failures, "there should not have been any failures");
  } else {
    if (!silent) {
      gclog_or_tty->print("(SKIPPING Roots, HeapRegionSets, HeapRegions, RemSet");
      if (G1StringDedup::is_enabled()) {
        gclog_or_tty->print(", StrDedup");
      }
      gclog_or_tty->print(") ");
    }
  }
}

void G1CollectedHeap::verify(bool silent) {
  verify(silent, VerifyOption_G1UsePrevMarking);
}

double G1CollectedHeap::verify(bool guard, const char* msg) {
  double verify_time_ms = 0.0;

  if (guard && total_collections() >= VerifyGCStartAt) {
    double verify_start = os::elapsedTime();
    HandleMark hm;  // Discard invalid handles created during verification
    prepare_for_verify();
    Universe::verify(VerifyOption_G1UsePrevMarking, msg);
    verify_time_ms = (os::elapsedTime() - verify_start) * 1000;
  }

  return verify_time_ms;
}

void G1CollectedHeap::verify_before_gc() {
  double verify_time_ms = verify(VerifyBeforeGC, " VerifyBeforeGC:");
  g1_policy()->phase_times()->record_verify_before_time_ms(verify_time_ms);
}

void G1CollectedHeap::verify_after_gc() {
  double verify_time_ms = verify(VerifyAfterGC, " VerifyAfterGC:");
  g1_policy()->phase_times()->record_verify_after_time_ms(verify_time_ms);
}

class PrintRegionClosure: public HeapRegionClosure {
  outputStream* _st;
public:
  PrintRegionClosure(outputStream* st) : _st(st) {}
  bool doHeapRegion(HeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

/**
 * 在某种验证条件下，判断obj是否是死亡对象
 * @param obj
 * @param hr
 * @param vo
 * @return
 */
bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const HeapRegion* hr,
                                       const VerifyOption vo) const {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return is_obj_dead(obj, hr); // 搜索 bool is_obj_dead(const oop obj, const HeapRegion* hr)
  case VerifyOption_G1UseNextMarking: return is_obj_ill(obj, hr);
  case VerifyOption_G1UseMarkWord:    return !obj->is_gc_marked();
  default:                            ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const VerifyOption vo) const {
  switch (vo) {
  case VerifyOption_G1UsePrevMarking: return is_obj_dead(obj);
  case VerifyOption_G1UseNextMarking: return is_obj_ill(obj);
  case VerifyOption_G1UseMarkWord:    return !obj->is_gc_marked();
  default:                            ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

void G1CollectedHeap::print_on(outputStream* st) const {
  st->print(" %-20s", "garbage-first heap");
  st->print(" total " SIZE_FORMAT "K, used " SIZE_FORMAT "K",
            capacity()/K, used_unlocked()/K);
  st->print(" [" INTPTR_FORMAT ", " INTPTR_FORMAT ", " INTPTR_FORMAT ")",
            _hrm.reserved().start(),
            _hrm.reserved().start() + _hrm.length() + HeapRegion::GrainWords,
            _hrm.reserved().end());
  st->cr();
  st->print("  region size " SIZE_FORMAT "K, ", HeapRegion::GrainBytes / K);
  uint young_regions = _young_list->length();
  st->print("%u young (" SIZE_FORMAT "K), ", young_regions,
            (size_t) young_regions * HeapRegion::GrainBytes / K);
  uint survivor_regions = g1_policy()->recorded_survivor_regions();
  st->print("%u survivors (" SIZE_FORMAT "K)", survivor_regions,
            (size_t) survivor_regions * HeapRegion::GrainBytes / K);
  st->cr();
  MetaspaceAux::print_on(st);
}

void G1CollectedHeap::print_extended_on(outputStream* st) const {
  print_on(st);

  // Print the per-region information.
  st->cr();
  st->print_cr("Heap Regions: (Y=young(eden), SU=young(survivor), "
               "HS=humongous(starts), HC=humongous(continues), "
               "CS=collection set, F=free, TS=gc time stamp, "
               "PTAMS=previous top-at-mark-start, "
               "NTAMS=next top-at-mark-start)");
  PrintRegionClosure blk(st);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::print_on_error(outputStream* st) const {
  this->CollectedHeap::print_on_error(st);

  if (_cm != NULL) {
    st->cr();
    _cm->print_on_error(st);
  }
}

void G1CollectedHeap::print_gc_threads_on(outputStream* st) const {
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    workers()->print_worker_threads_on(st);
  }
  _cmThread->print_on(st);
  st->cr();
  _cm->print_worker_threads_on(st);
  _cg1r->print_worker_threads_on(st);
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::print_worker_threads_on(st);
  }
}

/**
 * 将Closure apply到所有跟GC相关的线程中
 * @param tc
 */
void G1CollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  if (G1CollectedHeap::use_parallel_gc_threads()) { // 如果使用了并发gc，那么就调用
      // FlexibleWorkGang* workers() const { return _workers; }
    workers()->threads_do(tc); // 调用 比如  FlexibleWorkGang，搜索 AbstractWorkGang::threads_do
  }
  tc->do_thread(_cmThread); // 在 ConcurrentMarkThread 上应用ThreadClosure，ConcurrentMarkThread在JVM中只有一个
  _cg1r->threads_do(tc); // _cg1r = new ConcurrentG1Refine(this, _refine_cte_cl);，实际上是遍历所有的G1Refine线程，调用tc::do_thread
  if (G1StringDedup::is_enabled()) {
    G1StringDedup::threads_do(tc);
  }
}

void G1CollectedHeap::print_tracing_info() const {
  // We'll overload this to mean "trace GC pause statistics."
  if (TraceGen0Time || TraceGen1Time) {
    // The "G1CollectorPolicy" is keeping track of these stats, so delegate
    // to that.
    g1_policy()->print_tracing_info();
  }
  if (G1SummarizeRSetStats) {
    g1_rem_set()->print_summary_info();
  }
  if (G1SummarizeConcMark) {
    concurrent_mark()->print_summary_info();
  }
  g1_policy()->print_yg_surv_rate_info();
  SpecializationStats::print();
}

#ifndef PRODUCT
// Helpful for debugging RSet issues.

class PrintRSetsClosure : public HeapRegionClosure {
private:
  const char* _msg;
  size_t _occupied_sum;

public:
  bool doHeapRegion(HeapRegion* r) {
    HeapRegionRemSet* hrrs = r->rem_set();
    size_t occupied = hrrs->occupied();
    _occupied_sum += occupied;

    gclog_or_tty->print_cr("Printing RSet for region " HR_FORMAT,
                           HR_FORMAT_PARAMS(r));
    if (occupied == 0) {
      gclog_or_tty->print_cr("  RSet is empty");
    } else {
      hrrs->print();
    }
    gclog_or_tty->print_cr("----------");
    return false;
  }

  PrintRSetsClosure(const char* msg) : _msg(msg), _occupied_sum(0) {
    gclog_or_tty->cr();
    gclog_or_tty->print_cr("========================================");
    gclog_or_tty->print_cr("%s", msg);
    gclog_or_tty->cr();
  }

  ~PrintRSetsClosure() {
    gclog_or_tty->print_cr("Occupied Sum: " SIZE_FORMAT, _occupied_sum);
    gclog_or_tty->print_cr("========================================");
    gclog_or_tty->cr();
  }
};

void G1CollectedHeap::print_cset_rsets() {
  PrintRSetsClosure cl("Printing CSet RSets");
  collection_set_iterate(&cl);
}

void G1CollectedHeap::print_all_rsets() {
  PrintRSetsClosure cl("Printing All RSets");;
  heap_region_iterate(&cl);
}
#endif // PRODUCT

G1CollectedHeap* G1CollectedHeap::heap() {
  assert(_sh->kind() == CollectedHeap::G1CollectedHeap,
         "not a garbage-first heap");
  return _g1h;
}

void G1CollectedHeap::gc_prologue(bool full /* Ignored */) {
  // always_do_update_barrier = false;
  assert(InlineCacheBuffer::is_empty(), "should have cleaned up ICBuffer");
  // Fill TLAB's and such
  accumulate_statistics_all_tlabs();
  ensure_parsability(true);

  if (G1SummarizeRSetStats && (G1SummarizeRSetStatsPeriod > 0) &&
      (total_collections() % G1SummarizeRSetStatsPeriod == 0)) {
    g1_rem_set()->print_periodic_summary_info("Before GC RS summary");
  }
}

void G1CollectedHeap::gc_epilogue(bool full) {

  if (G1SummarizeRSetStats &&
      (G1SummarizeRSetStatsPeriod > 0) &&
      // we are at the end of the GC. Total collections has already been increased.
      ((total_collections() - 1) % G1SummarizeRSetStatsPeriod == 0)) {
    g1_rem_set()->print_periodic_summary_info("After GC RS summary");
  }

  // FIXME: what is this about?
  // I'm ignoring the "fill_newgen()" call if "alloc_event_enabled"
  // is set.
  COMPILER2_PRESENT(assert(DerivedPointerTable::is_empty(),
                        "derived pointer present"));
  // always_do_update_barrier = true;
  /**
   * 重新设置所有线程的tlab的大小
   * 搜索 void CollectedHeap::resize_all_tlabs()
   */
  resize_all_tlabs();
  allocation_context_stats().update(full);

  // We have just completed a GC. Update the soft reference
  // policy with the new heap occupancy
  Universe::update_heap_info_at_gc();
}

/**
 * 一次转移操作，需要通过VM_G1IncCollectionPause进行safepoint下的回收
 * 这个回收不进行初始标记借道操作
 * 调用者是 G1CollectedHeap::attempt_allocation_humongous 进行大对象回收 和 G1CollectedHeap::attempt_allocation_slow 进行普通对象回收
 * 根G1CollectedHeap::collect 方法比较,
 * do_collection_pause 只是构造VM_G1IncCollectionPause，不进行并发标记
 *   collect()方法中的cause有很多，比如在 attempt_allocation_humongous，
 *      或者用户的显式gc，或者离开关键区等，会根据不同的参数进行不同的回收，会判断是否进行初始标记，可能构造 VM_G1IncCollectionPause，VM_G1CollectFull
 * @param word_size
 * @param gc_count_before
 * @param succeeded
 * @param gc_cause
 * @return
 */
HeapWord* G1CollectedHeap::do_collection_pause(size_t word_size,
                                               uint gc_count_before,
                                               bool* succeeded,
                                               GCCause::Cause gc_cause) {
  assert_heap_not_locked_and_not_at_safepoint();
  g1_policy()->record_stop_world_start();
  // 仅仅是一次转移暂停，不进行初始标记暂停
  VM_G1IncCollectionPause op(gc_count_before,
                             word_size,
                             false, // 不开始一个新的标记周期
                             g1_policy()->max_pause_time_ms(),
                             gc_cause);

  op.set_allocation_context(AllocationContext::current());
  VMThread::execute(&op); // 我们看VMThread::execute，其实会调用op.evaluate()方法，对应的VM_G1IncCollectionPause.doit()方法

  HeapWord* result = op.result(); // 分配的结果
  /**
   * VM_GC_Operation::doit_prologue
   * 表示 这个 VM_G1IncCollectionPause 得到了执行
   */
  bool ret_succeeded = op.prologue_succeeded() && op.pause_succeeded();
  assert(result == NULL || ret_succeeded,
         "the result should be NULL if the VM did not succeed");
  *succeeded = ret_succeeded;

  assert_heap_not_locked(); // 在这里，不可以锁定Heap_lock
  return result;
}

/**
 * 通知开始进行并发标记
 * 在方法 do_collection_pause_at_safepoint() 中启动
 */
void
G1CollectedHeap::doConcurrentMark() {
  MutexLockerEx x(CGC_lock, Mutex::_no_safepoint_check_flag);
  if (!_cmThread->in_progress()) { // 全局只有一个ConcurrentMarkThread对象
    _cmThread->set_started();
    CGC_lock->notify(); // 通知在锁上面等待的 ConcurrentMarkThread::sleepBeforeNextCycle
  }
}

size_t G1CollectedHeap::pending_card_num() {
  size_t extra_cards = 0;
  JavaThread *curr = Threads::first();
  while (curr != NULL) {
    DirtyCardQueue& dcq = curr->dirty_card_queue();
    extra_cards += dcq.size();
    curr = curr->next();
  }
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  size_t buffer_size = dcqs.buffer_size();
  size_t buffer_num = dcqs.completed_buffers_num();

  // PtrQueueSet::buffer_size() and PtrQueue:size() return sizes
  // in bytes - not the number of 'entries'. We need to convert
  // into a number of cards.
  return (buffer_size * buffer_num + extra_cards) / oopSize;
}

size_t G1CollectedHeap::cards_scanned() {
  return g1_rem_set()->cardsScanned();
}

class RegisterHumongousWithInCSetFastTestClosure : public HeapRegionClosure {
 private:
  size_t _total_humongous;
  size_t _candidate_humongous;

  DirtyCardQueue _dcq;

  // We don't nominate objects with many remembered set entries, on
  // the assumption that such objects are likely still live.
  bool is_remset_small(HeapRegion* region) const {
    HeapRegionRemSet* const rset = region->rem_set();
    return G1EagerReclaimHumongousObjectsWithStaleRefs
      ? rset->occupancy_less_or_equal_than(G1RSetSparseRegionEntries)
      : rset->is_empty();
  }

  bool is_typeArray_region(HeapRegion* region) const {
    return oop(region->bottom())->is_typeArray();
  }

  bool humongous_region_is_candidate(G1CollectedHeap* heap, HeapRegion* region) const {
    assert(region->startsHumongous(), "Must start a humongous object");

    // Candidate selection must satisfy the following constraints
    // while concurrent marking is in progress:
    //
    // * In order to maintain SATB invariants, an object must not be
    // reclaimed if it was allocated before the start of marking and
    // has not had its references scanned.  Such an object must have
    // its references (including type metadata) scanned to ensure no
    // live objects are missed by the marking process.  Objects
    // allocated after the start of concurrent marking don't need to
    // be scanned.
    //

    // * An object must not be reclaimed if it is on the concurrent
    // mark stack.  Objects allocated after the start of concurrent
    // marking are never pushed on the mark stack.
    // 在标记开始前分配且尚未扫描其引用的对象都不能被回收。这是为了避免遗漏可能仍然活跃的对象。
    // 对象在并发标记栈上时也不能被回收，因为标记栈中包含了等待处理的对象引用。如果在标记栈上还有未处理的对象而被回收，可能会导致内存泄漏。
    // 为了满足上述两个条件，代码选择只考虑并发标记开始之后分配的对象作为候选，这种候选不会错，但是可能会遗漏。这种选择避免了对标记栈和 SATB 不变量的额外测试和处理
    // Nominating only objects allocated after the start of concurrent
    // marking is sufficient to meet both constraints.  This may miss
    // some objects that satisfy the constraints, but the marking data
    // structures don't support efficiently performing the needed
    // additional tests or scrubbing of the mark stack.
    // 代码当前仅处理 typeArray 类型的巨型对象。这类对象不包含对象引用，因此不需要处理其他堆区域的记忆集（remset）条目。
    // However, we presently only nominate is_typeArray() objects.
    // A humongous object containing references induces remembered
    // set entries on other regions.  In order to reclaim such an
    // object, those remembered sets would need to be cleaned up.
    //
    // We also treat is_typeArray() objects specially, allowing them
    // to be reclaimed even if allocated before the start of
    // concurrent mark.  For this we rely on mark stack insertion to
    // exclude is_typeArray() objects, preventing reclaiming an object
    // that is in the mark stack.  We also rely on the metadata for
    // such objects to be built-in and so ensured to be kept live.
    // Frequent allocation and drop of large binary blobs is an
    // important use case for eager reclaim, and this special handling
    // may reduce needed headroom.
    // 如果是一个typeArray region并且rset很小
    return is_typeArray_region(region) && is_remset_small(region);
  }

 public:
  RegisterHumongousWithInCSetFastTestClosure()
  : _total_humongous(0),
    _candidate_humongous(0),
    _dcq(&JavaThread::dirty_card_queue_set()) {
  }

  virtual bool doHeapRegion(HeapRegion* r) {
    if (!r->startsHumongous()) { // 如果不是巨型对象的HeapRegion，那么跳过
      return false;
    }
    // 执行到这里，这个HeapRegion肯定是巨型对象的起始区域
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    // 判断该区域是否应该被标记为回收候选者。判断依据包括是否为 typeArray 类型，以及该区域的记忆集（remset）是否足够小
    bool is_candidate = humongous_region_is_candidate(g1h, r); // 它可能是candidate，可能不是candidate
    uint rindex = r->hrm_index();
    g1h->set_humongous_reclaim_candidate(rindex, is_candidate); // 设置这个巨型对象是否是一个candidate
    if (is_candidate) {
        // 增加 _candidate_humongous 计数，并将该区域注册到 G1 堆的候选者列表中。
      _candidate_humongous++;
      g1h->register_humongous_region_with_in_cset_fast_test(rindex);
      // Is_candidate already filters out humongous object with large remembered sets.
      // If we have a humongous object with a few remembered sets, we simply flush these
      // remembered set entries into the DCQS. That will result in automatic
      // re-evaluation of their remembered set entries during the following evacuation
      // phase.
      if (!r->rem_set()->is_empty()) { // 如果该区域的记忆集合不是空的
        guarantee(r->rem_set()->occupancy_less_or_equal_than(G1RSetSparseRegionEntries),
                  "Found a not-small remembered set here. This is inconsistent with previous assumptions.");
        G1SATBCardTableLoggingModRefBS* bs = g1h->g1_barrier_set();
        HeapRegionRemSetIterator hrrs(r->rem_set()); // 遍历这个HeapRegion的记忆集合
        size_t card_index;
        while (hrrs.has_next(card_index)) {
          jbyte* card_ptr = (jbyte*)bs->byte_for_index(card_index); // 这个卡片对应的内存区域
          if (*card_ptr != CardTableModRefBS::dirty_card_val()) { //  不是脏卡片
            *card_ptr = CardTableModRefBS::dirty_card_val(); // 设置为脏卡片
            _dcq.enqueue(card_ptr); // 插入到脏卡片队列DirtyCardQueue中
          }
        }
        assert(hrrs.n_yielded() == r->rem_set()->occupied(),
               err_msg("Remembered set hash maps out of sync, cur: " SIZE_FORMAT " entries, next: " SIZE_FORMAT " entries",
               hrrs.n_yielded(), r->rem_set()->occupied()));
        r->rem_set()->clear_locked();
      }
      assert(r->rem_set()->is_empty(), "At this point any humongous candidate remembered set must be empty.");
    }
    _total_humongous++; // 总的大对象HeapRegion计数器加1

    return false;
  }

  size_t total_humongous() const { return _total_humongous; }
  size_t candidate_humongous() const { return _candidate_humongous; }

  void flush_rem_set_entries() { _dcq.flush(); } // flush操作会将对应的dcq中的元素添加到dcqs中
};

void G1CollectedHeap::register_humongous_regions_with_in_cset_fast_test() {
  if (!G1EagerReclaimHumongousObjects) {
    g1_policy()->phase_times()->record_fast_reclaim_humongous_stats(0.0, 0, 0);
    return;
  }
  double time = os::elapsed_counter();

  // Collect reclaim candidate information and register candidates with cset.
  RegisterHumongousWithInCSetFastTestClosure cl;
  heap_region_iterate(&cl);

  time = ((double)(os::elapsed_counter() - time) / os::elapsed_frequency()) * 1000.0;
  g1_policy()->phase_times()->record_fast_reclaim_humongous_stats(time,
                                                                  cl.total_humongous(),
                                                                  cl.candidate_humongous());
  _has_humongous_reclaim_candidates = cl.candidate_humongous() > 0;

  // Finally flush all remembered set entries to re-check into the global DCQS.
  cl.flush_rem_set_entries();
}

void
G1CollectedHeap::setup_surviving_young_words() {
  assert(_surviving_young_words == NULL, "pre-condition");
  uint array_length = g1_policy()->young_cset_region_length();
  _surviving_young_words = NEW_C_HEAP_ARRAY(size_t, (size_t) array_length, mtGC);
  if (_surviving_young_words == NULL) {
    vm_exit_out_of_memory(sizeof(size_t) * array_length, OOM_MALLOC_ERROR,
                          "Not enough space for young surv words summary.");
  }
  memset(_surviving_young_words, 0, (size_t) array_length * sizeof(size_t));
#ifdef ASSERT
  for (uint i = 0;  i < array_length; ++i) {
    assert( _surviving_young_words[i] == 0, "memset above" );
  }
#endif // !ASSERT
}

void
G1CollectedHeap::update_surviving_young_words(size_t* surv_young_words) {
  MutexLockerEx x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);
  uint array_length = g1_policy()->young_cset_region_length();
  for (uint i = 0; i < array_length; ++i) {
    _surviving_young_words[i] += surv_young_words[i];
  }
}

void
G1CollectedHeap::cleanup_surviving_young_words() {
  guarantee( _surviving_young_words != NULL, "pre-condition" );
  FREE_C_HEAP_ARRAY(size_t, _surviving_young_words, mtGC);
  _surviving_young_words = NULL;
}

class VerifyRegionRemSetClosure : public HeapRegionClosure {
  public:
    bool doHeapRegion(HeapRegion* hr) { // VerifyRegionRemSetClosure::doHeapRegion
      if (!hr->continuesHumongous()) { // 不处理大对象Region
        hr->verify_rem_set(); // 搜索 HeapRegion::verify_rem_set()
      }
      return false;
    }
};

#ifdef ASSERT
class VerifyCSetClosure: public HeapRegionClosure {
public:
  bool doHeapRegion(HeapRegion* hr) {
    // Here we check that the CSet region's RSet is ready for parallel
    // iteration. The fields that we'll verify are only manipulated
    // when the region is part of a CSet and is collected. Afterwards,
    // we reset these fields when we clear the region's RSet (when the
    // region is freed) so they are ready when the region is
    // re-allocated. The only exception to this is if there's an
    // evacuation failure and instead of freeing the region we leave
    // it in the heap. In that case, we reset these fields during
    // evacuation failure handling.
    guarantee(hr->rem_set()->verify_ready_for_par_iteration(), "verification");

    // Here's a good place to add any other checks we'd like to
    // perform on CSet regions.
    return false;
  }
};
#endif // ASSERT

#if TASKQUEUE_STATS
void G1CollectedHeap::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void G1CollectedHeap::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);

  TaskQueueStats totals;
  const int n = workers() != NULL ? workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    st->print("%3d ", i); task_queue(i)->stats.print(st); st->cr();
    totals += task_queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify());
}

void G1CollectedHeap::reset_taskqueue_stats() {
  const int n = workers() != NULL ? workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    task_queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS
/**
 *
 * 打印比如[GC pause (young|mixed) (initial-mark), 0.62417980 secs] 的信息
 * 这时候还没有flush，因此还需要配合log_gc_footer进行最终的打印
 */
void G1CollectedHeap::log_gc_header() {
  if (!G1Log::fine()) {
    return;
  }

  gclog_or_tty->gclog_stamp(_gc_tracer_stw->gc_id());

  GCCauseString gc_cause_str = GCCauseString("GC pause", gc_cause())
    .append(g1_policy()->gcs_are_young() ? "(young)" : "(mixed)")
    .append(g1_policy()->during_initial_mark_pause() ? " (initial-mark)" : "");

  gclog_or_tty->print("[%s", (const char*)gc_cause_str);
}

void G1CollectedHeap::log_gc_footer(double pause_time_sec) {
  if (!G1Log::fine()) {
    return;
  }

  if (G1Log::finer()) {
    if (evacuation_failed()) {
      gclog_or_tty->print(" (to-space exhausted)");
    }
    gclog_or_tty->print_cr(", %3.7f secs]", pause_time_sec);
    g1_policy()->phase_times()->note_gc_end();
    g1_policy()->phase_times()->print(pause_time_sec);
    g1_policy()->print_detailed_heap_transition();
  } else {
    if (evacuation_failed()) {
      gclog_or_tty->print("--");
    }
    g1_policy()->print_heap_transition();
    gclog_or_tty->print_cr(", %3.7f secs]", pause_time_sec);
  }
  gclog_or_tty->flush(); // 打印日志
}

/**
 * 这个方法就是evacuation pause，可能会进行初始标记
 * 这个方法是在STW中执行的(因为这个方法是在 VM_G1IncCollectionPause 中执行的， 而VM_G1IncCollectionPause是一个 VM_Operation)，可能会借道进行初始标记
 * 这是非full gc
 * 进行某种STW的safepoint暂停。这个STW可能是初始标记导致的，也可能是gc导致的
 * 我们看到，这个方法是被VM_G1IncCollectionPause::doit()调用的，而VM_G1IncCollectionPause是一个 VM_Operation，
 *      因此说明这个方法只是针对增量gc,并且当前线程是vm_thread，
 *      显然，搜索 Mode evaluation_mode，说明所有的VM_Operation都是需要在安全点执行的，只是在 VMThread::loop中决定的
 * 这个方法不是full gc，因为它调用了increment_total_collections(false)而
 * 而G1CollectedHeap::do_collection() 是 full gc
*/
bool
G1CollectedHeap::do_collection_pause_at_safepoint(double target_pause_time_ms) {
  assert_at_safepoint(true /* should_be_vm_thread */); // 搜索 #define assert_at_safepoint
  guarantee(!is_gc_active(), "collection is not reentrant");
  // 将_needs_gc设置为true，同时返回is_active()的结果
  // 当前有线程处于Critical Zone，直接放弃gc。在临界区状态结束以后，会重新立刻调度gc
  if (GC_locker::check_active_before_gc()) {
    return false;
  }
  // 临界区无线程，可以进行gc
  _gc_timer_stw->register_gc_start();

  _gc_tracer_stw->report_gc_start(gc_cause(), _gc_timer_stw->gc_start());

  SvcGCMarker sgcm(SvcGCMarker::MINOR);
  ResourceMark rm;

  print_heap_before_gc();
  trace_heap_before_gc(_gc_tracer_stw);

  verify_region_sets_optional();
  verify_dirty_young_regions();

  // This call will decide whether this pause is an initial-mark
  // pause. If it is, during_initial_mark_pause() will return true
  // for the duration of this pause.
    /**
     *  G1CollectorPolicy的成员方法
     * ，如果上一轮的并发标记已经结束，
     * 并且_initiate_conc_mark这个方法在转移暂停开始的时候被调用_if_possible是true，
     * 那么 _during_initial_mark_pause 就会被设置为True，这样，就可以开始一个新的标记轮回
     **/
  g1_policy()->decide_on_conc_mark_initiation();

  // 如果当前正在进行初始标记（g1_policy()->during_initial_mark_pause() = true），
  // 并且正在进行mixed gc(g1_policy()->gcs_are_young() == false()),那么assert将失败
  // We do not allow initial-mark to be piggy-backed on a mixed GC.
  // 初始标记只能被young gc所借道
  assert(!g1_policy()->during_initial_mark_pause() ||
          g1_policy()->gcs_are_young(), "sanity")-;

  // 如果当前正在进行并发标记（g1_policy()->mark_in_progress() = true），
  // 并且正在进行mixed gc(g1_policy()->gcs_are_young() == false()),那么assert将失败
  // We also do not allow mixed GCs during marking.
  assert(!mark_in_progress() || g1_policy()->gcs_are_young(), "sanity");

  // Record whether this pause is an initial mark. When the current
  // thread has completed its logging output and it's safe to signal
  // the CM thread, the flag's value in the policy has been reset.
  bool should_start_conc_mark = g1_policy()->during_initial_mark_pause(); // 是否应该开始并发标记，如果_during_initial_mark_pause=true，那么should_start_conc_mark

  // Inner scope for scope based logging, timers, and stats collection
  {
    EvacuationInfo evacuation_info;
    // 这是一个初始标记暂停
    if (g1_policy()->during_initial_mark_pause()) {
      // We are about to start a marking cycle, so we increment the
      // full collection counter.
      // full gc和初始标记暂停的时候，都会增加_old_marking_cycles_started的值，标记初始标记和full gc的次数
      increment_old_marking_cycles_started();
      register_concurrent_cycle_start(_gc_timer_stw->gc_start());
    }

    _gc_tracer_stw->report_yc_type(yc_type());

    TraceCPUTime tcpu(G1Log::finer(), true, gclog_or_tty);

    /**
     * 计算活跃线程数量。如果启动了动态线程数量，则会依据当前JVM大小以及非守护的JavaThread的数量动态判定active thread 的数量
     */
    uint active_workers = AdaptiveSizePolicy::calc_active_workers(workers()->total_workers(),
                                                                  workers()->active_workers(),
                                                                  Threads::number_of_non_daemon_threads()/*non-daemon线程的数量*/);
    // 只有当UseDynamicNumberOfGCThreads，active_workers才有可能不等于total_workers
    assert(UseDynamicNumberOfGCThreads ||
           active_workers == workers()->total_workers(),
           "If not dynamic should be using all the  workers");
    workers()->set_active_workers(active_workers);


    double pause_start_sec = os::elapsedTime();
    g1_policy()->phase_times()->note_gc_start(active_workers, mark_in_progress());
    log_gc_header();

    TraceCollectorStats tcs(g1mm()->incremental_collection_counters());
    TraceMemoryManagerStats tms(false /* fullGC */, gc_cause(),
                                yc_type() == Mixed /* allMemoryPoolsAffected */);

    // If the secondary_free_list is not empty, append it to the
    // free_list. No need to wait for the cleanup operation to finish;
    // the region allocation code will check the secondary_free_list
    // and wait if necessary. If the G1StressConcRegionFreeing flag is
    // set, skip this step so that the region allocation code has to
    // get entries from the secondary_free_list.
    if (!G1StressConcRegionFreeing) {
      append_secondary_free_list_if_not_empty_with_lock(); // 将整个_secondary_free_list添加到_free_list
    }

    assert(check_young_list_well_formed(), "young list should be well formed");
    assert(check_heap_region_claim_values(HeapRegion::InitialClaimValue),
           "sanity check");

    // Don't dynamically change the number of GC threads this early.  A value of
    // 0 is used to indicate serial work.  When parallel work is done,
    // it will be set.

    { // Call to jvmpi::post_class_unload_events must occur outside of active GC
      IsGCActiveMark x;

      gc_prologue(false);
      increment_total_collections(false /* full gc */); //  非full gc的计数加1
      increment_gc_time_stamp();

      if (VerifyRememberedSets) {
        if (!VerifySilently) {
          gclog_or_tty->print_cr("[Verifying RemSets before GC]");
        }
        VerifyRegionRemSetClosure v_cl;
        heap_region_iterate(&v_cl); // 在每一个Region上调用 HeapRegion::verify_rem_set
      }

      verify_before_gc();
      check_bitmaps("GC Start");

      COMPILER2_PRESENT(DerivedPointerTable::clear());

      // Please see comment in g1CollectedHeap.hpp and
      // G1CollectedHeap::ref_processing_init() to see how
      // reference processing currently works in G1.

      // Enable discovery in the STW reference processor
      ref_processor_stw()->enable_discovery(true /*verify_disabled*/,
                                            true /*verify_no_refs*/);

      {
        // We want to temporarily turn off discovery by the
        // CM ref processor, if necessary, and turn it back on
        // on again later if we do. Using a scoped
        // NoRefDiscovery object will do this.
        NoRefDiscovery no_cm_discovery(ref_processor_cm());

        // Forget the current alloc region (we might even choose it to be part
        // of the collection set!).
        _allocator->release_mutator_alloc_region();

        // We should call this after we retire the mutator alloc
        // region(s) so that all the ALLOC / RETIRE events are generated
        // before the start GC event.
        _hr_printer.start_gc(false /* full */, (size_t) total_collections());

        // This timing is only used by the ergonomics to handle our pause target.
        // It is unclear why this should not include the full pause. We will
        // investigate this in CR 7178365.
        //
        // Preserving the old comment here if that helps the investigation:
        //
        // The elapsed time induced by the start time below deliberately elides
        // the possible verification above.
        double sample_start_time_sec = os::elapsedTime();

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nBefore recording pause start.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->record_collection_pause_start(sample_start_time_sec);

        double scan_wait_start = os::elapsedTime();
        // We have to wait until the CM threads finish scanning the
        // root regions as it's the only way to ensure that all the
        // objects on them have been correctly scanned before we start
        // moving them during the GC.
        bool waited = _cm->root_regions()->wait_until_scan_finished();
        double wait_time_ms = 0.0;
        if (waited) {
          double scan_wait_end = os::elapsedTime();
          wait_time_ms = (scan_wait_end - scan_wait_start) * 1000.0;
        }
        g1_policy()->phase_times()->record_root_region_scan_wait_time(wait_time_ms);

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nAfter recording pause start.\nYoung_list:");
        _young_list->print();
#endif // YOUNG_LIST_VERBOSE

        if (g1_policy()->during_initial_mark_pause()) {
            /**
             * 进行初始标记暂停前的准备工作，搜索 ConcurrentMark::checkpointRootsInitialPre 查看具体实现
             */
          concurrent_mark()->checkpointRootsInitialPre(); //
        }

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nBefore choosing collection set.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->finalize_cset(target_pause_time_ms, evacuation_info);

        // Make sure the remembered sets are up to date. This needs to be
        // done before register_humongous_regions_with_cset(), because the
        // remembered sets are used there to choose eager reclaim candidates.
        // If the remembered sets are not up to date we might miss some
        // entries that need to be handled.
        g1_rem_set()->cleanupHRRS();

        register_humongous_regions_with_in_cset_fast_test();

        assert(check_cset_fast_test(), "Inconsistency in the InCSetState table.");

        _cm->note_start_of_gc();
        // We call this after finalize_cset() to
        // ensure that the CSet has been finalized.
        _cm->verify_no_cset_oops();

        if (_hr_printer.is_active()) {
          HeapRegion* hr = g1_policy()->collection_set();
          while (hr != NULL) {
            _hr_printer.cset(hr);
            hr = hr->next_in_collection_set();
          }
        }

#ifdef ASSERT
        VerifyCSetClosure cl;
        collection_set_iterate(&cl);
#endif // ASSERT

        // 开始进行young gc 的 evacuation
        setup_surviving_young_words();

        // 初始化GC AllocRegion，其中，GC AllocRegion包含了SurvivorGCAllocRegion和OldGCAllocRegion
        _allocator->init_gc_alloc_regions(evacuation_info);

        // Actually do the work...
        /**
         * 进行垃圾收集
         * 在这里会调用G1ParTask的work方法，进行根扫描，疏散等等工作，
         * 以及通过方法 cleanup_after_oops_into_collection_set_do进行回收失败的处理工作
         */
        evacuate_collection_set(evacuation_info);
        // 将回收集合中的Region放回到FreeList中去
        free_collection_set(g1_policy()->collection_set(), evacuation_info);

        eagerly_reclaim_humongous_regions(); // 采用激进的方式回收在回收集合中的大对象

        g1_policy()->clear_collection_set();

        cleanup_surviving_young_words();

        // Start a new incremental collection set for the next pause.
        g1_policy()->start_incremental_cset_building();

        clear_cset_fast_test();

        _young_list->reset_sampled_info();

        // Don't check the whole heap at this point as the
        // GC alloc regions from this pause have been tagged
        // as survivors and moved on to the survivor list.
        // Survivor regions will fail the !is_young() check.
        assert(check_young_list_empty(false /* check_heap */),
          "young list should be empty");

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("Before recording survivors.\nYoung List:");
        _young_list->print();
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->record_survivor_regions(_young_list->survivor_length(),
                                             _young_list->first_survivor_region(),
                                             _young_list->last_survivor_region());
        // 在这里，会将survivor Region添加到CSet中去
        _young_list->reset_auxilary_lists();

        if (evacuation_failed()) {
          _allocator->set_used(recalculate_used());
          uint n_queues = MAX2((int)ParallelGCThreads, 1);
          for (uint i = 0; i < n_queues; i++) {
            if (_evacuation_failed_info_array[i].has_failed()) {
              _gc_tracer_stw->report_evacuation_failed(_evacuation_failed_info_array[i]);
            }
          }
        } else {
          // The "used" of the the collection set have already been subtracted
          // when they were freed.  Add in the bytes evacuated.
          _allocator->increase_used(g1_policy()->bytes_copied_during_gc());
        }

        if (g1_policy()->during_initial_mark_pause()) { // 进行初始标记
          // We have to do this before we notify the CM threads that
          // they can start working to make sure that all the
          // appropriate initialization is done on the CM object.
            /**
                * 进行初始标记暂停前的准备工作，搜索 搜索ConcurrentMark::checkpointRootsInitialPre查看具体实现
             */
          concurrent_mark()->checkpointRootsInitialPost(); // 结束了初始标记的暂停，搜索ConcurrentMark::checkpointRootsInitialPost查看具体实现
          set_marking_started();
          // Note that we don't actually trigger the CM thread at
          // this point. We do that later when we're sure that
          // the current thread has completed its logging output.
        }

        allocate_dummy_regions();

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nEnd of the pause.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE
        // 回收结束以后重置MutatorAllocRegion
        _allocator->init_mutator_alloc_region();

        {
            /**
             * 计算需要扩展的字节数量
             */
          size_t expand_bytes = g1_policy()->expansion_amount();
          if (expand_bytes > 0) {
            size_t bytes_before = capacity();
            // No need for an ergo verbose message here,
            // expansion_amount() does this when it returns a value > 0.
            if (!expand(expand_bytes)) { // 扩展是否成功 G1CollectedHeap::expand
              // We failed to expand the heap. Cannot do anything about it.
            }
          }
        }

        // We redo the verification but now wrt to the new CSet which
        // has just got initialized after the previous CSet was freed.
        _cm->verify_no_cset_oops();
        _cm->note_end_of_gc();

        // This timing is only used by the ergonomics to handle our pause target.
        // It is unclear why this should not include the full pause. We will
        // investigate this in CR 7178365.
        double sample_end_time_sec = os::elapsedTime();
        double pause_time_ms = (sample_end_time_sec - sample_start_time_sec) * MILLIUNITS;
        g1_policy()->record_collection_pause_end(pause_time_ms, evacuation_info); // 设置_during_initial_mark_pause 为 false

        MemoryService::track_memory_usage();

        // In prepare_for_verify() below we'll need to scan the deferred
        // update buffers to bring the RSets up-to-date if
        // G1HRRSFlushLogBuffersOnVerify has been set. While scanning
        // the update buffers we'll probably need to scan cards on the
        // regions we just allocated to (i.e., the GC alloc
        // regions). However, during the last GC we called
        // set_saved_mark() on all the GC alloc regions, so card
        // scanning might skip the [saved_mark_word()...top()] area of
        // those regions (i.e., the area we allocated objects into
        // during the last GC). But it shouldn't. Given that
        // saved_mark_word() is conditional on whether the GC time stamp
        // on the region is current or not, by incrementing the GC time
        // stamp here we invalidate all the GC time stamps on all the
        // regions and saved_mark_word() will simply return top() for
        // all the regions. This is a nicer way of ensuring this rather
        // than iterating over the regions and fixing them. In fact, the
        // GC time stamp increment here also ensures that
        // saved_mark_word() will return top() between pauses, i.e.,
        // during concurrent refinement. So we don't need the
        // is_gc_active() check to decided which top to use when
        // scanning cards (see CR 7039627).
        increment_gc_time_stamp();

        if (VerifyRememberedSets) {
          if (!VerifySilently) {
            gclog_or_tty->print_cr("[Verifying RemSets after GC]");
          }
          VerifyRegionRemSetClosure v_cl;
          heap_region_iterate(&v_cl);
        }

        verify_after_gc();
        check_bitmaps("GC End");

        assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
        ref_processor_stw()->verify_no_references_recorded();

        // CM reference discovery will be re-enabled if necessary.
      }

      // We should do this after we potentially expand the heap so
      // that all the COMMIT events are generated before the end GC
      // event, and after we retire the GC alloc regions so that all
      // RETIRE events are generated before the end GC event.
      _hr_printer.end_gc(false /* full */, (size_t) total_collections());

#ifdef TRACESPINNING
      ParallelTaskTerminator::print_termination_counts();
#endif

      gc_epilogue(false);
    }

    // Print the remainder of the GC log output.
    log_gc_footer(os::elapsedTime() - pause_start_sec);

    // It is not yet to safe to tell the concurrent mark to
    // start as we have some optional output below. We don't want the
    // output from the concurrent mark thread interfering with this
    // logging output either.

    _hrm.verify_optional();
    verify_region_sets_optional();

    TASKQUEUE_STATS_ONLY(if (ParallelGCVerbose) print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());

    print_heap_after_gc();
    trace_heap_after_gc(_gc_tracer_stw);

    // We must call G1MonitoringSupport::update_sizes() in the same scoping level
    // as an active TraceMemoryManagerStats object (i.e. before the destructor for the
    // TraceMemoryManagerStats is called) so that the G1 memory pools are updated
    // before any GC notifications are raised.
    g1mm()->update_sizes();

    _gc_tracer_stw->report_evacuation_info(&evacuation_info);
    _gc_tracer_stw->report_tenuring_threshold(_g1_policy->tenuring_threshold());
    _gc_timer_stw->register_gc_end();
    _gc_tracer_stw->report_gc_end(_gc_timer_stw->gc_end(), _gc_timer_stw->time_partitions());
  }
  // It should now be safe to tell the concurrent mark thread to start
  // without its logging output interfering with the logging output
  // that came from the pause.

  if (should_start_conc_mark) { // young gc结束了，并且根据判断，我们的确应该进行并发标记，
    // CAUTION: after the doConcurrentMark() call below,
    // the concurrent marking thread(s) could be running
    // concurrently with us. Make sure that anything after
    // this point does not assume that we are the only GC thread
    // running. Note: of course, the actual marking work will
    // not start until the safepoint itself is released in
    // SuspendibleThreadSet::desynchronize().
    doConcurrentMark(); // 向并发标记线程发送信号, 这个信号会unblock并发标记线程，查看 ConcurrentMarkThread::sleepBeforeNextCycle()
  }

  return true;
}

void G1CollectedHeap::init_for_evac_failure(OopsInHeapRegionClosure* cl) {
  _drain_in_progress = false;
  set_evac_failure_closure(cl);
  _evac_failure_scan_stack = new (ResourceObj::C_HEAP, mtGC) GrowableArray<oop>(40, true);
}

void G1CollectedHeap::finalize_for_evac_failure() {
  assert(_evac_failure_scan_stack != NULL &&
         _evac_failure_scan_stack->length() == 0,
         "Postcondition");
  assert(!_drain_in_progress, "Postcondition");
  delete _evac_failure_scan_stack;
  _evac_failure_scan_stack = NULL;
}

void G1CollectedHeap::remove_self_forwarding_pointers() {
  assert(check_cset_heap_region_claim_values(HeapRegion::InitialClaimValue), "sanity");

  double remove_self_forwards_start = os::elapsedTime();

  G1ParRemoveSelfForwardPtrsTask rsfp_task(this);
  /**
   *  这一轮的Evacuation失败了，因此并发地使用G1ParRemoveSelfForwardPtrsTask来进行
   */
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    set_par_threads();
    workers()->run_task(&rsfp_task); // 并发进行删除自引用的任务
    set_par_threads(0);
  } else {
    rsfp_task.work(0);
  }

  assert(check_cset_heap_region_claim_values(HeapRegion::ParEvacFailureClaimValue), "sanity");

  // Reset the claim values in the regions in the collection set.
  reset_cset_heap_region_claim_values();

  assert(check_cset_heap_region_claim_values(HeapRegion::InitialClaimValue), "sanity");

  // Now restore saved marks, if any.
  assert(_objs_with_preserved_marks.size() ==
            _preserved_marks_of_objs.size(), "Both or none.");
  while (!_objs_with_preserved_marks.is_empty()) {
    oop obj = _objs_with_preserved_marks.pop();
    markOop m = _preserved_marks_of_objs.pop();
    obj->set_mark(m);
  }
  _objs_with_preserved_marks.clear(true);
  _preserved_marks_of_objs.clear(true);

  g1_policy()->phase_times()->record_evac_fail_remove_self_forwards((os::elapsedTime() - remove_self_forwards_start) * 1000.0);
}

/**
 * 将刚刚copy_to_survivor失败的对象obj放入到_evac_failure_scan_stack堆栈中，随后处理
 * @param obj
 */
void G1CollectedHeap::push_on_evac_failure_scan_stack(oop obj) {
  _evac_failure_scan_stack->push(obj);
}

void G1CollectedHeap::drain_evac_failure_scan_stack() {
  assert(_evac_failure_scan_stack != NULL, "precondition");

  while (_evac_failure_scan_stack->length() > 0) {
     oop obj = _evac_failure_scan_stack->pop();
     /**
      * 这个closure是 G1ParScanHeapEvacFailureClosure，
      * 其实是一个typedef G1ParCopyClosure<G1BarrierEvac, G1MarkNone> G1ParScanHeapEvacFailureClosure;
      * 参考 G1CollectedHeap::handle_evacuation_failure_par
      */
     _evac_failure_closure->set_region(heap_region_containing(obj));
     /**
      * 反向遍历这个对象的所有field，apply对应的_evac_failure_closure，即 G1ParScanHeapEvacFailureClosure
      * 搜索 void G1ParCopyClosure<barrier, do_mark_object>::do_oop_work
      */
     obj->oop_iterate_backwards(_evac_failure_closure);
  }
}

/**
 * _par_scan_state 是当前的pss，记住，一个pss是和一个Task即一个thread对应起来的
 * old是在将这个对象进行evacuate以前对象的oopDesc*
 * @param _par_scan_state
 * @param old
 * @return
 */
oop
G1CollectedHeap::handle_evacuation_failure_par(G1ParScanThreadState* _par_scan_state,
                                               oop old) {
  assert(obj_in_cs(old),
         err_msg("obj: " PTR_FORMAT " should still be in the CSet",
                 (HeapWord*) old));
  markOop m = old->mark(); // markOopDesc*
  oop forward_ptr = old->forward_to_atomic(old);
  if (forward_ptr == NULL) {
      // 自引用指针设置成功
    // Forward-to-self succeeded.
    assert(_par_scan_state != NULL, "par scan state");
    /**
     * 取出当前线程在copy_to_survivor调用之前设置的 evacuation failure survivor
     * G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, rp);
     */
    OopsInHeapRegionClosure* cl = _par_scan_state->evac_failure_closure();
    /**
     * 搜索 G1ParTask::work()
     * 这个queue_num其实就是当前构造pss时候的线程id
     */
    uint queue_num = _par_scan_state->queue_num();

    _evacuation_failed = true;
    _evacuation_failed_info_array[queue_num].register_copy_failure(old->size());
    if (_evac_failure_closure != cl) {
      MutexLockerEx x(EvacFailureStack_lock, Mutex::_no_safepoint_check_flag);
      assert(!_drain_in_progress,
             "Should only be true while someone holds the lock.");
      // Set the global evac-failure closure to the current thread's.
      assert(_evac_failure_closure == NULL, "Or locking has failed.");
      set_evac_failure_closure(cl); // 设置全局的负责待处理evacuation failure的 closure
      // Now do the common part.
      handle_evacuation_failure_common(old, m);
      // Reset to NULL.
      set_evac_failure_closure(NULL);
    } else {
      // The lock is already held, and this is recursive.
      assert(_drain_in_progress, "This should only be the recursive case.");
      handle_evacuation_failure_common(old, m);
    }
    return old;
  } else {
    // Forward-to-self failed. Either someone else managed to allocate
    // space for this object (old != forward_ptr) or they beat us in
    // self-forwarding it (old == forward_ptr).
    assert(old == forward_ptr || !obj_in_cs(forward_ptr),
           err_msg("obj: " PTR_FORMAT " forwarded to: " PTR_FORMAT " "
                   "should not be in the CSet",
                   (HeapWord*) old, (HeapWord*) forward_ptr));
    return forward_ptr;
  }
}

void G1CollectedHeap::handle_evacuation_failure_common(oop old, markOop m) {
  preserve_mark_if_necessary(old, m);

  HeapRegion* r = heap_region_containing(old);
  if (!r->evacuation_failed()) {
    r->set_evacuation_failed(true);
    _hr_printer.evac_failure(r);
  }

  push_on_evac_failure_scan_stack(old);

  /**
   * 如果当前没有处在清空状态（因为其他task可能也遇到了同样的情况，因此同时只能有一个task进入这个if 代码块）
   */
  if (!_drain_in_progress) {
    // prevent recursion in copy_to_survivor_space()
    _drain_in_progress = true;
    drain_evac_failure_scan_stack();
    _drain_in_progress = false;
  }
}

void G1CollectedHeap::preserve_mark_if_necessary(oop obj, markOop m) {
  assert(evacuation_failed(), "Oversaving!");
  // We want to call the "for_promotion_failure" version only in the
  // case of a promotion failure.
  if (m->must_be_preserved_for_promotion_failure(obj)) {
    _objs_with_preserved_marks.push(obj);
    _preserved_marks_of_objs.push(m);
  }
}

/**
 * 搜索 do_mark_object == G1MarkFromRoot 查看调用者,
 * 这个方法的调用可能发生在STW 的初始标记阶段，也可能发生在 非STW 的并发标记阶段
 * 发生在STW阶段的就是根扫描
 * 如果尚未标记该对象，则对其进行标记。 这用于标记根所指向的对象，这些对象保证在 GC 期间不会移动（即非 CSet 对象）。 它是 MT 安全的。
 * 这个方法没有递归，即没有将这个对象push到任何的标记栈以递归处理
 */
void G1ParCopyHelper::mark_object(oop obj) {
  assert(!_g1->heap_region_containing(obj)->in_collection_set(), "should not mark objects in the CSet");

  // We know that the object is not moving so it's safe to read its size.
  _cm->grayRoot(obj, (size_t) obj->size(), _worker_id);
}

/**
 * 一个对象已经拷贝到其他地方了，to_obj指向了对象的新的地址，这时候，这个对象由于地址发生了变化，
 * 因此是未标记的状态，这时候，我们需要将这个转发了的对象重新进行标记
 * @param from_obj
 * @param to_obj
 */
void G1ParCopyHelper::mark_forwarded_object(oop from_obj, oop to_obj) {
  assert(from_obj->is_forwarded(), "from obj should be forwarded");
  assert(from_obj->forwardee() == to_obj, "to obj should be the forwardee");
  assert(from_obj != to_obj, "should not be self-forwarded");

  assert(_g1->heap_region_containing(from_obj)->in_collection_set(), "from obj should be in the CSet");
  assert(!_g1->heap_region_containing(to_obj)->in_collection_set(), "should not mark objects in the CSet");

  // The object might be in the process of being copied by another
  // worker so we cannot trust that its to-space image is
  // well-formed. So we have to read its size from its from-space
  // image which we know should not be changing.
  /**
   * 搜索 ConcurrentMark::grayRoot 查看具体实现
   */
  _cm->grayRoot(to_obj, (size_t) from_obj->size(), _worker_id);
}

/**
 * 专门针对klass的mirror对象发生了转移的情况，因此，调用这个方法的时候，一定设置了_scanned_klass，即这个mirror所对应的klass对象
 * p 表示指向类元数据的指针，new_obj 表示一个新创建的对象
 * 在方法 G1ParCopyClosure<barrier, do_mark_object>::do_oop_work 中被调用
 */
template <class T>
void G1ParCopyHelper::do_klass_barrier(T* p, oop new_obj) {
    /**
     *  搜索 void record_modified_oops(), 就是设置Klass的_modified_oops变量为1，代表这个对象的oop被修改过,即对象被移动过
     */
  if (_g1->heap_region_containing_raw(new_obj)->is_young()) {
      // 记录这个对象的mirror被移动过，这里是被GC移动，当然，也有可能JVM中被用户移动(搜索  void Klass::klass_oop_store(oop* p, oop v) )
    _scanned_klass->record_modified_oops();
  }
}

template <G1Barrier barrier, G1Mark do_mark_object>
template <class T>
/**
 * 这个G1ParCopyClosure 在 G1ParTask::work() 中被构造
 * 当进行一般的YGC时， 参数设置为参数do_mark_object为 G1MarkNone， 当发现开启了并发标记，则将参数do_mark_object设置为G1MarkFromRoot
 * 对比 G1ParScanThreadState::do_oop_evac和G1ParCopyClosure<barrier, do_mark_object>::do_oop_work，
 *    二者调用了copy_to_survivor_space方法
 * @tparam barrier
 * @tparam do_mark_object
 * @tparam T 对象类型
 * @param p 当前正在进行迭代的对象的对象指针
 */
void G1ParCopyClosure<barrier, do_mark_object>::do_oop_work(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p); // 加载指针p所指向的内存对象，这个对象的值是一个指向堆中的内存的指针

  if (oopDesc::is_null(heap_oop)) {
    return;
  }
  /**
   * 一个inline方法  搜索 oop oopDesc::decode_heap_oop_not_null
   * 如果是heap_oop是一个narrow oop，那么对其进行decode为一个正常的wide oop
   */
  oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);// 获取对象的原始地址，如果对象本身是宽地址，那么参数和返回值是相同的

  assert(_worker_id == _par_scan_state->queue_num(), "sanity");

  const InCSetState state = _g1->in_cset_state(obj);
  if (state.is_in_cset()) { // 如果当前对象在回收集合中，这是需要移动的对象
    oop forwardee;
    markOop m = obj->mark(); // 读取对象的_mark标记，判断对象是否已经被标记
    if (m->is_marked()) { // 对象已经被标记，这里的被标记就是被转移，并不是并发标记的标记过程
        /**
         *  取出在前面遍历的时候已经存入的转发地址(可能是当前线程之前已经通过其他引用完成了对这个对象的转移，
         *      或者其他gc thread已经完成了对这个对象的转移)
         */
      forwardee = (oop) m->decode_pointer();
    } else {
        /**
         *  第一次遍历到对象，才会将对象拷贝到survivor space，并返回对象的wide地址
         *  转移到survivor，确定转发地址，这里可能会继续往 pss的_ref中添加新的引用
         *  搜索 G1ParScanThreadState::copy_to_survivor_space 查看具体实现
         */
      forwardee = _par_scan_state->copy_to_survivor_space(state, obj, m); // 将对象移动到survivor区域
    }
    assert(forwardee != NULL, "forwardee should not be NULL");
    oopDesc::encode_store_heap_oop(p, forwardee); // 将对象新的wide 地址进行编码，写入到对象原来的地址处
    /**
     * 如果 do_mark_object 是 G1MarkFromRoot 或者 G1MarkPromotedFromRoot,
     * 那么，我们需要在转移了对象以后(对象晋升了以后)对对象进行标记,即这次STW的转移操作需要借道进行初始标记
     */
    if (do_mark_object != G1MarkNone && forwardee != obj) { // 这里不处理自转发对象
      // If the object is self-forwarded we don't need to explicitly
      // mark it, the evacuation failure protocol will do so.
      mark_forwarded_object(obj, forwardee); // 尽管当前处于转移阶段，但是需要对对象进行标记，标记位灰色对象，表示对象可达
    }
    /**
     * G1BarrierKlass的含义是，是否需要拦截对象被修改以后的klass，比如，是否需要根据对象被修改，因此设置对应klass的标记位
     * 为当前正在scan的klass设置一个标记位，标记_modified_oops==1，这样，这个klass就是dirty的，
     *     那么在G1KlassScanClosure中就需要被处理。而没有被标记为dirty的，G1KlassScanClosure就会跳过这个klass
     */
    if (barrier == G1BarrierKlass) {
        /**
         *  调用父类的实现，搜索 G1ParCopyHelper::do_klass_barrier
         *  这时候，被转移的应该是ClassLoaderData的oop _class_loader
         *  从实现可以看到，只有当forwardee是young(eden + survivor)的时候，会将当前的对象所对应的klass的 _modified_oops 置位
         *  即，当前这个klass的mirror在heap中，但是klass却不在，因此，如果当前的mirror移动了，需要在这个对象的klass上设置一个标记，表示这个klass的mirror已经发生了移动
         *  相当于将这个klass的对象标记为脏对象，后面会针对脏对象进行处理
         */
      do_klass_barrier(p, forwardee);
    }
  } else { // 对象不在回收集合中，因此对象不需要转移，但是如果当前是在初始标记状态，那么虽然不需要转移，但是需要标记
    if (state.is_humongous()) {
      _g1->set_humongous_is_live(obj); // 如果是大对象，那么标记这个大对象为存活大对象，这样，这个大对象就不会进入回收候选进行激进回收
    }
    // The object is not in collection set. If we're a root scanning
    // closure during an initial mark pause then attempt to mark the object.
    /**
     * 为了区别一般的YGC和混合GC的初始标记阶段，
        使用了一个参数do_mark_object， 当进行一般的YGC时， 参数设置为G1MarkNone， 当发现开启了并发标记则设置为G1MarkFromRoot，
        当需要进行类卸载的时候，我们设置 G1MarkPromotedFromRoot，显然这时候这个方法就不会执行
     */
    if (do_mark_object == G1MarkFromRoot) { // 需要进行标记，这里对对象进行了标记
      mark_object(obj); //  对当前对象进行标记
    }
  }
  /**
   * 在对象发生了移动以后，需要更新引用关系
   * 只有转移失败的时候才会有 G1BarrierEvac
   * 搜索 template <class T> void update_rs(HeapRegion* from, T* p, int tid) {
   * 注意，需要区分开，对象从CSet中移动到survivor 区域，这时候，cset区域已经清空了，因此完全不需要rset了，但是survivor区域由于有了新的对象进来，因此需要更新维护其rset
   */
  if (barrier == G1BarrierEvac) {
    _par_scan_state->update_rs(_from, p, _worker_id);
  }
}

template void G1ParCopyClosure<G1BarrierEvac, G1MarkNone>::do_oop_work(oop* p);
template void G1ParCopyClosure<G1BarrierEvac, G1MarkNone>::do_oop_work(narrowOop* p);

class G1ParEvacuateFollowersClosure : public VoidClosure {
protected:
  G1CollectedHeap*              _g1h;
  G1ParScanThreadState*         _par_scan_state; // G1ParScanThreadState对象存放了那些从RSet中搜索到的可达、并且指向回收集合中的对象
  RefToScanQueueSet*            _queues;
  ParallelTaskTerminator*       _terminator;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }
  RefToScanQueueSet*      queues()         { return _queues; }
  ParallelTaskTerminator* terminator()     { return _terminator; }

public:
  G1ParEvacuateFollowersClosure(G1CollectedHeap* g1h,
                                G1ParScanThreadState* par_scan_state,
                                RefToScanQueueSet* queues,
                                ParallelTaskTerminator* terminator)
    : _g1h(g1h), _par_scan_state(par_scan_state),
      _queues(queues), _terminator(terminator) {}

  void do_void(); //具体实现搜索 G1ParEvacuateFollowersClosure::do_void

private:
  inline bool offer_termination();
};

bool G1ParEvacuateFollowersClosure::offer_termination() {
  G1ParScanThreadState* const pss = par_scan_state();
  pss->start_term_time();
  const bool res = terminator()->offer_termination();
  pss->end_term_time();
  return res;
}

void G1ParEvacuateFollowersClosure::do_void() {
  G1ParScanThreadState* const pss = par_scan_state(); // 获取当前的PSS对象
  pss->trim_queue(); // 搜索 G1ParScanThreadState::trim_queue
  do {
    pss->steal_and_trim_queue(queues());
  } while (!offer_termination());
}

/**
 *  G1KlassScanClosure 这个Closure是在G1CLDClosure中被引用，搜索 class G1CLDClosure : public CLDClosure 查看引用位置
 */
class G1KlassScanClosure : public KlassClosure {
 // 一个G1ParCopyHeaper的指针，可能是一个指向G1ParCopyClosure，搜索_klass_in_cld_closure可以看到，这里的这个G1ParCopyClosure会拦截klass
 G1ParCopyHelper* _closure;
 // 对应了 only_young，当调用这个evacuation的时候是一个young gc的过程，那么_process_only_dirty是true，否则是false
 bool             _process_only_dirty;
 int              _count;
 /**
  * 构造方法的调用查看 class G1CLDClosure : public CLDClosure
  *
  * G1KlassScanClosure本质上还是封装了一个G1ParCopyClosure，但是会拦截klass
  */
 public:
  G1KlassScanClosure(G1ParCopyHelper* closure, bool process_only_dirty)
      : _process_only_dirty(process_only_dirty), _closure(closure), _count(0) {}
    /**
     * 这是 G1KlassScanClosure::do_klass
     * 调用者是 void ClassLoaderData::oops_do
     */
  void do_klass(Klass* klass) { // 对每一个类进行处理(一个Klass对应了一个类)
    // If the klass has not been dirtied we know that there's
    // no references into the young gen and we can skip it.
    /**
     * 如果 _process_only_dirty = false，那么不管这个klass是否有修改，一定会对这个klass 去apply对应的封装好的G1ParCopyHelper* closure
     * 如果这个_process_only_dirty = true，即当前是一个young gc的阶段，那么只有这个klass对应的对象的oop已经被修改过（即这个klass有指向young generation的引用）
     * ，即这个klass是dirty的，才会处理这个klass
     * 搜索 record_modified_oops， 可以看到，它表征的是这个klass对应的mirror是否有位置的移动或者从null到非null的设置，因此，这个变化可能来自于用户代码，可能来自于gc。
     *  void Klass::klass_update_barrier_set(oop v)
     *
     *  如果仅仅是不需要初始标记的young gc，那么 _process_only_dirty = true，这时候只有当has_modified_oops的时候才会执行对这个klass的扫描，
     *  扫描过程中如果挪动了klass的mirror，会再次将_midified_oop置为true
     *  如果不是young gc，或者gc过程需要进行标记，那么_process_only_dirty = false，这时候无论是否_modified_oops=true，都会对这个klass进行扫描
     *
     * 这里的理解方式是
     * - 如果klass有指向年轻代的引用，即klass->has_modified_oops()，那么无论是什么类型的回收(Young，Mix， Old)，一定需要进行处理
     * - 如果klass没有指向年轻代的引用，即klass->has_modified_oops() == false，那么只有非Young GC(Mix/Old GC)，才需要进行处理
     */
   if (!_process_only_dirty || klass->has_modified_oops()) {
      // Clean the klass since we're going to scavenge all the metadata.
      klass->clear_modified_oops(); // 重置类的oop被修改的标记位

      // Tell the closure that this klass is the Klass to scavenge
      // and is the one to dirty if oops are left pointing into the young gen.
      _closure->set_scanned_klass(klass); // 设置当前的klass为当前的_closure正在处理的类

      /**
       * 搜索 void Klass::oops_do(OopClosure* cl) 查看方法的具体实现
       * 在这个klass上去apply封装好的G1ParCopyHelper* closure， 其实就是将klass对应的mirror的oop上apply对应的G1ParCopyClosure，
       * 注意这里是对这个klass的mirror进行apply G1ParCopyHelper* closure
       *
       * 这里，如果对象发生了移动，那么依然可能会重新将_modified_oops 置位为1
       * 搜 void Klass::oops_do(OopClosure* cl) 可以看到，这里最终传递给closure处理的是klass 的 mirror对象，
       * 这个对象可能是分配在堆中的，而klass本身不在堆中。因此，当这个mirror因为evacuation发生了移动，那么这个class就变成了dirty的了。
       *
       * 具体实现搜 G1ParCopyClosure<barrier, do_mark_object>::do_oop_work
       */
      klass->oops_do(_closure);

      _closure->set_scanned_klass(NULL); // 清空当前正在处理的类
    }
    _count++;
  }
};

/**
 *
 * 继承关系
 * AbstractWorkGang
  -- WorkGang
  		-- FlexiableWorkGang

    AbstractGangTask
        ---G1ParTask
 */
class G1ParTask : public AbstractGangTask {
protected:
  G1CollectedHeap*       _g1h;
  RefToScanQueueSet      *_queues;
  G1RootProcessor*       _root_processor;
  ParallelTaskTerminator _terminator;
  uint _n_workers;

  Mutex _stats_lock;
  Mutex* stats_lock() { return &_stats_lock; }

public:
  G1ParTask(G1CollectedHeap* g1h, RefToScanQueueSet *task_queues, G1RootProcessor* root_processor)
    : AbstractGangTask("G1 collection"),
      _g1h(g1h),
      _queues(task_queues),
      _root_processor(root_processor),
      _terminator(0, _queues),
      _stats_lock(Mutex::leaf, "parallel G1 stats lock", true)
  {}

  RefToScanQueueSet* queues() { return _queues; }

  RefToScanQueue *work_queue(int i) {
    return queues()->queue(i);
  }

  ParallelTaskTerminator* terminator() { return &_terminator; }

  virtual void set_for_termination(int active_workers) {
    _root_processor->set_num_workers(active_workers);
    terminator()->reset_for_reuse(active_workers);
    _n_workers = active_workers;
  }

  // Helps out with CLD processing.
  //
  // During InitialMark we need to:
  // 1) Scavenge all CLDs for the young GC.
  // 2) Mark all objects directly reachable from strong CLDs.
  /**
   *
    这段代码定义了一个模板类 G1CLDClosure，用于辅助处理 ClassLoaderData (CLD)。
    在 InitialMark 阶段，需要执行以下操作：
        对于年轻代 GC，需要清理所有 CLD 中的对象。
        标记所有直接从强引用 CLD 可达的对象。
   * @tparam do_mark_object
   */
  template <G1Mark do_mark_object>
  class G1CLDClosure : public CLDClosure {
      /**
       * oop_in_klass_closure与_oop_closure都是G1ParCopyClosure，但是拦截器不同，一个是G1BarrierNone， 一个是G1BarrierKlass
       * 构造函数参数中传入的G1ParCopyClosure, 拦截器是G1BarrierNone
       */
    G1ParCopyClosure<G1BarrierNone,  do_mark_object>* _oop_closure; //
    /**
     * 构造了一个新的G1ParCopyClosure，专门用来处理对象被修改以后的klass的修改
     * 基于当前传入的_oop_closure，顺道处理对应的klass，拦截器是G1BarrierKlass, 需要拦截Klass
     */
    G1ParCopyClosure<G1BarrierKlass, do_mark_object>  _oop_in_klass_closure; //
    // 用来处理CLD下面挂载的所有的klass
    G1KlassScanClosure                                _klass_in_cld_closure;// 封装了_oop_in_klass_closure,对应构造函数中的_process_only_dirty
    bool                                              _claim;

   public:
    G1CLDClosure(G1ParCopyClosure<G1BarrierNone, do_mark_object>* oop_closure,
                 bool only_young, //  查看构造的地方，在g1CollectedHeap中的void work(uint worker_id) { 方法中, only_young = false
                 bool claim) // 如果是处在并发标记期间，  claim=true，不在并发标记期间， claim = false
        : _oop_closure(oop_closure),
          _oop_in_klass_closure(oop_closure->g1(),
                                oop_closure->pss(),
                                oop_closure->rp()), // 可以看到，_oop_in_klass_closure就是一个G1ParCopyClosure
          _klass_in_cld_closure(&_oop_in_klass_closure, only_young), //  用来处理CLD中的所有klass
          _claim(claim) {

    }

    /**
     * G1CLDClosure::do_cld()方法
     * 搜索 ClassLoaderData::oops_do 查看 cld->oops_do的具体实现
     * 搜索 ClassLoaderDataGraph::roots_cld_do 查看G1CLDClosure的do_cld方法的调用方式
     * 这个方法用来对当前的ClassLoaderData对象进行处理
     * 搜索 ClassLoaderData::oops_do
     * @param cld
     */
    void do_cld(ClassLoaderData* cld) {
        /**
         * ClassLoaderData::oops_do
         */
      cld->oops_do(_oop_closure,  // 这里的_oop_closure其实就是 G1ParCopyClosure，每一个G1CLDClosure都封装了一个 G1ParCopyClosure,拦截器是G1BarrierNone
                   &_klass_in_cld_closure,  // 这是一个G1KlassScanClosure，里面包含了一个G1ParCopyClosure，这个G1ParCopyClosure的拦截器是G1BarrierKlass
                   _claim); // 是否需要主张占用这个CLD
    }
  };

  /**
   * 在执行这个方法以前，已经进行了 并发标记中的 根扫描子阶段
   * G1ParTask::work() 方法，
   * 这个G1ParTask是通过 FlexibleWorkGang来执行的
   *    用来进行收集阶段的STW状态下并行执行收集任务
   * 搜索 G1CollectedHeap::evacuate_collection_set 查看构造G1ParTask的过程
   * worker_id 是执行当前的G1ParTask的GangWorker的线程id
   * @param worker_id
   */
  void work(uint worker_id) {
    if (worker_id >= _n_workers) return;  // no work needed this round

    _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerStart, worker_id, os::elapsedTime());

    {
      ResourceMark rm;
      HandleMark   hm;

      ReferenceProcessor*             rp = _g1h->ref_processor_stw();
      /**
       * PSS队列中存放了那些根可达、并且子对象又在CSet中的对象
       * 可以看到，每一个Worker都会创建一个G1ParScanThreadState对象，负责维护这个worker在进行垃圾回收期间的一些引用状态等信息
       */
      G1ParScanThreadState            pss(_g1h, worker_id, rp);
      G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, rp);

      pss.set_evac_failure_closure(&evac_failure_cl);

      bool only_young = _g1h->g1_policy()->gcs_are_young(); // 当前的回收暂停是否是一次young gc出发的回收暂停

      /**
       * 可以看到，下面创建了两种cld，G1ParCopyClosure用来进行根扫描，G1CLDClosure用来进行cld相关的处理，但是其实也是交给G1ParCopyClosure最终处理
       * 根据条件不同，处理有两种情况需要考虑：
         - 是否需要进行mark，这是由于当前的evacuation可能是借道一次初始标记
         - 如果的确需要进行一次初始标记，那么需要根据ClassUnloadingWithConcurrentMark的配置，决定是否在初始标记的时候进行类卸载相关的处理
       */
      /**
       * 对于非初始标记阶段的年轻代gc，不进行标记，仅仅扫描
       *    scan_only_root_cl：仅扫描根对象，不执行标记操作。
            scan_only_cld_cl：处理 CLD，仅处理脏的类元数据，不需要回收 CLDs。
       */
      // Non-IM young GC.
      G1ParCopyClosure<G1BarrierNone, G1MarkNone>             scan_only_root_cl(_g1h, &pss, rp);
      G1CLDClosure<G1MarkNone>                                scan_only_cld_cl(&scan_only_root_cl,
                                                                               // 如果当前只是young gc，那么我们只需要处理脏的klasses
                                                                               only_young, // Only process dirty klasses.，
                                                                               false);     // No need to claim CLDs.

       /**
        * IM young GC.
        * Strong roots closures.
        * 扫描 + 标记
        * 对于初始标记阶段的年轻代GC，需要进行标记，但是我们需要根据ClassUnloadingWithConcurrentMark来决定是否
        * 强根的相关闭包：
             scan_mark_root_cl：扫描和标记强根。
             scan_mark_cld_cl：处理 CLD，需要回收 CLDs。
       */
      G1ParCopyClosure<G1BarrierNone, G1MarkFromRoot>         scan_mark_root_cl(_g1h, &pss, rp);
      G1CLDClosure<G1MarkFromRoot>                            scan_mark_cld_cl(&scan_mark_root_cl,
                                                                               false, // 如果需要标记，那么肯定不止处理 dirty的klass,而应该处理所有的klass
                                                                               true); // Need to claim CLDs.


      /**
       * strong_cl， weak_cl， strong_cld_cl, weak_cld_cl只负责具体的处理逻辑的不同，不关心自己到底扫描的是什么(但是cld只处理ClassLoaderData)
       * 具体处理的是什么类型的根(Java 根中的线程根还是CLD根，还是各种VM根(在VM根中SystemDictionray是存在weak root的，其他的VM根不存在weak root))和这个cl没有关系，
       * 但是根的类型不同，对于弱根和强根的判断标准不同，强根交给strong_cl处理，弱根交给weak_cl处理。
       * 同样的，CLD的强根和弱根的区分是is_weak()，强根交给strong_cld_cl处理，弱根交给weak_cld_cl处理
       */
     /**
        * IM young GC.
        * Weak roots closures.
      * 对于初始标记阶段的年轻代GC
      * 弱根的相关闭包：
         scan_mark_weak_root_cl：扫描和标记弱根
         scan_mark_weak_cld_cl：处理 CLD，需要回收 CLDs
         搜索 do_mark_object != G1MarkNone && forwardee != obj 和 搜索 do_mark_object == G1MarkFromRoot
              可以看到 G1MarkFromRoot和G1MarkPromotedFromRoot的区别
         对于强引用的根和类加载器数据，需要使用更为保守的标记策略（从根对象开始标记），
         而对于弱引用的根和类加载器数据，则可以使用更为宽松的标记策略（只标记已经晋升的对象）。

      */
      G1ParCopyClosure<G1BarrierNone, G1MarkPromotedFromRoot> scan_mark_weak_root_cl(_g1h, &pss, rp);
      G1CLDClosure<G1MarkPromotedFromRoot>                    scan_mark_weak_cld_cl(&scan_mark_weak_root_cl,
                                                                                    false, // only_young
                                                                                    true); // Need to claim CLDs.

      OopClosure* strong_root_cl; // 处理强根的Closure
      OopClosure* weak_root_cl; // 处理弱根的Closure
      CLDClosure* strong_cld_cl; // 处理强Class Loader Data的Closure
      CLDClosure* weak_cld_cl;// 处理弱Class Loader Data的Closure

      bool trace_metadata = false;

      /**
       * 初始标记暂停只是用户线程STW了,虚拟机的Worker thread不会暂停
       * 这时候如果发现有从根集合到老年代的引用，就会进行标记操作。这就是初始标记的处理过程。
       * 这个G1ParTask::work()方法肯定是在方法 do_collection_pause_at_safepoint 中调用的，
       *        调用前会有concurrent_mark()->checkpointRootsInitialPre()，
       *        调用后会有concurrent_mark()->checkpointRootsInitialPost()的调用，形成一个完成的STW状态下初始标记的过程
       * 在
       */
      if (_g1h->g1_policy()->during_initial_mark_pause()) { // IM GC, 即现在处于初始标记暂停阶段
          /**
           * 如果处于初始标记暂停阶段，在进行evacuate的时候，我们也需要标记被拷贝的对象
           * 在需要标记的情况下，我们需要根据ClassUnloadingWithConcurrentMark来决定
           */
        // We also need to mark copied objects.
        strong_root_cl = &scan_mark_root_cl; // 扫描 + 标记 在并发标记阶段，因此我们在对象完成转以后，也需要进行标记，防止并发标记对转移以后的对象进行漏标记
        strong_cld_cl  = &scan_mark_cld_cl;
        /**
         * 搜索 product(bool, ClassUnloadingWithConcurrentMark 查看ClassUnloadingWithConcurrentMark参数的具体定义
         * 这个参数的默认值是true
         * 这个参数的不同，造成了我们在处理weak_root和weak_cld的时候的区别，
         *          如果ClassUnloadingWithConcurrentMark==true，那么
         *                       weak_root_cl = &scan_mark_weak_root_cl;
                                 weak_cld_cl  = &scan_mark_weak_cld_cl;
                    如果ClassUnloadingWithConcurrentMark==false, 那么
                                 weak_root_cl = &scan_mark_root_cl;
                                 weak_cld_cl  = &scan_mark_cld_cl;

         */
         /**
          * 为什么只有在并发标记暂停的时候，才需要判断 ClassUnloadingWithConcurrentMark？？
          * 因为类的卸载发生在并发标记的并发清理阶段，当metadata区的分配失败，会设置_initiate_conc_mark_if_possible以促成一次带有初始标记的回收暂停
          * 如果需要在并发标记阶段进行类卸载(ClassUnloadingWithConcurrentMark)，那么就使用scan_mark_weak_root_cl和scan_mark_weak_cld_cl
          * 由于是G1MarkPromotedFromRoot，那么，如果扫描的对象不在CSet中，这个closure不会进行标记
          */
        if (ClassUnloadingWithConcurrentMark) { // 如果用户配置了ClassUnloadingWithConcurrentMark，即在并发标记的清理阶段卸载类
          weak_root_cl = &scan_mark_weak_root_cl; // do_mark_object此时为G1MarkPromotedFromRoot
          weak_cld_cl  = &scan_mark_weak_cld_cl; // do_mark_object此时为G1MarkPromotedFromRoot
          trace_metadata = true; // 只有当trace_metadata = true， weak_cld_cl和 strong_cld_cl 才会使用，否则为null
        } else { // ClassUnloadingWithConcurrentMark 为false
          weak_root_cl = &scan_mark_root_cl; // do_mark_object此时为G1MarkFromRoot，这时候无论对象是否在回收集合中并被转移，对象都会被标记
          weak_cld_cl  = &scan_mark_cld_cl; // do_mark_object此时为G1MarkFromRoot
        }
      } else { // 非IM GC， 即不是处于初始标记暂停阶段，在进行evacuate的时候，我们不需要标记
          /**
           * G1ParCopyClosure<G1BarrierNone, G1MarkNone>  scan_only_root_cl(_g1h, &pss, rp);
           * 不进行标记
           */
        strong_root_cl = &scan_only_root_cl; // 不在并发标记阶段，所以对于转移后的对象在这里不进行标记，标记还是交给后来的并发标记来完成
        weak_root_cl   = &scan_only_root_cl; //
        /**
         * G1CLDClosure<G1MarkNone>   scan_only_cld_cl(&scan_only_root_cl,
                                                       only_young, // Only process dirty klasses.
                                                       false);     // No need to claim CLDs.
         */
        strong_cld_cl  = &scan_only_cld_cl; // 这里的strong_cld_cl和weak_cld_cl都是相同的scan_only_cld_cl
        weak_cld_cl    = &scan_only_cld_cl;
      }

      /**
       * JVM中的根在这里也称为强根， 指的是JVM的堆外空间引用到堆空间的对象， 有栈或者全局变量等
       */
      pss.start_strong_roots(); // 设置强根处理的开始时间 _start_strong_roots
      /**
       * 根扫描开始, 方法实现搜索 void G1RootProcessor::evacuate_roots
       * 一次性将各种闭包应用于系统中的强可达根和弱可达根。 使用worker_i记录并报告子阶段的计时测量
       *
       * 查看 G1RootProcessor::evacuate_roots 和 G1RootProcessor::process_java_roots
       * 在这个方法里面调用了 G1RootProcessor::process_java_roots 和 G1RootProcessor::process_vm_roots
       */
      _root_processor->evacuate_roots(strong_root_cl,  // non_heap_root
                                      weak_root_cl, // non_heap_weak_root
                                      strong_cld_cl, // strong cld root(heap root)
                                      weak_cld_cl, // heap_weak_root(heap root)
                                      trace_metadata,  // 如果设置了ClassUnloadingWithConcurrentMark，那么trace_metadata=true
                                      worker_id // 当前的gc线程的id);

      /**
       * 这个方法是类 G1ParScanThreadState 的成员方法
       * 搜索 G1ParPushHeapRSClosure::do_oop_nv 方法 可以看到基于cset进行转移的时候，使用这个closure往pss的_ref队列中添加引用的过程
       *    比如 G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss)就将G1ParScanThreadState作为自己的成员
       *
       * 搜索 G1ParScanClosure::do_oop_nv(T* p) 方法，可以看到在 转移暂停的 copy_to_survivor 的过程中往这个pss的_ref中添加元素的过程，
       *    即当我们完成了一个obj的拷贝，那么这个obj的所有的reference都需要递归进行处理
       */
      G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss);
      /**
       * 搜索 void G1RootProcessor::scan_remembered_sets 查看具体实现
       * 处理DCQS中剩下的DCQ，同时，以 RSet为根进行遍历处理
       * 将 push_heap_rs_cl 闭包应用于 Collection Set中的所有Region的Rset的并集中的所有位置（已完成“set_region”以指示根所在的区域)
       */
      _root_processor->scan_remembered_sets(&push_heap_rs_cl,
                                            weak_root_cl,
                                            worker_id);
      pss.end_strong_roots(); // 设置强根处理的结束时间 _start_strong_roots

      {
        double start = os::elapsedTime();
        /**
         *  开始进行转移处理，G1ParEvacuateFollowersClosure 传入了 G1ParScanThreadState 对象 pss，
         *  因为pss中存放了根可达并且目标在回收集合中的对象
         *  工作方法 搜索 G1ParEvacuateFollowersClosure::do_void
         *  通过  G1ParScanThreadState::trim_queue() ，不断从pss中取出对象，进行递归扫描
         */
        G1ParEvacuateFollowersClosure evac(_g1h, &pss, _queues, &_terminator);
        evac.do_void();
        double elapsed_sec = os::elapsedTime() - start;
        double term_sec = pss.term_time();
        _g1h->g1_policy()->phase_times()->add_time_secs(G1GCPhaseTimes::ObjCopy, worker_id, elapsed_sec - term_sec);
        _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::Termination, worker_id, term_sec);
        _g1h->g1_policy()->phase_times()->record_thread_work_item(G1GCPhaseTimes::Termination, worker_id, pss.term_attempts());
      }
      _g1h->g1_policy()->record_thread_age_table(pss.age_table());
      _g1h->update_surviving_young_words(pss.surviving_young_words()+1);

      if (ParallelGCVerbose) {
        MutexLocker x(stats_lock());
        pss.print_termination_stats(worker_id);
      }

      assert(pss.queue_is_empty(), "should be empty");

      // Close the inner scope so that the ResourceMark and HandleMark
      // destructors are executed here and are included as part of the
      // "GC Worker Time".
    }
    _g1h->g1_policy()->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerEnd, worker_id, os::elapsedTime());
  }
};

class G1StringSymbolTableUnlinkTask : public AbstractGangTask {
private:
  BoolObjectClosure* _is_alive;
  int _initial_string_table_size;
  int _initial_symbol_table_size;

  bool  _process_strings;
  int _strings_processed;
  int _strings_removed;

  bool  _process_symbols;
  int _symbols_processed;
  int _symbols_removed;

  bool _do_in_parallel;
public:
  G1StringSymbolTableUnlinkTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols) :
    AbstractGangTask("String/Symbol Unlinking"),
    _is_alive(is_alive),
    _do_in_parallel(G1CollectedHeap::use_parallel_gc_threads()),
    _process_strings(process_strings), _strings_processed(0), _strings_removed(0),
    _process_symbols(process_symbols), _symbols_processed(0), _symbols_removed(0) {

    _initial_string_table_size = StringTable::the_table()->table_size();
    _initial_symbol_table_size = SymbolTable::the_table()->table_size();
    if (process_strings) {
      StringTable::clear_parallel_claimed_index();
    }
    if (process_symbols) {
      SymbolTable::clear_parallel_claimed_index();
    }
  }

  ~G1StringSymbolTableUnlinkTask() {
    guarantee(!_process_strings || !_do_in_parallel || StringTable::parallel_claimed_index() >= _initial_string_table_size,
              err_msg("claim value " INT32_FORMAT " after unlink less than initial string table size " INT32_FORMAT,
                      StringTable::parallel_claimed_index(), _initial_string_table_size));
    guarantee(!_process_symbols || !_do_in_parallel || SymbolTable::parallel_claimed_index() >= _initial_symbol_table_size,
              err_msg("claim value " INT32_FORMAT " after unlink less than initial symbol table size " INT32_FORMAT,
                      SymbolTable::parallel_claimed_index(), _initial_symbol_table_size));

    if (G1TraceStringSymbolTableScrubbing) {
      gclog_or_tty->print_cr("Cleaned string and symbol table, "
                             "strings: " SIZE_FORMAT " processed, " SIZE_FORMAT " removed, "
                             "symbols: " SIZE_FORMAT " processed, " SIZE_FORMAT " removed",
                             strings_processed(), strings_removed(),
                             symbols_processed(), symbols_removed());
    }
  }

  void work(uint worker_id) {
    if (_do_in_parallel) {
      int strings_processed = 0;
      int strings_removed = 0;
      int symbols_processed = 0;
      int symbols_removed = 0;
      if (_process_strings) {
        StringTable::possibly_parallel_unlink(_is_alive, &strings_processed, &strings_removed);
        Atomic::add(strings_processed, &_strings_processed);
        Atomic::add(strings_removed, &_strings_removed);
      }
      if (_process_symbols) {
        SymbolTable::possibly_parallel_unlink(&symbols_processed, &symbols_removed);
        Atomic::add(symbols_processed, &_symbols_processed);
        Atomic::add(symbols_removed, &_symbols_removed);
      }
    } else {
      if (_process_strings) {
        StringTable::unlink(_is_alive, &_strings_processed, &_strings_removed);
      }
      if (_process_symbols) {
        SymbolTable::unlink(&_symbols_processed, &_symbols_removed);
      }
    }
  }

  size_t strings_processed() const { return (size_t)_strings_processed; }
  size_t strings_removed()   const { return (size_t)_strings_removed; }

  size_t symbols_processed() const { return (size_t)_symbols_processed; }
  size_t symbols_removed()   const { return (size_t)_symbols_removed; }
};

class G1CodeCacheUnloadingTask VALUE_OBJ_CLASS_SPEC {
private:
  static Monitor* _lock;

  BoolObjectClosure* const _is_alive;
  const bool               _unloading_occurred;
  const uint               _num_workers;

  // Variables used to claim nmethods.
  nmethod* _first_nmethod;
  volatile nmethod* _claimed_nmethod;

  // The list of nmethods that need to be processed by the second pass.
  volatile nmethod* _postponed_list;
  volatile uint     _num_entered_barrier;

 public:
  G1CodeCacheUnloadingTask(uint num_workers, BoolObjectClosure* is_alive, bool unloading_occurred) :
      _is_alive(is_alive),
      _unloading_occurred(unloading_occurred),
      _num_workers(num_workers),
      _first_nmethod(NULL),
      _claimed_nmethod(NULL),
      _postponed_list(NULL),
      _num_entered_barrier(0)
  {
    nmethod::increase_unloading_clock();
    _first_nmethod = CodeCache::alive_nmethod(CodeCache::first());
    _claimed_nmethod = (volatile nmethod*)_first_nmethod;
  }

  ~G1CodeCacheUnloadingTask() {
    CodeCache::verify_clean_inline_caches();

    CodeCache::set_needs_cache_clean(false);
    guarantee(CodeCache::scavenge_root_nmethods() == NULL, "Must be");

    CodeCache::verify_icholder_relocations();
  }

 private:
  void add_to_postponed_list(nmethod* nm) {
      nmethod* old;
      do {
        old = (nmethod*)_postponed_list;
        nm->set_unloading_next(old);
      } while ((nmethod*)Atomic::cmpxchg_ptr(nm, &_postponed_list, old) != old);
  }

  void clean_nmethod(nmethod* nm) {
    bool postponed = nm->do_unloading_parallel(_is_alive, _unloading_occurred);

    if (postponed) {
      // This nmethod referred to an nmethod that has not been cleaned/unloaded yet.
      add_to_postponed_list(nm);
    }

    // Mark that this thread has been cleaned/unloaded.
    // After this call, it will be safe to ask if this nmethod was unloaded or not.
    nm->set_unloading_clock(nmethod::global_unloading_clock());
  }

  void clean_nmethod_postponed(nmethod* nm) {
    nm->do_unloading_parallel_postponed(_is_alive, _unloading_occurred);
  }

  static const int MaxClaimNmethods = 16;

  void claim_nmethods(nmethod** claimed_nmethods, int *num_claimed_nmethods) {
    nmethod* first;
    nmethod* last;

    do {
      *num_claimed_nmethods = 0;

      first = last = (nmethod*)_claimed_nmethod;

      if (first != NULL) {
        for (int i = 0; i < MaxClaimNmethods; i++) {
          last = CodeCache::alive_nmethod(CodeCache::next(last));

          if (last == NULL) {
            break;
          }

          claimed_nmethods[i] = last;
          (*num_claimed_nmethods)++;
        }
      }

    } while ((nmethod*)Atomic::cmpxchg_ptr(last, &_claimed_nmethod, first) != first);
  }

  nmethod* claim_postponed_nmethod() {
    nmethod* claim;
    nmethod* next;

    do {
      claim = (nmethod*)_postponed_list;
      if (claim == NULL) {
        return NULL;
      }

      next = claim->unloading_next();

    } while ((nmethod*)Atomic::cmpxchg_ptr(next, &_postponed_list, claim) != claim);

    return claim;
  }

 public:
  // Mark that we're done with the first pass of nmethod cleaning.
  void barrier_mark(uint worker_id) {
    MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
    _num_entered_barrier++;
    if (_num_entered_barrier == _num_workers) {
      ml.notify_all();
    }
  }

  // See if we have to wait for the other workers to
  // finish their first-pass nmethod cleaning work.
  void barrier_wait(uint worker_id) {
    if (_num_entered_barrier < _num_workers) {
      MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
      while (_num_entered_barrier < _num_workers) {
          ml.wait(Mutex::_no_safepoint_check_flag, 0, false);
      }
    }
  }

  // Cleaning and unloading of nmethods. Some work has to be postponed
  // to the second pass, when we know which nmethods survive.
  void work_first_pass(uint worker_id) {
    // The first nmethods is claimed by the first worker.
    if (worker_id == 0 && _first_nmethod != NULL) {
      clean_nmethod(_first_nmethod);
      _first_nmethod = NULL;
    }

    int num_claimed_nmethods;
    nmethod* claimed_nmethods[MaxClaimNmethods];

    while (true) {
      claim_nmethods(claimed_nmethods, &num_claimed_nmethods);

      if (num_claimed_nmethods == 0) {
        break;
      }

      for (int i = 0; i < num_claimed_nmethods; i++) {
        clean_nmethod(claimed_nmethods[i]);
      }
    }

    // The nmethod cleaning helps out and does the CodeCache part of MetadataOnStackMark.
    // Need to retire the buffers now that this thread has stopped cleaning nmethods.
    MetadataOnStackMark::retire_buffer_for_thread(Thread::current());
  }

  void work_second_pass(uint worker_id) {
    nmethod* nm;
    // Take care of postponed nmethods.
    while ((nm = claim_postponed_nmethod()) != NULL) {
      clean_nmethod_postponed(nm);
    }
  }
};

Monitor* G1CodeCacheUnloadingTask::_lock = new Monitor(Mutex::leaf, "Code Cache Unload lock");

class G1KlassCleaningTask : public StackObj {
  BoolObjectClosure*                      _is_alive;
  volatile jint                           _clean_klass_tree_claimed;
  ClassLoaderDataGraphKlassIteratorAtomic _klass_iterator;

 public:
  G1KlassCleaningTask(BoolObjectClosure* is_alive) :
      _is_alive(is_alive),
      _clean_klass_tree_claimed(0),
      _klass_iterator() {
  }

 private:
  bool claim_clean_klass_tree_task() {
    if (_clean_klass_tree_claimed) {
      return false;
    }

    return Atomic::cmpxchg(1, (jint*)&_clean_klass_tree_claimed, 0) == 0;
  }

  InstanceKlass* claim_next_klass() {
    Klass* klass;
    do {
      klass =_klass_iterator.next_klass();
    } while (klass != NULL && !klass->oop_is_instance());

    return (InstanceKlass*)klass;
  }

public:

  void clean_klass(InstanceKlass* ik) {
    ik->clean_weak_instanceklass_links(_is_alive);

    if (JvmtiExport::has_redefined_a_class()) {
      InstanceKlass::purge_previous_versions(ik);
    }
  }

  void work() {
    ResourceMark rm;

    // One worker will clean the subklass/sibling klass tree.
    if (claim_clean_klass_tree_task()) {
      Klass::clean_subklass_tree(_is_alive);
    }

    // All workers will help cleaning the classes,
    InstanceKlass* klass;
    while ((klass = claim_next_klass()) != NULL) {
      clean_klass(klass);
    }
  }
};

// To minimize the remark pause times, the tasks below are done in parallel.
class G1ParallelCleaningTask : public AbstractGangTask {
private:
  G1StringSymbolTableUnlinkTask _string_symbol_task;
  G1CodeCacheUnloadingTask      _code_cache_task;
  G1KlassCleaningTask           _klass_cleaning_task;

public:
  // The constructor is run in the VMThread.
  G1ParallelCleaningTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols, uint num_workers, bool unloading_occurred) :
      AbstractGangTask("Parallel Cleaning"),
      _string_symbol_task(is_alive, process_strings, process_symbols),
      _code_cache_task(num_workers, is_alive, unloading_occurred),
      _klass_cleaning_task(is_alive) {
  }

  void pre_work_verification() {
    // The VM Thread will have registered Metadata during the single-threaded phase of MetadataStackOnMark.
    assert(Thread::current()->is_VM_thread()
           || !MetadataOnStackMark::has_buffer_for_thread(Thread::current()), "Should be empty");
  }

  void post_work_verification() {
    assert(!MetadataOnStackMark::has_buffer_for_thread(Thread::current()), "Should be empty");
  }

  // The parallel work done by all worker threads.
  void work(uint worker_id) {
    pre_work_verification();

    // Do first pass of code cache cleaning.
    _code_cache_task.work_first_pass(worker_id);

    // Let the threads mark that the first pass is done.
    _code_cache_task.barrier_mark(worker_id);

    // Clean the Strings and Symbols.
    _string_symbol_task.work(worker_id);

    // Wait for all workers to finish the first code cache cleaning pass.
    _code_cache_task.barrier_wait(worker_id);

    // Do the second code cache cleaning work, which realize on
    // the liveness information gathered during the first pass.
    _code_cache_task.work_second_pass(worker_id);

    // Clean all klasses that were not unloaded.
    _klass_cleaning_task.work();

    post_work_verification();
  }
};


void G1CollectedHeap::parallel_cleaning(BoolObjectClosure* is_alive,
                                        bool process_strings,
                                        bool process_symbols,
                                        bool class_unloading_occurred) {
  uint n_workers = (G1CollectedHeap::use_parallel_gc_threads() ?
                    workers()->active_workers() : 1);

  G1ParallelCleaningTask g1_unlink_task(is_alive, process_strings, process_symbols,
                                        n_workers, class_unloading_occurred);
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    set_par_threads(n_workers);
    workers()->run_task(&g1_unlink_task); // 并发进行清理工作
    set_par_threads(0);
  } else {
    g1_unlink_task.work(0);
  }
}

void G1CollectedHeap::unlink_string_and_symbol_table(BoolObjectClosure* is_alive,
                                                     bool process_strings, bool process_symbols) {
  {
    uint n_workers = (G1CollectedHeap::use_parallel_gc_threads() ?
                     _g1h->workers()->active_workers() : 1);
    G1StringSymbolTableUnlinkTask g1_unlink_task(is_alive, process_strings, process_symbols);
    if (G1CollectedHeap::use_parallel_gc_threads()) {
      set_par_threads(n_workers);
      workers()->run_task(&g1_unlink_task);
      set_par_threads(0);
    } else {
      g1_unlink_task.work(0);
    }
  }

  if (G1StringDedup::is_enabled()) {
    G1StringDedup::unlink(is_alive);
  }
}

// in book
class G1RedirtyLoggedCardsTask : public AbstractGangTask {
 private:
  DirtyCardQueueSet* _queue;
 public:
  G1RedirtyLoggedCardsTask(DirtyCardQueueSet* queue) : AbstractGangTask("Redirty Cards"), _queue(queue) { }

  virtual void work(uint worker_id) {
    G1GCPhaseTimes* phase_times = G1CollectedHeap::heap()->g1_policy()->phase_times();
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::RedirtyCards, worker_id);

    RedirtyLoggedCardTableEntryClosure cl;
    if (G1CollectedHeap::heap()->use_parallel_gc_threads()) {
      _queue->par_apply_closure_to_all_completed_buffers(&cl);
    } else {
      _queue->apply_closure_to_all_completed_buffers(&cl);
    }

    phase_times->record_thread_work_item(G1GCPhaseTimes::RedirtyCards, worker_id, cl.num_processed());
  }
};

/**
 * 重新的redity操作， in book
 */
void G1CollectedHeap::redirty_logged_cards() {
  double redirty_logged_cards_start = os::elapsedTime();

  uint n_workers = (G1CollectedHeap::use_parallel_gc_threads() ?
                   _g1h->workers()->active_workers() : 1);
  // 一轮垃圾回收以后，G1CollectedHeap::dirty_card_queue_set()中存放的是垃圾收集过程中发生的脏卡片的dcq，
  // 由各个G1ParScanThreadState的update_rs()方法添加到这个G1CollectedHeap::dirty_card_queue_set()中
  // template <class T> void update_rs(HeapRegion* from, T* p, int tid)
  G1RedirtyLoggedCardsTask redirty_task(&dirty_card_queue_set());
  dirty_card_queue_set().reset_for_par_iteration();
  if (use_parallel_gc_threads()) { // 多线程运行
    set_par_threads(n_workers);
    workers()->run_task(&redirty_task);
    set_par_threads(0);
  } else {
    redirty_task.work(0); // 当前线程直接运行
  }

  DirtyCardQueueSet& dcq = JavaThread::dirty_card_queue_set();
  dcq.merge_bufferlists(&dirty_card_queue_set()); // 将G1CollectedHeap所维护的dirty_card_queue_set()merge到JavaThread的全局dcqs中
  assert(dirty_card_queue_set().completed_buffers_num() == 0, "All should be consumed");

  g1_policy()->phase_times()->record_redirty_logged_cards_time_ms((os::elapsedTime() - redirty_logged_cards_start) * 1000.0);
}

// Weak Reference Processing support

// An always "is_alive" closure that is used to preserve referents.
// If the object is non-null then it's alive.  Used in the preservation
// of referent objects that are pointed to by reference objects
// discovered by the CM ref processor.
class G1AlwaysAliveClosure: public BoolObjectClosure {
  G1CollectedHeap* _g1;
public:
  G1AlwaysAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  bool do_object_b(oop p) {
    if (p != NULL) {
      return true;
    }
    return false;
  }
};

bool G1STWIsAliveClosure::do_object_b(oop p) {
  // An object is reachable if it is outside the collection set,
  // or is inside and copied.
  return !_g1->obj_in_cs(p) || p->is_forwarded();
}

// Non Copying Keep Alive closure
class G1KeepAliveClosure: public OopClosure {
  G1CollectedHeap* _g1;
public:
  G1KeepAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  void do_oop(narrowOop* p) { guarantee(false, "Not needed"); }
  void do_oop(oop* p) {
    oop obj = *p;
    assert(obj != NULL, "the caller should have filtered out NULL values");

    const InCSetState cset_state = _g1->in_cset_state(obj);
    if (!cset_state.is_in_cset_or_humongous()) {
      return;
    }
    if (cset_state.is_in_cset()) {
      assert( obj->is_forwarded(), "invariant" );
      *p = obj->forwardee();
    } else {
      assert(!obj->is_forwarded(), "invariant" );
      assert(cset_state.is_humongous(),
             err_msg("Only allowed InCSet state is IsHumongous, but is %d", cset_state.value()));
      _g1->set_humongous_is_live(obj); // 如果是大对象，那么标记这个大对象为存活大对象，这样，这个大对象就不会进入回收候选进行激进回收
    }
  }
};

// Copying Keep Alive closure - can be called from both
// serial and parallel code as long as different worker
// threads utilize different G1ParScanThreadState instances
// and different queues.

class G1CopyingKeepAliveClosure: public OopClosure {
  G1CollectedHeap*         _g1h;
  OopClosure*              _copy_non_heap_obj_cl;
  G1ParScanThreadState*    _par_scan_state;

public:
  G1CopyingKeepAliveClosure(G1CollectedHeap* g1h,
                            OopClosure* non_heap_obj_cl,
                            G1ParScanThreadState* pss):
    _g1h(g1h),
    _copy_non_heap_obj_cl(non_heap_obj_cl),
    _par_scan_state(pss)
  {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }

  template <class T> void do_oop_work(T* p) {
    oop obj = oopDesc::load_decode_heap_oop(p);

    if (_g1h->is_in_cset_or_humongous(obj)) {
      // If the referent object has been forwarded (either copied
      // to a new location or to itself in the event of an
      // evacuation failure) then we need to update the reference
      // field and, if both reference and referent are in the G1
      // heap, update the RSet for the referent.
      //
      // If the referent has not been forwarded then we have to keep
      // it alive by policy. Therefore we have copy the referent.
      //
      // If the reference field is in the G1 heap then we can push
      // on the PSS queue. When the queue is drained (after each
      // phase of reference processing) the object and it's followers
      // will be copied, the reference field set to point to the
      // new location, and the RSet updated. Otherwise we need to
      // use the the non-heap or metadata closures directly to copy
      // the referent object and update the pointer, while avoiding
      // updating the RSet.

      if (_g1h->is_in_g1_reserved(p)) {
        _par_scan_state->push_on_queue(p);
      } else {
        assert(!Metaspace::contains((const void*)p),
               err_msg("Unexpectedly found a pointer from metadata: "
                              PTR_FORMAT, p));
        _copy_non_heap_obj_cl->do_oop(p);
      }
    }
  }
};

// Serial drain queue closure. Called as the 'complete_gc'
// closure for each discovered list in some of the
// reference processing phases.

class G1STWDrainQueueClosure: public VoidClosure {
protected:
  G1CollectedHeap* _g1h;
  G1ParScanThreadState* _par_scan_state;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }

public:
  G1STWDrainQueueClosure(G1CollectedHeap* g1h, G1ParScanThreadState* pss) :
    _g1h(g1h),
    _par_scan_state(pss)
  { }

  void do_void() {
    G1ParScanThreadState* const pss = par_scan_state();
    pss->trim_queue();
  }
};

// Parallel Reference Processing closures

// Implementation of AbstractRefProcTaskExecutor for parallel reference
// processing during G1 evacuation pauses.

class G1STWRefProcTaskExecutor: public AbstractRefProcTaskExecutor {
private:
  G1CollectedHeap*   _g1h;
  RefToScanQueueSet* _queues;
  FlexibleWorkGang*  _workers;
  int                _active_workers;

public:
  G1STWRefProcTaskExecutor(G1CollectedHeap* g1h,
                        FlexibleWorkGang* workers,
                        RefToScanQueueSet *task_queues,
                        int n_workers) :
    _g1h(g1h),
    _queues(task_queues),
    _workers(workers),
    _active_workers(n_workers)
  {
    assert(n_workers > 0, "shouldn't call this otherwise");
  }

  // Executes the given task using concurrent marking worker threads.
  virtual void execute(ProcessTask& task);
  virtual void execute(EnqueueTask& task);
};

// Gang task for possibly parallel reference processing

class G1STWRefProcTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::ProcessTask ProcessTask;
  ProcessTask&     _proc_task;
  G1CollectedHeap* _g1h;
  RefToScanQueueSet *_task_queues;
  ParallelTaskTerminator* _terminator;

public:
  G1STWRefProcTaskProxy(ProcessTask& proc_task,
                     G1CollectedHeap* g1h,
                     RefToScanQueueSet *task_queues,
                     ParallelTaskTerminator* terminator) :
    AbstractGangTask("Process reference objects in parallel"),
    _proc_task(proc_task),
    _g1h(g1h),
    _task_queues(task_queues),
    _terminator(terminator)
  {}

  virtual void work(uint worker_id) {
    // The reference processing task executed by a single worker.
    ResourceMark rm;
    HandleMark   hm;

    G1STWIsAliveClosure is_alive(_g1h);

    G1ParScanThreadState            pss(_g1h, worker_id, NULL);
    G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, NULL);

    pss.set_evac_failure_closure(&evac_failure_cl);

    G1ParScanExtRootClosure        only_copy_non_heap_cl(_g1h, &pss, NULL);

    G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(_g1h, &pss, NULL);

    OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;

    if (_g1h->g1_policy()->during_initial_mark_pause()) {
      // We also need to mark copied objects.
      copy_non_heap_cl = &copy_mark_non_heap_cl;
    }

    // Keep alive closure.
    G1CopyingKeepAliveClosure keep_alive(_g1h, copy_non_heap_cl, &pss);

    // Complete GC closure
    G1ParEvacuateFollowersClosure drain_queue(_g1h, &pss, _task_queues, _terminator);

    // Call the reference processing task's work routine.
    _proc_task.work(worker_id, is_alive, keep_alive, drain_queue);

    // Note we cannot assert that the refs array is empty here as not all
    // of the processing tasks (specifically phase2 - pp2_work) execute
    // the complete_gc closure (which ordinarily would drain the queue) so
    // the queue may not be empty.
  }
};

// Driver routine for parallel reference processing.
// Creates an instance of the ref processing gang
// task and has the worker threads execute it.
void G1STWRefProcTaskExecutor::execute(ProcessTask& proc_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  ParallelTaskTerminator terminator(_active_workers, _queues);
  G1STWRefProcTaskProxy proc_task_proxy(proc_task, _g1h, _queues, &terminator);

  _g1h->set_par_threads(_active_workers);
  _workers->run_task(&proc_task_proxy);
  _g1h->set_par_threads(0);
}

// Gang task for parallel reference enqueueing.

class G1STWRefEnqueueTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::EnqueueTask EnqueueTask;
  EnqueueTask& _enq_task;

public:
  G1STWRefEnqueueTaskProxy(EnqueueTask& enq_task) :
    AbstractGangTask("Enqueue reference objects in parallel"),
    _enq_task(enq_task)
  { }

  virtual void work(uint worker_id) {
    _enq_task.work(worker_id);
  }
};

// Driver routine for parallel reference enqueueing.
// Creates an instance of the ref enqueueing gang
// task and has the worker threads execute it.

void G1STWRefProcTaskExecutor::execute(EnqueueTask& enq_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  G1STWRefEnqueueTaskProxy enq_task_proxy(enq_task);

  _g1h->set_par_threads(_active_workers);
  _workers->run_task(&enq_task_proxy);
  _g1h->set_par_threads(0);
}

// End of weak reference support closures

// Abstract task used to preserve (i.e. copy) any referent objects
// that are in the collection set and are pointed to by reference
// objects discovered by the CM ref processor.

class G1ParPreserveCMReferentsTask: public AbstractGangTask {
protected:
  G1CollectedHeap* _g1h;
  RefToScanQueueSet      *_queues;
  ParallelTaskTerminator _terminator;
  uint _n_workers;

public:
  G1ParPreserveCMReferentsTask(G1CollectedHeap* g1h,int workers, RefToScanQueueSet *task_queues) :
    AbstractGangTask("ParPreserveCMReferents"),
    _g1h(g1h),
    _queues(task_queues),
    _terminator(workers, _queues),
    _n_workers(workers)
  { }

  void work(uint worker_id) {
    ResourceMark rm;
    HandleMark   hm;

    G1ParScanThreadState            pss(_g1h, worker_id, NULL);
    G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, NULL);

    pss.set_evac_failure_closure(&evac_failure_cl);

    assert(pss.queue_is_empty(), "both queue and overflow should be empty");

    G1ParScanExtRootClosure        only_copy_non_heap_cl(_g1h, &pss, NULL);

    G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(_g1h, &pss, NULL);

    OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;

    if (_g1h->g1_policy()->during_initial_mark_pause()) {
      // We also need to mark copied objects.
      copy_non_heap_cl = &copy_mark_non_heap_cl;
    }

    // Is alive closure
    G1AlwaysAliveClosure always_alive(_g1h);

    // Copying keep alive closure. Applied to referent objects that need
    // to be copied.
    G1CopyingKeepAliveClosure keep_alive(_g1h, copy_non_heap_cl, &pss);

    ReferenceProcessor* rp = _g1h->ref_processor_cm();

    uint limit = ReferenceProcessor::number_of_subclasses_of_ref() * rp->max_num_q();
    uint stride = MIN2(MAX2(_n_workers, 1U), limit);

    // limit is set using max_num_q() - which was set using ParallelGCThreads.
    // So this must be true - but assert just in case someone decides to
    // change the worker ids.
    assert(0 <= worker_id && worker_id < limit, "sanity");
    assert(!rp->discovery_is_atomic(), "check this code");

    // Select discovered lists [i, i+stride, i+2*stride,...,limit)
    for (uint idx = worker_id; idx < limit; idx += stride) {
      DiscoveredList& ref_list = rp->discovered_refs()[idx];

      DiscoveredListIterator iter(ref_list, &keep_alive, &always_alive);
      while (iter.has_next()) {
        // Since discovery is not atomic for the CM ref processor, we
        // can see some null referent objects.
        iter.load_ptrs(DEBUG_ONLY(true));
        oop ref = iter.obj();

        // This will filter nulls.
        if (iter.is_referent_alive()) {
          iter.make_referent_alive();
        }
        iter.move_to_next();
      }
    }

    // Drain the queue - which may cause stealing
    G1ParEvacuateFollowersClosure drain_queue(_g1h, &pss, _queues, &_terminator);
    drain_queue.do_void();
    // Allocation buffers were retired at the end of G1ParEvacuateFollowersClosure
    assert(pss.queue_is_empty(), "should be");
  }
};

// Weak Reference processing during an evacuation pause (part 1).
void G1CollectedHeap::process_discovered_references(uint no_of_gc_workers) {
  double ref_proc_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(rp->discovery_enabled(), "should have been enabled");

  // Any reference objects, in the collection set, that were 'discovered'
  // by the CM ref processor should have already been copied (either by
  // applying the external root copy closure to the discovered lists, or
  // by following an RSet entry).
  //
  // But some of the referents, that are in the collection set, that these
  // reference objects point to may not have been copied: the STW ref
  // processor would have seen that the reference object had already
  // been 'discovered' and would have skipped discovering the reference,
  // but would not have treated the reference object as a regular oop.
  // As a result the copy closure would not have been applied to the
  // referent object.
  //
  // We need to explicitly copy these referent objects - the references
  // will be processed at the end of remarking.
  //
  // We also need to do this copying before we process the reference
  // objects discovered by the STW ref processor in case one of these
  // referents points to another object which is also referenced by an
  // object discovered by the STW ref processor.

  assert(!G1CollectedHeap::use_parallel_gc_threads() ||
           no_of_gc_workers == workers()->active_workers(),
           "Need to reset active GC workers");

  set_par_threads(no_of_gc_workers);
  G1ParPreserveCMReferentsTask keep_cm_referents(this,
                                                 no_of_gc_workers,
                                                 _task_queues);

  if (G1CollectedHeap::use_parallel_gc_threads()) {
    workers()->run_task(&keep_cm_referents);
  } else {
    keep_cm_referents.work(0);
  }

  set_par_threads(0);

  // Closure to test whether a referent is alive.
  G1STWIsAliveClosure is_alive(this);

  // Even when parallel reference processing is enabled, the processing
  // of JNI refs is serial and performed serially by the current thread
  // rather than by a worker. The following PSS will be used for processing
  // JNI refs.

  // Use only a single queue for this PSS.
  G1ParScanThreadState            pss(this, 0, NULL);

  // We do not embed a reference processor in the copying/scanning
  // closures while we're actually processing the discovered
  // reference objects.
  G1ParScanHeapEvacFailureClosure evac_failure_cl(this, &pss, NULL);

  pss.set_evac_failure_closure(&evac_failure_cl);

  assert(pss.queue_is_empty(), "pre-condition");

  G1ParScanExtRootClosure        only_copy_non_heap_cl(this, &pss, NULL);

  G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(this, &pss, NULL);

  OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;

  if (_g1h->g1_policy()->during_initial_mark_pause()) {
    // We also need to mark copied objects.
    copy_non_heap_cl = &copy_mark_non_heap_cl;
  }

  // Keep alive closure.
  G1CopyingKeepAliveClosure keep_alive(this, copy_non_heap_cl, &pss);

  // Serial Complete GC closure
  G1STWDrainQueueClosure drain_queue(this, &pss);

  // Setup the soft refs policy...
  rp->setup_policy(false);

  ReferenceProcessorStats stats;
  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    stats = rp->process_discovered_references(&is_alive,
                                              &keep_alive,
                                              &drain_queue,
                                              NULL,
                                              _gc_timer_stw,
                                              _gc_tracer_stw->gc_id());
  } else {
    // Parallel reference processing
    assert(rp->num_q() == no_of_gc_workers, "sanity");
    assert(no_of_gc_workers <= rp->max_num_q(), "sanity");

    G1STWRefProcTaskExecutor par_task_executor(this, workers(), _task_queues, no_of_gc_workers);
    stats = rp->process_discovered_references(&is_alive,
                                              &keep_alive,
                                              &drain_queue,
                                              &par_task_executor,
                                              _gc_timer_stw,
                                              _gc_tracer_stw->gc_id());
  }

  _gc_tracer_stw->report_gc_reference_stats(stats);

  // We have completed copying any necessary live referent objects.
  assert(pss.queue_is_empty(), "both queue and overflow should be empty");

  double ref_proc_time = os::elapsedTime() - ref_proc_start;
  g1_policy()->phase_times()->record_ref_proc_time(ref_proc_time * 1000.0);
}

// Weak Reference processing during an evacuation pause (part 2).
void G1CollectedHeap::enqueue_discovered_references(uint no_of_gc_workers) {
  double ref_enq_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(!rp->discovery_enabled(), "should have been disabled as part of processing");

  // Now enqueue any remaining on the discovered lists on to
  // the pending list.
  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    rp->enqueue_discovered_references();
  } else {
    // Parallel reference enqueueing

    assert(no_of_gc_workers == workers()->active_workers(),
           "Need to reset active workers");
    assert(rp->num_q() == no_of_gc_workers, "sanity");
    assert(no_of_gc_workers <= rp->max_num_q(), "sanity");

    G1STWRefProcTaskExecutor par_task_executor(this, workers(), _task_queues, no_of_gc_workers);
    rp->enqueue_discovered_references(&par_task_executor);
  }

  rp->verify_no_references_recorded();
  assert(!rp->discovery_enabled(), "should have been disabled");

  // FIXME
  // CM's reference processing also cleans up the string and symbol tables.
  // Should we do that here also? We could, but it is a serial operation
  // and could significantly increase the pause time.

  double ref_enq_time = os::elapsedTime() - ref_enq_start;
  g1_policy()->phase_times()->record_ref_enq_time(ref_enq_time * 1000.0);
}

/**
 * young/mix gc
 * 这个方法是在 G1CollectedHeap::do_collection_pause_at_safepoint 调用的，执行的环境已经是STW了，因此是young/mix gc才会调用的
 * @param evacuation_info
 */
void G1CollectedHeap::evacuate_collection_set(EvacuationInfo& evacuation_info) {
  _expand_heap_after_alloc_failure = true; // 分配失败以后会试图扩展堆内存
  _evacuation_failed = false;

  // Should G1EvacuationFailureALot be in effect for this GC?
  NOT_PRODUCT(set_evacuation_failure_alot_for_current_gc();)
  // 搜索 void G1RemSet::prepare_for_oops_into_collection_set_do()
  g1_rem_set()->prepare_for_oops_into_collection_set_do();

  // Disable the hot card cache.
  G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
  hot_card_cache->reset_hot_cache_claimed_index(); // 这里这是在GC开始的时候重置热卡片缓存的起始处理位置，准备遍历或者清空整个热卡片缓存，并不是清空热卡片缓存
  hot_card_cache->set_use_cache(false);

  const uint n_workers = workers()->active_workers();
    assert(UseDynamicNumberOfGCThreads ||
           n_workers == workers()->total_workers(),
           "If not dynamic should be using all the  workers");
    set_par_threads(n_workers);

  init_for_evac_failure(NULL);
  // 为指向更年轻一代的引用的处理工作做准备。在G1GC的场景下，比如，脏卡片就是从老年代指向年轻代的引用
  // 这个方法我在G1GC中暂时没有看到有用的调用，因为这个方法主要是为后来 younger_refs_iterate()使用，但是我没有看到G1GC调用过 younger_refs_iterate()
  rem_set()->prepare_for_younger_refs_iterate(true);
  // 回收还没有开始，这时候这个G1CollectedHeap::dirty_card_queue_set() 应该是空的
  assert(dirty_card_queue_set().completed_buffers_num() == 0, "Should be empty");
  double start_par_time_sec = os::elapsedTime();
  double end_par_time_sec;

  {
    G1RootProcessor root_processor(this);
    /**
     * 构造G1ParTask对象，这个G1ParTask::work()方法将会被FlexibleWorkGang所管理的多个GangWorker执行，每一个GangWorker其实就是一个NamedThread
     */
    G1ParTask g1_par_task(this, _task_queues, &root_processor);
    // InitialMark needs claim bits to keep track of the marked-through CLDs.
    if (g1_policy()->during_initial_mark_pause()) {
        /**
         * 搜索 void ClassLoaderDataGraph::clear_claimed_marks()
         *  将ClassLoaderDataGraph对象下面的所有的ClassLoaderData的_claimed状态清除
         */
      ClassLoaderDataGraph::clear_claimed_marks();
    }

    /**
     * 是否并行执行
     * 可以看到，这个 use_parallel_gc_threads 方法既用来决定是否并行执行，比如在ConcurrentMark::calc_parallel_marking_threads中
     *      也用来决定是否并发执行，比如在这里
     */
    if (G1CollectedHeap::use_parallel_gc_threads()) { // 如果使用并行标记
      // The individual threads will set their evac-failure closures.
      if (ParallelGCVerbose) G1ParScanThreadState::print_termination_stats_hdr();
      // These tasks use ShareHeap::_process_strong_tasks
      assert(UseDynamicNumberOfGCThreads ||
             workers()->active_workers() == workers()->total_workers(),
             "If not dynamic should be using all the  workers");
      /*
       * workers()返回当前的全局的 FlexibleWorkGang 对象指针, 这个FlexibleWorkGang的构造是在 SharedHeap::SharedHeap的构造函数中进行创建并初始化了它负责的所有的GangWorker
       * run_task的实现，搜索 void FlexibleWorkGang::run_task
       * 以STW并行的方式执行g1_par_task
       * run_task()方法会等待所有的task全部执行完毕
       */
      workers()->run_task(&g1_par_task); // 所有的task全部执行完毕
    } else { //
      g1_par_task.set_for_termination(n_workers);
      g1_par_task.work(0);
    }
    end_par_time_sec = os::elapsedTime();

    // Closing the inner scope will execute the destructor
    // for the G1单独执行RootProcessor object. We record the current
    // elapsed time before closing the scope so that time
    // taken for the destructor is NOT included in the
    // reported parallel time.
  }

  G1GCPhaseTimes* phase_times = g1_policy()->phase_times();

  double par_time_ms = (end_par_time_sec - start_par_time_sec) * 1000.0;
  phase_times->record_par_time(par_time_ms);

  double code_root_fixup_time_ms =
        (os::elapsedTime() - end_par_time_sec) * 1000.0;
  phase_times->record_code_root_fixup_time(code_root_fixup_time_ms);

  set_par_threads(0);

  // Process any discovered reference objects - we have
  // to do this _before_ we retire the GC alloc regions
  // as we may have to copy some 'reachable' referent
  // objects (and their reachable sub-graphs) that were
  // not copied during the pause.
  process_discovered_references(n_workers);

  if (G1StringDedup::is_enabled()) {
    double fixup_start = os::elapsedTime();

    G1STWIsAliveClosure is_alive(this);
    G1KeepAliveClosure keep_alive(this);
    G1StringDedup::unlink_or_oops_do(&is_alive, &keep_alive, true, phase_times);

    double fixup_time_ms = (os::elapsedTime() - fixup_start) * 1000.0;
    phase_times->record_string_dedup_fixup_time(fixup_time_ms);
  }
  // 删除当前的_gc_alloc_regions
  _allocator->release_gc_alloc_regions(n_workers, evacuation_info);
  /**
   * void G1RemSet::cleanup_after_oops_into_collection_set_do()
   */
  g1_rem_set()->cleanup_after_oops_into_collection_set_do(); // 进行已经释放的HeapRegion的rset的清理

  // Reset and re-enable the hot card cache.
  // Note the counts for the cards in the regions in the
  // collection set are reset when the collection set is freed.
  hot_card_cache->reset_hot_cache();
  hot_card_cache->set_use_cache(true);

  purge_code_root_memory();

  if (g1_policy()->during_initial_mark_pause()) {
    // Reset the claim values set during marking the strong code roots
    reset_heap_region_claim_values(); //重置每一个HeapRegion的_claimed的状态为InitialClaimValue
  }

  finalize_for_evac_failure();
  // 如果回收失败
  if (evacuation_failed()) {
      /**
       * 删除自引用指针
       */
    remove_self_forwarding_pointers();

    // Reset the G1EvacuationFailureALot counters and flags
    // Note: the values are reset only when an actual
    // evacuation failure occurs.
    NOT_PRODUCT(reset_evacuation_should_fail();)
  }

  // Enqueue any remaining references remaining on the STW
  // reference processor's discovered lists. We need to do
  // this after the card table is cleaned (and verified) as
  // the act of enqueueing entries on to the pending list
  // will log these updates (and dirty their associated
  // cards). We need these updates logged to update any
  // RSets.
  enqueue_discovered_references(n_workers);
  // 垃圾收集结束了，进行redirty操作，即取出在收集期间的dcqs中的item，进行refine操作
  // 垃圾收集期间的dcqs都存放在 _dirty_card_queue_set 和 _into_cset_dirty_card_queue_set
  redirty_logged_cards();
  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

/**
 * 在大对象回收的时候，或者在普通的GC Pause已经evacuation 成功以后将整个回收集合回收的时候，会调用free_region
 */
void G1CollectedHeap::free_region(HeapRegion* hr,
                                  FreeRegionList* free_list,
                                  bool par,
                                  bool locked) {
  assert(!hr->is_free(), "the region should not be free");
  assert(!hr->is_empty(), "the region should not be empty");
  assert(_hrm.is_available(hr->hrm_index()), "region should be committed");
  assert(free_list != NULL, "pre-condition");

  if (G1VerifyBitmaps) {
    MemRegion mr(hr->bottom(), hr->end());
    concurrent_mark()->clearRangePrevBitmap(mr);
  }

  // Clear the card counts for this region.
  // Note: we only need to do this if the region is not young
  // (since we don't refine cards in young regions).
  if (!hr->is_young()) {
    _cg1r->hot_card_cache()->reset_card_counts(hr);
  }
  hr->hr_clear(par, true /* clear_space */, locked /* locked */);
  free_list->add_ordered(hr); // 将这个Region添加到一级空闲列表中
}

void G1CollectedHeap::free_humongous_region(HeapRegion* hr,
                                     FreeRegionList* free_list,
                                     bool par) {
  assert(hr->startsHumongous(), "this is only for starts humongous regions");
  assert(free_list != NULL, "pre-condition");

  size_t hr_capacity = hr->capacity();
  // We need to read this before we make the region non-humongous,
  // otherwise the information will be gone.
  uint last_index = hr->last_hc_index();
  hr->clear_humongous();
  free_region(hr, free_list, par);

  uint i = hr->hrm_index() + 1;
  while (i < last_index) {
    HeapRegion* curr_hr = region_at(i);
    assert(curr_hr->continuesHumongous(), "invariant");
    curr_hr->clear_humongous();
    free_region(curr_hr, free_list, par);
    i += 1;
  }
}

void G1CollectedHeap::remove_from_old_sets(const HeapRegionSetCount& old_regions_removed,
                                       const HeapRegionSetCount& humongous_regions_removed) {
  if (old_regions_removed.length() > 0 || humongous_regions_removed.length() > 0) {
    MutexLockerEx x(OldSets_lock, Mutex::_no_safepoint_check_flag);
    _old_set.bulk_remove(old_regions_removed);
    _humongous_set.bulk_remove(humongous_regions_removed);
  }

}

void G1CollectedHeap::prepend_to_freelist(FreeRegionList* list) {
  assert(list != NULL, "list can't be null");
  if (!list->is_empty()) {
    MutexLockerEx x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    _hrm.insert_list_into_free_list(list);
  }
}

void G1CollectedHeap::decrement_summary_bytes(size_t bytes) {
  _allocator->decrease_used(bytes);
}

class G1ParCleanupCTTask : public AbstractGangTask {
  G1SATBCardTableModRefBS* _ct_bs;
  G1CollectedHeap* _g1h;
  HeapRegion* volatile _su_head;
public:
  G1ParCleanupCTTask(G1SATBCardTableModRefBS* ct_bs,
                     G1CollectedHeap* g1h) :
    AbstractGangTask("G1 Par Cleanup CT Task"),
    _ct_bs(ct_bs), _g1h(g1h) { }
  /**
   * G1ParCleanupCTTask::work()
   * 调用者是 void G1CollectedHeap::cleanUpCardTable()
   */
  void work(uint worker_id) {
    HeapRegion* r;
    while (r = _g1h->pop_dirty_cards_region()) { // 在GC完成以后，遍历所有的脏卡片的HeapRegion，他们应该已经完全被refine了，因此，不应该再有dirty card和dirty card region了
      clear_cards(r);
    }
  }

  void clear_cards(HeapRegion* r) {
    // Cards of the survivors should have already been dirtied.
    if (!r->is_survivor()) {
      _ct_bs->clear(MemRegion(r->bottom(), r->end()));
    }
  }
};

#ifndef PRODUCT
class G1VerifyCardTableCleanup: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  G1SATBCardTableModRefBS* _ct_bs;
public:
  G1VerifyCardTableCleanup(G1CollectedHeap* g1h, G1SATBCardTableModRefBS* ct_bs)
    : _g1h(g1h), _ct_bs(ct_bs) { }
  virtual bool doHeapRegion(HeapRegion* r) {
    if (r->is_survivor()) {
      _g1h->verify_dirty_region(r);
    } else {
      _g1h->verify_not_dirty_region(r);
    }
    return false;
  }
};

void G1CollectedHeap::verify_not_dirty_region(HeapRegion* hr) {
  // All of the region should be clean.
  G1SATBCardTableModRefBS* ct_bs = g1_barrier_set();
  MemRegion mr(hr->bottom(), hr->end());
  ct_bs->verify_not_dirty_region(mr);
}

void G1CollectedHeap::verify_dirty_region(HeapRegion* hr) {
  // We cannot guarantee that [bottom(),end()] is dirty.  Threads
  // dirty allocated blocks as they allocate them. The thread that
  // retires each region and replaces it with a new one will do a
  // maximal allocation to fill in [pre_dummy_top(),end()] but will
  // not dirty that area (one less thing to have to do while holding
  // a lock). So we can only verify that [bottom(),pre_dummy_top()]
  // is dirty.
  G1SATBCardTableModRefBS* ct_bs = g1_barrier_set();
  MemRegion mr(hr->bottom(), hr->pre_dummy_top());
  if (hr->is_young()) {
    ct_bs->verify_g1_young_region(mr);
  } else {
    ct_bs->verify_dirty_region(mr);
  }
}

void G1CollectedHeap::verify_dirty_young_list(HeapRegion* head) {
  G1SATBCardTableModRefBS* ct_bs = g1_barrier_set();
  for (HeapRegion* hr = head; hr != NULL; hr = hr->get_next_young_region()) {
    verify_dirty_region(hr);
  }
}

void G1CollectedHeap::verify_dirty_young_regions() {
  verify_dirty_young_list(_young_list->first_region());
}

bool G1CollectedHeap::verify_no_bits_over_tams(const char* bitmap_name, CMBitMapRO* bitmap,
                                               HeapWord* tams, HeapWord* end) {
  guarantee(tams <= end,
            err_msg("tams: " PTR_FORMAT " end: " PTR_FORMAT, tams, end));
  HeapWord* result = bitmap->getNextMarkedWordAddress(tams, end);
  if (result < end) {
    gclog_or_tty->cr();
    gclog_or_tty->print_cr("## wrong marked address on %s bitmap: " PTR_FORMAT,
                           bitmap_name, result);
    gclog_or_tty->print_cr("## %s tams: " PTR_FORMAT " end: " PTR_FORMAT,
                           bitmap_name, tams, end);
    return false;
  }
  return true;
}

bool G1CollectedHeap::verify_bitmaps(const char* caller, HeapRegion* hr) {
  CMBitMapRO* prev_bitmap = concurrent_mark()->prevMarkBitMap();
  CMBitMapRO* next_bitmap = (CMBitMapRO*) concurrent_mark()->nextMarkBitMap();

  HeapWord* bottom = hr->bottom();
  HeapWord* ptams  = hr->prev_top_at_mark_start();
  HeapWord* ntams  = hr->next_top_at_mark_start();
  HeapWord* end    = hr->end();

  bool res_p = verify_no_bits_over_tams("prev", prev_bitmap, ptams, end);

  bool res_n = true;
  // We reset mark_in_progress() before we reset _cmThread->in_progress() and in this window
  // we do the clearing of the next bitmap concurrently. Thus, we can not verify the bitmap
  // if we happen to be in that state.
  if (mark_in_progress() || !_cmThread->in_progress()) {
    res_n = verify_no_bits_over_tams("next", next_bitmap, ntams, end);
  }
  if (!res_p || !res_n) {
    gclog_or_tty->print_cr("#### Bitmap verification failed for " HR_FORMAT,
                           HR_FORMAT_PARAMS(hr));
    gclog_or_tty->print_cr("#### Caller: %s", caller);
    return false;
  }
  return true;
}

void G1CollectedHeap::check_bitmaps(const char* caller, HeapRegion* hr) {
  if (!G1VerifyBitmaps) return;

  guarantee(verify_bitmaps(caller, hr), "bitmap verification");
}

class G1VerifyBitmapClosure : public HeapRegionClosure {
private:
  const char* _caller;
  G1CollectedHeap* _g1h;
  bool _failures;

public:
  G1VerifyBitmapClosure(const char* caller, G1CollectedHeap* g1h) :
    _caller(caller), _g1h(g1h), _failures(false) { }

  bool failures() { return _failures; }

  virtual bool doHeapRegion(HeapRegion* hr) {
    if (hr->continuesHumongous()) return false;

    bool result = _g1h->verify_bitmaps(_caller, hr);
    if (!result) {
      _failures = true;
    }
    return false;
  }
};

void G1CollectedHeap::check_bitmaps(const char* caller) {
  if (!G1VerifyBitmaps) return;

  G1VerifyBitmapClosure cl(caller, this);
  heap_region_iterate(&cl);
  guarantee(!cl.failures(), "bitmap verification");
}

class G1CheckCSetFastTableClosure : public HeapRegionClosure {
 private:
  bool _failures;
 public:
  G1CheckCSetFastTableClosure() : HeapRegionClosure(), _failures(false) { }

  virtual bool doHeapRegion(HeapRegion* hr) {
    uint i = hr->hrm_index();
    InCSetState cset_state = (InCSetState) G1CollectedHeap::heap()->_in_cset_fast_test.get_by_index(i);
    if (hr->isHumongous()) {
      if (hr->in_collection_set()) {
        gclog_or_tty->print_cr("\n## humongous region %u in CSet", i);
        _failures = true;
        return true;
      }
      if (cset_state.is_in_cset()) {
        gclog_or_tty->print_cr("\n## inconsistent cset state %d for humongous region %u", cset_state.value(), i);
        _failures = true;
        return true;
      }
      if (hr->continuesHumongous() && cset_state.is_humongous()) {
        gclog_or_tty->print_cr("\n## inconsistent cset state %d for continues humongous region %u", cset_state.value(), i);
        _failures = true;
        return true;
      }
    } else {
      if (cset_state.is_humongous()) {
        gclog_or_tty->print_cr("\n## inconsistent cset state %d for non-humongous region %u", cset_state.value(), i);
        _failures = true;
        return true;
      }
      if (hr->in_collection_set() != cset_state.is_in_cset()) {
        gclog_or_tty->print_cr("\n## in CSet %d / cset state %d inconsistency for region %u",
                               hr->in_collection_set(), cset_state.value(), i);
       _failures = true;
       return true;
      }
      if (cset_state.is_in_cset()) {
        if (hr->is_young() != (cset_state.is_young())) {
          gclog_or_tty->print_cr("\n## is_young %d / cset state %d inconsistency for region %u",
                                 hr->is_young(), cset_state.value(), i);
          _failures = true;
          return true;
        }
        if (hr->is_old() != (cset_state.is_old())) {
          gclog_or_tty->print_cr("\n## is_old %d / cset state %d inconsistency for region %u",
                                 hr->is_old(), cset_state.value(), i);
          _failures = true;
          return true;
        }
      }
    }
    return false;
  }

  bool failures() const { return _failures; }
};

bool G1CollectedHeap::check_cset_fast_test() {
  G1CheckCSetFastTableClosure cl;
  _hrm.iterate(&cl);
  return !cl.failures();
}
#endif // PRODUCT

/**
 * 调用者是 G1RemSet::cleanup_after_oops_into_collection_set_do
 */
void G1CollectedHeap::cleanUpCardTable() {
  G1SATBCardTableModRefBS* ct_bs = g1_barrier_set();
  double start = os::elapsedTime();

  {
    // Iterate over the dirty cards region list.
    /**
     * 具体实现，搜索 G1ParCleanupCTTask::work()
     * 这个方法的调用发生在已经完成了evacuation pause以后
     * 将对应的含有脏卡片的HeapRegion的卡片置为净卡片
     */
    G1ParCleanupCTTask cleanup_task(ct_bs, this);

    if (G1CollectedHeap::use_parallel_gc_threads()) {
      set_par_threads();
      workers()->run_task(&cleanup_task); // 并发进行卡表的清理工作
      set_par_threads(0);
    } else {
      while (_dirty_cards_region_list) {
        HeapRegion* r = _dirty_cards_region_list;
        cleanup_task.clear_cards(r);
        _dirty_cards_region_list = r->get_next_dirty_cards_region();
        if (_dirty_cards_region_list == r) {
          // The last region.
          _dirty_cards_region_list = NULL;
        }
        r->set_next_dirty_cards_region(NULL);
      }
    }
#ifndef PRODUCT
    if (G1VerifyCTCleanup || VerifyAfterGC) {
      G1VerifyCardTableCleanup cleanup_verifier(this, ct_bs);
      heap_region_iterate(&cleanup_verifier);
    }
#endif
  }

  double elapsed = os::elapsedTime() - start;
  g1_policy()->phase_times()->record_clear_ct_time(elapsed * 1000.0);
}

void G1CollectedHeap::free_collection_set(HeapRegion* cs_head, EvacuationInfo& evacuation_info) {
  size_t pre_used = 0;
  FreeRegionList local_free_list("Local List for CSet Freeing");

  double young_time_ms     = 0.0;
  double non_young_time_ms = 0.0;

  // Since the collection set is a superset of the the young list,
  // all we need to do to clear the young list is clear its
  // head and length, and unlink any young regions in the code below
  _young_list->clear();

  G1CollectorPolicy* policy = g1_policy();

  double start_sec = os::elapsedTime();
  bool non_young = true;

  HeapRegion* cur = cs_head;
  int age_bound = -1;
  size_t rs_lengths = 0;

  while (cur != NULL) {
    assert(!is_on_master_free_list(cur), "sanity");
    if (non_young) {
      if (cur->is_young()) {
        double end_sec = os::elapsedTime();
        double elapsed_ms = (end_sec - start_sec) * 1000.0;
        non_young_time_ms += elapsed_ms;

        start_sec = os::elapsedTime();
        non_young = false;
      }
    } else {
      if (!cur->is_young()) {
        double end_sec = os::elapsedTime();
        double elapsed_ms = (end_sec - start_sec) * 1000.0;
        young_time_ms += elapsed_ms;

        start_sec = os::elapsedTime();
        non_young = true;
      }
    }

    rs_lengths += cur->rem_set()->occupied_locked();

    HeapRegion* next = cur->next_in_collection_set();
    assert(cur->in_collection_set(), "bad CS");
    cur->set_next_in_collection_set(NULL);
    cur->set_in_collection_set(false);

    if (cur->is_young()) {
      int index = cur->young_index_in_cset();
      assert(index != -1, "invariant");
      assert((uint) index < policy->young_cset_region_length(), "invariant");
      size_t words_survived = _surviving_young_words[index];
      cur->record_surv_words_in_group(words_survived);

      // At this point the we have 'popped' cur from the collection set
      // (linked via next_in_collection_set()) but it is still in the
      // young list (linked via next_young_region()). Clear the
      // _next_young_region field.
      cur->set_next_young_region(NULL);
    } else {
      int index = cur->young_index_in_cset();
      assert(index == -1, "invariant");
    }

    assert( (cur->is_young() && cur->young_index_in_cset() > -1) ||
            (!cur->is_young() && cur->young_index_in_cset() == -1),
            "invariant" );

    if (!cur->evacuation_failed()) {
      MemRegion used_mr = cur->used_region();

      // And the region is empty.
      assert(!used_mr.is_empty(), "Should not have empty regions in a CS.");
      pre_used += cur->used();
      free_region(cur, &local_free_list, false /* par */, true /* locked */); // 释放这个Region，会把这个Region放回到free_list中
    } else {
      cur->uninstall_surv_rate_group();
      if (cur->is_young()) {
        cur->set_young_index_in_cset(-1);
      }
      cur->set_evacuation_failed(false);
      // The region is now considered to be old.
      cur->set_old();
      _old_set.add(cur);
      evacuation_info.increment_collectionset_used_after(cur->used());
    }
    cur = next;
  }

  evacuation_info.set_regions_freed(local_free_list.length());
  policy->record_max_rs_lengths(rs_lengths);
  policy->cset_regions_freed();

  double end_sec = os::elapsedTime();
  double elapsed_ms = (end_sec - start_sec) * 1000.0;

  if (non_young) {
    non_young_time_ms += elapsed_ms;
  } else {
    young_time_ms += elapsed_ms;
  }

  prepend_to_freelist(&local_free_list);
  decrement_summary_bytes(pre_used);
  policy->phase_times()->record_young_free_cset_time_ms(young_time_ms);
  policy->phase_times()->record_non_young_free_cset_time_ms(non_young_time_ms);
}

class G1FreeHumongousRegionClosure : public HeapRegionClosure {
 private:
  FreeRegionList* _free_region_list;
  HeapRegionSet* _proxy_set;
  HeapRegionSetCount _humongous_regions_removed;
  size_t _freed_bytes;
 public:

  G1FreeHumongousRegionClosure(FreeRegionList* free_region_list) :
    _free_region_list(free_region_list), _humongous_regions_removed(), _freed_bytes(0) {
  }

  virtual bool doHeapRegion(HeapRegion* r) {
    if (!r->startsHumongous()) { // 这个Closure只会对巨型对象的第一个HeapRegion进行处理，其它HeapRegion不负责
      return false;
    }

    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    oop obj = (oop)r->bottom();
    CMBitMap* next_bitmap = g1h->concurrent_mark()->nextMarkBitMap();

    // The following checks whether the humongous object is live are sufficient.
    // The main additional check (in addition to having a reference from the roots
    // or the young gen) is whether the humongous object has a remembered set entry.
    //
    // A humongous object cannot be live if there is no remembered set for it
    // because:
    // - there can be no references from within humongous starts regions referencing
    // the object because we never allocate other objects into them.
    // (I.e. there are no intra-region references that may be missed by the
    // remembered set)
    // - as soon there is a remembered set entry to the humongous starts region
    // (i.e. it has "escaped" to an old object) this remembered set entry will stay
    // until the end of a concurrent mark.
    //
    // It is not required to check whether the object has been found dead by marking
    // or not, in fact it would prevent reclamation within a concurrent cycle, as
    // all objects allocated during that time are considered live.
    // SATB marking is even more conservative than the remembered set.
    // So if at this point in the collection there is no remembered set entry,
    // nobody has a reference to it.
    // At the start of collection we flush all refinement logs, and remembered sets
    // are completely up-to-date wrt to references to the humongous object.
    //
    // Other implementation considerations:
    // - never consider object arrays at this time because they would pose
    // considerable effort for cleaning up the the remembered sets. This is
    // required because stale remembered sets might reference locations that
    // are currently allocated into.
    /**
     * 这段注释解释了在 G1 垃圾收集器中如何确定巨型对象（humongous objects）是否存活的过程，以及为什么这些检查是足够的。以下是对每一部分的详细解释：
     * 为了确定一个巨型对象是否存活，除了检查它是否有来自根集（roots）或年轻代（young gen）的引用外，还要检查它是否有一个记忆集（remembered set）条目。
     *
     * 这部分解释了为什么没有记忆集条目的巨型对象不可能是存活的：
            内部引用不存在：巨型对象区域（humongous starts regions）内部不包含其他对象，因此没有可能会被记忆集忽略的区域内引用。这意味着巨型对象的引用只能来自外部，而外部引用会在记忆集中记录。
            记忆集条目持久存在：一旦一个巨型对象的起始区域有RSet（表示它被引用），这个条目将会保留直到并发标记的结束。因此，如果记忆集中没有RSet，就意味着没有外部引用指向这个巨型对象。

        标记状态检查并非必须：不需要检查对象在标记过程中是否被标记为死亡。如果进行了这种检查，反而可能会阻止在并发标记周期内回收巨型对象，因为在这个周期内分配的所有对象都被默认视为存活。

        SATB（Snapshot-At-The-Beginning）标记更为保守：SATB 标记比记忆集更加保守。因此，如果此时记忆集中没有条目，就可以确信没有引用指向该巨型对象。

       在垃圾收集开始时，所有的 refinement logs（引用精化日志）都会被刷新，这确保了记忆集已经完全更新，能够准确反映出当前对巨型对象的引用情况。

       数组对象的特殊处理：此时不会考虑数组类型的对象，因为清理它们的记忆集会花费相当大的工作量。这是因为旧的记忆集条目可能会引用当前已经被重新分配的内存位置，这种情况会增加清理的复杂性。

     */
    uint region_idx = r->hrm_index();
    if (!g1h->is_humongous_reclaim_candidate(region_idx) || // 搜索 set_humongous_reclaim_candidate
        !r->rem_set()->is_empty()) {
        // 如果这个 Humongous Start Regin不是Candidate，或者它的RSet并不是空的，那么说明有从外面指过来的引用，不回收

      if (G1TraceEagerReclaimHumongousObjects) {
        gclog_or_tty->print_cr("Live humongous region %u size " SIZE_FORMAT " start " PTR_FORMAT " length " UINT32_FORMAT " with remset " SIZE_FORMAT " code roots " SIZE_FORMAT " is marked %d reclaim candidate %d type array %d",
                               region_idx,
                               obj->size()*HeapWordSize,
                               r->bottom(),
                               r->region_num(),
                               r->rem_set()->occupied(),
                               r->rem_set()->strong_code_roots_list_length(),
                               next_bitmap->isMarked(r->bottom()),
                               g1h->is_humongous_reclaim_candidate(region_idx),
                               obj->is_typeArray()
                              );
      }

      return false;
    }
    // 执行到这里，可以知道这个HeapRegion已经被标记位激进回收的候选对象，并且RSet是空的
    // 只处理typeArray
    guarantee(obj->is_typeArray(),
              err_msg("Only eagerly reclaiming type arrays is supported, but the object "
                      PTR_FORMAT " is not.",
                      r->bottom()));

    if (G1TraceEagerReclaimHumongousObjects) {
      gclog_or_tty->print_cr("Dead humongous region %u size " SIZE_FORMAT " start " PTR_FORMAT " length " UINT32_FORMAT " with remset " SIZE_FORMAT " code roots " SIZE_FORMAT " is marked %d reclaim candidate %d type array %d",
                             region_idx,
                             obj->size()*HeapWordSize,
                             r->bottom(),
                             r->region_num(),
                             r->rem_set()->occupied(),
                             r->rem_set()->strong_code_roots_list_length(),
                             next_bitmap->isMarked(r->bottom()),
                             g1h->is_humongous_reclaim_candidate(region_idx),
                             obj->is_typeArray()
                            );
    }
    // Need to clear mark bit of the humongous object if already set.
    // 此时，我们已经确定可以回收这个巨型对象的HeapRegion了，因此，如果对象在并发标记位图中已标记为存活，则清除该标记位。
    if (next_bitmap->isMarked(r->bottom())) {
      next_bitmap->clear(r->bottom());
    }
    _freed_bytes += r->used(); // 更新已释放的字节数 _freed_bytes。
    r->set_containing_set(NULL); // 将堆区域从包含集（containing set）中移除。
    _humongous_regions_removed.increment(1u, r->capacity()); // 增加 _humongous_regions_removed 计数，表示已移除一个巨型对象区域。
    g1h->free_humongous_region(r, _free_region_list, false); // 调用 free_humongous_region 方法实际释放区域，并将其加入 _free_region_list。

    return false;
  }

  HeapRegionSetCount& humongous_free_count() {
    return _humongous_regions_removed;
  }

  size_t bytes_freed() const {
    return _freed_bytes;
  }

  size_t humongous_reclaimed() const {
    return _humongous_regions_removed.length();
  }
};

void G1CollectedHeap::eagerly_reclaim_humongous_regions() {
  assert_at_safepoint(true);
  /**
   *   experimental(bool, G1EagerReclaimHumongousObjects, true,                  \
          "Try to reclaim dead large objects at every young GC.")           \
   */
  if (!G1EagerReclaimHumongousObjects ||
      (!_has_humongous_reclaim_candidates && !G1TraceEagerReclaimHumongousObjects)) {
    g1_policy()->phase_times()->record_fast_reclaim_humongous_time_ms(0.0, 0);
    return;
  }

  double start_time = os::elapsedTime();

  FreeRegionList local_cleanup_list("Local Humongous Cleanup List");

  G1FreeHumongousRegionClosure cl(&local_cleanup_list);
  heap_region_iterate(&cl);

  HeapRegionSetCount empty_set;
  remove_from_old_sets(empty_set, cl.humongous_free_count());

  G1HRPrinter* hr_printer = _g1h->hr_printer();
  if (hr_printer->is_active()) {
    FreeRegionListIterator iter(&local_cleanup_list);
    while (iter.more_available()) {
      HeapRegion* hr = iter.get_next();
      hr_printer->cleanup(hr);
    }
  }

  prepend_to_freelist(&local_cleanup_list); // 将 local_cleanup_list 中的所有回收区域加入到 G1 GC 的自由区域列表（free list）中，以便这些区域可以被重新分配。
  decrement_summary_bytes(cl.bytes_freed());

  g1_policy()->phase_times()->record_fast_reclaim_humongous_time_ms((os::elapsedTime() - start_time) * 1000.0,
                                                                    cl.humongous_reclaimed());
}

// This routine is similar to the above but does not record
// any policy statistics or update free lists; we are abandoning
// the current incremental collection set in preparation of a
// full collection. After the full GC we will start to build up
// the incremental collection set again.
// This is only called when we're doing a full collection
// and is immediately followed by the tearing down of the young list.

void G1CollectedHeap::abandon_collection_set(HeapRegion* cs_head) {
  HeapRegion* cur = cs_head;

  while (cur != NULL) {
    HeapRegion* next = cur->next_in_collection_set();
    assert(cur->in_collection_set(), "bad CS");
    cur->set_next_in_collection_set(NULL);
    cur->set_in_collection_set(false);
    cur->set_young_index_in_cset(-1);
    cur = next;
  }
}

void G1CollectedHeap::set_free_regions_coming() {
  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [cm thread] : "
                           "setting free regions coming");
  }

  assert(!free_regions_coming(), "pre-condition");
  _free_regions_coming = true;
}

void G1CollectedHeap::reset_free_regions_coming() {
  assert(free_regions_coming(), "pre-condition");

  {
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    _free_regions_coming = false;
    SecondaryFreeList_lock->notify_all();
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [cm thread] : "
                           "reset free regions coming");
  }
}

void G1CollectedHeap::wait_while_free_regions_coming() {
  // Most of the time we won't have to wait, so let's do a quick test
  // first before we take the lock.
  if (!free_regions_coming()) {
    return;
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [other] : "
                           "waiting for free regions");
  }

  {
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    while (free_regions_coming()) {
      SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag); // 在二级空闲列表中等待
    }
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [other] : "
                           "done waiting for free regions");
  }
}

/**
 * 创建了一个eden space的region(肯定是用户层面导致的创建，因此是eden space)
 * @param hr
 */
void G1CollectedHeap::set_region_short_lived_locked(HeapRegion* hr) {
  assert(heap_lock_held_for_gc(),
              "the heap lock should already be held by or for this thread");
  // 搜索 YoungList::push_region
  _young_list->push_region(hr);
}

class NoYoungRegionsClosure: public HeapRegionClosure {
private:
  bool _success;
public:
  NoYoungRegionsClosure() : _success(true) { }
  bool doHeapRegion(HeapRegion* r) {
    if (r->is_young()) {
      gclog_or_tty->print_cr("Region [" PTR_FORMAT ", " PTR_FORMAT ") tagged as young",
                             r->bottom(), r->end());
      _success = false;
    }
    return false;
  }
  bool success() { return _success; }
};

bool G1CollectedHeap::check_young_list_empty(bool check_heap, bool check_sample) {
  bool ret = _young_list->check_list_empty(check_sample);

  if (check_heap) {
    NoYoungRegionsClosure closure;
    heap_region_iterate(&closure);
    ret = ret && closure.success();
  }

  return ret;
}

/**
 * 遍历所有区域，并根据区域的类型进行不同的处理：
    老年代区域（old regions）：从 _old_set 中删除。
    空闲区域（free regions）、年轻代区域（young regions）、巨型对象区域（humongous regions）：跳过，不进行处理。
 */
class TearDownRegionSetsClosure : public HeapRegionClosure {
private:
  HeapRegionSet *_old_set;

public:
  TearDownRegionSetsClosure(HeapRegionSet* old_set) : _old_set(old_set) { }

  bool doHeapRegion(HeapRegion* r) {
    if (r->is_old()) {
      _old_set->remove(r); // 从_old_set中删除所有的old_region
    } else {
      // We ignore free regions, we'll empty the free list afterwards.
      // We ignore young regions, we'll empty the young list afterwards.
      // We ignore humongous regions, we're not tearing down the
      // humongous regions set.
      assert(r->is_free() || r->is_young() || r->isHumongous(),
             "it cannot be another type");
    }
    return false;
  }

  ~TearDownRegionSetsClosure() {
    assert(_old_set->is_empty(), "post-condition");
  }
};

/**
 * 在void G1CollectedHeap::shrink(size_t shrink_bytes)  中调用的时候，free_list_only = true
 * 在 do_collection()即full gc中调用的时候， free_list_only = false
 */
void G1CollectedHeap::tear_down_region_sets(bool free_list_only) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  if (!free_list_only) { // 在full gc的时候，free_list_only = false
    TearDownRegionSetsClosure cl(&_old_set);
    heap_region_iterate(&cl);

    // Note that emptying the _young_list is postponed and instead done as
    // the first step when rebuilding the regions sets again. The reason for
    // this is that during a full GC string deduplication needs to know if
    // a collected region was young or old when the full GC was initiated.
  }
  _hrm.remove_all_free_regions();
}

class RebuildRegionSetsClosure : public HeapRegionClosure {
private:
  bool            _free_list_only;
  HeapRegionSet*   _old_set;
  HeapRegionManager*   _hrm;
  size_t          _total_used;

public:
  RebuildRegionSetsClosure(bool free_list_only,
                           HeapRegionSet* old_set, HeapRegionManager* hrm) :
    _free_list_only(free_list_only),
    _old_set(old_set), _hrm(hrm), _total_used(0) {
    assert(_hrm->num_free_regions() == 0, "pre-condition");
    if (!free_list_only) {
      assert(_old_set->is_empty(), "pre-condition");
    }
  }
  /**
   * 这里的含义是，如果是full gc触发的(_free_list_only=true)，那么如果region是空的，那么全部加入到free_list中去
   * @param r
   * @return
   */
  bool doHeapRegion(HeapRegion* r) {
      /**
       * 这个region只是巨型对象的一部分,不处理
       */
    if (r->continuesHumongous()) {
      return false;
    }
    // 这里的意思是，无论怎样，只要这个Region是空的，就添加到_free_list中去。
    // 而如果这个Region不是空的并且_free_list_only=false（full gc的时候）,这个Region添加到_old_set中去
    // 所以，如果仅仅是shrink(_free_list_only=true)，那么只会将空的region添加到free list
    if (r->is_empty()) { // region本来就是空的，那么还是作为young
      // Add free regions to the free list
      r->set_free();  // HeapRegion::set_free
      r->set_allocation_context(AllocationContext::system());
      _hrm->insert_into_free_list(r); // 把这个region添加到hrm的freelist中去
    } else if (!_free_list_only) { // 如果这个Region不是空的，并且_free_list_only == false
      assert(!r->is_young(), "we should not come across young regions"); // 肯定不是Young Region

      if (r->isHumongous()) {
        // We ignore humongous regions, we left the humongous set unchanged
      } else {
        // Objects that were compacted would have ended up on regions
        // that were previously old or free.
        assert(r->is_free() || r->is_old(), "invariant");
        // We now consider them old, so register as such.
        r->set_old(); // 搜索 HeapRegion::set_old
        _old_set->add(r); // 将Region添加到_old_set中
      }
      _total_used += r->used();
    }

    return false;
  }

  size_t total_used() {
    return _total_used;
  }
};

/**
 * 在G1CollectedHeap::shrink 和 G1CollectedHeap::do_collection 中被调用
 * 重建Region Set/List，以便重新填充它们以反映堆的内容。
 * 唯一的例外是最初没有被拆除的巨型对象的Region。如果 free_list_only 为真，它将仅重建 master free list。
 * 这个方法 它在Full GC（free_list_only == false）或Heap Shrink（free_list_only == true）后调用。
 * 这里的含义是，在Full GC（free_list_only == false）的时候，会解散整个的Young List。如果Young List中的Region是空的,就加入到FreeList中，如果不为空，就放到Old Set中
 * 在Heap Shrink（free_list_only == true）(搜索 resize_if_necessary_after_full_collection)的时候，不会解散Young List，只会把空的Region加入到FreeList中，而不会把不空的Region加入到old set 中
 * @param free_list_only
 */
void G1CollectedHeap::rebuild_region_sets(bool free_list_only) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  // 如果不仅仅是_free_list(full gc的时候)，那么清空Young List
  if (!free_list_only) {
    _young_list->empty_list(); // 搜索 void YoungList::empty_list()，这里会把所有的Young Region设置成Old Region
  }

  RebuildRegionSetsClosure cl(free_list_only, &_old_set, &_hrm);
  heap_region_iterate(&cl); // 遍历HeapRegionManager中的_regions

  // 如果不仅仅是_free_list(full gc的时候)，那么清空Young List
  if (!free_list_only) {
    _allocator->set_used(cl.total_used());
  }
  assert(_allocator->used_unlocked() == recalculate_used(),
         err_msg("inconsistent _allocator->used_unlocked(), "
                 "value: " SIZE_FORMAT " recalculated: " SIZE_FORMAT,
                 _allocator->used_unlocked(), recalculate_used()));
}

void G1CollectedHeap::set_refine_cte_cl_concurrency(bool concurrent) {
  _refine_cte_cl->set_concurrent(concurrent);
}

bool G1CollectedHeap::is_in_closed_subset(const void* p) const {
  HeapRegion* hr = heap_region_containing(p);
  return hr->is_in(p);
}

// Methods for the mutator alloc region
/**
 * 区别 new_gc_alloc_region
 * @param word_size
 * @param force
 * @return
 */

HeapRegion* G1CollectedHeap::new_mutator_alloc_region(size_t word_size,
                                                      bool force) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(!force || g1_policy()->can_expand_young_list(),
         "if force is true we should be able to expand the young list");
  /**
   * 判断当前的_young_list的长度是否等于目标长度
   */
  bool young_list_full = g1_policy()->is_young_list_full();
  if (force || !young_list_full) {
      /**
       * 创建一个新的Young Region
       */
    HeapRegion* new_alloc_region = new_region(word_size,
                                              false /* is_old */, // 不是old，因为是为用户分配，肯定是分配在Young region
                                              false /* do_expand */); // 由于是MutatorAllocRegion，不是GC触发的，因此不进行扩展
    if (new_alloc_region != NULL) { // 创建成功
      set_region_short_lived_locked(new_alloc_region); // 将这个Region添加到YoungList中去
      _hr_printer.alloc(new_alloc_region, G1HRPrinter::Eden, young_list_full);
      check_bitmaps("Mutator Region Allocation", new_alloc_region);
      return new_alloc_region;
    }
  }
  return NULL;
}

/**
 * 在 MutatorAllocRegion::retire_region 中调用，
 * 区分G1CollectedHeap::retire_mutator_alloc_region 和 G1CollectedHeap::retire_gc_alloc_region
 * 前者是retire enden space，后者是retire survivor和old space
 * @param alloc_region
 * @param allocated_bytes
 */
void G1CollectedHeap::retire_mutator_alloc_region(HeapRegion* alloc_region,
                                                  size_t allocated_bytes) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(alloc_region->is_eden(), "all mutator alloc regions should be eden");
  /**
   * 将这个eden region添加到回收集合cset中,这里的添加方式是将alloc_region添加到回收集合链表的左侧
   * 同时，计算这个region的一些RSet的一些统计信息，这些信息将用来为了进行回收时间的预测
   */
  g1_policy()->add_region_to_incremental_cset_lhs(alloc_region);
  _allocator->increase_used(allocated_bytes);
  _hr_printer.retire(alloc_region);
  // We update the eden sizes here, when the region is retired,
  // instead of when it's allocated, since this is the point that its
  // used space has been recored in _summary_bytes_used.
  g1mm()->update_eden_size();
}

void G1CollectedHeap::set_par_threads() {
  // Don't change the number of workers.  Use the value previously set
  // in the workgroup.
  assert(G1CollectedHeap::use_parallel_gc_threads(), "shouldn't be here otherwise");
  uint n_workers = workers()->active_workers();
  assert(UseDynamicNumberOfGCThreads ||
           n_workers == workers()->total_workers(),
      "Otherwise should be using the total number of workers");
  if (n_workers == 0) {
    assert(false, "Should have been set in prior evacuation pause.");
    n_workers = ParallelGCThreads;
    workers()->set_active_workers(n_workers);
  }
  set_par_threads(n_workers);
}

// Methods for the GC alloc regions
/**
 *  gc进行的region的分配，因此调用方是
 *  SurvivorGCAllocRegion::allocate_new_region 和 OldGCAllocRegion::allocate_new_region
 *  区别是，SurvivorGCAllocRegion::allocate_new_region调用的时候，dest = Young
 *  OldGCAllocRegion::allocate_new_region调用的时候， dest = Old
 */
HeapRegion* G1CollectedHeap::new_gc_alloc_region(size_t word_size,
                                                 uint count,
                                                 InCSetState dest) {
  assert(FreeList_lock->owned_by_self(), "pre-condition");

  if (count < g1_policy()->max_regions(dest)) {
    const bool is_survivor = (dest.is_young()); // 如果dest = young，那么既然是gc请求分配，就只能是survivor
    /**
     * 搜索 G1CollectedHeap::new_region
     */
    HeapRegion* new_alloc_region = new_region(word_size,
                                              !is_survivor, // 如果是分配survivor，那么第二个参数(is_old)是false，如果是分配old，那么第二个参数(is_old)是true
                                              true /* do_expand */);
    if (new_alloc_region != NULL) {
      // We really only need to do this for old regions given that we
      // should never scan survivors. But it doesn't hurt to do it
      // for survivors too.
      new_alloc_region->record_timestamp(); // 记录这个HeapRegion的timestamp为GC timestamp
      if (is_survivor) {
        new_alloc_region->set_survivor();
        _hr_printer.alloc(new_alloc_region, G1HRPrinter::Survivor);
        check_bitmaps("Survivor Region Allocation", new_alloc_region);
      } else {
        new_alloc_region->set_old();
        _hr_printer.alloc(new_alloc_region, G1HRPrinter::Old);
        check_bitmaps("Old Region Allocation", new_alloc_region);
      }
      bool during_im = g1_policy()->during_initial_mark_pause();
      /**
       * 搜索方法 inline void HeapRegion::note_start_of_copying 查看具体实现
       */
      new_alloc_region->note_start_of_copying(during_im);
      return new_alloc_region;
    }
  }
  return NULL;
}

/**
 * 卸载一个gc alloc region，这里卸载的是gc_alloc_region，因此只有可能是survivor和old region,
 *                  而young region的卸载是retire_mutator_alloc_region
 *                  所以这两个方法的调用只会来自 SurvivorGCAllocRegion::retire_region 和 OldGCAllocRegion::retire_region
 *
 * @param alloc_region 当前需要卸载的region
 * @param allocated_bytes
 * @param dest 代表当前正在卸载的region的类型，对于gc alloc region，
 *             只有可能是survivor或者old，如果是survivor，那么dest=young，如果是old，那么dest = old
 */
void G1CollectedHeap::retire_gc_alloc_region(HeapRegion* alloc_region,
                                             size_t allocated_bytes,
                                             InCSetState dest) {
  bool during_im = g1_policy()->during_initial_mark_pause();
  /**
   * 如果这个暂停是初始标记，那么在数据拷贝完成以后，需要设置old region的ntams
   * 对于survivor region，ntams的值在整个生命周期中始终等于当前HeapRegion的bottom
   *
   * 将survivor region添加到回收集合是在整个evacuation pause结束以后统一从survivor list中添加进去的
   * 搜索 reset_auxilary_lists 查看将survivor region添加到增量回收集合中的过程
   */
  alloc_region->note_end_of_copying(during_im);
  g1_policy()->record_bytes_copied_during_gc(allocated_bytes);
  if (dest.is_young()) { //  当前卸载的是young region，即survivor region
    young_list()->add_survivor_region(alloc_region); // 将刚刚卸载的region加回到survivor list中
  } else { // 当前卸载的是old region，添加到G1CollectedHeap的_old_set中
    _old_set.add(alloc_region);
  }
  _hr_printer.retire(alloc_region);
}

// Heap region set verification

class VerifyRegionListsClosure : public HeapRegionClosure {
private:
  HeapRegionSet*   _old_set;
  HeapRegionSet*   _humongous_set;
  HeapRegionManager*   _hrm;

public:
  HeapRegionSetCount _old_count;
  HeapRegionSetCount _humongous_count;
  HeapRegionSetCount _free_count;

  VerifyRegionListsClosure(HeapRegionSet* old_set,
                           HeapRegionSet* humongous_set,
                           HeapRegionManager* hrm) :
    _old_set(old_set), _humongous_set(humongous_set), _hrm(hrm),
    _old_count(), _humongous_count(), _free_count(){ }

  bool doHeapRegion(HeapRegion* hr) {
    if (hr->continuesHumongous()) {
      return false;
    }

    if (hr->is_young()) {
      // TODO
    } else if (hr->startsHumongous()) {
      assert(hr->containing_set() == _humongous_set, err_msg("Heap region %u is starts humongous but not in humongous set.", hr->hrm_index()));
      _humongous_count.increment(1u, hr->capacity());
    } else if (hr->is_empty()) {
      assert(_hrm->is_free(hr), err_msg("Heap region %u is empty but not on the free list.", hr->hrm_index()));
      _free_count.increment(1u, hr->capacity());
    } else if (hr->is_old()) {
      assert(hr->containing_set() == _old_set, err_msg("Heap region %u is old but not in the old set.", hr->hrm_index()));
      _old_count.increment(1u, hr->capacity());
    } else {
      ShouldNotReachHere();
    }
    return false;
  }

  void verify_counts(HeapRegionSet* old_set, HeapRegionSet* humongous_set, HeapRegionManager* free_list) {
    guarantee(old_set->length() == _old_count.length(), err_msg("Old set count mismatch. Expected %u, actual %u.", old_set->length(), _old_count.length()));
    guarantee(old_set->total_capacity_bytes() == _old_count.capacity(), err_msg("Old set capacity mismatch. Expected " SIZE_FORMAT ", actual " SIZE_FORMAT,
        old_set->total_capacity_bytes(), _old_count.capacity()));

    guarantee(humongous_set->length() == _humongous_count.length(), err_msg("Hum set count mismatch. Expected %u, actual %u.", humongous_set->length(), _humongous_count.length()));
    guarantee(humongous_set->total_capacity_bytes() == _humongous_count.capacity(), err_msg("Hum set capacity mismatch. Expected " SIZE_FORMAT ", actual " SIZE_FORMAT,
        humongous_set->total_capacity_bytes(), _humongous_count.capacity()));

    guarantee(free_list->num_free_regions() == _free_count.length(), err_msg("Free list count mismatch. Expected %u, actual %u.", free_list->num_free_regions(), _free_count.length()));
    guarantee(free_list->total_capacity_bytes() == _free_count.capacity(), err_msg("Free list capacity mismatch. Expected " SIZE_FORMAT ", actual " SIZE_FORMAT,
        free_list->total_capacity_bytes(), _free_count.capacity()));
  }
};

void G1CollectedHeap::verify_region_sets() {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);

  // First, check the explicit lists.
  _hrm.verify();
  {
    // Given that a concurrent operation might be adding regions to
    // the secondary free list we have to take the lock before
    // verifying it.
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    _secondary_free_list.verify_list();
  }

  // If a concurrent region freeing operation is in progress it will
  // be difficult to correctly attributed any free regions we come
  // across to the correct free list given that they might belong to
  // one of several (free_list, secondary_free_list, any local lists,
  // etc.). So, if that's the case we will skip the rest of the
  // verification operation. Alternatively, waiting for the concurrent
  // operation to complete will have a non-trivial effect on the GC's
  // operation (no concurrent operation will last longer than the
  // interval between two calls to verification) and it might hide
  // any issues that we would like to catch during testing.
  if (free_regions_coming()) {
    return;
  }

  // Make sure we append the secondary_free_list on the free_list so
  // that all free regions we will come across can be safely
  // attributed to the free_list.
  append_secondary_free_list_if_not_empty_with_lock();

  // Finally, make sure that the region accounting in the lists is
  // consistent with what we see in the heap.

  VerifyRegionListsClosure cl(&_old_set, &_humongous_set, &_hrm);
  heap_region_iterate(&cl);
  cl.verify_counts(&_old_set, &_humongous_set, &_hrm);
}

// Optimized nmethod scanning

class RegisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->continuesHumongous(),
             err_msg("trying to add code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
                     " starting at " HR_FORMAT,
                     _nm, HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region())));

      // HeapRegion::add_strong_code_root_locked() avoids adding duplicate entries.
      hr->add_strong_code_root_locked(_nm);
    }
  }

public:
  RegisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

class UnregisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->continuesHumongous(),
             err_msg("trying to remove code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
                     " starting at " HR_FORMAT,
                     _nm, HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region())));

      hr->remove_strong_code_root(_nm);
    }
  }

public:
  UnregisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

void G1CollectedHeap::register_nmethod(nmethod* nm) {
  CollectedHeap::register_nmethod(nm);

  guarantee(nm != NULL, "sanity");
  RegisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl);
}

void G1CollectedHeap::unregister_nmethod(nmethod* nm) {
  CollectedHeap::unregister_nmethod(nm);

  guarantee(nm != NULL, "sanity");
  UnregisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl, true);
}

void G1CollectedHeap::purge_code_root_memory() {
  double purge_start = os::elapsedTime();
  G1CodeRootSet::purge();
  double purge_time_ms = (os::elapsedTime() - purge_start) * 1000.0;
  g1_policy()->phase_times()->record_strong_code_root_purge_time(purge_time_ms);
}

class RebuildStrongCodeRootClosure: public CodeBlobClosure {
  G1CollectedHeap* _g1h;

public:
  RebuildStrongCodeRootClosure(G1CollectedHeap* g1h) :
    _g1h(g1h) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = (cb != NULL) ? cb->as_nmethod_or_null() : NULL;
    if (nm == NULL) {
      return;
    }

    if (ScavengeRootsInCode) {
      _g1h->register_nmethod(nm);
    }
  }
};

void G1CollectedHeap::rebuild_strong_code_roots() {
  RebuildStrongCodeRootClosure blob_cl(this);
  CodeCache::blobs_do(&blob_cl);
}
