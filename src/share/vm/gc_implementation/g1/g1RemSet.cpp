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

#include "precompiled.hpp"
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/g1BlockOffsetTable.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1HotCardCache.hpp"
#include "gc_implementation/g1/g1GCPhaseTimes.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/heapRegionManager.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/intHisto.hpp"

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC

#define CARD_REPEAT_HISTO 0

#if CARD_REPEAT_HISTO
static size_t ct_freq_sz;
static jbyte* ct_freq = NULL;

void init_ct_freq_table(size_t heap_sz_bytes) {
  if (ct_freq == NULL) {
    ct_freq_sz = heap_sz_bytes/CardTableModRefBS::card_size;
    ct_freq = new jbyte[ct_freq_sz];
    for (size_t j = 0; j < ct_freq_sz; j++) ct_freq[j] = 0;
  }
}

void ct_freq_note_card(size_t index) {
  assert(0 <= index && index < ct_freq_sz, "Bounds error.");
  if (ct_freq[index] < 100) { ct_freq[index]++; }
}

static IntHistogram card_repeat_count(10, 10);

void ct_freq_update_histo_and_reset() {
  for (size_t j = 0; j < ct_freq_sz; j++) {
    card_repeat_count.add_entry(ct_freq[j]);
    ct_freq[j] = 0;
  }

}
#endif

G1RemSet::G1RemSet(G1CollectedHeap* g1, CardTableModRefBS* ct_bs)
  : _g1(g1), _conc_refine_cards(0),
    _ct_bs(ct_bs), _g1p(_g1->g1_policy()),
    _cg1r(g1->concurrent_g1_refine()), // 一个JVM中只有一个G1RemSet，一个G1RemSet只有一个G1RemSet对象
    _cset_rs_update_cl(NULL),
    _cards_scanned(NULL), _total_cards_scanned(0),
    _prev_period_summary()
{
  guarantee(n_workers() > 0, "There should be some workers");
  _cset_rs_update_cl = NEW_C_HEAP_ARRAY(G1ParPushHeapRSClosure*, n_workers(), mtGC);
  for (uint i = 0; i < n_workers(); i++) {
    _cset_rs_update_cl[i] = NULL;
  }
  if (G1SummarizeRSetStats) {
    _prev_period_summary.initialize(this);
  }
}

G1RemSet::~G1RemSet() {
  for (uint i = 0; i < n_workers(); i++) {
    assert(_cset_rs_update_cl[i] == NULL, "it should be");
  }
  FREE_C_HEAP_ARRAY(G1ParPushHeapRSClosure*, _cset_rs_update_cl, mtGC);
}

void CountNonCleanMemRegionClosure::do_MemRegion(MemRegion mr) {
  if (_g1->is_in_g1_reserved(mr.start())) {
    _n += (int) ((mr.byte_size() / CardTableModRefBS::card_size));
    if (_start_first == NULL) _start_first = mr.start();
  }
}

class ScanRSClosure : public HeapRegionClosure {
  size_t _cards_done, _cards;
  G1CollectedHeap* _g1h;

  G1ParPushHeapRSClosure* _oc; // 搜索 G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss);
  CodeBlobClosure* _code_root_cl;

  G1BlockOffsetSharedArray* _bot_shared;
  G1SATBCardTableModRefBS *_ct_bs;

  double _strong_code_root_scan_time_sec;
  uint   _worker_i;
  int    _block_size;
  bool   _try_claimed;

public:
  ScanRSClosure(G1ParPushHeapRSClosure* oc,
                CodeBlobClosure* code_root_cl,
                uint worker_i) :
    _oc(oc),
    _code_root_cl(code_root_cl),
    _strong_code_root_scan_time_sec(0.0),
    _cards(0),
    _cards_done(0),
    _worker_i(worker_i),
    _try_claimed(false)
  {
    _g1h = G1CollectedHeap::heap();
    _bot_shared = _g1h->bot_shared(); // 全局的 G1BlockOffsetSharedArray 对象
    _ct_bs = _g1h->g1_barrier_set();
    _block_size = MAX2<int>(G1RSetScanBlockSize, 1);
  }

  /**
   * 构造ScanRSClosure的时候，默认_try_claimed = false
   */
  void set_try_claimed() { _try_claimed = true; }

  /**
   * 方法 ScanRSClosure::scanCard
   * 在 ScanRSClosure::doHeapRegion 方法中被调用,index是卡片索引，r是这个卡片所在的region
   * 这个方法会通过 HeapRegionDCTOC  这个 closure 对MemRegion中的每一个obj进行递归扫描，扫描过程中根据上一轮的mark bit map进行对象的过滤，只扫描已经被标记的对象
   * @param index
   * @param r
   */
  void scanCard(size_t index, HeapRegion *r) {
    // Stack allocate the DirtyCardToOopClosure instance
    /**
     * 在这个对象的 HeapRegionDCTOC::walk_mem_region 方法中会根据prev mark bit map来判断对象在上一轮中是否已经被标记
     * _oc  是 G1ParPushHeapRSClosure
     */
    HeapRegionDCTOC cl(_g1h, r, _oc,
                       CardTableModRefBS::Precise); // 在栈上分配一个HeapRegionDCTOC对象，它是DirtyCardToOopClosure的子类

    // Set the "from" region in the closure.
    _oc->set_region(r);
    /**
     * 搜索 BlockOffsetSharedArray::address_for_index,
     * 根据给定的卡片索引（index）创建一个 MemRegion 对象 card_region，表示该卡片覆盖的内存区域。
     */
    MemRegion card_region(_bot_shared->address_for_index(index), G1BlockOffsetSharedArray::N_words);
    /**
     * 表示当前区域（HeapRegion）的预收集（pre-gc）分配的内存区域。
     */
    MemRegion pre_gc_allocated(r->bottom(), r->scan_top());
    /**
     * 查看这个卡片覆盖的区域和预收集区域的交叉部分区域，我们只处理这一片Region区域
     */
    MemRegion mr = pre_gc_allocated.intersection(card_region);
    if (!mr.is_empty() && !_ct_bs->is_card_claimed(index)) {
      // We make the card as "claimed" lazily (so races are possible
      // but they're benign), which reduces the number of duplicate
      // scans (the rsets of the regions in the cset can intersect).
      _ct_bs->set_card_claimed(index);  // 将这个卡片标记为claimed
      _cards_done++; // 计数器+1
      /**
       * 查看 DirtyCardToOopClosure::do_MemRegion，这个Closure会处理脏卡片覆盖的对象
       * mr的区域代表了这个卡片中已经分配了对象的区域
       * 搜索 DirtyCardToOopClosure::do_MemRegion
       */
      cl.do_MemRegion(mr);
    }
  }

  void printCard(HeapRegion* card_region, size_t card_index,
                 HeapWord* card_start) {
    gclog_or_tty->print_cr("T " UINT32_FORMAT " Region [" PTR_FORMAT ", " PTR_FORMAT ") "
                           "RS names card %p: "
                           "[" PTR_FORMAT ", " PTR_FORMAT ")",
                           _worker_i,
                           card_region->bottom(), card_region->end(),
                           card_index,
                           card_start, card_start + G1BlockOffsetSharedArray::N_words);
  }

  void scan_strong_code_roots(HeapRegion* r) {
    double scan_start = os::elapsedTime();
    r->strong_code_roots_do(_code_root_cl);
    _strong_code_root_scan_time_sec += (os::elapsedTime() - scan_start);
  }

  /**
   *
   * 这里的HeapRegion一定是回收结合中的 Region
   * 调用者 G1RemSet::scanRS
   * ScanRSClosure::doHeapRegion
   */
  bool doHeapRegion(HeapRegion* r) {
    assert(r->in_collection_set(), "should only be called on elements of CS.");
    HeapRegionRemSet* hrrs = r->rem_set(); // 获取回收集合中的这个HeapRegion的
    /**
     * 迭代器的三种状态定义在 enum ParIterState { Unclaimed, Claimed, Complete };
     */
    if (hrrs->iter_is_complete()) // 迭代器已经完成，_iter_state状态为Complete，直接返回
        return false; // All done.
      /**
       * 这里的意思是，如果_try_claimed为false，并且hrrs->claim_iter()为false(没有成功通过原子操作获取claim的权利)
       * 查看 HeapRegionRemSet::claim_iter()，主要是操作 _iter_state权限
       * 在G1RemSet::scanRS中会有两次遍历
       *    第一次遍历,_try_claimed=false，意思是目前还没有尝试获取这个region的所有权，因此必须调用hrrs->claim_iter()尝试获取这个HeapRegionRememberSet的所有权
       *        如果claim_iter()成功才会继续执行，claim_iter()失败就返回false
       *    第二次遍历， _try_claimed=true，代码就直接往下面走了，不会在这里返回
       */
    if (!_try_claimed && !hrrs->claim_iter()) // 成功地将_iter_state状态从Unclaimed 变成 Claimed
        return false;

    /**
     * 代码到了这里，说明try_claimed为true ，或者 hrrs->claim_iter()为true，或者两者都为true
     */
    // If we ever free the collection set concurrently, we should also
    // clear the card table concurrently therefore we won't need to
    // add regions of the collection set to the dirty cards region.
    _g1h->push_dirty_cards_region(r); // dirty cards region list中的region都是转移专用记忆集合需要被清空重建的region
    // If we didn't return above, then
    //   _try_claimed || r->claim_iter()
    // is true: either we're supposed to work on claimed-but-not-complete
    // regions, or we successfully claimed the region.
    /**
     * 构造一个HeapRegionRemSet的迭代器，用来对这个HeapRegionRemSet进行遍历
     */
    HeapRegionRemSetIterator iter(hrrs);
    size_t card_index;

    // We claim cards in block so as to recude the contention. The block size is determined by
    // the G1RSetScanBlockSize parameter.
    // 一个block一个block地扫描，这里_block_size是由G1RSetScanBlockSize控制，默认是64,即一次性扫描64张卡片
    /**
     * 第一次，jump_to_card=0，current_card在0 ~ 63 的卡片，会执行遍历
     * 当current_card=64的时候，发现 current_card >= jump_to_card + _block_size， 相当于已经完成了一批card 的遍历，于是修改jump_to_card=64（假如没有竞争）
     * 于是current_card接着不断递增(只要iter.has_next(card_index) 还是返回true)
     * 当current_card=128的时候，发现current_card >= jump_to_card + _block_size，相当于又已经完成了一批card 的遍历，于是修改jump_to_card=192（假如有竞争）
     * current_card不断递增，但是都是current_card < jump_to_card，不做任何处理，直到current_card = jump_to_card，才开始处理，
     *  直到处理到current_card >= jump_to_card + _block_size，才进行下一个block
     */
    size_t jump_to_card = hrrs->iter_claimed_next(_block_size);
      // 这里返回的时候，card_index带回了卡片索引值。搜 HeapRegionRemSetIterator::has_next
    for (size_t current_card = 0;iter.has_next(card_index); current_card++) { // 遍历这个Region的转移专用记忆集合的所有卡片，依次循环代表取出一张卡片
      if (current_card >= jump_to_card + _block_size) { // 已经遍历完了一个block中的卡片
        jump_to_card = hrrs->iter_claimed_next(_block_size); // 再往前进_block_size，比如从64变成128，从128变成192
      }
      if (current_card < jump_to_card) continue;// current_card必须从jump_to_card开始
      // 当current_card等于jump_to_card的时候，执行下面的代码
      HeapWord* card_start = _g1h->bot_shared()->address_for_index(card_index); // 这个卡片索引对应的引用者的堆内存地址
#if 0
      gclog_or_tty->print("Rem set iteration yielded card [" PTR_FORMAT ", " PTR_FORMAT ").\n",
                          card_start, card_start + CardTableModRefBS::card_size_in_words);
#endif
      // 引用者的HeapRegion
      HeapRegion* card_region = _g1h->heap_region_containing(card_start);
      _cards++;

      // 无条件将这个Region加入到 脏卡片region 列表中
      if (!card_region->is_on_dirty_cards_region_list()) {
        _g1h->push_dirty_cards_region(card_region); // 将这个引用者所在的Region放到脏卡片region的list
      }

      // If the card is dirty, then we will scan it during updateRS.
      if (!card_region->in_collection_set() &&
          !_ct_bs->is_card_dirty(card_index)) { // 如果这个卡片不在回收集合中，并且还没有被标记位脏卡片，那么就进行扫描
        scanCard(card_index, card_region); // 扫描引用者对应的region
      }
    }
    if (!_try_claimed) { // 第一次的时候，_try_claimed==false，会处理代码对象，
       // 代码走到这里，说明当前_iter_state的状态肯定为Claimed
      // Scan the strong code root list attached to the current region
      scan_strong_code_roots(r);

      hrrs->set_iter_complete(); // 在HeapRegionRememberSet中设置_iter_state状态为Completed
    }
    return false;
  }

  double strong_code_root_scan_time_sec() {
    return _strong_code_root_scan_time_sec;
  }

  size_t cards_done() { return _cards_done;}
  size_t cards_looked_up() { return _cards;}
};

/**
 * 搜索 G1RemSet::oops_into_collection_set_do 查看调用位置
 * @param oc 搜索 G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss);
 * @param code_root_cl
 * @param worker_i
 */
void G1RemSet::scanRS(G1ParPushHeapRSClosure* oc,
                      CodeBlobClosure* code_root_cl,
                      uint worker_i) {
  double rs_time_start = os::elapsedTime();
  HeapRegion *startRegion = _g1->start_cset_region_for_worker(worker_i); // 获取分配给当前worker的回收集合中的起始HeapRegion

  ScanRSClosure scanRScl(oc, code_root_cl, worker_i); // 扫描RSet的闭包，通过调用 ScanRSClosure::doHeapRegion
  //  G1CollectedHeap::collection_set_iterate_from
  // 首次遍历, _try_claimed 标记为false，那么必须claim成功对应的Region才能对她的card进行scan
  _g1->collection_set_iterate_from(startRegion, &scanRScl);
  scanRScl.set_try_claimed();
  // 二次遍历, _try_claimed = true，意味着，假如有的Region还处于_Claimed但是非Completed的状态，我也可以并发地去处理
  _g1->collection_set_iterate_from(startRegion, &scanRScl);

  double scan_rs_time_sec = (os::elapsedTime() - rs_time_start)
                            - scanRScl.strong_code_root_scan_time_sec();

  assert(_cards_scanned != NULL, "invariant");
  _cards_scanned[worker_i] = scanRScl.cards_done();

  _g1p->phase_times()->record_time_secs(G1GCPhaseTimes::ScanRS, worker_i, scan_rs_time_sec);
  _g1p->phase_times()->record_time_secs(G1GCPhaseTimes::CodeRoots, worker_i, scanRScl.strong_code_root_scan_time_sec());
}

// Closure used for updating RSets and recording references that
// point into the collection set. Only called during an
// evacuation pause.

/**
 * 这个closure是在GC线程中，对 卡片对应的内存进行扫描，如果这个卡片对应的内存对象中有志向CSet的引用，那么就将这个添加到 当前worker线程的 G1ParScanThreadState的引用队列 中
 */
class RefineRecordRefsIntoCSCardTableEntryClosure: public CardTableEntryClosure {
  G1RemSet* _g1rs;
  DirtyCardQueue* _into_cset_dcq;
public:
  RefineRecordRefsIntoCSCardTableEntryClosure(G1CollectedHeap* g1h,
                                              DirtyCardQueue* into_cset_dcq) :
    _g1rs(g1h->g1_rem_set()), _into_cset_dcq(into_cset_dcq)
  {}
    /**
     * RefineRecordRefsIntoCSCardTableEntryClosure::do_card_ptr
     * 调用者搜索 DirtyCardQueue::apply_closure_to_buffer
     * 对比 ConcurrentG1RefineThread 在更新RS的时候使用的Closure RefineCardTableEntryClosure的bool do_card_ptr方法
     */
  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    // The only time we care about recording cards that
    // contain references that point into the collection set
    // is during RSet updating within an evacuation pause.
    // In this case worker_i should be the id of a GC worker thread.
    assert(SafepointSynchronize::is_at_safepoint(), "not during an evacuation pause");
    assert(worker_i < (ParallelGCThreads == 0 ? 1 : ParallelGCThreads), "should be a GC worker");

    /**
     * 实现 搜索 G1RemSet::refine_card
     * 我们唯一关心包含指向回收集合的引用的卡片是在回收暂停期间的 RSet 更新期间。 在这种情况下，worker_i 应该是 GC 工作线程的 id。
     * 这个过程中，如果是GC挑起的 (check_for_refs_into_cset=true)，会将查询到的跨region的field添加到对应的worker thread的 G1ParScanThreadState 的引用队列中
     */
    if (_g1rs->refine_card(card_ptr, worker_i, true)) { // 返回true，说明对应的卡片的确有指向回收集合的引用
        /**
         * 如果卡片包含了指向回收集合的引用，那么我们需要把这个卡片记录在G1CollectedHeap::into_cset_dirty_card_queue_set中
         */
      // 'card_ptr' contains references that point into the collection
      // set. We need to record the card in the DCQS
      // (G1CollectedHeap::into_cset_dirty_card_queue_set())
      // that's used for that purpose.
      //
      // Enqueue the card
      /**
       * 如果卡片的确有指向回收集合的引用，那么我们需要将这个卡片加入到_into_cset_dcq
       * (由于_into_cset_dcq是基于G1CollectedHeap::into_cset_dirty_card_queue_set()构建的，因此实际上是加入到 into_cset_dirty_card_queue_set 中去了)
       * 由于DirtyCardQueue是PtrQueue的子类，这里调用的是 PtrQueue::enqueue() 方法
       */
      _into_cset_dcq->enqueue(card_ptr); // 记录这个包含有指向回收集合的卡片，加入到into_cset_dirty_card_queue_set
    }
    return true;
  }
};

/**
 * 调用方是 G1RemSet::oops_into_collection_set_do
 *
 * 查看最终的 RefineRecordRefsIntoCSCardTableEntryClosure，其实就做了一件事情，
 *      把当前DCQS中的所有的void **buf中指向回收集合的条目添加到into_cset_dcq中去
 * @param into_cset_dcq 这个dcq不是Java
 * @param worker_i
 */
void G1RemSet::updateRS(DirtyCardQueue* into_cset_dcq, uint worker_i) {
  G1GCParPhaseTimesTracker x(_g1p->phase_times(), G1GCPhaseTimes::UpdateRS, worker_i);
  // Apply the given closure to all remaining log entries.
  // 使用 RefineRecordRefsIntoCSCardTableEntryClosure 处理所有剩余的没有处理的DCQ
  RefineRecordRefsIntoCSCardTableEntryClosure into_cset_update_rs_cl(_g1, into_cset_dcq);
  /**
   * 具体实现查看 G1CollectedHeap::iterate_dirty_card_closure
   * 主要是遍历JavaThread的全局DCQS链表中的BufferNode，去apply对应的 RefineRecordRefsIntoCSCardTableEntryClosure
   */
  _g1->iterate_dirty_card_closure(&into_cset_update_rs_cl, into_cset_dcq, false, worker_i);
}

/**
 * 清空所有的HeapRegionRememberSet
 */
void G1RemSet::cleanupHRRS() {
  HeapRegionRemSet::cleanup();
}

/**
 * G1ParTask::work() -> scan_remembered_sets() -> oops_into_collection_set_do()
 * 静态方法
 * 搜 G1RootProcessor::scan_remembered_sets 查看调用位置，而G1RootProcessor::scan_remembered_sets 是在evacuate_root 中被调用的，
 *   即这个方法做的事情就是以RSet为根进行扫描
 */
void G1RemSet::oops_into_collection_set_do(G1ParPushHeapRSClosure* oc,
                                           CodeBlobClosure* code_root_cl,
                                           uint worker_i) {
#if CARD_REPEAT_HISTO
  ct_freq_update_histo_and_reset();
#endif

  // We cache the value of 'oc' closure into the appropriate slot in the
  // _cset_rs_update_cl for this worker
  assert(worker_i < n_workers(), "sanity");
  _cset_rs_update_cl[worker_i] = oc;

  // A DirtyCardQueue that is used to hold cards containing references
  // that point into the collection set. This DCQ is associated with a
  // special DirtyCardQueueSet (see g1CollectedHeap.hpp).  Under normal
  // circumstances (i.e. the pause successfully completes), these cards
  // are just discarded (there's no need to update the RSets of regions
  // that were in the collection set - after the pause these regions
  // are wholly 'free' of live objects. In the event of an evacuation
  // failure the cards/buffers in this queue set are passed to the
  // DirtyCardQueueSet that is used to manage RSet updates
  /**
   * DirtyCardQueue 用于保存包含指向回收集合的引用的卡片。 此 DCQ 与特殊的 DirtyCardQueueSet 关联（请参阅 g1CollectedHeap.hpp）。
   * 在正常情况下（即暂停成功完成），这些卡将被丢弃（无需更新收集集中区域的 RSets - 暂停后，这些区域完全“释放”活动对象。
   *    在这种情况下) ..   当疏散失败时，此队列集中的卡/缓冲区将被传递到用于管理 RSet 更新的 DirtyCardQueueSet
   * 在正常情况下，也就是垃圾收集暂停成功完成的情况下，这些卡片会被丢弃。
   *     这是因为在暂停之后，回收集合中的区域已经不再包含任何活动对象，因此不需要更新这些区域的 RSet（Remembered Set）。
      然而，在发生疏散失败（evacuation failure）时，这些卡片将被传递给管理 RSet 更新的 DirtyCardQueueSet。
        疏散失败是指在将对象从一个区域移动到另一个区域时遇到的问题，导致无法成功完成垃圾收集暂停，我们可以搜索 G1RemSet::cleanup_after_oops_into_collection_set_do 查看搜索失败的时候使用
        这个into_cset_dirty_card_queue_set 来恢复对应的RSet的过程

        创建一个全新的DCQ， 这个DCQ属于一个特殊的DCQS _into_cset_dirty_card_queue_set, 它存放的内容是所有指向了回收集合的DCQ
        在updateRS(&into_cset_dcq, worker_i)中将会往into_cset_dirty_card_queue_set中添加数据
   */
  DirtyCardQueue into_cset_dcq(&_g1->into_cset_dirty_card_queue_set());

  assert((ParallelGCThreads > 0) || worker_i == 0, "invariant");
  /**
   * 搜索 void G1RemSet::updateRS
   * 这里将会往 into_cset_dirty_card_queue_set 中添加指向回收集合的脏卡片
   * into_cset_dirty_card_queue_set仅仅在回收失败的时候被使用，用来恢复Collection Set的RSet
   */
  updateRS(&into_cset_dcq, worker_i);
  /**
   * 搜索 void G1RemSet::scanRS
   */
  scanRS(oc, code_root_cl, worker_i);

  // We now clear the cached values of _cset_rs_update_cl for this worker
  _cset_rs_update_cl[worker_i] = NULL;
}

void G1RemSet::prepare_for_oops_into_collection_set_do() {
  _g1->set_refine_cte_cl_concurrency(false);
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set(); // 用户线程级别的全局DCQS
  dcqs.concatenate_logs(); // 遍历所有用户线程的不完整的logs以及全局的_shared_dirty_card_queue，将这些logs添加到全局的已完成的链表中去

  guarantee( _cards_scanned == NULL, "invariant" );
  _cards_scanned = NEW_C_HEAP_ARRAY(size_t, n_workers(), mtGC); // 为每一个worker记录已经扫描的卡片的数量
  for (uint i = 0; i < n_workers(); ++i) { // 搜索 G1RemSet::scanRS查看对 _cards_scanned 的赋值
    _cards_scanned[i] = 0;
  }
  _total_cards_scanned = 0; // 扫描数量清零，准备扫描
}

void G1RemSet::cleanup_after_oops_into_collection_set_do() {
  guarantee( _cards_scanned != NULL, "invariant" );
  _total_cards_scanned = 0;
  for (uint i = 0; i < n_workers(); ++i) {
    _total_cards_scanned += _cards_scanned[i]; //  统计每一个worker扫描的卡片数量
  }
  FREE_C_HEAP_ARRAY(size_t, _cards_scanned, mtGC);
  _cards_scanned = NULL;
  // Cleanup after copy
  _g1->set_refine_cte_cl_concurrency(true);
  // Set all cards back to clean.
  /**
   * 搜索 G1CollectedHeap::cleanUpCardTable -> G1ParCleanupCTTask::work()
   * 清空所有的dirty cards region
   */
  _g1->cleanUpCardTable();

  DirtyCardQueueSet& into_cset_dcqs = _g1->into_cset_dirty_card_queue_set();
  int into_cset_n_buffers = into_cset_dcqs.completed_buffers_num();

  if (_g1->evacuation_failed()) {
    double restore_remembered_set_start = os::elapsedTime();

    // Restore remembered sets for the regions pointing into the collection set.
    // We just need to transfer the completed buffers from the DirtyCardQueueSet
    // used to hold cards that contain references that point into the collection set
    // to the DCQS used to hold the deferred RS updates.
    _g1->dirty_card_queue_set().merge_bufferlists(&into_cset_dcqs);  // 由于CSet被清理了，因此将之前指向CSet的DCQS迁移到G1CollectedHeap所维护的全局DCQS(区别JavaThread的全局静态DCQS)
    _g1->g1_policy()->phase_times()->record_evac_fail_restore_remsets((os::elapsedTime() - restore_remembered_set_start) * 1000.0);
  }
  // 如果回收成功了，意味着回收集合已经清空了，那么没必要保存指向回收集合的RSet了
  // Free any completed buffers in the DirtyCardQueueSet used to hold cards
  // which contain references that point into the collection.
  _g1->into_cset_dirty_card_queue_set().clear(); // 已经迁移到_g1->dirty_card_queue_set()了，没必要再保留 _g1->into_cset_dirty_card_queue_set()
  assert(_g1->into_cset_dirty_card_queue_set().completed_buffers_num() == 0,
         "all buffers should be freed");
  _g1->into_cset_dirty_card_queue_set().clear_n_completed_buffers();
}

/**
 * 对Region的RSet进行清理的Closure，调用者负责在每一个Region上apply这个类的doHeapRegion方法
 */
class ScrubRSClosure: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  BitMap* _region_bm;
  BitMap* _card_bm;
  CardTableModRefBS* _ctbs;
public:
  ScrubRSClosure(BitMap* region_bm, BitMap* card_bm) :
    _g1h(G1CollectedHeap::heap()),
    _region_bm(region_bm), _card_bm(card_bm),
    _ctbs(_g1h->g1_barrier_set()) {}

  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) { // 只要不是举行对象的连续region(巨型对象的非首个Region)
      r->rem_set()->scrub(_ctbs, _region_bm, _card_bm); // 调用HeapRegionRemSet::scrub方法
    }
    return false;
  }
};

void G1RemSet::scrub(BitMap* region_bm, BitMap* card_bm) {
  ScrubRSClosure scrub_cl(region_bm, card_bm);
  _g1->heap_region_iterate(&scrub_cl); // 对每一个region，调用ScrubRSClosure::doHeapRegion()方法
}

void G1RemSet::scrub_par(BitMap* region_bm, BitMap* card_bm,
                                uint worker_num, int claim_val) {
  ScrubRSClosure scrub_cl(region_bm, card_bm);
  _g1->heap_region_par_iterate_chunked(&scrub_cl,
                                       worker_num,
                                       n_workers(),
                                       claim_val);
}

G1TriggerClosure::G1TriggerClosure() :
  _triggered(false) { }

G1InvokeIfNotTriggeredClosure::G1InvokeIfNotTriggeredClosure(G1TriggerClosure* t_cl,
                                                             OopClosure* oop_cl)  :
  _trigger_cl(t_cl), _oop_cl(oop_cl) { }

G1Mux2Closure::G1Mux2Closure(OopClosure *c1, OopClosure *c2) :
  _c1(c1), _c2(c2) { }

G1UpdateRSOrPushRefOopClosure::
G1UpdateRSOrPushRefOopClosure(G1CollectedHeap* g1h,
                              G1RemSet* rs,
                              G1ParPushHeapRSClosure* push_ref_cl,
                              bool record_refs_into_cset,
                              uint worker_i) :
  _g1(g1h), _g1_rem_set(rs), _from(NULL),
  _record_refs_into_cset(record_refs_into_cset),
  _push_ref_cl(push_ref_cl), _worker_i(worker_i) { }

// Returns true if the given card contains references that point
// into the collection set, if we're checking for such references;
// false otherwise.

/**
 * in book
 * 这个refine_card的处理目标一定只是针对脏卡片，通过Refine线程或者GC线程来调用处理脏卡片
 * 处理脏卡片的基本原理是，如果是脏卡片(GC中要求脏卡片并且对应的field指向cset)，那么选择对这个脏卡片中的引用关系更新到对应的目标Region的RSet中，
 * 或者，如果是GC线程，则更新到worker线程的 G1ParScanThreadState的引用队列 中
 * 对于GC线程，只有将脏卡片处理完毕(即，由于对象移动带来的卡片信息不一致的问题得到了修正)，才能开始对回收集合中的数据基于卡片进行根搜索
 * @param card_ptr
 * @param worker_i
 * @param check_for_refs_into_cset
 * @return
 */
bool G1RemSet::refine_card(jbyte* card_ptr, uint worker_i,
                           bool check_for_refs_into_cset) {
  assert(_g1->is_in_exact(_ct_bs->addr_for(card_ptr)),
         err_msg("Card at " PTR_FORMAT " index " SIZE_FORMAT " representing heap at " PTR_FORMAT " (%u) must be in committed heap",
                 p2i(card_ptr),
                 _ct_bs->index_for(_ct_bs->addr_for(card_ptr)),
                 _ct_bs->addr_for(card_ptr),
                 _g1->addr_to_region(_ct_bs->addr_for(card_ptr))));

  // If the card is no longer dirty, nothing to do.
  // refine_card 只处理脏卡片，如果不是脏卡片则跳过
  if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
    // No need to return that this card contains refs that point
    // into the collection set.
    return false;
  }
  // 确认了这张卡片是脏卡片
  // 找到这个卡片对应的对象地址
  HeapWord* start = _ct_bs->addr_for(card_ptr);
  // 找到这个卡片对应的HeapRegion地址
  HeapRegion* r = _g1->heap_region_containing(start);

  // Why do we have to check here whether a card is on a young region,
  // given that we dirty young regions and, as a result, the
  // post-barrier is supposed to filter them out and never to enqueue
  // them? When we allocate a new region as the "allocation region" we
  // actually dirty its cards after we release the lock, since card
  // dirtying while holding the lock was a performance bottleneck. So,
  // as a result, it is possible for other threads to actually
  // allocate objects in the region (after the acquire the lock)
  // before all the cards on the region are dirtied. This is unlikely,
  // and it doesn't happen often, but it can happen. So, the extra
  // check below filters out those cards.
  /**
   * 假如a.field = b，b在回收集合中，a却在young区，那么是不必处理a中的卡片的
   */
  if (r->is_young()) { // 这是因为在年轻代中的对象通常在年轻代的垃圾回收过程中处理，而不会包含指向集合集的引用。
    return false;
  }

  // While we are processing RSet buffers during the collection, we
  // actually don't want to scan any cards on the collection set,
  // since we don't want to update remebered sets with entries that
  // point into the collection set, given that live objects from the
  // collection set are about to move and such entries will be stale
  // very soon. This change also deals with a reliability issue which
  // involves scanning a card in the collection set and coming across
  // an array that was being chunked and looking malformed. Note,
  // however, that if evacuation fails, we have to scan any objects
  // that were not moved and create any missing entries.
   /**
   * 假如a.field = b，b在回收集合中，a在回收集合中，那么是不必处理a中的卡片的
   */
  if (r->in_collection_set()) { //我们不处理回收集合中的卡片，因为回收集合肯定会被扫描的，只处理非回收集合，我们关系的是 非回收集合的 field是否指向回收集合中的对象
    return false;
  }

  // The result from the hot card cache insert call is either:
  //   * pointer to the current card
  //     (implying that the current card is not 'hot'),
  //   * null
  //     (meaning we had inserted the card ptr into the "hot" card cache,
  //     which had some headroom),
  //   * a pointer to a "hot" card that was evicted from the "hot" cache.
  //
  // 搜搜
  G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
  if (hot_card_cache->use_cache()) { // 默认使用cache
    assert(!check_for_refs_into_cset, "sanity");
    assert(!SafepointSynchronize::is_at_safepoint(), "sanity");

    // 只有当缓存已经满了，或者卡片并不热，才会对卡片进行立刻refine，此时返回null
    // 搜索 jbyte* G1HotCardCache::insert(jbyte* card_ptr)
    card_ptr = hot_card_cache->insert(card_ptr);
    if (card_ptr == NULL) {
      // There was no eviction. Nothing to do.
      return false;
    }
    // 插入到热卡片缓存中返回非Null，说明需要进行立刻的Refine，这个返回的card并不一定就是参数card
    start = _ct_bs->addr_for(card_ptr); // 弹出的car对应的Heap内存的地址
    r = _g1->heap_region_containing(start);// 弹出的card 的 Region

    // Checking whether the region we got back from the cache
    // is young here is inappropriate. The region could have been
    // freed, reallocated and tagged as young while in the cache.
    // Hence we could see its young type change at any time.
  }

  // Don't use addr_for(card_ptr + 1) which can ask for
  // a card beyond the heap.  This is not safe without a perm
  // gen at the upper end of the heap.
  HeapWord* end   = start + CardTableModRefBS::card_size_in_words;
  MemRegion dirtyRegion(start, end); // 创建一个代表这个脏卡片对应的对象的起始地址的MemRegion

#if CARD_REPEAT_HISTO
  init_ct_freq_table(_g1->max_capacity());
  ct_freq_note_card(_ct_bs->index_for(start));
#endif
  // 查看 inline void G1ParPushHeapRSClosure::do_oop_nv(T* p)
  G1ParPushHeapRSClosure* oops_in_heap_closure = NULL; // 搜索 G1ParPushHeapRSClosure::do_oop_nv 可以看到，这个类的基本职责就是查看对象引用是否指向回收集合或者大对象，
  if (check_for_refs_into_cset) { // 如果需要检查into collection set，那么，我们
    // ConcurrentG1RefineThreads have worker numbers larger than what
    // _cset_rs_update_cl[] is set up to handle. But those threads should
    // only be active outside of a collection which means that when they
    // reach here they should have check_for_refs_into_cset == false.
    assert((size_t)worker_i < n_workers(), "index of worker larger than _cset_rs_update_cl[].length");
    oops_in_heap_closure = _cset_rs_update_cl[worker_i]; // 获取当前的worker thread 的 G1ParPushHeapRSClosure， 每一个worker thread都有自己的 worker thread
  }
  // check_for_refs_into_cset = false，搜索 G1UpdateRSOrPushRefOopClosure::do_oop_nv
  G1UpdateRSOrPushRefOopClosure update_rs_oop_cl(_g1,
                                                 _g1->g1_rem_set(),
                                                 oops_in_heap_closure, // 如果check_for_refs_into_cset = false，oops_in_heap_closure为空
                                                 check_for_refs_into_cset, // 如果是GC引起的，那么check_for_refs_into_cset=true
                                                 worker_i);
  update_rs_oop_cl.set_from(r); // 设置卡片来自于的 HeapRegion

  G1TriggerClosure trigger_cl; // inline void G1TriggerClosure::do_oop_nv(T* p)
  FilterIntoCSClosure into_cs_cl(NULL, _g1, &trigger_cl); // FilterIntoCSClosure::do_oop_nv(T* p),如果对象的确在回收集合中或者是大对象，那么设置trigger_cl 为triggered
  G1InvokeIfNotTriggeredClosure invoke_cl(&trigger_cl, &into_cs_cl);  // G1InvokeIfNotTriggeredClosure::do_oop_nv(T* p)
  G1Mux2Closure mux(&invoke_cl, &update_rs_oop_cl); // check_for_refs_into_cset = true， 搜搜 G1Mux2Closure::do_oop_nv
  // 搜索 FilterOutOfRegionClosure::FilterOutOfRegionClosure
  // 为true和为false的唯一区别是是否执行 invoke_cl
  FilterOutOfRegionClosure filter_then_update_rs_oop_cl(r,
                        (check_for_refs_into_cset ?
                                (OopClosure*)&mux : // check_for_refs_into_cset = true
                                (OopClosure*)&update_rs_oop_cl)); // check_for_refs_into_cset = false  inline void FilterOutOfRegionClosure::do_oop_nv
  // 这个卡片是刚刚从 Hotcache中弹出来的，尽管HotCache中插入卡片的时候都保证不是young region的卡片，
  // 但是可能在弹出来的时候这个heapRegion已经在清理阶段被reallocated然后成为了young，因此还是需要进行check
  // The region for the current card may be a young region. The
  // current card may have been a card that was evicted from the
  // card cache. When the card was inserted into the cache, we had
  // determined that its region was non-young. While in the cache,
  // the region may have been freed during a cleanup pause, reallocated
  // and tagged as young.
  //
  // We wish to filter out cards for such a region but the current
  // thread, if we're running concurrently, may "see" the young type
  // change at any time (so an earlier "is_young" check may pass or
  // fail arbitrarily). We tell the iteration code to perform this
  // filtering when it has been determined that there has been an actual
  // allocation in this region and making it safe to check the young type.
  bool filter_young = true;
  // 搜索 HeapWord* HeapRegion::oops_on_card_seq_iterate_careful
  HeapWord* stop_point =
    r->oops_on_card_seq_iterate_careful(dirtyRegion,
                                        &filter_then_update_rs_oop_cl,
                                        filter_young, // 是否filter out
                                        card_ptr);

  // If stop_point is non-null, then we encountered an unallocated region
  // (perhaps the unfilled portion of a TLAB.)  For now, we'll dirty the
  // card and re-enqueue: if we put off the card until a GC pause, then the
  // unallocated portion will be filled in.  Alternatively, we might try
  // the full complexity of the technique used in "regular" precleaning.
  if (stop_point != NULL) {
    // The card might have gotten re-dirtied and re-enqueued while we
    // worked.  (In fact, it's pretty likely.)
    if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
      *card_ptr = CardTableModRefBS::dirty_card_val(); // 将卡片标记位脏卡片
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      DirtyCardQueue* sdcq =
        JavaThread::dirty_card_queue_set().shared_dirty_card_queue();
      sdcq->enqueue(card_ptr); // 将卡片放到 脏卡片队列中去
    }
  } else {
    _conc_refine_cards++;
  }

  // This gets set to true if the card being refined has
  // references that point into the collection set.
  bool has_refs_into_cset = trigger_cl.triggered(); // 正在Refine的卡片是否有指向回收集合的引用

  // We should only be detecting that the card contains references
  // that point into the collection set if the current thread is
  // a GC worker thread.
  assert(!has_refs_into_cset || SafepointSynchronize::is_at_safepoint(),
           "invalid result at non safepoint");

  return has_refs_into_cset; // 返回的结果是是否有指向回收集合的引用
}

void G1RemSet::print_periodic_summary_info(const char* header) {
  G1RemSetSummary current;
  current.initialize(this);

  _prev_period_summary.subtract_from(&current);
  print_summary_info(&_prev_period_summary, header);

  _prev_period_summary.set(&current);
}

void G1RemSet::print_summary_info() {
  G1RemSetSummary current;
  current.initialize(this);

  print_summary_info(&current, " Cumulative RS summary");
}

void G1RemSet::print_summary_info(G1RemSetSummary * summary, const char * header) {
  assert(summary != NULL, "just checking");

  if (header != NULL) {
    gclog_or_tty->print_cr("%s", header);
  }

#if CARD_REPEAT_HISTO
  gclog_or_tty->print_cr("\nG1 card_repeat count histogram: ");
  gclog_or_tty->print_cr("  # of repeats --> # of cards with that number.");
  card_repeat_count.print_on(gclog_or_tty);
#endif

  summary->print_on(gclog_or_tty);
}

void G1RemSet::prepare_for_verify() {
  if (G1HRRSFlushLogBuffersOnVerify &&
      (VerifyBeforeGC || VerifyAfterGC)
      &&  (!_g1->full_collection() || G1VerifyRSetsDuringFullGC)) {
    cleanupHRRS();
    _g1->set_refine_cte_cl_concurrency(false);
    if (SafepointSynchronize::is_at_safepoint()) {
      DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
      dcqs.concatenate_logs();
    }

    G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
    bool use_hot_card_cache = hot_card_cache->use_cache();
    hot_card_cache->set_use_cache(false);

    DirtyCardQueue into_cset_dcq(&_g1->into_cset_dirty_card_queue_set());
    updateRS(&into_cset_dcq, 0);
    _g1->into_cset_dirty_card_queue_set().clear();

    hot_card_cache->set_use_cache(use_hot_card_cache);
    assert(JavaThread::dirty_card_queue_set().completed_buffers_num() == 0, "All should be consumed");
  }
}
