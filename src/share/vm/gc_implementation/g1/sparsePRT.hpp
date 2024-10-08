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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_SPARSEPRT_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_SPARSEPRT_HPP

#include "gc_implementation/g1/g1CollectedHeap.hpp"
#include "gc_implementation/g1/heapRegion.hpp"
#include "memory/allocation.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "runtime/mutex.hpp"
#include "utilities/globalDefinitions.hpp"

// Sparse remembered set for a heap region (the "owning" region).  Maps
// indices of other regions to short sequences of cards in the other region
// that might contain pointers into the owner region.

// These tables only expand while they are accessed in parallel --
// deletions may be done in single-threaded code.  This allows us to allow
// unsynchronized reads/iterations, as long as expansions caused by
// insertions only enqueue old versions for deletions, but do not delete
// old versions synchronously.
/**
 * 一个对当前Region(Owning Region)的稀疏PRT的RSHashTable的Entry。
 *  我们通过方法 RSHashTable::add_card 可以看到，一个SparsePRTEntry其实对应了一个外部指过来的Region_index，
 *      而一个外部的region可能有多个对应的卡片指向了当前的owning region，因此，卡片索引存放在了_cards[]中，
 *          我们通过方法SparsePRTEntry::AddCardResult SparsePRTEntry::add_card可以看到向这个SparsePRTEntry中添加卡片索引的过程
 * 查看RSHashTable::add_card 了解向稀疏PRT中添加对象的逻辑
 *
 * 这个映射关系的key是其他Region的卡片索引，映射关系的value是其他区域的一系列卡片，这一系列卡片指向了当前的Region(Owning Region)
   这些表仅在并行访问时才会扩展——删除可以在单线程代码中完成。 这允许我们允许不同步的读取/迭代，只要插入引起的扩展仅将旧版本排入队列进行删除，但不同步删除旧版本。
 */
class SparsePRTEntry: public CHeapObj<mtGC> {
public:
  enum SomePublicConstants {
    NullEntry     = -1,
    UnrollFactor  =  4
  };
private:
  // typedef int RegionIdx_t;   // needs to hold [ 0..max_regions() )
  RegionIdx_t _region_ind; // 当前的这个SparsePRTEntry对应的Region的索引
  int         _next_index; //  这个SparsePRTEntry在对应的RSHashTable的entries中的下一个索引值
  // typedef int CardIdx_t;     // needs to hold [ 0..CardsPerRegion )
  CardIdx_t   _cards[1]; // 当前的这个SparsePRTEntry对应的卡片索引的列表
  // WARNING: Don't put any data members beyond this line. Card array has, in fact, variable length.
  // It should always be the last data member.
public:
  // Returns the size of the entry, used for entry allocation.
  // 静态方法，返回整个 SparsePRTEntry 的实际大小，考虑到了可变数组_cards的实际大小，但是cards_num()也是静态方法，这意味着SparsePRTEntry类的所有对象的cards_num()都相同
  static size_t size() { return sizeof(SparsePRTEntry) + sizeof(CardIdx_t) * (cards_num() - 1); }
  // Returns the size of the card array.
  // 静态方法，返回卡片数量
  static int cards_num() {
    // The number of cards should be a multiple of 4, because that's our current
    // unrolling factor.
    /**
     * 经过~(UnrollFactor - 1)，变成了低位为0、高位为1的数，然后通过G1RSetSparseRegionEntries进行与操作，变成了大小肯定是4的整数倍的数字
     */
    static const int s = MAX2<int>(G1RSetSparseRegionEntries & ~(UnrollFactor - 1), UnrollFactor);
    return s;
  }

  // Set the region_ind to the given value, and delete all cards.
  inline void init(RegionIdx_t region_ind);

  RegionIdx_t r_ind() const { return _region_ind; }
  bool valid_entry() const { return r_ind() >= 0; }
  void set_r_ind(RegionIdx_t rind) { _region_ind = rind; }

  int next_index() const { return _next_index; }
  int* next_index_addr() { return &_next_index; }
  void set_next_index(int ni) { _next_index = ni; }

  // Returns "true" iff the entry contains the given card index.
  inline bool contains_card(CardIdx_t card_index) const;

  // Returns the number of non-NULL card entries.
  inline int num_valid_cards() const;

  // Requires that the entry not contain the given card index.  If there is
  // space available, add the given card index to the entry and return
  // "true"; otherwise, return "false" to indicate that the entry is full.
  enum AddCardResult {
    overflow,
    found,
    added
  };
  inline AddCardResult add_card(CardIdx_t card_index); // add_card是非常频繁的操作，因此定义为内联函数

  // Copy the current entry's cards into "cards".
  inline void copy_cards(CardIdx_t* cards) const;
  // Copy the current entry's cards into the "_card" array of "e."
  inline void copy_cards(SparsePRTEntry* e) const;

  inline CardIdx_t card(int i) const { return _cards[i]; }
};

class RSHashTable : public CHeapObj<mtGC> {

  friend class RSHashTableIter;

  enum SomePrivateConstants {
    NullEntry = -1
  };

  size_t _capacity;
  size_t _capacity_mask;
  size_t _occupied_entries; // entry的数量，一个entry是一个SparsePRTEntry对象，在RSHashTable中代表了一个外部region对当前region的引用关系。一个SparsePRTEntry会挂载很多的cards
  size_t _occupied_cards; // cards的数量

  SparsePRTEntry* _entries;
  int* _buckets;  //这个链地址法的桶结构，桶中存放的是一个index，这是这个桶下面的第一个SparsePRTEntry在_entries中的偏移量
  int  _free_region; // 当前第一个连续空闲的SparsePRTEntry区域的index
  int  _free_list; // 空闲的SparsePRTEntry链表的第一个SparsePRTEntry节点在_entries中的索引值

  // Requires that the caller hold a lock preventing parallel modifying
  // operations, and that the the table be less than completely full.  If
  // an entry for "region_ind" is already in the table, finds it and
  // returns its address; otherwise returns "NULL."
  SparsePRTEntry* entry_for_region_ind(RegionIdx_t region_ind) const;

  // Requires that the caller hold a lock preventing parallel modifying
  // operations, and that the the table be less than completely full.  If
  // an entry for "region_ind" is already in the table, finds it and
  // returns its address; otherwise allocates, initializes, inserts and
  // returns a new entry for "region_ind".
  SparsePRTEntry* entry_for_region_ind_create(RegionIdx_t region_ind);

  // Returns the index of the next free entry in "_entries".
  int alloc_entry();
  // Declares the entry "fi" to be free.  (It must have already been
  // deleted from any bucket lists.
  void free_entry(int fi);

public:
  RSHashTable(size_t capacity);
  ~RSHashTable();

  // Attempts to ensure that the given card_index in the given region is in
  // the sparse table.  If successful (because the card was already
  // present, or because it was successfullly added) returns "true".
  // Otherwise, returns "false" to indicate that the addition would
  // overflow the entry for the region.  The caller must transfer these
  // entries to a larger-capacity representation.
  bool add_card(RegionIdx_t region_id, CardIdx_t card_index);

  bool get_cards(RegionIdx_t region_id, CardIdx_t* cards);

  bool delete_entry(RegionIdx_t region_id);

  bool contains_card(RegionIdx_t region_id, CardIdx_t card_index) const;

  void add_entry(SparsePRTEntry* e);

  // 具体实现搜索 SparsePRTEntry* RSHashTable::get_entry(RegionIdx_t region_ind)
  SparsePRTEntry* get_entry(RegionIdx_t region_id);

  void clear();

  size_t capacity() const      { return _capacity;       }
  size_t capacity_mask() const { return _capacity_mask;  }
  size_t occupied_entries() const { return _occupied_entries; }
  size_t occupied_cards() const   { return _occupied_cards;   } // void RSHashTable::add_entry(SparsePRTEntry* e) {
  size_t mem_size() const;
  // SparsePRTEntry::size()返回的是一个SparsePRTEntry对象所占用的内存大小(字节数)
  // 根据输入的索引值i，返回这个RSHashTable的第i个SparsePRTEntry的对象指针
  SparsePRTEntry* entry(int i) const { return (SparsePRTEntry*)((char*)_entries + SparsePRTEntry::size() * i); }

  void print();
};

// ValueObj because will be embedded in HRRS iterator.
class RSHashTableIter VALUE_OBJ_CLASS_SPEC {
  int _tbl_ind;         // [-1, 0.._rsht->_capacity)
  int _bl_ind;          // [-1, 0.._rsht->_capacity)
  short _card_ind;      // [0..SparsePRTEntry::cards_num())
  RSHashTable* _rsht;

  // If the bucket list pointed to by _bl_ind contains a card, sets
  // _bl_ind to the index of that entry, and returns the card.
  // Otherwise, returns SparseEntry::NullEntry.
  CardIdx_t find_first_card_in_list();

  // Computes the proper card index for the card whose offset in the
  // current region (as indicated by _bl_ind) is "ci".
  // This is subject to errors when there is iteration concurrent with
  // modification, but these errors should be benign.
  size_t compute_card_ind(CardIdx_t ci);

public:
  RSHashTableIter(RSHashTable* rsht) :
    _tbl_ind(RSHashTable::NullEntry), // So that first increment gets to 0.
    _bl_ind(RSHashTable::NullEntry),
    _card_ind((SparsePRTEntry::cards_num() - 1)),
    _rsht(rsht) {}

  bool has_next(size_t& card_index);
};

// Concurrent accesss to a SparsePRT must be serialized by some external
// mutex.

class SparsePRTIter;
class SparsePRTCleanupTask;

class SparsePRT VALUE_OBJ_CLASS_SPEC {
  friend class SparsePRTCleanupTask;

  //  Iterations are done on the _cur hash table, since they only need to
  //  see entries visible at the start of a collection pause.
  //  All other operations are done using the _next hash table.
  RSHashTable* _cur; // 搜索class RSHashTable获取它的定义，它的key是SparsePRTEntry
  RSHashTable* _next; // 扩展以后的RSHashTable

  HeapRegion* _hr; // 反向引用，当前SparsePRT对应的HeapRegion

  enum SomeAdditionalPrivateConstants {
    InitialCapacity = 16 // 稀疏表的RSHashTable _next的初始大小
  };

  void expand();

  bool _expanded;

  bool expanded() { return _expanded; }
  void set_expanded(bool b) { _expanded = b; }

  SparsePRT* _next_expanded;

  SparsePRT* next_expanded() { return _next_expanded; }
  void set_next_expanded(SparsePRT* nxt) { _next_expanded = nxt; }

  bool should_be_on_expanded_list();
  // 注意，_head_expanded_list是一个静态变量，即全局只有一个， 并不属于一个SparsePRT对象
  static SparsePRT* _head_expanded_list;

public:
  SparsePRT(HeapRegion* hr);

  ~SparsePRT();

  size_t occupied() const { return _next->occupied_cards(); } // 所以，这里是统计对应的RSHashTable中总的card的数量，而不是entry的数量
  size_t mem_size() const;

  // Attempts to ensure that the given card_index in the given region is in
  // the sparse table.  If successful (because the card was already
  // present, or because it was successfullly added) returns "true".
  // Otherwise, returns "false" to indicate that the addition would
  // overflow the entry for the region.  The caller must transfer these
  // entries to a larger-capacity representation.
  bool add_card(RegionIdx_t region_id, CardIdx_t card_index);

  // If the table hold an entry for "region_ind",  Copies its
  // cards into "cards", which must be an array of length at least
  // "SparePRTEntry::cards_num()", and returns "true"; otherwise,
  // returns "false".
  bool get_cards(RegionIdx_t region_ind, CardIdx_t* cards);

  // Return the pointer to the entry associated with the given region.
  SparsePRTEntry* get_entry(RegionIdx_t region_ind);

  // If there is an entry for "region_ind", removes it and return "true";
  // otherwise returns "false."
  bool delete_entry(RegionIdx_t region_ind);

  // Clear the table, and reinitialize to initial capacity.
  void clear();

  // Ensure that "_cur" and "_next" point to the same table.
  void cleanup();

  // Clean up all tables on the expanded list.  Called single threaded.
  static void cleanup_all();
  RSHashTable* cur() const { return _cur; }

  static void add_to_expanded_list(SparsePRT* sprt);
  static SparsePRT* get_from_expanded_list();

  // The purpose of these three methods is to help the GC workers
  // during the cleanup pause to recreate the expanded list, purging
  // any tables from it that belong to regions that are freed during
  // cleanup (if we don't purge those tables, there is a race that
  // causes various crashes; see CR 7014261).
  //
  // We chose to recreate the expanded list, instead of purging
  // entries from it by iterating over it, to avoid this serial phase
  // at the end of the cleanup pause.
  //
  // The three methods below work as follows:
  // * reset_for_cleanup_tasks() : Nulls the expanded list head at the
  //   start of the cleanup pause.
  // * do_cleanup_work() : Called by the cleanup workers for every
  //   region that is not free / is being freed by the cleanup
  //   pause. It creates a list of expanded tables whose head / tail
  //   are on the thread-local SparsePRTCleanupTask object.
  // * finish_cleanup_task() : Called by the cleanup workers after
  //   they complete their cleanup task. It adds the local list into
  //   the global expanded list. It assumes that the
  //   ParGCRareEvent_lock is being held to ensure MT-safety.
  /**
   * 方法解释
reset_for_cleanup_tasks():
    作用: 在清理暂停开始时调用，将全局扩展列表的头指针设置为 NULL。
    目的: 清空全局扩展列表，以便在清理过程中重新创建它。这样可以避免在清理期间因区域释放而导致的竞争条件。

do_cleanup_work(SparsePRTCleanupTask* sprt_cleanup_task):
    作用: 由清理线程调用，处理每个未被释放或正在被释放的区域。这会将这些区域对应的稀疏表对象添加到线程本地的 SparsePRTCleanupTask 对象中。
    目的: 创建一个包含所有扩展表的本地列表，这些表在当前清理任务中仍然有效。这样做的目的是避免在清理过程中对全局扩展列表进行修改，从而避免竞争条件。
finish_cleanup_task(SparsePRTCleanupTask* sprt_cleanup_task):
    作用: 在清理线程完成清理任务后调用，将本地列表中的扩展表添加到全局扩展列表中。
    目的: 将本地列表中的表合并到全局扩展列表中。为了确保多线程安全，必须在持有 ParGCRareEvent_lock 锁的情况下进行合并。
总结
    reset_for_cleanup_tasks(): 清空全局扩展列表的头指针，为清理过程做准备。
    do_cleanup_work(SparsePRTCleanupTask* sprt_cleanup_task): 处理每个待清理区域，将其对应的稀疏表添加到线程本地的清理任务列表中。
    finish_cleanup_task(SparsePRTCleanupTask* sprt_cleanup_task): 将线程本地的清理任务列表合并到全局扩展列表中，以确保全局扩展列表的正确性和一致性。
   */
  static void reset_for_cleanup_tasks();
  void do_cleanup_work(SparsePRTCleanupTask* sprt_cleanup_task);
  static void finish_cleanup_task(SparsePRTCleanupTask* sprt_cleanup_task);

  bool contains_card(RegionIdx_t region_id, CardIdx_t card_index) const {
    return _next->contains_card(region_id, card_index);
  }
};

class SparsePRTIter: public RSHashTableIter {
public:
  SparsePRTIter(const SparsePRT* sprt) :
    RSHashTableIter(sprt->cur()) {}

  bool has_next(size_t& card_index) {
    return RSHashTableIter::has_next(card_index);
  }
};

// This allows each worker during a cleanup pause to create a
// thread-local list of sparse tables that have been expanded and need
// to be processed at the beginning of the next GC pause. This lists
// are concatenated into the single expanded list at the end of the
// cleanup pause.
class SparsePRTCleanupTask VALUE_OBJ_CLASS_SPEC {
private:
  SparsePRT* _head;
  SparsePRT* _tail;

public:
  SparsePRTCleanupTask() : _head(NULL), _tail(NULL) { }

  void add(SparsePRT* sprt);  //  实现void SparsePRTCleanupTask::add(SparsePRT* sprt)
  SparsePRT* head() { return _head; }
  SparsePRT* tail() { return _tail; }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_SPARSEPRT_HPP
