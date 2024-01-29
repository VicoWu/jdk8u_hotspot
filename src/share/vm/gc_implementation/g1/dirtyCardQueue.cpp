/*
 * Copyright (c) 2001, 2016, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/dirtyCardQueue.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/workgroup.hpp"

bool DirtyCardQueue::apply_closure(CardTableEntryClosure* cl,
                                   bool consume,
                                   uint worker_i) {
  bool res = true;
  if (_buf != NULL) {
    res = apply_closure_to_buffer(cl, _buf, _index, _sz,
                                  consume,
                                  worker_i);
    if (res && consume) _index = _sz;
  }
  return res;
}

/**
 * 搜索 bool DirtyCardQueueSet::
apply_closure_to_completed_buffer_helper 查看调用者
 buf中的数据是位于[index, sz]之间
 这是一个静态方法，用来对一个void** buf代表的转移专用记忆集合 去apply对应的Closure
 */
bool DirtyCardQueue::apply_closure_to_buffer(CardTableEntryClosure* cl,
                                             void** buf,
                                             size_t index, size_t sz,
                                             bool consume,
                                             uint worker_i) {
  if (cl == NULL) return true;
  for (size_t i = index; i < sz; i += oopSize) { // 处理从index 到 sz之间的所有的卡片，注意,sz的大小是以byte为单位，所以每次step的粒度是oopSize
    int ind = byte_index_to_index((int)i);
    jbyte* card_ptr = (jbyte*)buf[ind]; // 根据索引值获取对应的卡片地址，我们从prtQueue的enqueue中可以看到，在DCQ中存放的是引用者的地址,比如a.field=b，那么存放的是a的地址
    if (card_ptr != NULL) {
      // Set the entry to null, so we don't do it again (via the test
      // above) if we reconsider this buffer.
      if (consume) buf[ind] = NULL;
      /**
       * 调用对应的RefineRecordRefsIntoCSCardTableEntryClosure的do_card_ptr()方法
       * 搜索 bool do_card_ptr(jbyte* card_ptr, uint worker_i)
       */
      if (!cl->do_card_ptr(card_ptr, worker_i)) return false;
    }
  }
  return true;
}

#ifdef _MSC_VER // the use of 'this' below gets a warning, make it go away
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif // _MSC_VER

DirtyCardQueueSet::DirtyCardQueueSet(bool notify_when_complete) :
  PtrQueueSet(notify_when_complete),
  _mut_process_closure(NULL),
  _shared_dirty_card_queue(this, true /*perm*/),
  _free_ids(NULL),
  _processed_buffers_mut(0), _processed_buffers_rs_thread(0)
{
  _all_active = true;
}

// Determines how many mutator threads can process the buffers in parallel.
uint DirtyCardQueueSet::num_par_ids() {
  return (uint)os::initial_active_processor_count();
}

void DirtyCardQueueSet::initialize(CardTableEntryClosure* cl, Monitor* cbl_mon, Mutex* fl_lock,
                                   int process_completed_threshold,
                                   int max_completed_queue,
                                   Mutex* lock, PtrQueueSet* fl_owner) {
  _mut_process_closure = cl;
  PtrQueueSet::initialize(cbl_mon, fl_lock, process_completed_threshold,
                          max_completed_queue, fl_owner);
  set_buffer_size(G1UpdateBufferSize);
  /**
   * 设置锁变量，标记自己虽然是 PrtQueue，但是是一个属于DirtyCardQueueSet 的全局PrtQueue，是存在多线程竞争关系的
   * 在方法 DirtyCardQueueSet::concatenate_logs 中可以看到_shared_dirty_card_queue的使用
   */
  _shared_dirty_card_queue.set_lock(lock); // 实现方法参考 void set_lock(Mutex* lock) { _lock = lock; }
  _free_ids = new FreeIdSet((int) num_par_ids(), _cbl_mon);
}

void DirtyCardQueueSet::handle_zero_index_for_thread(JavaThread* t) {
  t->dirty_card_queue().handle_zero_index();
}

void DirtyCardQueueSet::iterate_closure_all_threads(CardTableEntryClosure* cl,
                                                    bool consume,
                                                    uint worker_i) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  for(JavaThread* t = Threads::first(); t; t = t->next()) {
    bool b = t->dirty_card_queue().apply_closure(cl, consume);
    guarantee(b, "Should not be interrupted.");
  }
  bool b = shared_dirty_card_queue()->apply_closure(cl,
                                                    consume,
                                                    worker_i);
  guarantee(b, "Should not be interrupted.");
}

bool DirtyCardQueueSet::mut_process_buffer(void** buf) {

  // Used to determine if we had already claimed a par_id
  // before entering this method.
  bool already_claimed = false;

  // We grab the current JavaThread.
  JavaThread* thread = JavaThread::current();

  // We get the the number of any par_id that this thread
  // might have already claimed.
  uint worker_i = thread->get_claimed_par_id();

  // If worker_i is not UINT_MAX then the thread has already claimed
  // a par_id. We make note of it using the already_claimed value
  if (worker_i != UINT_MAX) {
    already_claimed = true;
  } else {

    // Otherwise we need to claim a par id
    worker_i = _free_ids->claim_par_id();

    // And store the par_id value in the thread
    thread->set_claimed_par_id(worker_i);
  }

  bool b = false;
  if (worker_i != UINT_MAX) {
    b = DirtyCardQueue::apply_closure_to_buffer(_mut_process_closure, buf, 0,
                                                _sz, true, worker_i);
    if (b) Atomic::inc(&_processed_buffers_mut);

    // If we had not claimed an id before entering the method
    // then we must release the id.
    if (!already_claimed) {

      // we release the id
      _free_ids->release_par_id(worker_i);

      // and set the claimed_id in the thread to UINT_MAX
      thread->set_claimed_par_id(UINT_MAX);
    }
  }
  return b;
}


/**
 * 获取以DCQS中的以_completed_buffers_head为链表的第一个节点_completed_buffers_head
 */

BufferNode*
DirtyCardQueueSet::get_completed_buffer(int stop_at) {
  BufferNode* nd = NULL;
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  /**
   * 可以看到，如果_n_completed_buffers==0,即已经处理完了，此时stop_at=0，那么就会返回null
   */
  if ((int)_n_completed_buffers <= stop_at) {
    _process_completed = false;
    return NULL;
  }

  if (_completed_buffers_head != NULL) {
    nd = _completed_buffers_head;
    _completed_buffers_head = nd->next();
    if (_completed_buffers_head == NULL)
      _completed_buffers_tail = NULL;
    _n_completed_buffers--; // 取出了一个节点，因此数量减去1
    assert(_n_completed_buffers >= 0, "Invariant");
  }
  debug_only(assert_completed_buffer_list_len_correct_locked());
  return nd;
}

/**
 * 调用方法 搜索 DirtyCardQueueSet::apply_closure_to_completed_buffer
 * @param cl
 * @param worker_i
 * @param nd
 * @return
 */
bool DirtyCardQueueSet::
apply_closure_to_completed_buffer_helper(CardTableEntryClosure* cl,
                                         uint worker_i,
                                         BufferNode* nd) {
  if (nd != NULL) {
      /**
       * 根据当前的BufferNode获取对应的buf双指针
       * 一个void **buf 是属于一个PtrQueue对象的，而不是属于一个PtrQueueSet的
       */
    void **buf = BufferNode::make_buffer_from_node(nd); //
    size_t index = nd->index(); // 在**buf中的索引，当前的索引位置。前面说过，往一个buf中写入数据是从 _nd一直向0写数据，所以当前的有效数据位于[index, _sz]之间
    bool b =
            // 调用静态方法DirtyCardQueue::apply_closure_to_buffer， 对于这个buf，去apply对应的CardTableEntryClosure
      DirtyCardQueue::apply_closure_to_buffer(cl, buf,
                                              index, _sz,
                                              true, worker_i);
    if (b) {
      deallocate_buffer(buf); // 调用者是 void PtrQueueSet::deallocate_buffer(void** buf) {
      return true;  // In normal case, go on to next buffer.
    } else {
      enqueue_complete_buffer(buf, index); // 重新把刚刚的buf加入到DCQS的队列中
      return false;
    }
  } else {
    return false;
  }
}

/**
 * 获取这个DCQS在stop_at位置的BufferNode，然后在这个位置apply对应的CardTableEntryClosure(具体实现类是RefineRecordRefsIntoCSCardTableEntryClosure)
 * 调用位置 查看 G1CollectedHeap::iterate_dirty_card_closure，可以看到，当前的DCQS对象是Java_Thread的全局DCQS对象，
 *          这时候stop_at = 0，表示处理所有， 对应的closure 是 RefineRecordRefsIntoCSCardTableEntryClosure
 * 同时Refine现成也会调用这个方法来处理DCQS,调用方法在ConcurrentG1RefineThread::run 中，
 *          但是Refine现成处理的时候stop_at是green_zone，对应的closure 是 RefineCardTableEntryClosure
 * RefineRecordRefsIntoCSCardTableEntryClosure 和 RefineCardTableEntryClosure 都是CardTableEntryClosure的子类
 */
bool DirtyCardQueueSet::apply_closure_to_completed_buffer(CardTableEntryClosure* cl,
                                                          uint worker_i,
                                                          int stop_at,
                                                          bool during_pause) {
  assert(!during_pause || stop_at == 0, "Should not leave any completed buffers during a pause");
  BufferNode* nd = get_completed_buffer(stop_at); // 这里stop_at不起任何作用，get_completed_buffer永远只是把DCQS的头结点取出来，如果DCQS中没有元素了，返回null
  bool res = apply_closure_to_completed_buffer_helper(cl, worker_i, nd); // 如果nd==null，返回false
  if (res)
      Atomic::inc(&_processed_buffers_rs_thread);
  return res; // 返回值是res。 从上层调用者可以看到，只要返回true，证明DCQS中还有元素，从而反复调用这个方法apply_closure_to_completed_buffer()
}

void DirtyCardQueueSet::apply_closure_to_all_completed_buffers(CardTableEntryClosure* cl) {
  BufferNode* nd = _completed_buffers_head;
  while (nd != NULL) {
    bool b =
      DirtyCardQueue::apply_closure_to_buffer(cl,
                                              BufferNode::make_buffer_from_node(nd),
                                              0, _sz, false);
    guarantee(b, "Should not stop early.");
    nd = nd->next();
  }
}

void DirtyCardQueueSet::par_apply_closure_to_all_completed_buffers(CardTableEntryClosure* cl) {
  BufferNode* nd = _cur_par_buffer_node;
  while (nd != NULL) {
    BufferNode* next = (BufferNode*)nd->next();
    BufferNode* actual = (BufferNode*)Atomic::cmpxchg_ptr((void*)next, (volatile void*)&_cur_par_buffer_node, (void*)nd);
    if (actual == nd) {
      bool b =
        DirtyCardQueue::apply_closure_to_buffer(cl,
                                                BufferNode::make_buffer_from_node(actual),
                                                0, _sz, false);
      guarantee(b, "Should not stop early.");
      nd = next;
    } else {
      nd = actual;
    }
  }
}

// Deallocates any completed log buffers
void DirtyCardQueueSet::clear() {
  BufferNode* buffers_to_delete = NULL;
  {
    MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
    while (_completed_buffers_head != NULL) {
      BufferNode* nd = _completed_buffers_head;
      _completed_buffers_head = nd->next();
      nd->set_next(buffers_to_delete);
      buffers_to_delete = nd;
    }
    _n_completed_buffers = 0;
    _completed_buffers_tail = NULL;
    debug_only(assert_completed_buffer_list_len_correct_locked());
  }
  while (buffers_to_delete != NULL) {
    BufferNode* nd = buffers_to_delete;
    buffers_to_delete = nd->next();
    deallocate_buffer(BufferNode::make_buffer_from_node(nd));
  }

}

void DirtyCardQueueSet::abandon_logs() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  clear();
  // Since abandon is done only at safepoints, we can safely manipulate
  // these queues.
  for (JavaThread* t = Threads::first(); t; t = t->next()) {
    t->dirty_card_queue().reset();
  }
  shared_dirty_card_queue()->reset();
}


void DirtyCardQueueSet::concatenate_logs() {
  // Iterate over all the threads, if we find a partial log add it to
  // the global list of logs.  Temporarily turn off the limit on the number
  // of outstanding buffers.
  int save_max_completed_queue = _max_completed_queue;
  _max_completed_queue = max_jint;
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  for (JavaThread* t = Threads::first(); t; t = t->next()) {
    DirtyCardQueue& dcq = t->dirty_card_queue();
    if (dcq.size() != 0) {
      void **buf = t->dirty_card_queue().get_buf();
      // We must NULL out the unused entries, then enqueue.
      for (size_t i = 0; i < t->dirty_card_queue().get_index(); i += oopSize) {
        buf[PtrQueue::byte_index_to_index((int)i)] = NULL;
      }
      enqueue_complete_buffer(dcq.get_buf(), dcq.get_index());
      dcq.reinitialize();
    }
  }
  if (_shared_dirty_card_queue.size() != 0) {
    enqueue_complete_buffer(_shared_dirty_card_queue.get_buf(),
                            _shared_dirty_card_queue.get_index());
    _shared_dirty_card_queue.reinitialize();
  }
  // Restore the completed buffer queue limit.
  _max_completed_queue = save_max_completed_queue;
}
