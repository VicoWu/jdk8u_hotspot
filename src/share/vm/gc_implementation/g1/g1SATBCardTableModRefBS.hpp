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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1SATBCARDTABLEMODREFBS_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1SATBCARDTABLEMODREFBS_HPP

#include "gc_implementation/g1/g1RegionToSpaceMapper.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "memory/memRegion.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/macros.hpp"

class DirtyCardQueueSet;
class G1SATBCardTableLoggingModRefBS;

// This barrier is specialized to use a logging barrier to support
// snapshot-at-the-beginning marking.

/**
 * Card Table Modification Reference Barrier Set ， 卡表修改引用 栅栏集合
 * 主要用写屏障来支持SATB的写操作。
 * 需要跟子类G1SATBCardTableLoggingModRefBS相区别，子类G1SATBCardTableLoggingModRefBS主要负责RSet的写屏障
 */
class G1SATBCardTableModRefBS: public CardTableModRefBSForCTRS {
protected:
  enum G1CardValues {
    g1_young_gen = CT_MR_BS_last_reserved << 1 // g1_young_region = 11000000
  };

public:
  static int g1_young_card_val()   { return g1_young_gen; }

  // Add "pre_val" to a set of objects that may have been disconnected from the
  // pre-marking object graph.
  static void enqueue(oop pre_val);

  G1SATBCardTableModRefBS(MemRegion whole_heap,
                          int max_covered_regions);

  bool is_a(BarrierSet::Name bsn) {
    return bsn == BarrierSet::G1SATBCT || CardTableModRefBS::is_a(bsn);
  }

  virtual bool has_write_ref_pre_barrier() { return true; }

  // This notes that we don't need to access any BarrierSet data
  // structures, so this can be called from a static context.
  template <class T> static void write_ref_field_pre_static(T* field, oop newVal) {
    T heap_oop = oopDesc::load_heap_oop(field);
    if (!oopDesc::is_null(heap_oop)) {
      enqueue(oopDesc::decode_heap_oop(heap_oop));
    }
  }

  // We export this to make it available in cases where the static
  // type of the barrier set is known.  Note that it is non-virtual.
  template <class T> inline void inline_write_ref_field_pre(T* field, oop newVal) {
    write_ref_field_pre_static(field, newVal);
  }

  // These are the more general virtual versions.
  virtual void write_ref_field_pre_work(oop* field, oop new_val) {
    inline_write_ref_field_pre(field, new_val);
  }
  virtual void write_ref_field_pre_work(narrowOop* field, oop new_val) {
    inline_write_ref_field_pre(field, new_val);
  }
  virtual void write_ref_field_pre_work(void* field, oop new_val) {
    guarantee(false, "Not needed");
  }

  template <class T> void write_ref_array_pre_work(T* dst, int count);
  virtual void write_ref_array_pre(oop* dst, int count, bool dest_uninitialized);
  virtual void write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized);

/*
   Claimed and deferred bits are used together in G1 during the evacuation
   pause. These bits can have the following state transitions:
   1. The claimed bit can be put over any other card state. Except that
      the "dirty -> dirty and claimed" transition is checked for in
      G1 code and is not used.
   2. Deferred bit can be set only if the previous state of the card
      was either clean or claimed. mark_card_deferred() is wait-free.
      We do not care if the operation is be successful because if
      it does not it will only result in duplicate entry in the update
      buffer because of the "cache-miss". So it's not worth spinning.
 */

  bool is_card_claimed(size_t card_index) {
    jbyte val = _byte_map[card_index];
    return (val & (clean_card_mask_val() | claimed_card_val())) == claimed_card_val();
  }

  /**
   * 查看clean_card_val和claimed_card_val的实现可以看到，enum CardValues使用二进制的某一个位是否为1来标记当前的卡片的状态是否为该为的对应状态
   * 这是因为一个卡片可能同时处于CardValues中的一个或者多个状态
   * @param card_index
   */
  void set_card_claimed(size_t card_index) {
      jbyte val = _byte_map[card_index];
      if (val == clean_card_val()) { // 如果卡片是clean的状态，则直接设置为已经处理的状态，clean 的值为-1
        val = (jbyte)claimed_card_val(); // claimed的状态值为2
      } else {
        val |= (jbyte)claimed_card_val(); // 将claim状态为设置为1，同时保留其他状态位的值
      }
      _byte_map[card_index] = val;
  }

  void verify_g1_young_region(MemRegion mr) PRODUCT_RETURN;
  void g1_mark_as_young(const MemRegion& mr);

  bool mark_card_deferred(size_t card_index);

  bool is_card_deferred(size_t card_index) {
    jbyte val = _byte_map[card_index];
    return (val & (clean_card_mask_val() | deferred_card_val())) == deferred_card_val();
  }
};

class G1SATBCardTableLoggingModRefBSChangedListener : public G1MappingChangedListener {
 private:
  G1SATBCardTableLoggingModRefBS* _card_table;
 public:
  G1SATBCardTableLoggingModRefBSChangedListener() : _card_table(NULL) { }

  void set_card_table(G1SATBCardTableLoggingModRefBS* card_table) { _card_table = card_table; }

  virtual void on_commit(uint start_idx, size_t num_regions, bool zero_filled);
};

// Adds card-table logging to the post-barrier.
// Usual invariant: all dirty cards are logged in the DirtyCardQueueSet.

// 在 CardTableRS::CardTableRS中被构造，一个BS往往属于一个RSet
class G1SATBCardTableLoggingModRefBS: public G1SATBCardTableModRefBS {
  friend class G1SATBCardTableLoggingModRefBSChangedListener;
 private:
  G1SATBCardTableLoggingModRefBSChangedListener _listener;
  DirtyCardQueueSet& _dcqs; // 所有的脏卡片被记录在 DirtyCardQueueSet中
 public:
    // G1SATBCardTableLoggingModRefBS::compute_size
  static size_t compute_size(size_t mem_region_size_in_words) {
        // mem_region_size_in_words是整个堆内存的大小(以HeapWord为单位)， card_size_in_words代表一张卡片对应的HeapWord数量
    size_t number_of_slots = (mem_region_size_in_words / card_size_in_words); // 需要的卡片数量，即slot数量
    return ReservedSpace::allocation_align_size_up(number_of_slots);
  }

  G1SATBCardTableLoggingModRefBS(MemRegion whole_heap,
                                 int max_covered_regions);

  virtual void initialize() { }
  virtual void initialize(G1RegionToSpaceMapper* mapper);

  virtual void resize_covered_region(MemRegion new_region) { ShouldNotReachHere(); }

  bool is_a(BarrierSet::Name bsn) {
    return bsn == BarrierSet::G1SATBCTLogging ||
      G1SATBCardTableModRefBS::is_a(bsn);
  }

  void write_ref_field_work(void* field, oop new_val, bool release = false);

  // Can be called from static contexts.
  static void write_ref_field_static(void* field, oop new_val);

  // NB: if you do a whole-heap invalidation, the "usual invariant" defined
  // above no longer applies.
  void invalidate(MemRegion mr, bool whole_heap = false);

  void write_region_work(MemRegion mr)    { invalidate(mr); }
  void write_ref_array_work(MemRegion mr) { invalidate(mr); }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1SATBCARDTABLEMODREFBS_HPP
