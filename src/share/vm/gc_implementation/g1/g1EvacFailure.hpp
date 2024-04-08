/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1EVACFAILURE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1EVACFAILURE_HPP

#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "gc_implementation/g1/dirtyCardQueue.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1_globals.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/heapRegion.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "utilities/workgroup.hpp"

// Closures and tasks associated with any self-forwarding pointers
// installed as a result of an evacuation failure.

class UpdateRSetDeferred : public OopsInHeapRegionClosure {
private:
  G1CollectedHeap* _g1;
  DirtyCardQueue *_dcq;
  G1SATBCardTableModRefBS* _ct_bs;

public:
  UpdateRSetDeferred(G1CollectedHeap* g1, DirtyCardQueue* dcq) :
    _g1(g1), _ct_bs(_g1->g1_barrier_set()), _dcq(dcq) {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }
  /**
   * 这是 UpdateRSetDeferred::do_oop_work()  方法
   * @tparam T
   * @param p
   */
  template <class T> void do_oop_work(T* p) {
      /**
       *  p 是某一个field，很显然，这个field所在的region肯定是在_from中的，
       *  而p指向的对象oopDesc::load_decode_heap_oop(p) 不在_from中，并且_from不是survivor，那么，我们就需要将
       */
    assert(_from->is_in_reserved(p), "paranoia");
    if (!_from->is_in_reserved(oopDesc::load_decode_heap_oop(p)) &&
        !_from->is_survivor()) {
        /**
         * 关于 index_for，搜索 inline size_t G1BlockOffsetSharedArray::index_for
         */
      size_t card_index = _ct_bs->index_for(p);
      /**
       * 设置这个对象的卡表信息，把卡表设置为deferred
       * 关于_dcq，搜索 RemoveSelfForwardPtrHRClosure(G1CollectedHeap* g1h, 可以看到，是G1CollectedHeap的全局的queue
       */
      if (_ct_bs->mark_card_deferred(card_index)) {
        _dcq->enqueue((jbyte*)_ct_bs->byte_for_index(card_index)); // 把 field所在的卡片的索引放入到_dcq中
      }
    }
  }
};

class RemoveSelfForwardPtrObjClosure: public ObjectClosure {
private:
  G1CollectedHeap* _g1;
  ConcurrentMark* _cm;
  HeapRegion* _hr;
  size_t _marked_bytes;
  OopsInHeapRegionClosure *_update_rset_cl;
  bool _during_initial_mark;
  bool _during_conc_mark;
  uint _worker_id;
  HeapWord* _end_of_last_gap;
  HeapWord* _last_gap_threshold;
  HeapWord* _last_obj_threshold;

  /**
   * 搜索 RemoveSelfForwardPtrObjClosure rspc 查看构造方法
   */
public:
  RemoveSelfForwardPtrObjClosure(G1CollectedHeap* g1, ConcurrentMark* cm,
                                 HeapRegion* hr,
                                 OopsInHeapRegionClosure* update_rset_cl,
                                 bool during_initial_mark,
                                 bool during_conc_mark,
                                 uint worker_id) :
    _g1(g1), _cm(cm), _hr(hr), _marked_bytes(0),
    _update_rset_cl(update_rset_cl),
    _during_initial_mark(during_initial_mark),
    _during_conc_mark(during_conc_mark),
    _worker_id(worker_id),
    _end_of_last_gap(hr->bottom()),
    _last_gap_threshold(hr->bottom()),
    _last_obj_threshold(hr->bottom()) { }

  size_t marked_bytes() { return _marked_bytes; }

  // <original comment>
  // The original idea here was to coalesce evacuated and dead objects.
  // However that caused complications with the block offset table (BOT).
  // In particular if there were two TLABs, one of them partially refined.
  // |----- TLAB_1--------|----TLAB_2-~~~(partially refined part)~~~|
  // The BOT entries of the unrefined part of TLAB_2 point to the start
  // of TLAB_2. If the last object of the TLAB_1 and the first object
  // of TLAB_2 are coalesced, then the cards of the unrefined part
  // would point into middle of the filler object.
  // The current approach is to not coalesce and leave the BOT contents intact.
  // </original comment>
  //
  // We now reset the BOT when we start the object iteration over the
  // region and refine its entries for every object we come across. So
  // the above comment is not really relevant and we should be able
  // to coalesce dead objects if we want to.
  /**
   * RemoveSelfForwardPtrObjClosure::do_object 方法
   * 调用者是 RemoveSelfForwardPtrHRClosure::doHeapRegion
   */
  void do_object(oop obj) {
    HeapWord* obj_addr = (HeapWord*) obj;
    assert(_hr->is_in(obj_addr), "sanity");
    size_t obj_size = obj->size();
    HeapWord* obj_end = obj_addr + obj_size;

    if (_end_of_last_gap != obj_addr) {
      // there was a gap before obj_addr
      _last_gap_threshold = _hr->cross_threshold(_end_of_last_gap, obj_addr);
    }
    /**
     * 判断对象是否是一个自转发对象
     * 一个自转发对象意味着这个对象在copy_to_survivor的时候失败了
     */
    if (obj->is_forwarded() && obj->forwardee() == obj) {
      // The object failed to move.

      // We consider all objects that we find self-forwarded to be
      // live. What we'll do is that we'll update the prev marking
      // info so that they are all under PTAMS and explicitly marked.
      if (!_cm->isPrevMarked(obj)) {
        _cm->markPrev(obj); // 在_prevMarkBitMap 也标记这个对象
      }
      if (_during_initial_mark) {
        // For the next marking info we'll only mark the
        // self-forwarded objects explicitly if we are during
        // initial-mark (since, normally, we only mark objects pointed
        // to by roots if we succeed in copying them). By marking all
        // self-forwarded objects we ensure that we mark any that are
        // still pointed to be roots. During concurrent marking, and
        // after initial-mark, we don't need to mark any objects
        // explicitly and all objects in the CSet are considered
        // (implicitly) live. So, we won't mark them explicitly and
        // we'll leave them over NTAMS.
        /**
         * 对于next marking 信息，如果我们在初始标记期间，我们将仅显式标记自转发的对象（因为，通常，如果我们成功复制它们，我们仅标记根指向的对象）。
         * 通过标记所有自转发的对象，我们确保标记任何仍然被根指向的对象。
         * 在并发标记期间的初始标记之后，我们不需要显式标记任何对象，因为这些对象肯定可以从初始标记过程中被间接的访问到并被标记
         * 并且 CSet 中的所有对象都被视为（隐式）活动的。
         * 因此，我们不会明确标记它们，而是将它们留给 N TAMS。
         */
        _cm->grayRoot(obj, obj_size, _worker_id, _hr);
      }
      _marked_bytes += (obj_size * HeapWordSize);
      obj->set_mark(markOopDesc::prototype());

      // While we were processing RSet buffers during the collection,
      // we actually didn't scan any cards on the collection set,
      // since we didn't want to update remembered sets with entries
      // that point into the collection set, given that live objects
      // from the collection set are about to move and such entries
      // will be stale very soon.
      // This change also dealt with a reliability issue which
      // involved scanning a card in the collection set and coming
      // across an array that was being chunked and looking malformed.
      // The problem is that, if evacuation fails, we might have
      // remembered set entries missing given that we skipped cards on
      // the collection set. So, we'll recreate such entries now.
      /**
       * 搜索 _update_rset_cl(g1h, &_dcq)
       * 其实就是 UpdateRSetDeferred::do_oop_work，这里到了UpdateRSetDeferred::do_oop_work就是处理的是objd对象的每一个field
       */
      obj->oop_iterate(_update_rset_cl);
    } else {
        /**
         * 这个对象的转发地址不是指向自己的，因此这个对象或者已经被evacuated，或者是死的
         */
      // The object has been either evacuated or is dead. Fill it with a
      // dummy object.
      MemRegion mr(obj_addr, obj_size);
      CollectedHeap::fill_with_object(mr);

      /**
       * 把这个对象在prev bit map中的标记位清除，这样的话这个对象就可以被清除了
       */
      // must nuke all dead objects which we skipped when iterating over the region
      _cm->clearRangePrevBitmap(MemRegion(_end_of_last_gap, obj_end));
    }
    _end_of_last_gap = obj_end;
    _last_obj_threshold = _hr->cross_threshold(obj_addr, obj_end);
  }
};

class RemoveSelfForwardPtrHRClosure: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  ConcurrentMark* _cm;
  uint _worker_id;

  DirtyCardQueue _dcq;
  UpdateRSetDeferred _update_rset_cl;

  /**
   * 搜索 RemoveSelfForwardPtrHRClosure rsfp_cl 查看对象的构造过程
   * 可以看到，每一个worker会构造一个 RemoveSelfForwardPtrHRClosure rsfp_cl，因此一个worker会有一个_dcq，他们都属于全局g1h的dcqs
   */
public:
  RemoveSelfForwardPtrHRClosure(G1CollectedHeap* g1h,
                                uint worker_id) :
    _g1h(g1h), _dcq(&g1h->dirty_card_queue_set()), _update_rset_cl(g1h, &_dcq),
    _worker_id(worker_id), _cm(_g1h->concurrent_mark()) {
    }

    /**
     * RemoveSelfForwardPtrHRClosure::doHeapRegion
     * 这个closure会在回收失败以后，被多个worker id并发调用，遍历整个CSet中的所有的 HeapRegion
     * 下层会调用RemoveSelfForwardPtrObjClosure 来遍历当前的HeapRegion
     */
  bool doHeapRegion(HeapRegion *hr) {
    bool during_initial_mark = _g1h->g1_policy()->during_initial_mark_pause();
    bool during_conc_mark = _g1h->mark_in_progress();

    assert(!hr->isHumongous(), "sanity");
    assert(hr->in_collection_set(), "bad CS");
    if (hr->claimHeapRegion(HeapRegion::ParEvacFailureClaimValue)) {
        /**
         * 这个HeapRegion的确有失败的evacuation
         * 这里的意思是，这个HeapRegion中至少存在一个obj在evacuate的失败了（因为在copy_to_survivor方法中，
         *    如果在处理某一个obj的时候失败了，经过简单的针对这个obj的失败处理，就继续处理下一个object，而不会失败掉）
         */
      if (hr->evacuation_failed()) { // 这是一个存在evacuation failure 的 heap region
        RemoveSelfForwardPtrObjClosure rspc(_g1h, _cm, hr, &_update_rset_cl,
                                            during_initial_mark,
                                            during_conc_mark,
                                            _worker_id);

        hr->note_self_forwarding_removal_start(during_initial_mark,
                                               during_conc_mark);
        _g1h->check_bitmaps("Self-Forwarding Ptr Removal", hr);

        // In the common case (i.e. when there is no evacuation
        // failure) we make sure that the following is done when
        // the region is freed so that it is "ready-to-go" when it's
        // re-allocated. However, when evacuation failure happens, a
        // region will remain in the heap and might ultimately be added
        // to a CSet in the future. So we have to be careful here and
        // make sure the region's RSet is ready for parallel iteration
        // whenever this might be required in the future.
        hr->rem_set()->reset_for_par_iteration();
        hr->reset_bot();
        _update_rset_cl.set_region(hr);
        /**
         * 遍历这个HeapRegion的所有obj，在每个obj上apply对应的RemoveSelfForwardPtrObjClosure
         * 具体调用 搜索  RemoveSelfForwardPtrObjClosure::do_object
         */
        hr->object_iterate(&rspc);

        hr->rem_set()->clean_strong_code_roots(hr);

        hr->note_self_forwarding_removal_end(during_initial_mark,
                                             during_conc_mark,
                                             rspc.marked_bytes());
      }
    }
    return false;
  }
};

class G1ParRemoveSelfForwardPtrsTask: public AbstractGangTask {
protected:
  G1CollectedHeap* _g1h;

public:
  G1ParRemoveSelfForwardPtrsTask(G1CollectedHeap* g1h) :
    AbstractGangTask("G1 Remove Self-forwarding Pointers"),
    _g1h(g1h) { }
  /**
   * G1ParRemoveSelfForwardPtrsTask::work() 方法
   * 在一轮evacuation 结束以后，处理evacuation failure
   */
  void work(uint worker_id) {
    RemoveSelfForwardPtrHRClosure rsfp_cl(_g1h, worker_id);
    /**
     *  搜索 HeapRegion* G1CollectedHeap::start_cset_region_for_worker
     *  查找一个worker id负责的collecti set中的region
     */
    HeapRegion* hr = _g1h->start_cset_region_for_worker(worker_id);
    _g1h->collection_set_iterate_from(hr, &rsfp_cl);
  }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1EVACFAILURE_HPP
