/*
 * Copyright (c) 2011, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/g1AllocRegion.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "runtime/orderAccess.inline.hpp"

G1CollectedHeap* G1AllocRegion::_g1h = NULL;
HeapRegion* G1AllocRegion::_dummy_region = NULL;

/**
 * 在G1CollectedHeap::initialize()中，也就是堆内存初始化的时候，会调用setup()方法
 * @param g1h
 * @param dummy_region
 */
void G1AllocRegion::setup(G1CollectedHeap* g1h, HeapRegion* dummy_region) {
  assert(_dummy_region == NULL, "should be set once");
  assert(dummy_region != NULL, "pre-condition");
  assert(dummy_region->free() == 0, "pre-condition");

  // Make sure that any allocation attempt on this region will fail
  // and will not trigger any asserts.
  assert(allocate(dummy_region, 1, false) == NULL, "should fail");
  assert(par_allocate(dummy_region, 1, false) == NULL, "should fail");
  assert(allocate(dummy_region, 1, true) == NULL, "should fail");
  assert(par_allocate(dummy_region, 1, true) == NULL, "should fail");

  _g1h = g1h;
  _dummy_region = dummy_region;
}

void G1AllocRegion::fill_up_remaining_space(HeapRegion* alloc_region,
                                            bool bot_updates) {
  assert(alloc_region != NULL && alloc_region != _dummy_region,
         "pre-condition");

  // Other threads might still be trying to allocate using a CAS out
  // of the region we are trying to retire, as they can do so without
  // holding the lock. So, we first have to make sure that noone else
  // can allocate out of it by doing a maximal allocation. Even if our
  // CAS attempt fails a few times, we'll succeed sooner or later
  // given that failed CAS attempts mean that the region is getting
  // closed to being full.
  size_t free_word_size = alloc_region->free() / HeapWordSize;

  // This is the minimum free chunk we can turn into a dummy
  // object. If the free space falls below this, then noone can
  // allocate in this region anyway (all allocation requests will be
  // of a size larger than this) so we won't have to perform the dummy
  // allocation.
  size_t min_word_size_to_fill = CollectedHeap::min_fill_size();

  while (free_word_size >= min_word_size_to_fill) {
    HeapWord* dummy = par_allocate(alloc_region, free_word_size, bot_updates);
    if (dummy != NULL) {
      // If the allocation was successful we should fill in the space.
      CollectedHeap::fill_with_object(dummy, free_word_size);
      alloc_region->set_pre_dummy_top(dummy);
      break;
    }

    free_word_size = alloc_region->free() / HeapWordSize;
    // It's also possible that someone else beats us to the
    // allocation and they fill up the region. In that case, we can
    // just get out of the loop.
  }
  assert(alloc_region->free() / HeapWordSize < min_word_size_to_fill,
         "post-condition");
}

/**
 * 调用者 搜索 HeapWord* G1AllocRegion::attempt_allocation_locked
 * 方法中会调用子类的retire_region方法，比如MutatorAllocRegion,
 *    SurvivorGCAllocRegion, OldGCAllocRegion的retire_region方法
 */
void G1AllocRegion::retire(bool fill_up) {
  assert(_alloc_region != NULL, ar_ext_msg(this, "not initialized properly"));

  trace("retiring");
  /**
   *  一个G1AllocRegion的具体实现类的对象中都有一个当前的active region，叫做 _alloc_region
   */
  HeapRegion* alloc_region = _alloc_region;
  if (alloc_region != _dummy_region) {
    // We never have to check whether the active region is empty or not,
    // and potentially free it if it is, given that it's guaranteed that
    // it will never be empty.
    assert(!alloc_region->is_empty(),
           ar_ext_msg(this, "the alloc region should never be empty"));

    if (fill_up) {
        /**
         *
         *  从几个G1AllocRegion的实现可以看到，只有OldGCAllocRegion的_bot_updates是true
         *  而 SurvivorGCAllocRegion 和 MutatorGCAllocRegion的_bot_updates都是false
         */
      fill_up_remaining_space(alloc_region, _bot_updates);
    }

    assert(alloc_region->used() >= _used_bytes_before,
           ar_ext_msg(this, "invariant"));
    size_t allocated_bytes = alloc_region->used() - _used_bytes_before;
    /**
     * 会调用子类的retire_region方法，比如 MutatorAllocRegion,
     * SurvivorGCAllocRegion, OldGCAllocRegion的 retire_region方法
     */
    retire_region(alloc_region, allocated_bytes);
    _used_bytes_before = 0;
    _alloc_region = _dummy_region;
  }
  trace("retired");
}

/**
 * 在方法 G1AllocRegion::attempt_allocation_locked中和G1AllocRegion::attempt_allocation_force中调用，
 *      调用完成以后，设置当前的G1AllocRegion具体实现类的_alloc_region
 */
HeapWord* G1AllocRegion::new_alloc_region_and_allocate(size_t word_size,
                                                       bool force) {
  assert(_alloc_region == _dummy_region, ar_ext_msg(this, "pre-condition"));
  assert(_used_bytes_before == 0, ar_ext_msg(this, "pre-condition"));

  trace("attempting region allocation");
  // 这里的allocate_new_region就是调用G1AllocRegion的具体实现类的具体实现
  HeapRegion* new_alloc_region = allocate_new_region(word_size, force); // 分配一个新的region
  if (new_alloc_region != NULL) { // Region创建成功
    new_alloc_region->reset_pre_dummy_top();
    // Need to do this before the allocation
    _used_bytes_before = new_alloc_region->used();
    /**
     * 在新的region中分配对象，这个对象一定应该是分配成功的，因为region是新的
     */
    HeapWord* result = allocate(new_alloc_region, word_size, _bot_updates); //
    assert(result != NULL, ar_ext_msg(this, "the allocation should succeeded"));

    OrderAccess::storestore();
    // Note that we first perform the allocation and then we store the
    // region in _alloc_region. This is the reason why an active region
    // can never be empty.
    /**
     * 将这个新的Region设置为当前的active region，
     * 即G1AllocRegion设置当前这个新的region为正在分配的region，而上一个Region已经卸载了
     */
    update_alloc_region(new_alloc_region);
    trace("region allocation successful");
    return result;
  } else {
    trace("region allocation failed");
    return NULL;
  }
  ShouldNotReachHere();
}

void G1AllocRegion::fill_in_ext_msg(ar_ext_msg* msg, const char* message) {
  msg->append("[%s] %s c: %u b: %s r: " PTR_FORMAT " u: " SIZE_FORMAT,
              _name, message, _count, BOOL_TO_STR(_bot_updates),
              p2i(_alloc_region), _used_bytes_before);
}

/**
 * 初始化一个region，G1AlloRegion的子类在创建一个region的时候都会调用init方法
 */
void G1AllocRegion::init() {
  trace("initializing");
  assert(_alloc_region == NULL && _used_bytes_before == 0,
         ar_ext_msg(this, "pre-condition"));
  assert(_dummy_region != NULL, ar_ext_msg(this, "should have been set"));
  _alloc_region = _dummy_region; // 刚刚初始化的时候，新的active region 都指向dummy_region
  _count = 0;
  trace("initialized");
}

void G1AllocRegion::set(HeapRegion* alloc_region) {
  trace("setting");
  // We explicitly check that the region is not empty to make sure we
  // maintain the "the alloc region cannot be empty" invariant.
  assert(alloc_region != NULL && !alloc_region->is_empty(),
         ar_ext_msg(this, "pre-condition"));
  assert(_alloc_region == _dummy_region &&
         _used_bytes_before == 0 && _count == 0,
         ar_ext_msg(this, "pre-condition"));

  _used_bytes_before = alloc_region->used();
  _alloc_region = alloc_region;
  _count += 1;
  trace("set");
}

void G1AllocRegion::update_alloc_region(HeapRegion* alloc_region) {
  trace("update");
  // We explicitly check that the region is not empty to make sure we
  // maintain the "the alloc region cannot be empty" invariant.
  assert(alloc_region != NULL && !alloc_region->is_empty(),
         ar_ext_msg(this, "pre-condition"));

  _alloc_region = alloc_region; // 将这个新申请到的HeapRegion对象设置为当前active region
  _alloc_region->set_allocation_context(allocation_context());
  _count += 1; // 计数器+1， 因为G1AllocRegion对象的生命周期中会负责很多次的Region的申请，卸载，再申请，再卸载
  trace("updated");
}

/**
 * 释放掉当前G1AllocRegion所管理的active 的HeapRegion
 * 注意区分 G1AllocRegion::retire()，G1AllocRegion::release()会将_alloc_region置为Null，而 G1AllocRegion::retire() 会将_alloc_region置为dummy_region
 * @return
 */
HeapRegion* G1AllocRegion::release() {
  trace("releasing");
  HeapRegion* alloc_region = _alloc_region;
  retire(false /* fill_up */); // 先将当前所管理的Region卸载掉
  assert(_alloc_region == _dummy_region, // retire()一定会将_alloc_region置为_dummy_region
         ar_ext_msg(this, "post-condition of retire()"));
  _alloc_region = NULL; // 当前管理的Region置为空
  trace("released");
  // 如果retire()以前的region已经是dummy_region，返回null，否则，返回之前的Region
  return (alloc_region == _dummy_region) ? NULL : alloc_region;
}

#if G1_ALLOC_REGION_TRACING
void G1AllocRegion::trace(const char* str, size_t word_size, HeapWord* result) {
  // All the calls to trace that set either just the size or the size
  // and the result are considered part of level 2 tracing and are
  // skipped during level 1 tracing.
  if ((word_size == 0 && result == NULL) || (G1_ALLOC_REGION_TRACING > 1)) {
    const size_t buffer_length = 128;
    char hr_buffer[buffer_length];
    char rest_buffer[buffer_length];

    HeapRegion* alloc_region = _alloc_region;
    if (alloc_region == NULL) {
      jio_snprintf(hr_buffer, buffer_length, "NULL");
    } else if (alloc_region == _dummy_region) {
      jio_snprintf(hr_buffer, buffer_length, "DUMMY");
    } else {
      jio_snprintf(hr_buffer, buffer_length,
                   HR_FORMAT, HR_FORMAT_PARAMS(alloc_region));
    }

    if (G1_ALLOC_REGION_TRACING > 1) {
      if (result != NULL) {
        jio_snprintf(rest_buffer, buffer_length, SIZE_FORMAT " " PTR_FORMAT,
                     word_size, result);
      } else if (word_size != 0) {
        jio_snprintf(rest_buffer, buffer_length, SIZE_FORMAT, word_size);
      } else {
        jio_snprintf(rest_buffer, buffer_length, "");
      }
    } else {
      jio_snprintf(rest_buffer, buffer_length, "");
    }

    tty->print_cr("[%s] %u %s : %s %s",
                  _name, _count, hr_buffer, str, rest_buffer);
  }
}
#endif // G1_ALLOC_REGION_TRACING

G1AllocRegion::G1AllocRegion(const char* name,
                             bool bot_updates)
  : _name(name), _bot_updates(bot_updates),
    _alloc_region(NULL), _count(0), _used_bytes_before(0),
    _allocation_context(AllocationContext::system()) { }

// 创建eden region，即为用户线程直接分配对象的region
HeapRegion* MutatorAllocRegion::allocate_new_region(size_t word_size,
                                                    bool force) {
  return _g1h->new_mutator_alloc_region(word_size, force);
}

/**
 * MutatorAllocRegion是G1AllocRegion的子类，这个方法 在 void G1AllocRegion::retire中调用
 * @param alloc_region
 * @param allocated_bytes
 */
void MutatorAllocRegion::retire_region(HeapRegion* alloc_region,
                                       size_t allocated_bytes) {
    /**
     * eden区域的retire调用的是retire_mutator_alloc_region这个方法，区别survivor和 old区域的region，调用的是 retire_gc_alloc_region
     */
  _g1h->retire_mutator_alloc_region(alloc_region, allocated_bytes);
}

// 创建survivor region，即作为survivor 的region
HeapRegion* SurvivorGCAllocRegion::allocate_new_region(size_t word_size,
                                                       bool force) {
  assert(!force, "not supported for GC alloc regions");
  return _g1h->new_gc_alloc_region(word_size, count(), InCSetState::Young);
}

/**
 * 在 void G1AllocRegion::retire 中调用
 * 比较  SurvivorGCAllocRegion::retire_region 和 OldGCAllocRegion::retire_region
 * @param alloc_region
 * @param allocated_bytes
 */
void SurvivorGCAllocRegion::retire_region(HeapRegion* alloc_region,
                                          size_t allocated_bytes) {
  _g1h->retire_gc_alloc_region(alloc_region, allocated_bytes, InCSetState::Young);
}

// 创建老年代 region，即作为老年代的region，可能来自年轻代Region的晋升，或者大对象的直接分配
HeapRegion* OldGCAllocRegion::allocate_new_region(size_t word_size,
                                                  bool force) {
  assert(!force, "not supported for GC alloc regions");
  return _g1h->new_gc_alloc_region(word_size, count(), InCSetState::Old);
}

/**
 * 在 void G1AllocRegion::retire 中调用
 * 比较  SurvivorGCAllocRegion::retire_region 和 OldGCAllocRegion::retire_region
 * @param alloc_region
 * @param allocated_bytes
 */
void OldGCAllocRegion::retire_region(HeapRegion* alloc_region,
                                     size_t allocated_bytes) {
  _g1h->retire_gc_alloc_region(alloc_region, allocated_bytes, InCSetState::Old);
}

HeapRegion* OldGCAllocRegion::release() {
  HeapRegion* cur = get();
  if (cur != NULL) {
    // Determine how far we are from the next card boundary. If it is smaller than
    // the minimum object size we can allocate into, expand into the next card.
    HeapWord* top = cur->top();
    HeapWord* aligned_top = (HeapWord*)align_ptr_up(top, G1BlockOffsetSharedArray::N_bytes);

    size_t to_allocate_words = pointer_delta(aligned_top, top, HeapWordSize);

    if (to_allocate_words != 0) {
      // We are not at a card boundary. Fill up, possibly into the next, taking the
      // end of the region and the minimum object size into account.
      to_allocate_words = MIN2(pointer_delta(cur->end(), cur->top(), HeapWordSize),
                               MAX2(to_allocate_words, G1CollectedHeap::min_fill_size()));

      // Skip allocation if there is not enough space to allocate even the smallest
      // possible object. In this case this region will not be retained, so the
      // original problem cannot occur.
      if (to_allocate_words >= G1CollectedHeap::min_fill_size()) {
        HeapWord* dummy = attempt_allocation(to_allocate_words, true /* bot_updates */);
        CollectedHeap::fill_with_object(dummy, to_allocate_words);
      }
    }
  }
  return G1AllocRegion::release();
}


