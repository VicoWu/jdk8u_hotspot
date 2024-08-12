/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/g1BlockOffsetTable.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "gc_implementation/g1/heapRegionManager.inline.hpp"
#include "memory/allocation.hpp"
#include "memory/padded.inline.hpp"
#include "memory/space.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC
/**
 * 细粒度PRT(Per Region Table)，每一个HeapRegion对象会有一个PRT对象与之一一对应
 * 是一个双向链表，链表中的每一个节点是一个HeapRegion，每个节点中存放了一个key是card index的位图。向细粒度PRT中添加引用为：
 */
class PerRegionTable: public CHeapObj<mtGC> {
  friend class OtherRegionsTable; // 反向引用包含自己的OtherRegionsTable
  friend class HeapRegionRemSetIterator;

  HeapRegion*     _hr; // 反向引用了当前的PRT所对应的owning region
  BitMap          _bm; // 位图，位图的key是卡片索引，因此传入一个卡片索引，通过在位图中执行一次查找，就知道这个索引对应的内存区域是否有指向该Region的引用
  jint            _occupied; // 该Region的RSet所对应的卡片的总的数量

  // next pointer for free/allocated 'all' list
  PerRegionTable* _next; // OtherRegionsTable的全局链表的next指针，用来在  OtherRegionTable的PerRegionTable * _first_all_fine_prts和_last_all_fine_prts组成的双向链表中维护前后指针

    // prev pointer for the allocated 'all' list
  PerRegionTable* _prev; // OtherRegionsTable的全局链表的prev指针，用来在  OtherRegionTable的PerRegionTable * _first_all_fine_prts和_last_all_fine_prts组成的双向链表中维护前后指针

  // next pointer in collision list
  PerRegionTable * _collision_list_next; // 如果在_fine_grain_regions中基于链地址法存在冲突，那么_collision_list_next就保存了链中的下一个节点

  // Global free list of PRTs
  static PerRegionTable* _free_list;

protected:
  // We need access in order to union things into the base table.
  BitMap* bm() { return &_bm; }

  void recount_occupied() {
    _occupied = (jint) bm()->count_one_bits();
  }

  PerRegionTable(HeapRegion* hr) :
    _hr(hr),
    _occupied(0),
    _bm(HeapRegion::CardsPerRegion, false /* in-resource-area */),
    _collision_list_next(NULL), _next(NULL), _prev(NULL)
  {}

  /**
   * 这是PerRegionTable::add_card_work方法
   * CardIdx_t其实就是int，代表了卡片在卡表中的索引
   * 向该细粒度PTR（PerRegionTable）中添加一个卡片
   * @param from_card 指向该对象的对象对应的卡片的索引，(比如，a.field=b，那么from_card就是对象a对应的卡片在卡表中的索引
   * @param par 是否处于并行状态
   */
  void add_card_work(CardIdx_t from_card, bool par) {
    if (!_bm.at(from_card)) { // 这个卡片在当前的细粒度PTR中不存在
      if (par) { //处在并行状态，需要使用CAS来更新_occupied
        if (_bm.par_at_put(from_card, 1)) { // 通过cas的方式将index=from_card位置的值更新为1，如果更新成功，则返回true
          Atomic::inc(&_occupied);  // _bm更新索引位置from_card成功，因此自增_occupied
        }
      } else { // 非并发，没有冲突问题，直接更新
        _bm.at_put(from_card, 1);
        _occupied++;
      }
    }
  }

  /**
   * 这个方法是 PerRegionTable::add_reference_work方法
   * @param from
   * @param par
   */
  void add_reference_work(OopOrNarrowOopStar from, bool par) {
    // Must make this robust in case "from" is not in "_hr", because of
    // concurrency.

    if (G1TraceHeapRegionRememberedSet) {
      gclog_or_tty->print_cr("    PRT::Add_reference_work(" PTR_FORMAT "->" PTR_FORMAT").",
                             from,
                             UseCompressedOops
                             ? (void *)oopDesc::load_decode_heap_oop((narrowOop*)from)
                             : (void *)oopDesc::load_decode_heap_oop((oop*)from));
    }

    HeapRegion* loc_hr = hr();
    // If the test below fails, then this table was reused concurrently
    // with this operation.  This is OK, since the old table was coarsened,
    // and adding a bit to the new table is never incorrect.
    // If the table used to belong to a continues humongous region and is
    // now reused for the corresponding start humongous region, we need to
    // make sure that we detect this. Thus, we call is_in_reserved_raw()
    // instead of just is_in_reserved() here.
    if (loc_hr->is_in_reserved_raw(from)) {
      size_t hw_offset = pointer_delta((HeapWord*)from, loc_hr->bottom());
      CardIdx_t from_card = (CardIdx_t)
          hw_offset >> (CardTableModRefBS::card_shift - LogHeapWordSize);

      assert(0 <= from_card && (size_t)from_card < HeapRegion::CardsPerRegion,
             "Must be in range.");
      add_card_work(from_card, par); // 向细粒度的PRT中添加卡片
    }
  }

public:
    /**
     * 这个方法是PerRegionTable的方法，通过读取_hr的值，并转换成HeapRegion的指针,代表当前这个PRT所对应的HeapRegion
     * Read-Acquire  具有 Read-Acquire 语义的 Read 操作保证，所有后续的读写只有在该 Read 执行完毕后才能执行
     * Write-Release 具有 Write-Release 语义的 Write 操作保证，只有之前的所有读写都已经执行完毕，该 write 才能执行
     * 关于有序内存访问，参考 https://novoland.github.io/%E5%B9%B6%E5%8F%91/2014/07/26/Java%E5%86%85%E5%AD%98%E6%A8%A1%E5%9E%8B.html
     * @return
     */
  HeapRegion* hr() const {
    return (HeapRegion*) OrderAccess::load_ptr_acquire(&_hr);
  }

  jint occupied() const {
    // Overkill, but if we ever need it...
    // guarantee(_occupied == _bm.count_one_bits(), "Check");
    return _occupied;
  }

  void init(HeapRegion* hr, bool clear_links_to_all_list) {
    if (clear_links_to_all_list) {
      set_next(NULL);
      set_prev(NULL);
    }
    _collision_list_next = NULL;
    _occupied = 0;
    _bm.clear();
    // Make sure that the bitmap clearing above has been finished before publishing
    // this PRT to concurrent threads.
    OrderAccess::release_store_ptr(&_hr, hr);
  }

  /**
   * 这个方法其实是PerRegionTable::add_reference
   */
  void add_reference(OopOrNarrowOopStar from) {
    add_reference_work(from, /*parallel*/ true);
  }

  void seq_add_reference(OopOrNarrowOopStar from) {
    add_reference_work(from, /*parallel*/ false);
  }

  void scrub(CardTableModRefBS* ctbs, BitMap* card_bm) {
    HeapWord* hr_bot = hr()->bottom();
    size_t hr_first_card_index = ctbs->index_for(hr_bot);
    bm()->set_intersection_at_offset(*card_bm, hr_first_card_index);
    recount_occupied();
  }

  /**
   * 这个方法是PerRegionTable的方法
   * @param from_card_index
   */
  void add_card(CardIdx_t from_card_index) {
    add_card_work(from_card_index, /*parallel*/ true);
  }

  void seq_add_card(CardIdx_t from_card_index) {
    add_card_work(from_card_index, /*parallel*/ false);
  }

  // (Destructively) union the bitmap of the current table into the given
  // bitmap (which is assumed to be of the same size.)
  void union_bitmap_into(BitMap* bm) {
    bm->set_union(_bm);
  }

  // Mem size in bytes.
  size_t mem_size() const {
    return sizeof(PerRegionTable) + _bm.size_in_words() * HeapWordSize;
  }

  // 这是方法 PerRegionTable::contains_reference
  bool contains_reference(OopOrNarrowOopStar from) const {
    assert(hr()->is_in_reserved(from), "Precondition.");
    size_t card_ind = pointer_delta(from, hr()->bottom(),
                                    CardTableModRefBS::card_size);
    return _bm.at(card_ind);
  }

  // Bulk-free the PRTs from prt to last, assumes that they are
  // linked together using their _next field.
  static void bulk_free(PerRegionTable* prt, PerRegionTable* last) {
    while (true) {
      PerRegionTable* fl = _free_list;
      last->set_next(fl);
      PerRegionTable* res = (PerRegionTable*) Atomic::cmpxchg_ptr(prt, &_free_list, fl);
      if (res == fl) {
        return;
      }
    }
    ShouldNotReachHere();
  }

  static void free(PerRegionTable* prt) {
    bulk_free(prt, prt);
  }

  // Returns an initialized PerRegionTable instance.
  static PerRegionTable* alloc(HeapRegion* hr) {
    PerRegionTable* fl = _free_list;
    while (fl != NULL) {
      PerRegionTable* nxt = fl->next();
      PerRegionTable* res =
        (PerRegionTable*)
        Atomic::cmpxchg_ptr(nxt, &_free_list, fl);
      if (res == fl) {
        fl->init(hr, true);
        return fl;
      } else {
        fl = _free_list;
      }
    }
    assert(fl == NULL, "Loop condition.");
    return new PerRegionTable(hr);
  }

  PerRegionTable* next() const { return _next; }
  void set_next(PerRegionTable* next) { _next = next; }
  PerRegionTable* prev() const { return _prev; }
  void set_prev(PerRegionTable* prev) { _prev = prev; }

  // Accessor and Modification routines for the pointer for the
  // singly linked collision list that links the PRTs within the
  // OtherRegionsTable::_fine_grain_regions hash table.
  //
  // It might be useful to also make the collision list doubly linked
  // to avoid iteration over the collisions list during scrubbing/deletion.
  // OTOH there might not be many collisions.

  /**
   * 这个方法是PerRegionTable::collision_list_next的方法
   * 解决hash冲突的方式，按照链地址法解决hash查找的冲突
   * @return
   */
  PerRegionTable* collision_list_next() const {
    return _collision_list_next;
  }

  void set_collision_list_next(PerRegionTable* next) {
    _collision_list_next = next;
  }

  PerRegionTable** collision_list_next_addr() {
    return &_collision_list_next;
  }

  static size_t fl_mem_size() {
    PerRegionTable* cur = _free_list;
    size_t res = 0;
    while (cur != NULL) {
      res += cur->mem_size();
      cur = cur->next();
    }
    return res;
  }

  static void test_fl_mem_size();
};

PerRegionTable* PerRegionTable::_free_list = NULL;

size_t OtherRegionsTable::_max_fine_entries = 0;
size_t OtherRegionsTable::_mod_max_fine_entries_mask = 0;
size_t OtherRegionsTable::_fine_eviction_stride = 0;
size_t OtherRegionsTable::_fine_eviction_sample_size = 0;

OtherRegionsTable::OtherRegionsTable(HeapRegion* hr, Mutex* m) :
  _g1h(G1CollectedHeap::heap()),
  _hr(hr), _m(m),
  _coarse_map(G1CollectedHeap::heap()->max_regions(),
              false /* in-resource-area */),
  _fine_grain_regions(NULL),
  _first_all_fine_prts(NULL), _last_all_fine_prts(NULL),
  _n_fine_entries(0), _n_coarse_entries(0),
  _fine_eviction_start(0),
  _sparse_table(hr)
{
  typedef PerRegionTable* PerRegionTablePtr;

  if (_max_fine_entries == 0) {
    assert(_mod_max_fine_entries_mask == 0, "Both or none.");
    size_t max_entries_log = (size_t)log2_long((jlong)G1RSetRegionEntries);
    _max_fine_entries = (size_t)1 << max_entries_log;
    _mod_max_fine_entries_mask = _max_fine_entries - 1;

    assert(_fine_eviction_sample_size == 0
           && _fine_eviction_stride == 0, "All init at same time.");
    _fine_eviction_sample_size = MAX2((size_t)4, max_entries_log);
    _fine_eviction_stride = _max_fine_entries / _fine_eviction_sample_size;
  }

  _fine_grain_regions = NEW_C_HEAP_ARRAY3(PerRegionTablePtr, _max_fine_entries,
                        mtGC, CURRENT_PC, AllocFailStrategy::RETURN_NULL);

  if (_fine_grain_regions == NULL) {
    vm_exit_out_of_memory(sizeof(void*)*_max_fine_entries, OOM_MALLOC_ERROR,
                          "Failed to allocate _fine_grain_entries.");
  }

  for (size_t i = 0; i < _max_fine_entries; i++) {
    _fine_grain_regions[i] = NULL;
  }
}

/**
 * link_to_all其实就是将这个PerRegionTable添加到OtherRegionsTable对象所维护的一个全局双向链表中去，与哈希表无关
 * @param prt
 */
void OtherRegionsTable::link_to_all(PerRegionTable* prt) {
  // We always append to the beginning of the list for convenience;
  // the order of entries in this list does not matter.
  if (_first_all_fine_prts != NULL) { // 当前的OtherRegionsTable对象之前已经有某个PerRegionTable对象
    assert(_first_all_fine_prts->prev() == NULL, "invariant");
    _first_all_fine_prts->set_prev(prt);
    prt->set_next(_first_all_fine_prts); // 新插入的prt总是放在第一个位置，之前的_first_all_fine_prts作为这个节点的下一个节点
  } else { // 当前的OtherRegionsTable对象还不存在第一个PerRegionTable对象
    // this is the first element we insert. Adjust the "last" pointer
    _last_all_fine_prts = prt; // 这第一个prt是第一个，当然也是最后一个，因此更新_last_all_fine_prts
    assert(prt->next() == NULL, "just checking");
  }
  // the new element is always the first element without a predecessor
  prt->set_prev(NULL); //新插入的PerRegionTable在 所在的hash位置对应的链中，总是第一个，不会有前驱节点
  _first_all_fine_prts = prt; // 新插入的prt总是放在第一个位置

  assert(prt->prev() == NULL, "just checking");
  assert(_first_all_fine_prts == prt, "just checking");
  assert((_first_all_fine_prts == NULL && _last_all_fine_prts == NULL) ||
         (_first_all_fine_prts != NULL && _last_all_fine_prts != NULL),
         "just checking");
  assert(_last_all_fine_prts == NULL || _last_all_fine_prts->next() == NULL,
         "just checking");
  assert(_first_all_fine_prts == NULL || _first_all_fine_prts->prev() == NULL,
         "just checking");
}

void OtherRegionsTable::unlink_from_all(PerRegionTable* prt) {
  if (prt->prev() != NULL) {
    assert(_first_all_fine_prts != prt, "just checking");
    prt->prev()->set_next(prt->next());
    // removing the last element in the list?
    if (_last_all_fine_prts == prt) {
      _last_all_fine_prts = prt->prev();
    }
  } else {
    assert(_first_all_fine_prts == prt, "just checking");
    _first_all_fine_prts = prt->next();
    // list is empty now?
    if (_first_all_fine_prts == NULL) {
      _last_all_fine_prts = NULL;
    }
  }

  if (prt->next() != NULL) {
    prt->next()->set_prev(prt->prev());
  }

  prt->set_next(NULL);
  prt->set_prev(NULL);

  assert((_first_all_fine_prts == NULL && _last_all_fine_prts == NULL) ||
         (_first_all_fine_prts != NULL && _last_all_fine_prts != NULL),
         "just checking");
  assert(_last_all_fine_prts == NULL || _last_all_fine_prts->next() == NULL,
         "just checking");
  assert(_first_all_fine_prts == NULL || _first_all_fine_prts->prev() == NULL,
         "just checking");
}

int**  FromCardCache::_cache = NULL;
uint   FromCardCache::_max_regions = 0;
size_t FromCardCache::_static_mem_size = 0;

void FromCardCache::initialize(uint n_par_rs, uint max_num_regions) {
  guarantee(_cache == NULL, "Should not call this multiple times");

  _max_regions = max_num_regions;
  _cache = Padded2DArray<int, mtGC>::create_unfreeable(n_par_rs,
                                                       _max_regions,
                                                       &_static_mem_size);

  invalidate(0, _max_regions);
}

void FromCardCache::invalidate(uint start_idx, size_t new_num_regions) {
  guarantee((size_t)start_idx + new_num_regions <= max_uintx,
            err_msg("Trying to invalidate beyond maximum region, from %u size " SIZE_FORMAT,
                    start_idx, new_num_regions));
  for (uint i = 0; i < HeapRegionRemSet::num_par_rem_sets(); i++) {
    uint end_idx = (start_idx + (uint)new_num_regions);
    assert(end_idx <= _max_regions, "Must be within max.");
    for (uint j = start_idx; j < end_idx; j++) {
      set(i, j, InvalidCard);
    }
  }
}

#ifndef PRODUCT
void FromCardCache::print(outputStream* out) {
  for (uint i = 0; i < HeapRegionRemSet::num_par_rem_sets(); i++) {
    for (uint j = 0; j < _max_regions; j++) {
      out->print_cr("_from_card_cache[" UINT32_FORMAT "][" UINT32_FORMAT "] = " INT32_FORMAT ".",
                    i, j, at(i, j));
    }
  }
}
#endif

void FromCardCache::clear(uint region_idx) {
  uint num_par_remsets = HeapRegionRemSet::num_par_rem_sets();
  for (uint i = 0; i < num_par_remsets; i++) {
    set(i, region_idx, InvalidCard);
  }
}

void OtherRegionsTable::initialize(uint max_regions) {
  FromCardCache::initialize(HeapRegionRemSet::num_par_rem_sets(), max_regions);
}

void OtherRegionsTable::invalidate(uint start_idx, size_t num_regions) {
  FromCardCache::invalidate(start_idx, num_regions);
}

void OtherRegionsTable::print_from_card_cache() {
  FromCardCache::print();
}

 /**
  * 在RSet中维护引用关系
  * @param from from region的region id
  * @param tid 当前的线程ID，作为在FromCardCache中进行查找的一个key，避免线程冲突
  */
void OtherRegionsTable::add_reference(OopOrNarrowOopStar from, int tid) {
     // 搜索HeapRegion* hr() . hrm_index() 返回这个HeapRegion在HeapRegionManager中的地址
  uint cur_hrm_ind = hr()->hrm_index();

  if (G1TraceHeapRegionRememberedSet) {
    gclog_or_tty->print_cr("ORT::add_reference_work(" PTR_FORMAT "->" PTR_FORMAT ").",
                                                    from,
                                                    UseCompressedOops
                                                    ? (void *)oopDesc::load_decode_heap_oop((narrowOop*)from)
                                                    : (void *)oopDesc::load_decode_heap_oop((oop*)from));
  }
  // 计算来自引用者的卡表索引。我们要做的就是将引用方的卡片索引添加到被引用方的RSet中去
  int from_card = (int)(uintptr_t(from) >> CardTableModRefBS::card_shift);

  if (G1TraceHeapRegionRememberedSet) {
    gclog_or_tty->print_cr("Table for [" PTR_FORMAT "...): card %d (cache = " INT32_FORMAT ")",
                  hr()->bottom(), from_card,
                  FromCardCache::at((uint)tid, cur_hrm_ind));
  }

  /**
   * 静态方法，查看是否已经包含了这个from_card的索引。搜索 static bool contains_or_replace
   * 返回true， 说明已经找到，即这个from_card已经存在于tid + cur_hrm_ind 作为key
   * 返回false，说明没找到(但是在方法contains_or_replace中会进行设置，将from_card放到缓存中)
   */
  if (FromCardCache::contains_or_replace((uint)tid, cur_hrm_ind, from_card)) {
    if (G1TraceHeapRegionRememberedSet) {
      gclog_or_tty->print_cr("  from-card cache hit.");
    }
    assert(contains_reference(from), err_msg("We just found " PTR_FORMAT " in the FromCardCache", from));
    return;
  }
  // FromCardCache::contains_or_replace返回了false，在方法contains_or_replace中已经将from_card放入到了缓存中

  // 获取引用者所在的HeapRegion和Region的索引值
  // Note that this may be a continued H region.
  HeapRegion* from_hr = _g1h->heap_region_containing_raw(from);
  RegionIdx_t from_hrm_ind = (RegionIdx_t) from_hr->hrm_index();

  // If the region is already coarsened, return.
  if (_coarse_map.at(from_hrm_ind)) { // 这个引用者的region index已经存在于粗粒度PRT中了
    if (G1TraceHeapRegionRememberedSet) {
      gclog_or_tty->print_cr("  coarse map hit.");
    }
    assert(contains_reference(from), err_msg("We just found " PTR_FORMAT " in the Coarse table", from));
    return; // 直接返回
  }

  // 粗粒度索引中不存在这个from region，继续在prt中进行搜索
  // Otherwise find a per-region table to add it to.
  /**
   *  region index根据细粒度PerRegionTable的size取模，取模以后在find_region_table中进行hash查找
   */
  size_t ind = from_hrm_ind & _mod_max_fine_entries_mask;
  /**
   * 调用OtherRegionsTable::find_region_table，其实就是在细粒度的PRT(_fine_grain_regions**)中尝试查找这个from_hr是否存在
   */
  PerRegionTable* prt = find_region_table(ind, from_hr); // 搜索细粒度索引表_fine_grain_regions，看看是否找到对应的PerRegionTable
  if (prt == NULL) { // 没找到这样一个PerRegionTable
    MutexLockerEx x(_m, Mutex::_no_safepoint_check_flag);
    // Confirm that it's really not there...
    prt = find_region_table(ind, from_hr); // 加锁然后确认
    if (prt == NULL) { // 在细粒度索引中并没有找到

      uintptr_t from_hr_bot_card_index =
        uintptr_t(from_hr->bottom())
          >> CardTableModRefBS::card_shift;
      CardIdx_t card_index = from_card - from_hr_bot_card_index; // 获取from_card的偏移量，即索引值
      assert(0 <= card_index && (size_t)card_index < HeapRegion::CardsPerRegion,
             "Must be in range.");
      // 如果没有找到，并且我们enable了稀疏索引，那么就将这个region添加到稀疏索引中
      if (G1HRRSUseSparseTable &&  // G1HRRSUseSparseTable是是否使用稀疏索引的标志位
          _sparse_table.add_card(from_hrm_ind, card_index)) { //  向稀疏PRT中添加卡片成功, 搜索 SparsePRT::add_card 查看方法的具体实现
          // add_card()返回true，说明添加成功，并没有overflow
        if (G1RecordHRRSOops) {
          HeapRegionRemSet::record(hr(), from);
          if (G1TraceHeapRegionRememberedSet) {
            gclog_or_tty->print("   Added card " PTR_FORMAT " to region "
                                "[" PTR_FORMAT "...) for ref " PTR_FORMAT ".\n",
                                align_size_down(uintptr_t(from),
                                                CardTableModRefBS::card_size),
                                hr()->bottom(), from);
          }
        }
        if (G1TraceHeapRegionRememberedSet) {
          gclog_or_tty->print_cr("   added card to sparse table.");
        }
        assert(contains_reference_locked(from), err_msg("We just added " PTR_FORMAT " to the Sparse table", from));
        // 可以看到，如果前面的add_card()返回了true，这个方法就直接返回了，即引用信息添加到稀疏索引中，就不会添加到细粒度索引了
        return;
      } else {
        if (G1TraceHeapRegionRememberedSet) {
          gclog_or_tty->print_cr("   [tid %d] sparse table entry "
                        "overflow(f: %d, t: %u)",
                        tid, from_hrm_ind, cur_hrm_ind);
        }
      }
      // 并没有enable G1HRRSUseSparseTable，或者，尽管enable了，但是 并没有成功添加引用关系到稀疏索引表(RSHashTable存满了）
      // 向细粒度PRT索引中添加这个Region的信息。需要关注这个哈希表是否已经满了
      if (_n_fine_entries == _max_fine_entries) { // 当前细粒度哈希索引里面entry的数量已经等于最大entry的数量
          /**
           * 调用 OtherRegionsTable::delete_region_table，
           *    从细粒度索引的哈希表中删除一个PerRegionTable(当然，我们看方法的具体实现，会对应同步删除粗粒度表，具体细节不用关注)
           */
        prt = delete_region_table();
        // There is no need to clear the links to the 'all' list here:
        // prt will be reused immediately, i.e. remain in the 'all' list.
        prt->init(from_hr, false /* clear_links_to_all_list */);
      } else {// 细粒度的哈希索引还没有满，那么就重新申请一个PerRegionTable对象
        prt = PerRegionTable::alloc(from_hr); // 为这个From HeapRegion分配一个PerRegionTable对象
        link_to_all(prt); // 调用 OtherRegionsTable::link_to_all， 将这个 PerRegionTable 放入OtherRegionsTable所维护的PerRegionTable对象的双向链表中
      }

      // 将新申请的或者初始化的细粒度PerRegionTable加入细粒度PerRegionTable表集合中
      PerRegionTable* first_prt = _fine_grain_regions[ind]; // 哈希表中的第一个PerRegionTable地址
      prt->set_collision_list_next(first_prt); // 我们通过OtherRegionsTable::find_region_table可以看到发生哈希冲突的时候遍历链表的方法
      // The assignment into _fine_grain_regions allows the prt to
      // start being used concurrently. In addition to
      // collision_list_next which must be visible (else concurrent
      // parsing of the list, if any, may fail to see other entries),
      // the content of the prt must be visible (else for instance
      // some mark bits may not yet seem cleared or a 'later' update
      // performed by a concurrent thread could be undone when the
      // zeroing becomes visible). This requires store ordering.

      /**
       * 对 _fine_grain_regions 的分配允许 prt 开始同时使用。 除了 collision_list_next 必须可见（否则列表的并发解析（如果有）可能无法看到其他条目）之外，
       * prt 的内容也必须可见（否则例如某些标记位可能尚未清除或“ 当归零变得可见时，并发线程执行的后续更新可能会被撤消）。 这需要Store操作有序
       */
      OrderAccess::release_store_ptr((volatile PerRegionTable*)&_fine_grain_regions[ind], prt);
      _n_fine_entries++; // 细粒度的PRT数量加1
      // 将稀疏表的卡片移动到细粒度hash表中
      if (G1HRRSUseSparseTable) { // 如果使用稀疏表
        // Transfer from sparse to fine-grain.
        SparsePRTEntry *sprt_entry = _sparse_table.get_entry(from_hrm_ind);
        assert(sprt_entry != NULL, "There should have been an entry");
        for (int i = 0; i < SparsePRTEntry::cards_num(); i++) { // 遍历这个SparsePRTEntry对象的所有卡片
          CardIdx_t c = sprt_entry->card(i); //
          if (c != SparsePRTEntry::NullEntry) {
            prt->add_card(c); // 不是一个空卡片，就把这个卡片添加到prt中
          }
        }
        // Now we can delete the sparse entry.
        bool res = _sparse_table.delete_entry(from_hrm_ind); //  将这个卡片从稀疏索引中删除，因此相当于从稀疏卡片中移动到了细粒度卡片中
        assert(res, "It should have been there.");
      }
    }
    assert(prt != NULL && prt->hr() == from_hr, "consequence");
  }
  // Note that we can't assert "prt->hr() == from_hr", because of the
  // possibility of concurrent reuse.  But see head comment of
  // OtherRegionsTable for why this is OK.
  assert(prt != NULL, "Inv");

  prt->add_reference(from); // 调用了PerRegionTable::add_reference

  if (G1RecordHRRSOops) {
    HeapRegionRemSet::record(hr(), from);
    if (G1TraceHeapRegionRememberedSet) {
      gclog_or_tty->print("Added card " PTR_FORMAT " to region "
                          "[" PTR_FORMAT "...) for ref " PTR_FORMAT ".\n",
                          align_size_down(uintptr_t(from),
                                          CardTableModRefBS::card_size),
                          hr()->bottom(), from);
    }
  }
  assert(contains_reference(from), err_msg("We just added " PTR_FORMAT " to the PRT", from));
}

/**
 * 这个方法在_fine_grain_regions中查找对应的hr
 * 从这里可以看到，_fine_grain_regions其实是一个数组哈希表，
 *  哈希表中的每一个元素是细粒度PRT PerRegionTable,然后通过链地址法链接起来处于同一个hash位置的所有的PRT
 * @param ind
 * @param hr
 * @return
 */
PerRegionTable*
OtherRegionsTable::find_region_table(size_t ind, HeapRegion* hr) const {
  assert(0 <= ind && ind < _max_fine_entries, "Preconditions.");
  PerRegionTable* prt = _fine_grain_regions[ind];
  while (prt != NULL && prt->hr() != hr) { // 通过链地址法，在冲突链中查找
    prt = prt->collision_list_next();
  }
  // Loop postcondition is the method postcondition.
  return prt;
}

jint OtherRegionsTable::_n_coarsenings = 0;

/**
 * OtherRegionsTable中的方法，用来在_n_fine_entries == _max_fine_entries 从细粒度表的_fine_grain_regions删除一个PerRegionTable元素
 * 删除以后，通过粗粒度表_coarse_map来记录这个引用关系
 * @return
 */
PerRegionTable* OtherRegionsTable::delete_region_table() {
  assert(_m->owned_by_self(), "Precondition");
  assert(_n_fine_entries == _max_fine_entries, "Precondition");
  PerRegionTable* max = NULL;
  jint max_occ = 0;
  PerRegionTable** max_prev = NULL;
  size_t max_ind;

  /**
   * _fine_eviction_start是OtherRegionsTable对象维护的，因此每次调用delete_region_table()，都会从上一个位置开始
   */
  size_t i = _fine_eviction_start;
  for (size_t k = 0; k < _fine_eviction_sample_size; k++) {
    size_t ii = i;
    // Make sure we get a non-NULL sample.
    while (_fine_grain_regions[ii] == NULL) { // 直到找到一个非空的元素，这个元素是一个PerRegionPRT*，指向了对应的PerRegionPRT对象
      ii++;
      if (ii == _max_fine_entries) ii = 0;
      guarantee(ii != i, "We must find one.");
    }
    PerRegionTable** prev = &_fine_grain_regions[ii];
    PerRegionTable* cur = *prev;
    // 处理哈希表的ii位置的这个冲突链表(_fine_grain_regions使用链地址法解决冲突，因为每一个hash位置都挂在了一个冲突列表，列表的每一个元素都是一个PerRegionTable* )
    while (cur != NULL) { //
      jint cur_occ = cur->occupied(); // 调用PerRegionTable::occupied方法
      if (max == NULL || cur_occ > max_occ) { // 找出并记录occupied最大的PerRegionTable，记录在max中
        max = cur;  // 记录这个occupied最大的PerRegionTable的地址
        max_prev = prev;
        max_ind = i; // 这个最大值所对应的细粒度哈希表的位置
        max_occ = cur_occ;
      }
      prev = cur->collision_list_next_addr();
      cur = cur->collision_list_next(); // 冲突链表的下一个PerRegionTable
    }
    i = i + _fine_eviction_stride;
    if (i >= _n_fine_entries) i = i - _n_fine_entries;
  }

  _fine_eviction_start++;

  if (_fine_eviction_start >= _n_fine_entries) {
    _fine_eviction_start -= _n_fine_entries;
  }

  guarantee(max != NULL, "Since _n_fine_entries > 0");
  guarantee(max_prev != NULL, "Since max != NULL.");

  // 设置对应的粗粒度表
  // Set the corresponding coarse bit.
  size_t max_hrm_index = (size_t) max->hr()->hrm_index();
  if (!_coarse_map.at(max_hrm_index)) { // 如果粗粒度表中还没有这个准备删除的PerRegionTable对应的Region的索引，就添加到粗粒度表中
    _coarse_map.at_put(max_hrm_index, true); //
    _n_coarse_entries++; // 元素数量加1
    if (G1TraceHeapRegionRememberedSet) {
      gclog_or_tty->print("Coarsened entry in region [" PTR_FORMAT "...] "
                 "for region [" PTR_FORMAT "...] (%d coarse entries).\n",
                 hr()->bottom(),
                 max->hr()->bottom(),
                 _n_coarse_entries);
    }
  }

  // Unsplice.
  *max_prev = max->collision_list_next();
  Atomic::inc(&_n_coarsenings); // 从上面的逻辑来看，这个_n_coarsenings并不是_coarse_map元素的个数(因为上面的逻辑中_coarse_map可能已经存在该region)
  _n_fine_entries--; // 整个哈希表中减少了一个PerRegionTable*元素，因此计数器_n_fine_entries减一
  return max; // 返回刚刚从细粒度哈希表中删除的元素
}


/**
 * 这个方法必须在STW的环境下被单线程执行
 */
// At present, this must be called stop-world single-threaded.
void OtherRegionsTable::scrub(CardTableModRefBS* ctbs,
                              BitMap* region_bm, BitMap* card_bm) {
  // First eliminated garbage regions from the coarse map.
  if (G1RSScrubVerbose) {
    gclog_or_tty->print_cr("Scrubbing region %u:", hr()->hrm_index());
  }

  assert(_coarse_map.size() == region_bm->size(), "Precondition");
  if (G1RSScrubVerbose) {
    gclog_or_tty->print("   Coarse map: before = " SIZE_FORMAT "...",
                        _n_coarse_entries);
  }
  _coarse_map.set_intersection(*region_bm);
  _n_coarse_entries = _coarse_map.count_one_bits();
  if (G1RSScrubVerbose) {
    gclog_or_tty->print_cr("   after = " SIZE_FORMAT ".", _n_coarse_entries);
  }

  // Now do the fine-grained maps.
  for (size_t i = 0; i < _max_fine_entries; i++) { // 对整个_fine_grain_regions哈希表进行遍历
    PerRegionTable* cur = _fine_grain_regions[i]; // 对这个hash位置挂在的冲突链表进行逐个处理
    PerRegionTable** prev = &_fine_grain_regions[i];
    while (cur != NULL) {
      PerRegionTable* nxt = cur->collision_list_next();
      // If the entire region is dead, eliminate.
      if (G1RSScrubVerbose) {
        gclog_or_tty->print_cr("     For other region %u:",
                               cur->hr()->hrm_index());
      }
      if (!region_bm->at((size_t) cur->hr()->hrm_index())) {
        *prev = nxt;
        cur->set_collision_list_next(NULL);
        _n_fine_entries--;
        if (G1RSScrubVerbose) {
          gclog_or_tty->print_cr("          deleted via region map.");
        }
        unlink_from_all(cur);
        PerRegionTable::free(cur);
      } else {
        // Do fine-grain elimination.
        if (G1RSScrubVerbose) {
          gclog_or_tty->print("          occ: before = %4d.", cur->occupied());
        }
        cur->scrub(ctbs, card_bm); // 调用PerRegionTable::scrub方法，搜索void scrub(CardTableModRefBS* ctbs, BitMap* card_bm)
        if (G1RSScrubVerbose) {
          gclog_or_tty->print_cr("          after = %4d.", cur->occupied());
        }
        // Did that empty the table completely?
        if (cur->occupied() == 0) { // 当前的PRT已经全部是空的了
          *prev = nxt;
          cur->set_collision_list_next(NULL);
          _n_fine_entries--;
          unlink_from_all(cur);
          PerRegionTable::free(cur);
        } else {
          prev = cur->collision_list_next_addr();
        }
      }
      cur = nxt;
    }
  }
  // Since we may have deleted a from_card_cache entry from the RS, clear
  // the FCC.
  clear_fcc();
}

bool OtherRegionsTable::occupancy_less_or_equal_than(size_t limit) const {
  if (limit <= (size_t)G1RSetSparseRegionEntries) {
    return occ_coarse() == 0 && _first_all_fine_prts == NULL && occ_sparse() <= limit;
  } else {
    // Current uses of this method may only use values less than G1RSetSparseRegionEntries
    // for the limit. The solution, comparing against occupied() would be too slow
    // at this time.
    Unimplemented();
    return false;
  }
}

bool OtherRegionsTable::is_empty() const {
  return occ_sparse() == 0 && occ_coarse() == 0 && _first_all_fine_prts == NULL;
}

/**
 * 当前总共的cards的数量，调用者是YoungList::rs_length_sampling_next
 * 需要注意到，粗粒度表的精确度只到region，因此针对粗粒度表的cards的数量的统计是按照最大值统计的
 * @return
 */
size_t OtherRegionsTable::occupied() const {
  size_t sum = occ_fine(); // 细粒度表
  sum += occ_sparse(); //  稀疏表
  sum += occ_coarse();  // 粗粒度表
  return sum;
}

/**
 * 统计细粒度表占用
 * @return
 */
size_t OtherRegionsTable::occ_fine() const {
  size_t sum = 0;

  size_t num = 0;
  /**
   * 细粒度表的占用统计是直接遍历双向链表_first_all_fine_prts,
   *    这个双向链表和细粒度表PerRegionTable** _fine_grain_regions其实是相同的一系列的PRT的不同组织结构，前者是双向链表，后者是哈希
   */
  PerRegionTable * cur = _first_all_fine_prts;
  while (cur != NULL) {
    sum += cur->occupied(); // 当前的这个PerRegionTable一共包含了多少个card
    cur = cur->next();
    num++;
  }
  guarantee(num == _n_fine_entries, "just checking");
  return sum;
}

/**
 * 统计粗粒度表占用
 * 比如粗粒度表中一共有10个entry，指代了10个(Region A -> Region B)的pair关系，由于粗粒度表只能到达region的粒度，而一个region的卡片数量是HeapRegion::CardsPerRegion，
 * 因此，一个entry数量为10的粗粒度表，最多代表_n_coarse_entries * HeapRegion::CardsPerRegion个应用关系
 * @return
 */
size_t OtherRegionsTable::occ_coarse() const {
  return (_n_coarse_entries * HeapRegion::CardsPerRegion); // 搜CardsPerRegion = GrainBytes >> CardTableModRefBS::card_shift;可以看到，CardsPerRegion
}

/**
 * 统计稀疏表占用
 * @return
 */
size_t OtherRegionsTable::occ_sparse() const {
  return _sparse_table.occupied(); // 搜索 size_t occupied() const { return _next->occupied_cards(); }，和上面的occ_coarse和occ_fine一样，都是统计的精确到cards的数量
}

size_t OtherRegionsTable::mem_size() const {
  size_t sum = 0;
  // all PRTs are of the same size so it is sufficient to query only one of them.
  if (_first_all_fine_prts != NULL) {
    assert(_last_all_fine_prts != NULL &&
      _first_all_fine_prts->mem_size() == _last_all_fine_prts->mem_size(), "check that mem_size() is constant");
    sum += _first_all_fine_prts->mem_size() * _n_fine_entries;
  }
  sum += (sizeof(PerRegionTable*) * _max_fine_entries);
  sum += (_coarse_map.size_in_words() * HeapWordSize);
  sum += (_sparse_table.mem_size());
  sum += sizeof(OtherRegionsTable) - sizeof(_sparse_table); // Avoid double counting above.
  return sum;
}

size_t OtherRegionsTable::static_mem_size() {
  return FromCardCache::static_mem_size();
}

size_t OtherRegionsTable::fl_mem_size() {
  return PerRegionTable::fl_mem_size();
}

void OtherRegionsTable::clear_fcc() {
  FromCardCache::clear(hr()->hrm_index());
}

void OtherRegionsTable::clear() {
  // if there are no entries, skip this step
  if (_first_all_fine_prts != NULL) {
    guarantee(_first_all_fine_prts != NULL && _last_all_fine_prts != NULL, "just checking");
    PerRegionTable::bulk_free(_first_all_fine_prts, _last_all_fine_prts);
    memset(_fine_grain_regions, 0, _max_fine_entries * sizeof(_fine_grain_regions[0]));
  } else {
    guarantee(_first_all_fine_prts == NULL && _last_all_fine_prts == NULL, "just checking");
  }

  _first_all_fine_prts = _last_all_fine_prts = NULL;
  _sparse_table.clear();
  _coarse_map.clear();
  _n_fine_entries = 0;
  _n_coarse_entries = 0;

  clear_fcc();
}

bool OtherRegionsTable::del_single_region_table(size_t ind,
                                                HeapRegion* hr) {
  assert(0 <= ind && ind < _max_fine_entries, "Preconditions.");
  PerRegionTable** prev_addr = &_fine_grain_regions[ind];
  PerRegionTable* prt = *prev_addr;
  while (prt != NULL && prt->hr() != hr) {
    prev_addr = prt->collision_list_next_addr();
    prt = prt->collision_list_next();
  }
  if (prt != NULL) {
    assert(prt->hr() == hr, "Loop postcondition.");
    *prev_addr = prt->collision_list_next();
    unlink_from_all(prt);
    PerRegionTable::free(prt);
    _n_fine_entries--;
    return true;
  } else {
    return false;
  }
}

bool OtherRegionsTable::contains_reference(OopOrNarrowOopStar from) const {
  // Cast away const in this case.
  MutexLockerEx x((Mutex*)_m, Mutex::_no_safepoint_check_flag);
  return contains_reference_locked(from);
}

/**
 * 流程: 代码首先通过 coarse map 进行快速检查，如果没有找到相应记录，则查找更精细的区域表 (PerRegionTable) 或稀疏表 (sparse table)。
        目的: 这一系列检查确保了在垃圾回收过程中能够准确地找到和处理跨区域的引用。
        返回值: 如果引用 from 被记录在任何一个检查的表中，函数返回 true；否则返回 false。
 * @param from
 * @return
 */
bool OtherRegionsTable::contains_reference_locked(OopOrNarrowOopStar from) const {
  HeapRegion* hr = _g1h->heap_region_containing_raw(from);
  RegionIdx_t hr_ind = (RegionIdx_t) hr->hrm_index();
  // Is this region in the coarse map?
  if (_coarse_map.at(hr_ind)) return true;

  PerRegionTable* prt = find_region_table(hr_ind & _mod_max_fine_entries_mask,
                                     hr);
  if (prt != NULL) {
    return prt->contains_reference(from);

  } else {
    uintptr_t from_card =
      (uintptr_t(from) >> CardTableModRefBS::card_shift);
    uintptr_t hr_bot_card_index =
      uintptr_t(hr->bottom()) >> CardTableModRefBS::card_shift;
    assert(from_card >= hr_bot_card_index, "Inv");
    CardIdx_t card_index = from_card - hr_bot_card_index;
    assert(0 <= card_index && (size_t)card_index < HeapRegion::CardsPerRegion,
           "Must be in range.");
    return _sparse_table.contains_card(hr_ind, card_index);
  }
}

/**
 * OtherRegionsTable::do_cleanup_work会调用SparsePRT::do_cleanup_work方法
 */
void
OtherRegionsTable::do_cleanup_work(HRRSCleanupTask* hrrs_cleanup_task) {
  _sparse_table.do_cleanup_work(hrrs_cleanup_task);
}

// Determines how many threads can add records to an rset in parallel.
// This can be done by either mutator threads together with the
// concurrent refinement threads or GC threads.
uint HeapRegionRemSet::num_par_rem_sets() {
  return MAX2(DirtyCardQueueSet::num_par_ids() + ConcurrentG1Refine::thread_num(), (uint)ParallelGCThreads);
}

HeapRegionRemSet::HeapRegionRemSet(G1BlockOffsetSharedArray* bosa,
                                   HeapRegion* hr)
  : _bosa(bosa),
    _m(Mutex::leaf, FormatBuffer<128>("HeapRegionRemSet lock #%u", hr->hrm_index()), true),
    _code_roots(), _other_regions(hr, &_m), _iter_state(Unclaimed), _iter_claimed(0) {
  reset_for_par_iteration();
}

void HeapRegionRemSet::setup_remset_size() {
  // Setup sparse and fine-grain tables sizes.
  // table_size = base * (log(region_size / 1M) + 1)
  const int LOG_M = 20;
  int region_size_log_mb = MAX2(HeapRegion::LogOfHRGrainBytes - LOG_M, 0);
  if (FLAG_IS_DEFAULT(G1RSetSparseRegionEntries)) {
    G1RSetSparseRegionEntries = G1RSetSparseRegionEntriesBase * (region_size_log_mb + 1);
  }
  if (FLAG_IS_DEFAULT(G1RSetRegionEntries)) {
    G1RSetRegionEntries = G1RSetRegionEntriesBase * (region_size_log_mb + 1);
  }
  guarantee(G1RSetSparseRegionEntries > 0 && G1RSetRegionEntries > 0 , "Sanity");
}

bool HeapRegionRemSet::claim_iter() {
    /**
     * 检查迭代状态是否为 Unclaimed，如果不是，则直接返回 false，表示无法声明迭代权利。
     */
  if (_iter_state != Unclaimed) return false;
    // 如果迭代状态为 Unclaimed，则使用原子操作 Atomic::cmpxchg 将迭代状态从 Unclaimed 更改为 Claimed，并将返回值保存在变量 res 中。
  jint res = Atomic::cmpxchg(Claimed, (jint*)(&_iter_state), Unclaimed);
  return (res == Unclaimed); // 检查 res 是否等于 Unclaimed，如果等于，则说明成功将迭代状态从 Unclaimed 更改为 Claimed，返回 true，表示已成功声明迭代权利；否则，返回 false，表示无法声明迭代权利。
}

void HeapRegionRemSet::set_iter_complete() {
  _iter_state = Complete;
}

bool HeapRegionRemSet::iter_is_complete() {
  return _iter_state == Complete;
}

#ifndef PRODUCT
void HeapRegionRemSet::print() {
  HeapRegionRemSetIterator iter(this);
  size_t card_index;
  while (iter.has_next(card_index)) {
    HeapWord* card_start =
      G1CollectedHeap::heap()->bot_shared()->address_for_index(card_index);
    gclog_or_tty->print_cr("  Card " PTR_FORMAT, card_start);
  }
  if (iter.n_yielded() != occupied()) {
    gclog_or_tty->print_cr("Yielded disagrees with occupied:");
    gclog_or_tty->print_cr("  %6d yielded (%6d coarse, %6d fine).",
                  iter.n_yielded(),
                  iter.n_yielded_coarse(), iter.n_yielded_fine());
    gclog_or_tty->print_cr("  %6d occ     (%6d coarse, %6d fine).",
                  occupied(), occ_coarse(), occ_fine());
  }
  guarantee(iter.n_yielded() == occupied(),
            "We should have yielded all the represented cards.");
}
#endif

void HeapRegionRemSet::cleanup() {
  SparsePRT::cleanup_all();
}

void HeapRegionRemSet::clear() {
  MutexLockerEx x(&_m, Mutex::_no_safepoint_check_flag);
  clear_locked();
}

void HeapRegionRemSet::clear_locked() {
  _code_roots.clear();
  _other_regions.clear();
  assert(occupied_locked() == 0, "Should be clear.");
  reset_for_par_iteration();
}

void HeapRegionRemSet::reset_for_par_iteration() {
  _iter_state = Unclaimed;
  _iter_claimed = 0;
  // It's good to check this to make sure that the two methods are in sync.
  assert(verify_ready_for_par_iteration(), "post-condition");
}

/**
 * 在ScrubRSClosure的doHeapRegion()中被调用，用来清理当前HeapRegion的RSet
 *
 * @param ctbs
 * @param region_bm
 * @param card_bm
 */
void HeapRegionRemSet::scrub(CardTableModRefBS* ctbs,
                             BitMap* region_bm, BitMap* card_bm) {
  _other_regions.scrub(ctbs, region_bm, card_bm);// 查看OtherRegionsTable::scrub
}

// Code roots support
//
// The code root set is protected by two separate locking schemes
// When at safepoint the per-hrrs lock must be held during modifications
// except when doing a full gc.
// When not at safepoint the CodeCache_lock must be held during modifications.
// When concurrent readers access the contains() function
// (during the evacuation phase) no removals are allowed.

void HeapRegionRemSet::add_strong_code_root(nmethod* nm) {
  assert(nm != NULL, "sanity");
  assert((!CodeCache_lock->owned_by_self() || SafepointSynchronize::is_at_safepoint()),
          err_msg("should call add_strong_code_root_locked instead. CodeCache_lock->owned_by_self(): %s, is_at_safepoint(): %s",
                  BOOL_TO_STR(CodeCache_lock->owned_by_self()), BOOL_TO_STR(SafepointSynchronize::is_at_safepoint())));
  // Optimistic unlocked contains-check
  if (!_code_roots.contains(nm)) {
    MutexLockerEx ml(&_m, Mutex::_no_safepoint_check_flag);
    add_strong_code_root_locked(nm);
  }
}

void HeapRegionRemSet::add_strong_code_root_locked(nmethod* nm) {
  assert(nm != NULL, "sanity");
  assert((CodeCache_lock->owned_by_self() ||
         (SafepointSynchronize::is_at_safepoint() &&
          (_m.owned_by_self() || Thread::current()->is_VM_thread()))),
          err_msg("not safely locked. CodeCache_lock->owned_by_self(): %s, is_at_safepoint(): %s, _m.owned_by_self(): %s, Thread::current()->is_VM_thread(): %s",
                  BOOL_TO_STR(CodeCache_lock->owned_by_self()), BOOL_TO_STR(SafepointSynchronize::is_at_safepoint()),
                  BOOL_TO_STR(_m.owned_by_self()), BOOL_TO_STR(Thread::current()->is_VM_thread())));
  _code_roots.add(nm);
}

void HeapRegionRemSet::remove_strong_code_root(nmethod* nm) {
  assert(nm != NULL, "sanity");
  assert_locked_or_safepoint(CodeCache_lock);

  MutexLockerEx ml(CodeCache_lock->owned_by_self() ? NULL : &_m, Mutex::_no_safepoint_check_flag);
  _code_roots.remove(nm);

  // Check that there were no duplicates
  guarantee(!_code_roots.contains(nm), "duplicate entry found");
}

void HeapRegionRemSet::strong_code_roots_do(CodeBlobClosure* blk) const {
  _code_roots.nmethods_do(blk);
}

void HeapRegionRemSet::clean_strong_code_roots(HeapRegion* hr) {
  _code_roots.clean(hr);
}

size_t HeapRegionRemSet::strong_code_roots_mem_size() {
  return _code_roots.mem_size();
}

HeapRegionRemSetIterator:: HeapRegionRemSetIterator(HeapRegionRemSet* hrrs) :
  _hrrs(hrrs),
  _g1h(G1CollectedHeap::heap()),
  _coarse_map(&hrrs->_other_regions._coarse_map),
  _bosa(hrrs->bosa()),
  _is(Sparse), // 默认是稀疏索引
  // Set these values so that we increment to the first region.
  _coarse_cur_region_index(-1),
  _coarse_cur_region_cur_card(HeapRegion::CardsPerRegion-1),
  _cur_card_in_prt(HeapRegion::CardsPerRegion),
  _fine_cur_prt(NULL),
  _n_yielded_coarse(0),
  _n_yielded_fine(0),
  _n_yielded_sparse(0),
  _sparse_iter(&hrrs->_other_regions._sparse_table) {}

bool HeapRegionRemSetIterator::coarse_has_next(size_t& card_index) {
  if (_hrrs->_other_regions._n_coarse_entries == 0) return false;
  // Go to the next card.
  _coarse_cur_region_cur_card++;
  // Was the last the last card in the current region?
  if (_coarse_cur_region_cur_card == HeapRegion::CardsPerRegion) {
    // Yes: find the next region.  This may leave _coarse_cur_region_index
    // Set to the last index, in which case there are no more coarse
    // regions.
    _coarse_cur_region_index =
      (int) _coarse_map->get_next_one_offset(_coarse_cur_region_index + 1);
    if ((size_t)_coarse_cur_region_index < _coarse_map->size()) {
      _coarse_cur_region_cur_card = 0;
      HeapWord* r_bot =
        _g1h->region_at((uint) _coarse_cur_region_index)->bottom();
      _cur_region_card_offset = _bosa->index_for(r_bot);
    } else {
      return false;
    }
  }
  // If we didn't return false above, then we can yield a card.
  card_index = _cur_region_card_offset + _coarse_cur_region_cur_card;
  return true;
}

bool HeapRegionRemSetIterator::fine_has_next(size_t& card_index) {
  if (fine_has_next()) {
    _cur_card_in_prt =
      _fine_cur_prt->_bm.get_next_one_offset(_cur_card_in_prt + 1);
  }
  if (_cur_card_in_prt == HeapRegion::CardsPerRegion) {
    // _fine_cur_prt may still be NULL in case if there are not PRTs at all for
    // the remembered set.
    if (_fine_cur_prt == NULL || _fine_cur_prt->next() == NULL) {
      return false;
    }
    PerRegionTable* next_prt = _fine_cur_prt->next();
    switch_to_prt(next_prt);
    _cur_card_in_prt = _fine_cur_prt->_bm.get_next_one_offset(_cur_card_in_prt + 1);
  }

  card_index = _cur_region_card_offset + _cur_card_in_prt;
  guarantee(_cur_card_in_prt < HeapRegion::CardsPerRegion,
            err_msg("Card index " SIZE_FORMAT " must be within the region", _cur_card_in_prt));
  return true;
}

bool HeapRegionRemSetIterator::fine_has_next() {
  return _cur_card_in_prt != HeapRegion::CardsPerRegion;
}

void HeapRegionRemSetIterator::switch_to_prt(PerRegionTable* prt) {
  assert(prt != NULL, "Cannot switch to NULL prt");
  _fine_cur_prt = prt;

  HeapWord* r_bot = _fine_cur_prt->hr()->bottom();
  _cur_region_card_offset = _bosa->index_for(r_bot);

  // The bitmap scan for the PRT always scans from _cur_region_cur_card + 1.
  // To avoid special-casing this start case, and not miss the first bitmap
  // entry, initialize _cur_region_cur_card with -1 instead of 0.
  _cur_card_in_prt = (size_t)-1;
}

/**
 * 一个HeapRegionRemSetIterator在构造的时候已经和一个HeapRegionRemSet(一个HeapRegionRemSet对应了一个Region的转移专用记忆集合)绑定了，
 *      专门用来对这个HeapRegionRemSet进行遍历，遍历的时候每次调用has_next，
 *          都会将转移专用记忆集合中的一个条目中存放的card_index(引用这个Region的其他Region的卡片的索引值)放到card_index中
 */
bool HeapRegionRemSetIterator::has_next(size_t& card_index) {
  switch (_is) { // 从构造函数可以看到，默认是Sparse
  case Sparse: {
    if (_sparse_iter.has_next(card_index)) { // RSHashTableIter::has_next
      _n_yielded_sparse++;
      return true;
    }
    // Otherwise, deliberate fall-through
    _is = Fine;
    PerRegionTable* initial_fine_prt = _hrrs->_other_regions._first_all_fine_prts;
    if (initial_fine_prt != NULL) {
      switch_to_prt(_hrrs->_other_regions._first_all_fine_prts);
    }
  }
  case Fine:
    if (fine_has_next(card_index)) {
      _n_yielded_fine++;
      return true;
    }
    // Otherwise, deliberate fall-through
    _is = Coarse;
  case Coarse:
    if (coarse_has_next(card_index)) {
      _n_yielded_coarse++;
      return true;
    }
    // Otherwise...
    break;
  }
  assert(ParallelGCThreads > 1 ||
         n_yielded() == _hrrs->occupied(),
         "Should have yielded all the cards in the rem set "
         "(in the non-par case).");
  return false;
}



OopOrNarrowOopStar* HeapRegionRemSet::_recorded_oops = NULL;
HeapWord**          HeapRegionRemSet::_recorded_cards = NULL;
HeapRegion**        HeapRegionRemSet::_recorded_regions = NULL;
int                 HeapRegionRemSet::_n_recorded = 0;

HeapRegionRemSet::Event* HeapRegionRemSet::_recorded_events = NULL;
int*         HeapRegionRemSet::_recorded_event_index = NULL;
int          HeapRegionRemSet::_n_recorded_events = 0;

void HeapRegionRemSet::record(HeapRegion* hr, OopOrNarrowOopStar f) {
  if (_recorded_oops == NULL) {
    assert(_n_recorded == 0
           && _recorded_cards == NULL
           && _recorded_regions == NULL,
           "Inv");
    _recorded_oops    = NEW_C_HEAP_ARRAY(OopOrNarrowOopStar, MaxRecorded, mtGC);
    _recorded_cards   = NEW_C_HEAP_ARRAY(HeapWord*,          MaxRecorded, mtGC);
    _recorded_regions = NEW_C_HEAP_ARRAY(HeapRegion*,        MaxRecorded, mtGC);
  }
  if (_n_recorded == MaxRecorded) {
    gclog_or_tty->print_cr("Filled up 'recorded' (%d).", MaxRecorded);
  } else {
    _recorded_cards[_n_recorded] =
      (HeapWord*)align_size_down(uintptr_t(f),
                                 CardTableModRefBS::card_size);
    _recorded_oops[_n_recorded] = f;
    _recorded_regions[_n_recorded] = hr;
    _n_recorded++;
  }
}

void HeapRegionRemSet::record_event(Event evnt) {
  if (!G1RecordHRRSEvents) return;

  if (_recorded_events == NULL) {
    assert(_n_recorded_events == 0
           && _recorded_event_index == NULL,
           "Inv");
    _recorded_events = NEW_C_HEAP_ARRAY(Event, MaxRecordedEvents, mtGC);
    _recorded_event_index = NEW_C_HEAP_ARRAY(int, MaxRecordedEvents, mtGC);
  }
  if (_n_recorded_events == MaxRecordedEvents) {
    gclog_or_tty->print_cr("Filled up 'recorded_events' (%d).", MaxRecordedEvents);
  } else {
    _recorded_events[_n_recorded_events] = evnt;
    _recorded_event_index[_n_recorded_events] = _n_recorded;
    _n_recorded_events++;
  }
}

void HeapRegionRemSet::print_event(outputStream* str, Event evnt) {
  switch (evnt) {
  case Event_EvacStart:
    str->print("Evac Start");
    break;
  case Event_EvacEnd:
    str->print("Evac End");
    break;
  case Event_RSUpdateEnd:
    str->print("RS Update End");
    break;
  }
}

void HeapRegionRemSet::print_recorded() {
  int cur_evnt = 0;
  Event cur_evnt_kind = Event_illegal;
  int cur_evnt_ind = 0;
  if (_n_recorded_events > 0) {
    cur_evnt_kind = _recorded_events[cur_evnt];
    cur_evnt_ind = _recorded_event_index[cur_evnt];
  }

  for (int i = 0; i < _n_recorded; i++) {
    while (cur_evnt < _n_recorded_events && i == cur_evnt_ind) {
      gclog_or_tty->print("Event: ");
      print_event(gclog_or_tty, cur_evnt_kind);
      gclog_or_tty->cr();
      cur_evnt++;
      if (cur_evnt < MaxRecordedEvents) {
        cur_evnt_kind = _recorded_events[cur_evnt];
        cur_evnt_ind = _recorded_event_index[cur_evnt];
      }
    }
    gclog_or_tty->print("Added card " PTR_FORMAT " to region [" PTR_FORMAT "...]"
                        " for ref " PTR_FORMAT ".\n",
                        _recorded_cards[i], _recorded_regions[i]->bottom(),
                        _recorded_oops[i]);
  }
}

void HeapRegionRemSet::reset_for_cleanup_tasks() {
  SparsePRT::reset_for_cleanup_tasks();
}

/**
 * 搜索 hr->rem_set()->do_cleanup_work看到该方法的调用位置是在方法bool doHeapRegion(HeapRegion *hr)，
 * 调用了OtherRegionsTable::do_cleanup_work, 而 OtherRegionsTable::do_cleanup_work会调用SparkPRT::do_cleanup_work()
 * 实际上是将对应的SparsePRT加入当前的hrrs_cleanup_task的任务列表中去
 * @param hrrs_cleanup_task
 */
void HeapRegionRemSet::do_cleanup_work(HRRSCleanupTask* hrrs_cleanup_task) {
  _other_regions.do_cleanup_work(hrrs_cleanup_task);
}

void
HeapRegionRemSet::finish_cleanup_task(HRRSCleanupTask* hrrs_cleanup_task) {
  SparsePRT::finish_cleanup_task(hrrs_cleanup_task);
}

#ifndef PRODUCT
void PerRegionTable::test_fl_mem_size() {
  PerRegionTable* dummy = alloc(NULL);

  size_t min_prt_size = sizeof(void*) + dummy->bm()->size_in_words() * HeapWordSize;
  assert(dummy->mem_size() > min_prt_size,
         err_msg("PerRegionTable memory usage is suspiciously small, only has " SIZE_FORMAT " bytes. "
                 "Should be at least " SIZE_FORMAT " bytes.", dummy->mem_size(), min_prt_size));
  free(dummy);
  guarantee(dummy->mem_size() == fl_mem_size(), "fl_mem_size() does not return the correct element size");
  // try to reset the state
  _free_list = NULL;
  delete dummy;
}

void HeapRegionRemSet::test_prt() {
  PerRegionTable::test_fl_mem_size();
}

void HeapRegionRemSet::test() {
  os::sleep(Thread::current(), (jlong)5000, false);
  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  // Run with "-XX:G1LogRSetRegionEntries=2", so that 1 and 5 end up in same
  // hash bucket.
  HeapRegion* hr0 = g1h->region_at(0);
  HeapRegion* hr1 = g1h->region_at(1);
  HeapRegion* hr2 = g1h->region_at(5);
  HeapRegion* hr3 = g1h->region_at(6);
  HeapRegion* hr4 = g1h->region_at(7);
  HeapRegion* hr5 = g1h->region_at(8);

  HeapWord* hr1_start = hr1->bottom();
  HeapWord* hr1_mid = hr1_start + HeapRegion::GrainWords/2;
  HeapWord* hr1_last = hr1->end() - 1;

  HeapWord* hr2_start = hr2->bottom();
  HeapWord* hr2_mid = hr2_start + HeapRegion::GrainWords/2;
  HeapWord* hr2_last = hr2->end() - 1;

  HeapWord* hr3_start = hr3->bottom();
  HeapWord* hr3_mid = hr3_start + HeapRegion::GrainWords/2;
  HeapWord* hr3_last = hr3->end() - 1;

  HeapRegionRemSet* hrrs = hr0->rem_set();

  // Make three references from region 0x101...
  hrrs->add_reference((OopOrNarrowOopStar)hr1_start);
  hrrs->add_reference((OopOrNarrowOopStar)hr1_mid);
  hrrs->add_reference((OopOrNarrowOopStar)hr1_last);

  hrrs->add_reference((OopOrNarrowOopStar)hr2_start);
  hrrs->add_reference((OopOrNarrowOopStar)hr2_mid);
  hrrs->add_reference((OopOrNarrowOopStar)hr2_last);

  hrrs->add_reference((OopOrNarrowOopStar)hr3_start);
  hrrs->add_reference((OopOrNarrowOopStar)hr3_mid);
  hrrs->add_reference((OopOrNarrowOopStar)hr3_last);

  // Now cause a coarsening.
  hrrs->add_reference((OopOrNarrowOopStar)hr4->bottom());
  hrrs->add_reference((OopOrNarrowOopStar)hr5->bottom());

  // Now, does iteration yield these three?
  HeapRegionRemSetIterator iter(hrrs);
  size_t sum = 0;
  size_t card_index;
  while (iter.has_next(card_index)) {
    HeapWord* card_start =
      G1CollectedHeap::heap()->bot_shared()->address_for_index(card_index);
    gclog_or_tty->print_cr("  Card " PTR_FORMAT ".", card_start);
    sum++;
  }
  guarantee(sum == 11 - 3 + 2048, "Failure");
  guarantee(sum == hrrs->occupied(), "Failure");
}
#endif
