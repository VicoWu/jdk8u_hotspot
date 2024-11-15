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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_HPP

#include "gc_implementation/g1/g1RegionToSpaceMapper.hpp"
#include "memory/memRegion.hpp"
#include "runtime/virtualspace.hpp"
#include "utilities/globalDefinitions.hpp"

// The CollectedHeap type requires subtypes to implement a method
// "block_start".  For some subtypes, notably generational
// systems using card-table-based write barriers, the efficiency of this
// operation may be important.  Implementations of the "BlockOffsetArray"
// class may be useful in providing such efficient implementations.
//
// While generally mirroring the structure of the BOT for GenCollectedHeap,
// the following types are tailored more towards G1's uses; these should,
// however, be merged back into a common BOT to avoid code duplication
// and reduce maintenance overhead.
//
//    G1BlockOffsetTable (abstract)
//    -- G1BlockOffsetArray                (uses G1BlockOffsetSharedArray)
//       -- G1BlockOffsetArrayContigSpace
//
// A main impediment to the consolidation of this code might be the
// effect of making some of the block_start*() calls non-const as
// below. Whether that might adversely affect performance optimizations
// that compilers might normally perform in the case of non-G1
// collectors needs to be carefully investigated prior to any such
// consolidation.

// Forward declarations
class G1BlockOffsetSharedArray;
class G1OffsetTableContigSpace;

class G1BlockOffsetTable VALUE_OBJ_CLASS_SPEC {
  friend class VMStructs;
protected:
  // These members describe the region covered by the table.

  // The space this table is covering.
  HeapWord* _bottom;    // == reserved.start
  HeapWord* _end;       // End of currently allocated region.

public:
  // Initialize the table to cover the given space.
  // The contents of the initial table are undefined.
  G1BlockOffsetTable(HeapWord* bottom, HeapWord* end) :
    _bottom(bottom), _end(end)
    {
      assert(_bottom <= _end, "arguments out of order");
    }

  // Note that the committed size of the covered space may have changed,
  // so the table size might also wish to change.
  virtual void resize(size_t new_word_size) = 0;

  virtual void set_bottom(HeapWord* new_bottom) {
    assert(new_bottom <= _end,
           err_msg("new_bottom (" PTR_FORMAT ") > _end (" PTR_FORMAT ")",
                   p2i(new_bottom), p2i(_end)));
    _bottom = new_bottom;
    resize(pointer_delta(_end, _bottom));
  }

  // Requires "addr" to be contained by a block, and returns the address of
  // the start of that block.  (May have side effects, namely updating of
  // shared array entries that "point" too far backwards.  This can occur,
  // for example, when LAB allocation is used in a space covered by the
  // table.)
  virtual HeapWord* block_start_unsafe(const void* addr) = 0;
  // Same as above, but does not have any of the possible side effects
  // discussed above.
  virtual HeapWord* block_start_unsafe_const(const void* addr) const = 0;

  // Returns the address of the start of the block containing "addr", or
  // else "null" if it is covered by no block.  (May have side effects,
  // namely updating of shared array entries that "point" too far
  // backwards.  This can occur, for example, when lab allocation is used
  // in a space covered by the table.)
  inline HeapWord* block_start(const void* addr);
  // Same as above, but does not have any of the possible side effects
  // discussed above.
  inline HeapWord* block_start_const(const void* addr) const;
};

class G1BlockOffsetSharedArrayMappingChangedListener : public G1MappingChangedListener {
 public:
  virtual void on_commit(uint start_idx, size_t num_regions, bool zero_filled) {
    // Nothing to do. The BOT is hard-wired to be part of the HeapRegion, and we cannot
    // retrieve it here since this would cause firing of several asserts. The code
    // executed after commit of a region already needs to do some re-initialization of
    // the HeapRegion, so we combine that.
  }
};

// This implementation of "G1BlockOffsetTable" divides the covered region
// into "N"-word subregions (where "N" = 2^"LogN".  An array with an entry
// for each such subregion indicates how far back one must go to find the
// start of the chunk that includes the first word of the subregion.
//
// Each BlockOffsetArray is owned by a Space.  However, the actual array
// may be shared by several BlockOffsetArrays; this is useful
// when a single resizable area (such as a generation) is divided up into
// several spaces in which contiguous allocation takes place,
// such as, for example, in G1 or in the train generation.)

// Here is the shared array type.

/**
 * 在G1CollectedHeap中被构造，搜索 _bot_shared = new G1BlockOffsetSharedArray(_reserved, bot_storage)
 */
class G1BlockOffsetSharedArray: public CHeapObj<mtGC> {
  friend class G1BlockOffsetArray;
  friend class G1BlockOffsetArrayContigSpace;
  friend class VMStructs;

private:
  G1BlockOffsetSharedArrayMappingChangedListener _listener;
  // The reserved region covered by the shared array.
  MemRegion _reserved;

  // End of the current committed region.
  HeapWord* _end;

  // Array for keeping offsets for retrieving object start fast given an
  // address.
  volatile u_char* _offset_array;          // byte array keeping backwards offsets

  /**
   * 存放到_offset_array中的字地址的偏移量不可以大于N_words
   * @param offset
   * @param msg
   */
  void check_offset(size_t offset, const char* msg) const {
    assert(offset <= N_words,
           err_msg("%s - "
                   "offset: " SIZE_FORMAT ", N_words: " UINT32_FORMAT,
                   msg, offset, N_words));
  }

  // Bounds checking accessors:
  // For performance these have to devolve to array accesses in product builds.
  inline u_char offset_array(size_t index) const;

  inline void set_offset_array_raw(size_t index, u_char offset);

  inline void set_offset_array(size_t index, u_char offset);

  inline void set_offset_array(size_t index, HeapWord* high, HeapWord* low);

  inline void set_offset_array(size_t left, size_t right, u_char offset);

  bool is_card_boundary(HeapWord* p) const;

public:

  // Return the number of slots needed for an offset array
  // that covers mem_region_words words.
  // G1BlockOffsetSharedArray::compute_size
  static size_t compute_size(size_t mem_region_words) {
    size_t number_of_slots = (mem_region_words / N_words); // mem_region_words代表整个堆内存的HeapWord数量，N_words代表一张卡片对应的内存的卡片数量(512 /8 = 64)
    return ReservedSpace::allocation_align_size_up(number_of_slots);
  }

  enum SomePublicConstants {
      /**
       * 以字节为单位的偏移数组的大小为2的多少次方，2^9=512
       * 搜索 G1BlockOffsetSharedArray::index_for_raw 可以看到LogN的含义，给定一个以字节为单位的偏移量，如果右移9位，就得到这个地址在字节数组中的index
       */
    LogN = 9,
    /**
     * 以HeapWord(字)的大小为单位的偏移数组的位数
     * LogN 表示偏移数组的大小（以2为底的对数），而 LogHeapWordSize 表示 HeapWord 的大小（以2为底的对数）。
     *  因此，LogN_words 表示偏移数组索引的位数，它等于偏移数组大小的对数减去 HeapWord 的大小的对数，这样就可以确定偏移数组的索引位数
     * 假如LogHeapWordSize=3，即一个HeapWord的大小为2 ^ 3 = 8字节，那么LogN_words = 9 - 3 = 6，即一个偏移数组只能表示2^6=64个HeapWord
     */
    LogN_words = LogN - LogHeapWordSize,
    /**
     * 偏移数组的大小（以字节为单位），假如LogN = 9，那么N_Bytes=512
     */
    N_bytes = 1 << LogN,
    /**
     * 偏移数组的大小(以HeapWord为单位),，假如LogN=9，而一个字是8个字节，因此LogHeapWordSize=3，
     *      因此LogN_words = 9 - 3 = 6，那么N_words = 64，代表64个HeapWord，即一个卡片代表了64个HeapWord，对应了64 * 8 = 512Byte
     */
    N_words = 1 << LogN_words
  };

  // Initialize the table to cover from "base" to (at least)
  // "base + init_word_size".  In the future, the table may be expanded
  // (see "resize" below) up to the size of "_reserved" (which must be at
  // least "init_word_size".) The contents of the initial table are
  // undefined; it is the responsibility of the constituent
  // G1BlockOffsetTable(s) to initialize cards.
  G1BlockOffsetSharedArray(MemRegion heap, G1RegionToSpaceMapper* storage);

  // Return the appropriate index into "_offset_array" for "p".
  inline size_t index_for(const void* p) const;
  inline size_t index_for_raw(const void* p) const;

  // Return the address indicating the start of the region corresponding to
  // "index" in "_offset_array".
  /**
   * 具体实现 搜索 HeapWord* BlockOffsetSharedArray::address_for_index
   * @param index
   * @return
   */
  inline HeapWord* address_for_index(size_t index) const;
  // Variant of address_for_index that does not check the index for validity.
   /**
    * 根据在_offset_array数组中的索引，返回这个索引对应的字地址(注意，不是返回这个index在_offset_array中的值)
    * @param index
    * @return
    */
  inline HeapWord* address_for_index_raw(size_t index) const {
    return _reserved.start() + (index << LogN_words);
  }
};

// And here is the G1BlockOffsetTable subtype that uses the array.

class G1BlockOffsetArray: public G1BlockOffsetTable {
  friend class G1BlockOffsetSharedArray;
  friend class G1BlockOffsetArrayContigSpace;
  friend class VMStructs;
private:
  enum SomePrivateConstants {
    N_words = G1BlockOffsetSharedArray::N_words,
    LogN    = G1BlockOffsetSharedArray::LogN
  };

  // This is the array, which can be shared by several BlockOffsetArray's
  // servicing different
  G1BlockOffsetSharedArray* _array; // 被多个G1BlockOffsetArray共享的一个array

  // The space that owns this subregion.
  G1OffsetTableContigSpace* _gsp;// 注意， HeapRegion是G1OffsetTableContigSpace的子类，这里就代表了一个HeapRegion

  // The portion [_unallocated_block, _sp.end()) of the space that
  // is a single block known not to contain any objects.
  // NOTE: See BlockOffsetArrayUseUnallocatedBlock flag.
  HeapWord* _unallocated_block;

  // Sets the entries
  // corresponding to the cards starting at "start" and ending at "end"
  // to point back to the card before "start": the interval [start, end)
  // is right-open.
  void set_remainder_to_point_to_start(HeapWord* start, HeapWord* end);
  // Same as above, except that the args here are a card _index_ interval
  // that is closed: [start_index, end_index]
  void set_remainder_to_point_to_start_incl(size_t start, size_t end);

protected:

  G1OffsetTableContigSpace* gsp() const { return _gsp; }

  inline size_t block_size(const HeapWord* p) const;

  // Returns the address of a block whose start is at most "addr".
  // If "has_max_index" is true, "assumes "max_index" is the last valid one
  // in the array.
  inline HeapWord* block_at_or_preceding(const void* addr,
                                         bool has_max_index,
                                         size_t max_index) const;

  // "q" is a block boundary that is <= "addr"; "n" is the address of the
  // next block (or the end of the space.)  Return the address of the
  // beginning of the block that contains "addr".  Does so without side
  // effects (see, e.g., spec of  block_start.)
  inline HeapWord*
  forward_to_block_containing_addr_const(HeapWord* q, HeapWord* n,
                                         const void* addr) const;

  // "q" is a block boundary that is <= "addr"; return the address of the
  // beginning of the block that contains "addr".  May have side effects
  // on "this", by updating imprecise entries.
  inline HeapWord* forward_to_block_containing_addr(HeapWord* q,
                                                    const void* addr);

  // "q" is a block boundary that is <= "addr"; "n" is the address of the
  // next block (or the end of the space.)  Return the address of the
  // beginning of the block that contains "addr".  May have side effects
  // on "this", by updating imprecise entries.
  /**
   * “q”是块边界，<=“addr”； “n”是下一个块的地址（或空间的末尾）。
   * 返回包含“addr”的块的开头地址。 通过更新不精确的条目，可能会对“this”产生副作用。
   */
  HeapWord* forward_to_block_containing_addr_slow(HeapWord* q,
                                                  HeapWord* n,
                                                  const void* addr);

  // Requires that "*threshold_" be the first array entry boundary at or
  // above "blk_start", and that "*index_" be the corresponding array
  // index.  If the block starts at or crosses "*threshold_", records
  // "blk_start" as the appropriate block start for the array index
  // starting at "*threshold_", and for any other indices crossed by the
  // block.  Updates "*threshold_" and "*index_" to correspond to the first
  // index after the block end.
  void alloc_block_work2(HeapWord** threshold_, size_t* index_,
                         HeapWord* blk_start, HeapWord* blk_end);

public:
  // The space may not have it's bottom and top set yet, which is why the
  // region is passed as a parameter. The elements of the array are
  // initialized to zero.
  G1BlockOffsetArray(G1BlockOffsetSharedArray* array, MemRegion mr);

  // Note: this ought to be part of the constructor, but that would require
  // "this" to be passed as a parameter to a member constructor for
  // the containing concrete subtype of Space.
  // This would be legal C++, but MS VC++ doesn't allow it.
  void set_space(G1OffsetTableContigSpace* sp);

  // Resets the covered region to one with the same _bottom as before but
  // the "new_word_size".
  void resize(size_t new_word_size);

  virtual HeapWord* block_start_unsafe(const void* addr);
  virtual HeapWord* block_start_unsafe_const(const void* addr) const;

  // Used by region verification. Checks that the contents of the
  // BOT reflect that there's a single object that spans the address
  // range [obj_start, obj_start + word_size); returns true if this is
  // the case, returns false if it's not.
  bool verify_for_object(HeapWord* obj_start, size_t word_size) const;

  void check_all_cards(size_t left_card, size_t right_card) const;

  virtual void print_on(outputStream* out) PRODUCT_RETURN;
};

// A subtype of BlockOffsetArray that takes advantage of the fact
// that its underlying space is a ContiguousSpace, so that its "active"
// region can be more efficiently tracked (than for a non-contiguous space).
/**
 * G1BlockOffsetArrayContigSpace是 G1BlockOffsetArray 的子类， 同时G1BlockOffsetArray是G1BlockOffsetTable的子类
   HeapRegion是G1OffsetTableContigSpace的子类，其成员变量包含了一个对应的G1BlockOffsetArrayContigSpace _offset，
        这个_offset在G1OffsetTableContigSpace中构造
 */
class G1BlockOffsetArrayContigSpace: public G1BlockOffsetArray {
  friend class VMStructs;

  // allocation boundary at which offset array must be updated
  /**
   * G1BlockOffsetpublicArrayContigSpace 的变量
   * _next_offset_threshold是一个字地址的界限，如果内存分配超过了这个界限，意味着偏移数组需要被更新
   */

  HeapWord* _next_offset_threshold;
  /**
   * G1BlockOffsetpublicArrayContigSpace的变量
   * 与上面的界限_next_offset_threshold所对应的偏移数组的偏移量，即当前分配的对象的地址对应的索引
   */
  size_t    _next_offset_index;      // index corresponding to that boundary

  // Work function to be called when allocation start crosses the next
  // threshold in the contig space.
  /**
   * 调用方是 G1BlockOffsetArrayContigSpace::alloc_block
   * 这个方法的全称是 G1BlockOffsetpublicArrayContigSpace::alloc_block_work1
   * 这个方法的调用发生在下一个分配对象的结束地址会会跨越threshold
   */
  void alloc_block_work1(HeapWord* blk_start, HeapWord* blk_end) {
      /**
       * 搜索 G1BlockOffsetArray::alloc_block_work2
       */
    alloc_block_work2(&_next_offset_threshold,
                      &_next_offset_index, // 偏移数组的下一个索引值，将在这个索引值的地方写入偏移量
                      blk_start, blk_end); // 这里会计算blk_start和_next_offset_threshold之间的距离，将这个距离作为value写入到index的位置
  }

  // Zero out the entry for _bottom (offset will be zero). Does not check for availability of the
  // memory first.
  void zero_bottom_entry_raw();
  // Variant of initialize_threshold that does not check for availability of the
  // memory first.
  HeapWord* initialize_threshold_raw();
  /**
   * 构造函数 G1BlockOffsetArrayContigSpace， 在 G1OffsetTableContigSpace的构造函数中被构造
   * 搜索 G1OffsetTableContigSpace::G1OffsetTableContigSpace 查看 其构造函数
   * 可以看到， array是在g1CollectedHeap中被构造，因此堆内存中只有一个全局的G1BlockOffsetSharedArray对象
   */
 public:
  G1BlockOffsetArrayContigSpace(G1BlockOffsetSharedArray* array, MemRegion mr);

  // Initialize the threshold to reflect the first boundary after the
  // bottom of the covered region.
  HeapWord* initialize_threshold();

  void reset_bot() {
    zero_bottom_entry_raw();
    initialize_threshold_raw();
  }

  // Return the next threshold, the point at which the table should be
  // updated.
  HeapWord* threshold() const { return _next_offset_threshold; }

  // These must be guaranteed to work properly (i.e., do nothing)
  // when "blk_start" ("blk" for second version) is "NULL".  In this
  // implementation, that's true because NULL is represented as 0, and thus
  // never exceeds the "_next_offset_threshold".
  /**
   * 方法的全称为 G1BlockOffsetArrayContigSpace::alloc_block
   * 这个方法的调用者是下面的重载方法 alloc_block
   */
  void alloc_block(HeapWord* blk_start, HeapWord* blk_end) {
      /**
       * 如果分配的block的结束位置需要跨越_next_offset_threshold，那么需要调用alloc_block_work1，专门处理跨域_threshold的分配
       * 如果这个分配地址没有超过 _next_offset_threshold，那么就不需要做任何处理，即不需要更新块偏移表
       */
    if (blk_end > _next_offset_threshold) //
      alloc_block_work1(blk_start, blk_end);
  }

  /**
   * 查看调用者 HeapWord* G1OffsetTableContigSpace::allocate
   * G1BlockOffsetArrayContigSpace::alloc_block
   * @param blk
   * @param size
   */
  void alloc_block(HeapWord* blk, size_t size) {
     alloc_block(blk, blk+size);
  }

  HeapWord* block_start_unsafe(const void* addr);
  HeapWord* block_start_unsafe_const(const void* addr) const;

  void set_for_starts_humongous(HeapWord* new_top);

  virtual void print_on(outputStream* out) PRODUCT_RETURN;
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1BLOCKOFFSETTABLE_HPP
