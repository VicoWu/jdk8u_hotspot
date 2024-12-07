/*
 * Copyright (c) 2001, 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/g1/heapRegion.hpp"
#include "gc_implementation/g1/satbQueue.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "runtime/thread.inline.hpp"

/**
 * 继承关系： G1SATBCardTableLoggingModRefBS -> G1SATBCardTableModRefBS -> CardTableModRefBSForCTRS -> CardTableModRefBS
 * @param whole_heap
 * @param max_covered_regions
 */
G1SATBCardTableModRefBS::G1SATBCardTableModRefBS(MemRegion whole_heap,
                                                 int max_covered_regions) :
    CardTableModRefBSForCTRS(whole_heap, max_covered_regions) // 在 G1SATBCardTableLoggingModRefBS::G1SATBCardTableLoggingModRefBS 中传入的是2
{
  _kind = G1SATBCT;
}

/**
 * 通过写屏障拦截用户的写操作，然后向SATB队列中插入元素
 * 对应的，RSet的 G1SATBCardTableLoggingModRefBS::write_ref_field_work 则是通过写屏障拦截用户的写操作，然后修改对应的RSet
 * @param pre_val
 */
void G1SATBCardTableModRefBS::enqueue(oop pre_val) {
  // Nulls should have been already filtered.
  assert(pre_val->is_oop(true), "Error");

  if (!JavaThread::satb_mark_queue_set().is_active()) return;
  Thread* thr = Thread::current();
  if (thr->is_Java_thread()) {
    JavaThread* jt = (JavaThread*)thr;
    /**
     * 搜索 ObjPtrQueue& satb_mark_queue()
     */
    jt->satb_mark_queue().enqueue(pre_val); // 每个Java线程都有一个自己独立的satb_mark_queue，向这个队列中插入satb的一条记录
  } else {
    MutexLockerEx x(Shared_SATB_Q_lock, Mutex::_no_safepoint_check_flag);
    JavaThread::satb_mark_queue_set().shared_satb_queue()->enqueue(pre_val);
  }
}

template <class T> void
G1SATBCardTableModRefBS::write_ref_array_pre_work(T* dst, int count) {
  if (!JavaThread::satb_mark_queue_set().is_active()) return;
  T* elem_ptr = dst;
  for (int i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = oopDesc::load_heap_oop(elem_ptr);
    if (!oopDesc::is_null(heap_oop)) {
      enqueue(oopDesc::decode_heap_oop_not_null(heap_oop));
    }
  }
}

void G1SATBCardTableModRefBS::write_ref_array_pre(oop* dst, int count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}
void G1SATBCardTableModRefBS::write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}
// in book
bool G1SATBCardTableModRefBS::mark_card_deferred(size_t card_index) {
  jbyte val = _byte_map[card_index];
  // It's already processed
  /**
   * 检查当前卡是否已经被标记为延迟标记。
        如果卡已经被标记为延迟标记，则直接返回 false，表示不需要重复处理。
        这里需要与clean_card_mask_val()，是为了排除卡片处于clean的状态，因为clean的状态码是11111111
   */
  if ((val & (clean_card_mask_val() | deferred_card_val())) == deferred_card_val()) {
    return false;
  }
  /**
   * 检查当前卡是否属于年轻代（young generation， eden or survivor）。
        如果是年轻代的卡，则不需要记录所有指向年轻代区域的指针，直接返回 false。
   */
  if  (val == g1_young_gen) {
    // the card is for a young gen region. We don't need to keep track of all pointers into young
    return false;
  }

  // Cached bit can be installed either on a clean card or on a claimed card.
  jbyte new_val = val;
  /**
   * 检查当前卡是否为干净卡（clean card）。
        如果是干净卡，则将其标记为延迟标记。由于clean card 是11111111，因此不能通过逻辑或的方式进行状态处理
   */
  if (val == clean_card_val()) {
    new_val = (jbyte)deferred_card_val();
  } else {
      /**
       * 如果不是干净卡，则检查当前卡是否为已声明卡（claimed card）。
            如果是已声明卡，则添加延迟处理标记。
       */
    if (val & claimed_card_val()) {
      new_val = val | (jbyte)deferred_card_val();  // 添加延迟处理标记,保留claimed标记
    }
  }
  if (new_val != val) {
      /**
       * 如果 new_val 与原来的值不相同，则进行原子操作更新卡标记数组中的值。
       */
    Atomic::cmpxchg(new_val, &_byte_map[card_index], val);
  }
  return true;
}
// in book
void G1SATBCardTableModRefBS::g1_mark_as_young(const MemRegion& mr) {
  jbyte *const first = byte_for(mr.start());
  jbyte *const last = byte_after(mr.last());

  // Below we may use an explicit loop instead of memset() because on
  // certain platforms memset() can give concurrent readers phantom zeros.
  if (UseMemSetInBOT) {
    memset(first, g1_young_gen, last - first);
  } else {
    for (jbyte* i = first; i < last; i++) {
      *i = g1_young_gen;
    }
  }
}

#ifndef PRODUCT
void G1SATBCardTableModRefBS::verify_g1_young_region(MemRegion mr) {
  verify_region(mr, g1_young_gen,  true);
}
#endif

void G1SATBCardTableLoggingModRefBSChangedListener::on_commit(uint start_idx, size_t num_regions, bool zero_filled) {
  // Default value for a clean card on the card table is -1. So we cannot take advantage of the zero_filled parameter.
  MemRegion mr(G1CollectedHeap::heap()->bottom_addr_for_region(start_idx), num_regions * HeapRegion::GrainWords);
  _card_table->clear(mr);
}

/**
 * 继承关系： G1SATBCardTableLoggingModRefBS -> G1SATBCardTableModRefBS -> CardTableModRefBSForCTRS -> CardTableModRefBS
 * @param whole_heap
 * @param max_covered_regions
 */
G1SATBCardTableLoggingModRefBS::
G1SATBCardTableLoggingModRefBS(MemRegion whole_heap,
                               int max_covered_regions) : // CardTableRS::CardTableRS中传入的是2
  G1SATBCardTableModRefBS(whole_heap, max_covered_regions),
  _dcqs(JavaThread::dirty_card_queue_set()),
  _listener()
{
  _kind = G1SATBCTLogging;
  _listener.set_card_table(this);
}

/**
 * 搜索 _ct_bs = new G1SATBCardTableLoggingModRefBS(whole_heap,
 * @param mapper
 */
void G1SATBCardTableLoggingModRefBS::initialize(G1RegionToSpaceMapper* mapper) {
  mapper->set_mapping_changed_listener(&_listener);

  _byte_map_size = mapper->reserved().byte_size();

  _guard_index = cards_required(_whole_heap.word_size()) - 1;
  _last_valid_index = _guard_index - 1;

  HeapWord* low_bound  = _whole_heap.start();
  HeapWord* high_bound = _whole_heap.end();

  _cur_covered_regions = 1;
  _covered[0] = _whole_heap;

  _byte_map = (jbyte*) mapper->reserved().start();
  byte_map_base = _byte_map - (uintptr_t(low_bound) >> card_shift);
  assert(byte_for(low_bound) == &_byte_map[0], "Checking start of map");
  assert(byte_for(high_bound-1) <= &_byte_map[_last_valid_index], "Checking end of map");

  if (TraceCardTableModRefBS) {
    gclog_or_tty->print_cr("G1SATBCardTableModRefBS::G1SATBCardTableModRefBS: ");
    gclog_or_tty->print_cr("  "
                  "  &_byte_map[0]: " INTPTR_FORMAT
                  "  &_byte_map[_last_valid_index]: " INTPTR_FORMAT,
                  p2i(&_byte_map[0]),
                  p2i(&_byte_map[_last_valid_index]));
    gclog_or_tty->print_cr("  "
                  "  byte_map_base: " INTPTR_FORMAT,
                  p2i(byte_map_base));
  }
}

/**
 * 这个方法的调用发生在已经完成了对对象的赋值操作处理以后
 * 用来写入对应的卡片信息到DCQ中
 * @param field
 * @param new_val
 * @param release
 */
void
G1SATBCardTableLoggingModRefBS::write_ref_field_work(void* field,
                                                     oop new_val, // oopDesc*
                                                     bool release) {
    /**
     * 搜索 jbyte* byte_for(const void* p) const { 查看 byte_for方法
     * 取出field指针对应的卡片地址
     */
  volatile jbyte* byte = byte_for(field); // 获取源对象地址，比如a.field = b ， 那么 byte就是a的地址
  if (*byte == g1_young_gen) { // 对于a.field = b , 那么 *byte 表示对byte取值，即byte所指向的对象本身的地址，即b的地址
    return;
  }
  OrderAccess::storeload(); // store-load写屏障，保证数据可见性
  if (*byte != dirty_card) { // 检查该字节是否已经标记为 dirty_card，如果不是，则将其标记为 dirty_card。
    *byte = dirty_card;
    Thread* thr = Thread::current();
    /**
     * 根据当前线程类型，将该字节指针放入相应的脏卡队列中：
        如果当前线程是 Java 线程，则将该字节指针放入该线程的脏卡队列中。
        如果当前线程不是 Java 线程，则将该字节指针放入共享脏卡队列中。
     */
    if (thr->is_Java_thread()) { // 如果是java线程，那么就放到线程本地的DCQ中
      JavaThread* jt = (JavaThread*)thr;
      /**
       * 将源对象地址存入到这个Java线程本地的转移专用记忆集合日志中，后续会有Refine线程来进行处理
       * 在 gc过程中也会enqueue ， 搜索 template <class T> void update_rs
       * 对于这个 dcq的处理，搜索 DirtyCardQueueSet::iterate_closure_all_threads
       */
      jt->dirty_card_queue().enqueue(byte);
    } else { // 不是Java线程，就使用公共的 _shared_dirty_card_queue 中
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      _dcqs.shared_dirty_card_queue()->enqueue(byte); // 如果不是Java线程，那么就放到DCQS的对应的_shared_dirty_card_queue()中
    }
  }
}

void
G1SATBCardTableLoggingModRefBS::write_ref_field_static(void* field,
                                                       oop new_val) {
  uintptr_t field_uint = (uintptr_t)field;
  uintptr_t new_val_uint = cast_from_oop<uintptr_t>(new_val);
  uintptr_t comb = field_uint ^ new_val_uint;
  comb = comb >> HeapRegion::LogOfHRGrainBytes;
  if (comb == 0) return;
  if (new_val == NULL) return;
  // Otherwise, log it.
  G1SATBCardTableLoggingModRefBS* g1_bs =
    (G1SATBCardTableLoggingModRefBS*)Universe::heap()->barrier_set();
  g1_bs->write_ref_field_work(field, new_val);
}

void
G1SATBCardTableLoggingModRefBS::invalidate(MemRegion mr, bool whole_heap) {
  volatile jbyte* byte = byte_for(mr.start());
  jbyte* last_byte = byte_for(mr.last());
  Thread* thr = Thread::current();
  if (whole_heap) {
    while (byte <= last_byte) {
      *byte = dirty_card;
      byte++;
    }
  } else {
    // skip all consecutive young cards
    for (; byte <= last_byte && *byte == g1_young_gen; byte++);

    if (byte <= last_byte) {
      OrderAccess::storeload();
      // Enqueue if necessary.
      if (thr->is_Java_thread()) {
        JavaThread* jt = (JavaThread*)thr;
        for (; byte <= last_byte; byte++) {
          if (*byte == g1_young_gen) {
            continue;
          }
          if (*byte != dirty_card) {
            *byte = dirty_card;
            jt->dirty_card_queue().enqueue(byte);
          }
        }
      } else {
        MutexLockerEx x(Shared_DirtyCardQ_lock,
                        Mutex::_no_safepoint_check_flag);
        for (; byte <= last_byte; byte++) {
          if (*byte == g1_young_gen) {
            continue;
          }
          if (*byte != dirty_card) {
            *byte = dirty_card;
            _dcqs.shared_dirty_card_queue()->enqueue(byte);
          }
        }
      }
    }
  }
}
