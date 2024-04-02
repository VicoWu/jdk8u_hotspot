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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1OOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1OOPCLOSURES_INLINE_HPP

#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/g1OopClosures.hpp"
#include "gc_implementation/g1/g1ParScanThreadState.inline.hpp"
#include "gc_implementation/g1/g1RemSet.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "memory/iterator.inline.hpp"
#include "runtime/prefetch.inline.hpp"

/*
 * This really ought to be an inline function, but apparently the C++
 * compiler sometimes sees fit to ignore inline declarations.  Sigh.
 */

template <class T>
inline void FilterIntoCSClosure::do_oop_nv(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop) &&
      _g1->is_in_cset_or_humongous(oopDesc::decode_heap_oop_not_null(heap_oop))) {
    _oc->do_oop(p);
  }
}

template <class T>
inline void FilterOutOfRegionClosure::do_oop_nv(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {
    HeapWord* obj_hw = (HeapWord*)oopDesc::decode_heap_oop_not_null(heap_oop);
    if (obj_hw < _r_bottom || obj_hw >= _r_end) {
      _oc->do_oop(p);
    }
  }
}

// This closure is applied to the fields of the objects that have just been copied.
/**
 * 调用者是 oop G1ParScanThreadState::copy_to_survivor_space
 * 一个obj刚刚从一个区域拷贝到了survivor区域，那么需要用这个方法处理这个对象的所有的field
 */
template <class T>
inline void G1ParScanClosure::do_oop_nv(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p);

  if (!oopDesc::is_null(heap_oop)) {
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    const InCSetState state = _g1->in_cset_state(obj);
    if (state.is_in_cset()) { // 这个field在回收集合中，那么就放到_par_scan_state的集合中，等待递归进行复制到目标区域
      // We're not going to even bother checking whether the object is
      // already forwarded or not, as this usually causes an immediate
      // stall. We'll try to prefetch the object (for write, given that
      // we might need to install the forwarding reference) and we'll
      // get back to it when pop it from the queue
      Prefetch::write(obj->mark_addr(), 0);
      Prefetch::read(obj->mark_addr(), (HeapWordSize*2));

      // slightly paranoid test; I'm trying to catch potential
      // problems before we go into push_on_queue to know where the
      // problem is coming from
      assert((obj == oopDesc::load_decode_heap_oop(p)) ||
             (obj->is_forwarded() &&
                 obj->forwardee() == oopDesc::load_decode_heap_oop(p)),
             "p should still be pointing to obj or to its forwardee");

      _par_scan_state->push_on_queue(p);
    } else { // 不在目标集合中
      if (state.is_humongous()) {
        _g1->set_humongous_is_live(obj);
      }
      // 仅仅需要更新RS,用来更新引用关系
      _par_scan_state->update_rs(_from, p, _worker_id);
    }
  }
}

/**
 * 这个方法是在根扫描的过程中调用的, 类G1ParPushHeapRSClosure 的构造
 *   需要搜索 G1ParPushHeapRSClosure push_heap_rs_cl(_g1h, &pss)
 * @tparam T
 * @param p
 */
template <class T>
inline void G1ParPushHeapRSClosure::do_oop_nv(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p); // 从指针 p 所指向的内存位置加载一个堆对象指针

  if (!oopDesc::is_null(heap_oop)) { // 如果指针不为空，表示指针指向了一个有效的堆对象
      /**
       * 解码非空的堆对象指针，将其存储在变量 obj 中
       * oopDesc::decode_heap_oop_not_null 是一个静态函数，用于将加载的堆对象指针解码为对应的堆对象。
       */
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    if (_g1->is_in_cset_or_humongous(obj)) {
        /**
         * Prefetch::write(obj->mark_addr(), 0);：
            这段代码通过 obj->mark_addr() 获取对象的标记位地址，然后使用 Prefetch::write() 预取指令将标记位所在的内存页加载到处理器的高速缓存中。
                 参数 0 表示预取的方式，通常用于写操作。预取可以在处理器空闲时提前将数据加载到高速缓存，以减少后续读取操作的延迟。

         Prefetch::read(obj->mark_addr(), (HeapWordSize*2));：
            这段代码类似地使用 Prefetch::read() 预取指令，将对象的标记位所在内存页加载到高速缓存中。
                不同的是，参数 HeapWordSize*2 表示预取的方式为读取，且预取的范围是标记位所在的内存页的两倍大小。这样做可以预取更多的相关数据，提高访问性能。
         */
      Prefetch::write(obj->mark_addr(), 0);
      Prefetch::read(obj->mark_addr(), (HeapWordSize*2));

      // Place on the references queue
      // 将指针 p 放入引用队列中，以便后续的扫描操作
      _par_scan_state->push_on_queue(p);
    } else {
      assert(!_g1->obj_in_cs(obj), "checking");
    }
  }
}

/**
 * 调用者查看 inline void CMTask::process_grey_object
 */
template <class T>
inline void G1CMOopClosure::do_oop_nv(T* p) {
  oop obj = oopDesc::load_decode_heap_oop(p); // 根据指针取出p所指向的object
  if (_cm->verbose_high()) {
    gclog_or_tty->print_cr("[%u] we're looking at location "
                           "*" PTR_FORMAT " = " PTR_FORMAT,
                           _task->worker_id(), p2i(p), p2i((void*) obj));
  }
  _task->deal_with_reference(obj);
}


/**
 * 这个方法的调用发生在并发标记阶段，因此当前的状态不是STW的
 * 可以看到，这个do_oop_nv并没有任何递归的操作，仅仅是传入什么对象就扫描什么对象
 * 这个Closure的构造发生在方法 void ConcurrentMark::scanRootRegion 中
 * @tparam T
 * @param p
 */
template <class T>
inline void G1RootRegionScanClosure::do_oop_nv(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p); // 获取oop* 指针指向的oop对象
  if (!oopDesc::is_null(heap_oop)) { // 调用oopDesc类的静态方法，判断对象是否为null
    /**
     * 如果不是narrowoop，那么直接返回，如果是narrow oop，需要进行相应的解码工作
     * 实现类搜索 inline oop oopDesc::decode_heap_oop_not_null
     */
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    HeapRegion* hr = _g1h->heap_region_containing((HeapWord*) obj);
    /**
     * 标记对象，将标记结果存放在_cm对象的_nextMarkBitMap中
     * 这里没有进行相应的递归操作，只是对root region进行了扫描和标记
     */

    _cm->grayRoot(obj, obj->size(), _worker_id, hr);
  }
}

template <class T>
inline void G1Mux2Closure::do_oop_nv(T* p) {
  // Apply first closure; then apply the second.
  _c1->do_oop(p);
  _c2->do_oop(p);
}

template <class T>
inline void G1TriggerClosure::do_oop_nv(T* p) {
  // Record that this closure was actually applied (triggered).
  _triggered = true;
}

template <class T>
inline void G1InvokeIfNotTriggeredClosure::do_oop_nv(T* p) {
  if (!_trigger_cl->triggered()) {
    _oop_cl->do_oop(p);
  }
}

template <class T>
inline void G1UpdateRSOrPushRefOopClosure::do_oop_nv(T* p) {
  oop obj = oopDesc::load_decode_heap_oop(p);
  if (obj == NULL) {
    return;
  }
#ifdef ASSERT
  // can't do because of races
  // assert(obj == NULL || obj->is_oop(), "expected an oop");

  // Do the safe subset of is_oop
#ifdef CHECK_UNHANDLED_OOPS
  oopDesc* o = obj.obj();
#else
  oopDesc* o = obj;
#endif // CHECK_UNHANDLED_OOPS
  assert((intptr_t)o % MinObjAlignmentInBytes == 0, "not oop aligned");
  assert(Universe::heap()->is_in_reserved(obj), "must be in heap");
#endif // ASSERT

  assert(_from != NULL, "from region must be non-NULL");
  assert(_from->is_in_reserved(p), "p is not in from");

  HeapRegion* to = _g1->heap_region_containing(obj);
  if (_from == to) {
    // Normally this closure should only be called with cross-region references.
    // But since Java threads are manipulating the references concurrently and we
    // reload the values things may have changed.
    return;
  }
  // The _record_refs_into_cset flag is true during the RSet
  // updating part of an evacuation pause. It is false at all
  // other times:
  //  * rebuilding the remembered sets after a full GC
  //  * during concurrent refinement.
  //  * updating the remembered sets of regions in the collection
  //    set in the event of an evacuation failure (when deferred
  //    updates are enabled).

  if (_record_refs_into_cset && to->in_collection_set()) {
    // We are recording references that point into the collection
    // set and this particular reference does exactly that...
    // If the referenced object has already been forwarded
    // to itself, we are handling an evacuation failure and
    // we have already visited/tried to copy this object
    // there is no need to retry.
    if (!self_forwarded(obj)) {
      assert(_push_ref_cl != NULL, "should not be null");
      // Push the reference in the refs queue of the G1ParScanThreadState
      // instance for this worker thread.
      _push_ref_cl->do_oop(p);
     }

    // Deferred updates to the CSet are either discarded (in the normal case),
    // or processed (if an evacuation failure occurs) at the end
    // of the collection.
    // See G1RemSet::cleanup_after_oops_into_collection_set_do().
  } else {
    // We either don't care about pushing references that point into the
    // collection set (i.e. we're not during an evacuation pause) _or_
    // the reference doesn't point into the collection set. Either way
    // we add the reference directly to the RSet of the region containing
    // the referenced object.
    assert(to->rem_set() != NULL, "Need per-region 'into' remsets.");
    to->rem_set()->add_reference(p, _worker_i);
  }
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1OOPCLOSURES_INLINE_HPP
