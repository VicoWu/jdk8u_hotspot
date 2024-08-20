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
#include "gc_implementation/g1/heapRegion.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "gc_implementation/g1/sparsePRT.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "memory/space.inline.hpp"
#include "runtime/mutexLocker.hpp"

#define SPARSE_PRT_VERBOSE 0

#define UNROLL_CARD_LOOPS  1

void SparsePRTEntry::init(RegionIdx_t region_ind) {
  _region_ind = region_ind;
  _next_index = NullEntry;

#if UNROLL_CARD_LOOPS
  assert((cards_num() & (UnrollFactor - 1)) == 0, "Invalid number of cards in the entry");
  for (int i = 0; i < cards_num(); i += UnrollFactor) {
    _cards[i] = NullEntry;
    _cards[i + 1] = NullEntry;
    _cards[i + 2] = NullEntry;
    _cards[i + 3] = NullEntry;
  }
#else
  for (int i = 0; i < cards_num(); i++)
    _cards[i] = NullEntry;
#endif
}

/**
 * 判断当前的SparsePRTEntry对象(前面说过，一个SparsePRTEntry对象对应了一个指向当前owning region的region)是否含有某个卡片
 */
bool SparsePRTEntry::contains_card(CardIdx_t card_index) const {
#if UNROLL_CARD_LOOPS
  assert((cards_num() & (UnrollFactor - 1)) == 0, "Invalid number of cards in the entry");
  for (int i = 0; i < cards_num(); i += UnrollFactor) {
    if (_cards[i] == card_index ||
        _cards[i + 1] == card_index ||
        _cards[i + 2] == card_index ||
        _cards[i + 3] == card_index) return true;
  }
#else
  for (int i = 0; i < cards_num(); i++) {
    if (_cards[i] == card_index) return true; // 在一个SparsePRTEntry中遍历每一个卡片，看看是否等于card_index
  }
#endif
  // Otherwise, we're full.
  return false;
}

int SparsePRTEntry::num_valid_cards() const {
  int sum = 0;
#if UNROLL_CARD_LOOPS
  assert((cards_num() & (UnrollFactor - 1)) == 0, "Invalid number of cards in the entry");
  for (int i = 0; i < cards_num(); i += UnrollFactor) {
    sum += (_cards[i] != NullEntry);
    sum += (_cards[i + 1] != NullEntry);
    sum += (_cards[i + 2] != NullEntry);
    sum += (_cards[i + 3] != NullEntry);
  }
#else
  for (int i = 0; i < cards_num(); i++) {
    sum += (_cards[i] != NullEntry);
  }
#endif
  // Otherwise, we're full.
  return sum;
}

/**
 * 向这个SparsePRTEntry中添加对应的卡片，调用者是 RSHashTable::add_card
 * 这段代码的目的是在 SparsePRTEntry 对象中添加卡片索引。通过使用展开的方式遍历，可以在一次循环中处理多个元素，从而提高了性能。
 * 如果数组足够大，且展开因子合适，展开方式可能会更加高效。而非展开方式遍历则更为简单，适用于较小的数组或展开因子较小的情况。
 * @param card_index 卡片在卡表中的索引
 * @return 返回 SparsePRTEntry::AddCardResult，包括found, added，以及overflow
 */
SparsePRTEntry::AddCardResult SparsePRTEntry::add_card(CardIdx_t card_index) {
#if UNROLL_CARD_LOOPS // 通过展开的方式遍历
  assert((cards_num() & (UnrollFactor - 1)) == 0, "Invalid number of cards in the entry");
  CardIdx_t c;
  /**
   * cards_num()返回的当前的这个SparsePRTEntry对象的_cards数组的长度，
   * UnrollFactor(展开因子)等于4，即我们四个为一组进行一轮查找
   *
   */
  for (int i = 0; i < cards_num(); i += UnrollFactor) {
    c = _cards[i];
    if (c == card_index) return found; //找到，已经存在，直接返回
    if (c == NullEntry) { _cards[i] = card_index; return added; }// 发现了一个空位置，将这个卡片索引插入到这个空位置，然后返回added，表示已经添加成功

    c = _cards[i + 1];
    if (c == card_index) return found;
    if (c == NullEntry) { _cards[i + 1] = card_index; return added; }

    c = _cards[i + 2];
    if (c == card_index) return found;
    if (c == NullEntry) { _cards[i + 2] = card_index; return added; }

    c = _cards[i + 3];
    if (c == card_index) return found;
    if (c == NullEntry) { _cards[i + 3] = card_index; return added; }
  }
#else // 通过非展开的方式遍历
  for (int i = 0; i < cards_num(); i++) {
    CardIdx_t c = _cards[i];
    if (c == card_index) return found;
    if (c == NullEntry) { _cards[i] = card_index; return added; }
  }
#endif
  // Otherwise, we're full.
  return overflow; // 遍历了当前的SparsePRTEntry对象的所有位置，既没有找到，也没有看到空位置，返回overflow
}

void SparsePRTEntry::copy_cards(CardIdx_t* cards) const {
#if UNROLL_CARD_LOOPS
  assert((cards_num() & (UnrollFactor - 1)) == 0, "Invalid number of cards in the entry");
  for (int i = 0; i < cards_num(); i += UnrollFactor) {
    cards[i] = _cards[i];
    cards[i + 1] = _cards[i + 1];
    cards[i + 2] = _cards[i + 2];
    cards[i + 3] = _cards[i + 3];
  }
#else
  for (int i = 0; i < cards_num(); i++) {
    cards[i] = _cards[i];
  }
#endif
}

void SparsePRTEntry::copy_cards(SparsePRTEntry* e) const {
  copy_cards(&e->_cards[0]);
}

// ----------------------------------------------------------------------

RSHashTable::RSHashTable(size_t capacity) : // 大小默认是16
  _capacity(capacity), _capacity_mask(capacity-1),
  _occupied_entries(0), _occupied_cards(0),
  _entries((SparsePRTEntry*)NEW_C_HEAP_ARRAY(char, SparsePRTEntry::size() * capacity, mtGC)),
  _buckets(NEW_C_HEAP_ARRAY(int, capacity, mtGC)), // 一个大小为16的数组
  _free_list(NullEntry), _free_region(0)
{
  clear();
}

RSHashTable::~RSHashTable() {
  if (_entries != NULL) {
    FREE_C_HEAP_ARRAY(SparsePRTEntry, _entries, mtGC);
    _entries = NULL;
  }
  if (_buckets != NULL) {
    FREE_C_HEAP_ARRAY(int, _buckets, mtGC);
    _buckets = NULL;
  }
}

void RSHashTable::clear() {
  _occupied_entries = 0;
  _occupied_cards = 0;
  guarantee(_entries != NULL, "INV");
  guarantee(_buckets != NULL, "INV");

  guarantee(_capacity <= ((size_t)1 << (sizeof(int)*BitsPerByte-1)) - 1,
                "_capacity too large");

  // This will put -1 == NullEntry in the key field of all entries.
  memset(_entries, NullEntry, _capacity * SparsePRTEntry::size());
  memset(_buckets, NullEntry, _capacity * sizeof(int));
  _free_list = NullEntry;
  _free_region = 0;
}

/**
 * 向SparkPRT的RSHashTable对象中添加卡片
 * 调用者是从SparsePRT中来的，查看调用者方法 bool SparsePRT::add_card
 * @param region_ind  region的索引
 * @param card_index 卡片对应的卡表索引
 * @return
 */
bool RSHashTable::add_card(RegionIdx_t region_ind, CardIdx_t card_index) {
  // 根据region_ind获取或者创建对应的HashMap的SparsePRTEntry,所以，返回值e不应该是null
  // 一个 SparsePRTEntry 对应了一个HeapRegion，而一个SparsePRTEntry显然含有这个HeapRegion的多个卡片
  SparsePRTEntry* e = entry_for_region_ind_create(region_ind);
  assert(e != NULL && e->r_ind() == region_ind,
         "Postcondition of call above.");
  /**
   * 向Entry中添加对应的卡片索引。可以看到，当前owning region可能被某个region中的多个卡片所指
   *    (比如a.field = c,b.field = c， c是当前Region, a和b属于同一个Region的不同卡片)，
   *    因此，通过region_ind获取SparsePRTEntry，然后将对应的索引添加到这个entry中。这就如果一个链地址法
   */
  SparsePRTEntry::AddCardResult res = e->add_card(card_index); // 向这个SparsePRTEntry中添加对应的卡片索引
  if (res == SparsePRTEntry::added) _occupied_cards++; //返回了added，因此添加计数加1
#if SPARSE_PRT_VERBOSE
  gclog_or_tty->print_cr("       after add_card[%d]: valid-cards = %d.",
                         pointer_delta(e, _entries, SparsePRTEntry::size()),
                         e->num_valid_cards());
#endif
  assert(e->num_valid_cards() > 0, "Postcondition");
  return res != SparsePRTEntry::overflow; //只要不是返回overflow(可以是found，可以是added)，就返回成功
}

bool RSHashTable::get_cards(RegionIdx_t region_ind, CardIdx_t* cards) {
  int ind = (int) (region_ind & capacity_mask()); // 取模，模的大小是16
  int cur_ind = _buckets[ind];
  SparsePRTEntry* cur; // 在数组下面，挂载了
  while (cur_ind != NullEntry &&
         (cur = entry(cur_ind))->r_ind() != region_ind) {
    cur_ind = cur->next_index();
  }

  if (cur_ind == NullEntry) return false;
  // Otherwise...
  assert(cur->r_ind() == region_ind, "Postcondition of loop + test above.");
  assert(cur->num_valid_cards() > 0, "Inv");
  cur->copy_cards(cards);
  return true;
}

SparsePRTEntry* RSHashTable::get_entry(RegionIdx_t region_ind) {
  int ind = (int) (region_ind & capacity_mask());
  int cur_ind = _buckets[ind];
  SparsePRTEntry* cur;
  while (cur_ind != NullEntry &&
         (cur = entry(cur_ind))->r_ind() != region_ind) {
    cur_ind = cur->next_index();
  }

  if (cur_ind == NullEntry) return NULL;
  // Otherwise...
  assert(cur->r_ind() == region_ind, "Postcondition of loop + test above.");
  assert(cur->num_valid_cards() > 0, "Inv");
  return cur;
}

bool RSHashTable::delete_entry(RegionIdx_t region_ind) {
  int ind = (int) (region_ind & capacity_mask());
  int* prev_loc = &_buckets[ind];
  int cur_ind = *prev_loc;
  SparsePRTEntry* cur;
  while (cur_ind != NullEntry &&
         (cur = entry(cur_ind))->r_ind() != region_ind) {
    prev_loc = cur->next_index_addr();
    cur_ind = *prev_loc;
  }

  if (cur_ind == NullEntry) return false;
  // Otherwise, splice out "cur".
  *prev_loc = cur->next_index();
  _occupied_cards -= cur->num_valid_cards();
  free_entry(cur_ind);
  _occupied_entries--;
  return true;
}

/**
 * 查找From region的region_ind对应的SparsePRTEntry(还没有到进一步查找card的那一步)
 * @param region_ind
 * @return
 */
SparsePRTEntry*
RSHashTable::entry_for_region_ind(RegionIdx_t region_ind) const {
  assert(occupied_entries() < capacity(), "Precondition");
  int ind = (int) (region_ind & capacity_mask());// 取模
  int cur_ind = _buckets[ind];  // 获取这个桶中存放的
  SparsePRTEntry* cur;
  // 关于entry()，搜索 SparsePRTEntry* entry(int i)
  while (cur_ind != NullEntry &&
         (cur = entry(cur_ind))->r_ind() != region_ind) {  // entry(cur_ind)返回RSHashTable的entries中的第cur_ind个 SparsePRTEntry，然后看一下这个 SparsePRTEntry 的RegionID是否就是要查找的ID
      // 如果不是，那么就通过SparsePRTEntry的next_index往下链接
    cur_ind = cur->next_index();
  }

  if (cur_ind != NullEntry) {
    assert(cur->r_ind() == region_ind, "Loop postcondition + test");
    return cur;
  } else {
    return NULL;
  }
}


SparsePRTEntry*
RSHashTable::entry_for_region_ind_create(RegionIdx_t region_ind) {
  SparsePRTEntry* res = entry_for_region_ind(region_ind); // 找到这个region对应的SparsePRTEntry，搜 RSHashTable::entry_for_region_ind
  if (res == NULL) { // 如果没有找到，就尝试创建一个entry
    int new_ind = alloc_entry();
    assert(0 <= new_ind && (size_t)new_ind < capacity(), "There should be room.");
    res = entry(new_ind); // 搜索 SparsePRTEntry* entry(int i)
    res->init(region_ind); // 为这个Region初始化这个SparsePRTEntry
    // Insert at front.
    int ind = (int) (region_ind & capacity_mask()); // 获取这个region对应的桶的index
    res->set_next_index(_buckets[ind]); // 设置自己的下一个SparsePRTEntry在entry中的索引
    _buckets[ind] = new_ind; // 更新这个桶下面挂载的第一个 SparsePRTEntry 的index，获取了这个SparsePRTEntry，就可以通过它的next指针获取这个bucket下面所有的SparsePRTEntry
    _occupied_entries++; // entry数量加1.注意，是etnries的数量加1，不是cards的数量+1， cards数量+1是在方法add_entry中进行的
  }
  return res;
}

int RSHashTable::alloc_entry() {
  int res;
  // 随着不断分配的进行，整个RSHashTable的usage越来越大，随着释放的进行，空闲列表也越来越多，很多分配都开始从空闲列表中进行
  if (_free_list != NullEntry) { // _free_list大表当前entry()中的空闲条目的索引值
    res = _free_list;
    _free_list = entry(res)->next_index(); // 更新_free_list
    return res; // 返回entry中的空闲条目索引
  } else if ((size_t) _free_region+1 < capacity()) { // 没有free节点的时候，那就自行往后递增，当_free_region耗尽的时候，只要capacity()是够用的，那么基本上都会从_free_list中取到
    res = _free_region;
    _free_region++;
    return res;
  } else {
    return NullEntry; // 没找到
  }
}

/**
 * 当且仅当释放一个entry的时候，_free_list才会不为空
 * @param fi
 */
void RSHashTable::free_entry(int fi) {
  entry(fi)->set_next_index(_free_list); //待释放的这个SparsePRT的next_index设置为前一个_free_list，相当于维护了一个free_list链表，释放的时候是把最新的释放节点添加到链表头部
  _free_list = fi; //设置为头结点
}

void RSHashTable::add_entry(SparsePRTEntry* e) {
  assert(e->num_valid_cards() > 0, "Precondition.");
  SparsePRTEntry* e2 = entry_for_region_ind_create(e->r_ind());
  e->copy_cards(e2);
  _occupied_cards += e2->num_valid_cards(); // 更新关于cards数量的统计信息
  assert(e2->num_valid_cards() > 0, "Postcondition.");
}

/**
 * 以当前的_bl_ind为起点，遍历当前的冲突列表，只要找到的SparsePRTEntry含有卡片，那么就返回卡片索引
 * @return
 */
CardIdx_t RSHashTableIter::find_first_card_in_list() {
  CardIdx_t res;
  while (_bl_ind != RSHashTable::NullEntry) {
    res = _rsht->entry(_bl_ind)->card(0); // 找到这个Entry的第一张卡片
    if (res != SparsePRTEntry::NullEntry) {
      return res;
    } else {
      _bl_ind = _rsht->entry(_bl_ind)->next_index(); // 冲突链表的下一个SparsePRTEntry
    }
  }
  // Otherwise, none found:
  return SparsePRTEntry::NullEntry;
}

size_t RSHashTableIter::compute_card_ind(CardIdx_t ci) {
  return (_rsht->entry(_bl_ind)->r_ind() * HeapRegion::CardsPerRegion) + ci; //  根据卡片的局部索引，获取全局索引
}

bool RSHashTableIter::has_next(size_t& card_index) {
  _card_ind++; // 递增当前卡片的索引（_card_ind），代表开始检查当前的SparsePRTEntry的下一个卡片
  CardIdx_t ci;
  if (_card_ind < SparsePRTEntry::cards_num() &&
      ((ci = _rsht->entry(_bl_ind)->card(_card_ind)) !=
       SparsePRTEntry::NullEntry)) { // 当前的 卡片直接存在
    card_index = compute_card_ind(ci); // 计算对应卡片的全局索引值，而不是这个卡片在这个Region的相对索引值
    return true;
  }
  // 当前的SparsePRTEntry已经没有下一张卡片，尝试查看下一个可用的SparsePRTEntry
  // Otherwise, must find the next valid entry.
  _card_ind = 0;

  if (_bl_ind != RSHashTable::NullEntry) {
      _bl_ind = _rsht->entry(_bl_ind)->next_index(); // 找到冲突链表的下一个SparsePRTEntry对应的index
      ci = find_first_card_in_list(); // 以_bl_ind为起点，沿着当前的冲突链表的下一个位置查找
      if (ci != SparsePRTEntry::NullEntry) {
        card_index = compute_card_ind(ci); // 计算对应卡片的全局索引值，而不是这个卡片在这个Region的相对索引值
        return true;
      }
  }

  // If we didn't return above, must go to the next non-null table index.
  _tbl_ind++; // 尝试下一个bucket
  while ((size_t)_tbl_ind < _rsht->capacity()) {
    _bl_ind = _rsht->_buckets[_tbl_ind]; // 这个bucket中挂载的第一个SparsePRTEntry在entry中的索引
    ci = find_first_card_in_list(); // 找到第一张卡片
    if (ci != SparsePRTEntry::NullEntry) {
      card_index = compute_card_ind(ci);  // 计算对应卡片的全局索引值，而不是这个卡片在这个Region的相对索引值
      return true;
    }
    // Otherwise, try next entry.
    _tbl_ind++; //尝试下一个_buckets
  }
  // Otherwise, there were no entry.
  return false;
}

/**
 * 在RSHashTable中查找对应的包含了对应的card_index的Region的SparsePRTEntry
 * @param region_index
 * @param card_index
 * @return
 */
bool RSHashTable::contains_card(RegionIdx_t region_index, CardIdx_t card_index) const {
  SparsePRTEntry* e = entry_for_region_ind(region_index);
  return (e != NULL && e->contains_card(card_index)); // 搜索 SparsePRTEntry::contains_card
}

size_t RSHashTable::mem_size() const {
  return sizeof(RSHashTable) +
    capacity() * (SparsePRTEntry::size() + sizeof(int));
}

// ----------------------------------------------------------------------

SparsePRT* SparsePRT::_head_expanded_list = NULL; //  这是一个全局变量

/**
 * 将当前的刚刚完成了扩展的SparsePRT对象，添加到全局扩展列表中
 * @param sprt
 */
void SparsePRT::add_to_expanded_list(SparsePRT* sprt) {
  // We could expand multiple times in a pause -- only put on list once.
  if (sprt->expanded()) return; // 如果这个sprt之前已经进行过扩展，那么就没必要重复加入到expand列表中
  sprt->set_expanded(true); // 标记这个sprt为已扩展
  /**
   * _head_expanded_list是一个静态变量，即全局只有一个， 并不属于一个SparsePRT对象
   */
  SparsePRT* hd = _head_expanded_list;  // 获取当前的全局扩展列表(是一个静态变量)，准别将当前扩展了并且还没加入到全局扩展列表中的对象加入到全局扩展列表中
  /**
   * 下面代码的目的，是试图将当前的SparsePRT对象sprt设置为_head_expanded_list
   * 由于有多线程冲突的风险，因此需要通过原子交换的方式进行多次尝试
   */
  while (true) {
    sprt->_next_expanded = hd;  //将刚刚扩展完的SparkPRT对象的_next_expanded指针指向全局扩展列表
    /**
     * 多线程环境下的原子比较交换，比较地址_head_expanded_list处的值是否等于hd，如果相等，则将_head_expanded_list地址的值更新为sprt
     * 搜索inline static intptr_t cmpxchg_ptr 查看原子交换方法cmpxchg_ptr的具体实现原理
     * 当原子交换成功，  返回值res==hd，如果原子交换失败，返回值res是地址_head_expanded_list最新的值(这个最新的值是其它线程更新上来的，即有现成冲突)
     */
    SparsePRT* res =
      (SparsePRT*)
      Atomic::cmpxchg_ptr(sprt, &_head_expanded_list, hd);
    if (res == hd) return; // 原子交换成功，已经将_head_expanded_list替换为sprt，直接返回
    else hd = res; // 原子交换失败，即 res != hd，意味着发生了线程冲突，有其他线程在这段代码执行期间往地址_head_expanded_list中放入了新的值，因此，我们将hd设置为最新的值，尝试进行重新原子交换
  }
}


SparsePRT* SparsePRT::get_from_expanded_list() {
  SparsePRT* hd = _head_expanded_list;
  while (hd != NULL) {
    SparsePRT* next = hd->next_expanded();
    SparsePRT* res =
      (SparsePRT*)
      Atomic::cmpxchg_ptr(next, &_head_expanded_list, hd);
    if (res == hd) {
      hd->set_next_expanded(NULL);
      return hd;
    } else {
      hd = res;
    }
  }
  return NULL;
}

void SparsePRT::reset_for_cleanup_tasks() {
  _head_expanded_list = NULL;
}

/**
 * 调用者是 OtherRegionsTable::do_cleanup_work
 * 对当前的SparsePRT进行清理工作，但是只有当这个SparsePRT进行过扩展才会进行清理
 * 当这个SparsePRT进行过扩展，那么SparsePRT::_next会指
 */向扩展的列表
void SparsePRT::do_cleanup_work(SparsePRTCleanupTask* sprt_cleanup_task) {
  if (should_be_on_expanded_list()) {
    sprt_cleanup_task->add(this); // 将当前的SparsePRT添加到sprt_cleanup_task对象中
  }
}

void SparsePRT::finish_cleanup_task(SparsePRTCleanupTask* sprt_cleanup_task) {
  assert(ParGCRareEvent_lock->owned_by_self(), "pre-condition");
  SparsePRT* head = sprt_cleanup_task->head();
  SparsePRT* tail = sprt_cleanup_task->tail();
  if (head != NULL) {
    assert(tail != NULL, "if head is not NULL, so should tail");

    tail->set_next_expanded(_head_expanded_list);
    _head_expanded_list = head;
  } else {
    assert(tail == NULL, "if head is NULL, so should tail");
  }
}

/**
 * 返回当前的SparsePRT是否是已经被expand过
 * @return
 */
bool SparsePRT::should_be_on_expanded_list() {
  if (_expanded) { // 只要这个SparsePRT进行了扩展，_cur 和 _next就不同，_next 还是之前的RSHashTable, _next已经指向了扩展后的RSHashTable
    assert(_cur != _next, "if _expanded is true, cur should be != _next");
  } else { // 第一次扩展以前，或者经过了clean_up以后，_cur 和 _next相同，二者是同一个 RSHashTable
    assert(_cur == _next, "if _expanded is false, cur should be == _next");
  }
  return expanded();
}

/**
 * 这是一个全局的静态方法，在垃圾收集的时候使用
 */
void SparsePRT::cleanup_all() {
  // First clean up all expanded tables so they agree on next and cur.
  SparsePRT* sprt = get_from_expanded_list();
  while (sprt != NULL) {
    sprt->cleanup();  // SparsePRT::cleanup()
    sprt = get_from_expanded_list();
  }
}


SparsePRT::SparsePRT(HeapRegion* hr) :
  _hr(hr), _expanded(false), _next_expanded(NULL)
{
  _cur = new RSHashTable(InitialCapacity);
  _next = _cur; // 在没有expand以前，_next和_cur是相等的
}


SparsePRT::~SparsePRT() {
  assert(_next != NULL && _cur != NULL, "Inv");
  if (_cur != _next) { delete _cur; } // 对_cur和_next进行析构
  delete _next;
}


size_t SparsePRT::mem_size() const {
  // We ignore "_cur" here, because it either = _next, or else it is
  // on the deleted list.
  return sizeof(SparsePRT) + _next->mem_size();
}

/**
 * 向稀疏PRT中添加卡片
 * 这个方法是在维护对象的引用关系的时候触发的，查看方法 OtherRegionsTable::add_reference
 * @param region_id 这个Region的region_id
 * @param card_index 卡片索引，即这个Region中的oop相对于这个Region的bottom的卡片索引
 * @return
 */
bool SparsePRT::add_card(RegionIdx_t region_id, CardIdx_t card_index) {
#if SPARSE_PRT_VERBOSE
  gclog_or_tty->print_cr("  Adding card %d from region %d to region %u sparse.",
                         card_index, region_id, _hr->hrm_index());
#endif
  if (_next->occupied_entries() * 2 > _next->capacity()) {
    expand(); // 当前已经占用的entry已经用到了capacity的一半，因此进行扩展
  }
  /**
   * 将这个from_region_ind 和 对应的卡片索引添加到to region的OtherRegionsTable的SparkPRT的RSHashTable _next中，
   *  搜索bool RSHashTable::add_card
   */
  return _next->add_card(region_id, card_index);
}

bool SparsePRT::get_cards(RegionIdx_t region_id, CardIdx_t* cards) {
  return _next->get_cards(region_id, cards);
}

SparsePRTEntry* SparsePRT::get_entry(RegionIdx_t region_id) {
  return _next->get_entry(region_id);
}

// 在方法 OtherRegionsTable::add_reference中，该条目添加到细粒度表以后，会从稀疏表中删除，
bool SparsePRT::delete_entry(RegionIdx_t region_id) {
  return _next->delete_entry(region_id);
}

/**
 * 完全初始化一个SparsePRT，容量为初始容量
 */
void SparsePRT::clear() {
  // If they differ, _next is bigger then cur, so next has no chance of
  // being the initial size.
  if (_next != _cur) {
    delete _next;
  }

  if (_cur->capacity() != InitialCapacity) {
    delete _cur;
    _cur = new RSHashTable(InitialCapacity);
  } else {
    _cur->clear();
  }
  _next = _cur;
  _expanded = false;
}

/**
 * SparsePRT::cleanup()
 * 这里只是删除_cur，同时将_cur和_next都指向_next，相当于让状态进入一个一致状态
 */
void SparsePRT::cleanup() {
  // Make sure that the current and next tables agree.
  if (_cur != _next) {
    delete _cur;
  }
  _cur = _next;
  set_expanded(false);
}

/**
 * 在SparsePRT::add_card的时候会根据当前这个 SparsePRT对象的使用率来决定是否进行expand
 */
void SparsePRT::expand() {
  RSHashTable* last = _next; // 暂存_next即原来的链表，因为在下面需要逐个遍历原来的链表，以将原来的链表中的元素拷贝出来
  _next = new RSHashTable(last->capacity() * 2); // 在进行扩展以后，_next开始与_cur不同，_cur还是初始化的RSHashTable，_next就指向新的扩展以后的RSHashTable

#if SPARSE_PRT_VERBOSE
  gclog_or_tty->print_cr("  Expanded sparse table for %u to %d.",
                         _hr->hrm_index(), _next->capacity());
#endif
  for (size_t i = 0; i < last->capacity(); i++) { // 将旧的列表_last中的entry一个一个迁移到新的列表中去迁移拷贝到新的位置
    SparsePRTEntry* e = last->entry((int)i);
    if (e->valid_entry()) {
#if SPARSE_PRT_VERBOSE
      gclog_or_tty->print_cr("    During expansion, transferred entry for %d.",
                    e->r_ind());
#endif
      _next->add_entry(e);  // 将旧的对象迁移拷贝到新的位置
    }
  }
  if (last != _cur) { // last != _cur的含义是，扩展以前的_next != _cur，即当前的扩展已经不是第一次扩展，这里删除刚刚的_next，并不是删除_cur
    delete last; // 析构调原来的对象，释放内存
  }
  add_to_expanded_list(this); // 将当前这个已经进行了扩展的SparsePRT对象，添加到全局的静态扩展列表中去
}

// 实际上是将当前的SparsePRT对象添加到以SparsePRTCleanupTask的_head和 _tail标记的链表的末尾
void SparsePRTCleanupTask::add(SparsePRT* sprt) {
  assert(sprt->should_be_on_expanded_list(), "pre-condition"); // 这个SparsePRT必须是在expanded list上

  sprt->set_next_expanded(NULL);
  if (_tail != NULL) {
    _tail->set_next_expanded(sprt);
  } else {
    _head = sprt;
  }
  _tail = sprt;
}
