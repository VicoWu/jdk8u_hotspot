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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_INLINE_HPP

#include "gc_implementation/g1/g1BlockOffsetTable.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/heapRegion.inline.hpp"
#include "memory/space.hpp"

/**
 * 方法的调用者是 G1OffsetTableContigSpace::block_start
 * 给定一个addr字地址，返回包含这个字地址的块的首地址(也是以字为单位)
 *
 */
inline HeapWord* G1BlockOffsetTable::block_start(const void* addr) {
  if (addr >= _bottom && addr < _end) {
      /**
       * 如果方法的调用者是G1OffsetTableContigSpace::block_start，
       * 那么这里其实会使用G1BlockOffsetTable的子类G1OffsetTableContigSpace
       *    的方法 G1BlockOffsetArrayContigSpace::block_start_unsafe
       */
    return block_start_unsafe(addr);
  } else {
    return NULL;
  }
}

inline HeapWord*
G1BlockOffsetTable::block_start_const(const void* addr) const {
  if (addr >= _bottom && addr < _end) {
    return block_start_unsafe_const(addr);
  } else {
    return NULL;
  }
}

#define check_index(index, msg)                                                \
  assert((index) < (_reserved.word_size() >> LogN_words),                      \
         err_msg("%s - index: "SIZE_FORMAT", _vs.committed_size: "SIZE_FORMAT, \
                 msg, (index), (_reserved.word_size() >> LogN_words)));        \
  assert(G1CollectedHeap::heap()->is_in_exact(address_for_index_raw(index)),   \
         err_msg("Index "SIZE_FORMAT" corresponding to "PTR_FORMAT             \
                 " (%u) is not in committed area.",                            \
                 (index),                                                      \
                 p2i(address_for_index_raw(index)),                            \
                 G1CollectedHeap::heap()->addr_to_region(address_for_index_raw(index))));

u_char G1BlockOffsetSharedArray::offset_array(size_t index) const {
  check_index(index, "index out of range");
  return _offset_array[index];
}

/**
 * 调用方法是 G1BlockOffsetSharedArray::set_offset_array
 * 从上面的调用方法可以看到，这个_offset_array的key是卡片索引，value是两个HeapWord地址之间的差值
 * @param index
 * @param offset
 */
inline void G1BlockOffsetSharedArray::set_offset_array_raw(size_t index, u_char offset) {
  _offset_array[index] = offset;
}

void G1BlockOffsetSharedArray::set_offset_array(size_t index, u_char offset) {
  check_index(index, "index out of range");
  set_offset_array_raw(index, offset);
}

/**
 * @param index 指定要设置的偏移数组条目的索引
 * @param high 新block的高地址
 * @param low 新block的低地址
 */
void G1BlockOffsetSharedArray::set_offset_array(size_t index, HeapWord* high, HeapWord* low) {
  check_index(index, "index out of range");
  assert(high >= low, "addresses out of order");
   /**
    * 计算high和low地址之间的偏移量(以字Word地址为单位的偏移量)，并将结果存储在offset变量中。
    * point_delta定义在globalDefinitions.hpp中
    */
  size_t offset = pointer_delta(high, low);
  check_offset(offset, "offset too large"); // 这个字地址的偏移量不可以大于N_words
  /**
   * inline void G1BlockOffsetSharedArray::set_offset_array_raw(size_t index, u_char offset) {
      _offset_array[index] = offset;
    }
   */
  set_offset_array(index, (u_char)offset); // 用计算得到的偏移量设置偏移数组中指定索引位置的条目
}

/**
 * 调用者是 G1BlockOffsetArray::set_remainder_to_point_to_start_incl
 * 将_offset_array中索引从left 到 right的值都设置为offset
 * 这个方法和上面的set_offset_array是重载的两个方法
 * 假如left = 1, right = 15, 那么 会把i在[1, 15]左闭右闭区间的索引的值全部设置为offset
 * @param left
 * @param right
 * @param offset
 */
void G1BlockOffsetSharedArray::set_offset_array(size_t left, size_t right, u_char offset) {
  check_index(right, "right index out of range");
  assert(left <= right, "indexes out of order");
  size_t num_cards = right - left + 1; // 假如left = 1, right = 15, num_cards = 15
  if (UseMemSetInBOT) {
    memset(const_cast<u_char*> (&_offset_array[left]), offset, num_cards);
  } else {
    size_t i = left;
    const size_t end = i + num_cards;
    for (; i < end; i++) { // 会把 i在[1, 15]左闭右闭区间的索引的值全部设置为offset
      _offset_array[i] = offset;
    }
  }
}

// Variant of index_for that does not check the index for validity.
inline size_t G1BlockOffsetSharedArray::index_for_raw(const void* p) const {
    /**
     * 这里首先对p进行了强制类型转换 (char*)p,即，如果void *的实际类型
     *   是HeapWord *即字地址，那么会首先将字地址转换成字节地址
     */
  return pointer_delta((char*)p, _reserved.start(), sizeof(char))  // 计算给定的地址(以字节为单位)和保留地址之间(以字节为单位)的地址偏移量(以字节为单位),
                >> LogN; // 搜索 SomePublicConstants查看LogN, 当LogN=9，意味着将两个地址的差值右移9位，就得到了这个偏移地址在索引中的偏移位置
}

/**
 * 接受一个指向某个地址的指针(以字Word为单位),HeapWord *，返回该地址在偏移数组中的索引位置。
 * @param p
 * @return
 */
inline size_t G1BlockOffsetSharedArray::index_for(const void* p) const {
  char* pc = (char*)p;
  /**
   * 偏移数组（OffsetArray）是针对内存区域（通常是Java堆）进行管理的数据结构，它只能处理与其关联的内存范围内的地址。
   * 如果给定的地址不在偏移数组的保留区域内，就意味着该地址不属于偏移数组所管理的内存范围，因此无法计算其在偏移数组中的索引位置。
   */
  assert(pc >= (char*)_reserved.start() &&
         pc <  (char*)_reserved.end(),
         err_msg("p (" PTR_FORMAT ") not in reserved [" PTR_FORMAT ", " PTR_FORMAT ")",
                 p2i(p), p2i(_reserved.start()), p2i(_reserved.end())));
  /**
   * 地址p和保留地址之间的差值，然后右移LogN位，就得到了地址p应该属于的在偏移数组中的偏移量
   */

  size_t result = index_for_raw(p);
  check_index(result, "bad index from address");
  return result;
}

inline HeapWord*
G1BlockOffsetSharedArray::address_for_index(size_t index) const {
  check_index(index, "index out of range");
  HeapWord* result = address_for_index_raw(index); // 输入索引值，返回这个转移专用记忆集合对应的内存的字地址(不是字节地址)
  assert(result >= _reserved.start() && result < _reserved.end(),
         err_msg("bad address from index result " PTR_FORMAT
                 " _reserved.start() " PTR_FORMAT " _reserved.end() "
                 PTR_FORMAT,
                 p2i(result), p2i(_reserved.start()), p2i(_reserved.end())));
  return result;
}

#undef check_index

inline size_t
G1BlockOffsetArray::block_size(const HeapWord* p) const {
  return gsp()->block_size(p);
}

/**
 * 给定一个字(HeapWord)地址，返回这个地址
 * 调用方是 G1BlockOffsetArrayContigSpace::block_start_unsafe
 * @param addr
 * @param has_max_index
 * @param max_index
 * @return
 */
inline HeapWord*
G1BlockOffsetArray::block_at_or_preceding(const void* addr,
                                          bool has_max_index,
                                          size_t max_index) const {
  assert(_array->offset_array(0) == 0, "objects can't cross covered areas");
  size_t index = _array->index_for(addr); // 这个字地址在_offset_array数组中的索引位置
  // We must make sure that the offset table entry we use is valid.  If
  // "addr" is past the end, start at the last known one and go forward.
  if (has_max_index) {
    index = MIN2(index, max_index);
  }
  /**
   * 这个索引位置所对应的字地址，index和q是一一映射的
   * 所以 index 每回退k，那么q就回退 k * N_words
   */
  HeapWord* q = _array->address_for_index(index);

  /**
   * 取出这个索引位置上的偏移值
   * 需要对照偏移数组的数据产生过程来理解这个基于偏移数组迅速找到包含对象的块地址的过程，参考 G1BlockOffsetArray::alloc_block_work2
   *
   * start_card = 1,
   * i = 0, reach =  1 - 1 + (16 - 1) = 15, 这时候通过调用 set_offset_array(1, 15, offset)，会把[1,15]区间内的值全部设置为64 + 0 = 64
   * i = 1, reach = 1 - 1 + (256 - 1) = 255，这时候通过调用 set_offset_array(16, 255, offset)，会把[16,255]区间内的值全部设置为64 + 1 = 65
   *
   *  如果index = 27，
   *      这时候，_offset_array[27] = 65,  65 - 64 = 1, 调用 power_to_cards_back(1) 获取需要回退的卡片索引值，这个值是16,于是index 回退16，到了27 - 16 = 11
   *      由于_offset_array[11] = 64, 64 - 64 = 0, 调用 power_to_cards_back(1) 获取需要回退的卡片索引值，这个值是1， 于是index 回退1，到了11 - 1 = 10
   *      由于_offset_array[10] = 64, 64 - 64 = 0, 调用 power_to_cards_back(1) 获取需要回退的卡片索引值，这个值是1， 于是index 回退1，到了10 - 1 = 9
   *      ....
   *      由于_offset_array[1] = 64, 64 - 64 = 0, 调用 power_to_cards_back(1) 获取需要回退的卡片索引值，这个值是1， 于是index 回退1，到了1 - 1 = 0，这个块的字地址假如是0X11111111
   *      _offset_array[0] = 7，由于15小于N_words, 说明这个index对应了对象的起始位置，通过上一个卡片的首地址 0x1111111 - 7(0x111) = 0x11111000，这个就是这个对象的头部地址
   */
  uint offset = _array->offset_array(index);  // Extend u_char to uint.
  while (offset >= N_words) { // 不断减小offset，直到offset < N_words
    // The excess of the offset from N_words indicates a power of Base
    // to go back by.

    /**
     * 1 << (LogBase * (offset - N_words)) ，即  base^(offset - N_words)
     * offset 与 N_words 的偏移量超出部分, 表示要往后查找的 Base 次幂。
     */
    size_t n_cards_back = BlockOffsetArray::entry_to_cards_back(offset);
    q -= (N_words * n_cards_back); // 回退 n_cards_back 个字(word)
    assert(q >= gsp()->bottom(), "Went below bottom!");
    index -= n_cards_back; // 回退n_cards_back个索引位置
    offset = _array->offset_array(index);// 取出新的索引位置的offset
  }
  // 终于找到了,此时这个index的offset的值小于N_words
  assert(offset < N_words, "offset too large");
  q -= offset;
  return q; // 返回这个index在array中对应的值小于N_words的内存的字地址
}

/**
 *
 * @param q
 * @param n
 * @param addr
 * @return
 */
inline HeapWord*
G1BlockOffsetArray::
forward_to_block_containing_addr_const(HeapWord* q, HeapWord* n,
                                       const void* addr) const {
  if (addr >= gsp()->top()) return gsp()->top();
  while (n <= addr) {
    q = n;
    oop obj = oop(q);
    if (obj->klass_or_null() == NULL) return q;
    n += block_size(q);
  }
  assert(q <= n, "wrong order for q and addr");
  assert(addr < n, "wrong order for addr and n");
  return q;
}

/**
 * 调用方是 G1BlockOffsetArrayContigSpace::block_start_unsafe
 * 这里的q是G1BlockOffsetArray::block_at_or_preceding的返回值,
 *  addr也是G1BlockOffsetArray::block_at_or_preceding的参数addr
 * @param q
 * @param addr
 * @return
 */
inline HeapWord*
G1BlockOffsetArray::forward_to_block_containing_addr(HeapWord* q,
                                                     const void* addr) {
  if (oop(q)->klass_or_null() == NULL) return q;
  HeapWord* n = q + block_size(q);
  // In the normal case, where the query "addr" is a card boundary, and the
  // offset table chunks are the same size as cards, the block starting at
  // "q" will contain addr, so the test below will fail, and we'll fall
  // through quickly.
  /**
   * 在正常情况下，“addr”是Card的边界，并且block的大小与卡大小相同(都是512B)，因此从“q”开始的块将包含addr，因此下面的测试将失败，我们就不用经过forward_to_block_containing_addr_slow
   * 但是如果n <= addr,那么我们只能通过forward_to_block_containing_addr_slow来进行慢路径查找
   */
  if (n <= addr) {
      /**
       *  “q”是块边界，<=“addr”； “n”是下一个块的地址（或空间的末尾）。
       *  返回包含“addr”的块的开头地址。 通过更新不精确的条目，可能会对“this”产生副作用。
       */
    q = forward_to_block_containing_addr_slow(q, n, addr);
  }
  assert(q <= addr, "wrong order for current and arg");
  return q;
}

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_INLINE_HPP
