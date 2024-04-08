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

#include "precompiled.hpp"
#include "gc_implementation/g1/ptrQueue.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"


/**
 * PtrQueue是ObjPtrQueue和DirtyCardQueue的父类
 * @param qset
 * @param perm
 * @param active
 */
PtrQueue::PtrQueue(PtrQueueSet* qset, bool perm, bool active) :
  _qset(qset), _buf(NULL), _index(0), _sz(0), _active(active),
  _perm(perm), _lock(NULL)
{}

PtrQueue::~PtrQueue() {
  assert(_perm || (_buf == NULL), "queue must be flushed before delete");
}

void PtrQueue::flush_impl() {
  if (!_perm && _buf != NULL) {
    if (_index == _sz) {
      // No work to do.
      qset()->deallocate_buffer(_buf);
    } else {
      // We must NULL out the unused entries, then enqueue.
      for (size_t i = 0; i < _index; i += oopSize) {
        _buf[byte_index_to_index((int)i)] = NULL;
      }
      qset()->enqueue_complete_buffer(_buf);
    }
    _buf = NULL;
    _index = 0;
  }
}

/**
 * 将对象放入到一个已知是active的DCQ中去。这个DCQ可能已经满了，这时候就需要把DCQ添加到DCQS中，并申请新的DCQ
 * 查看调用者 void enqueue(void* ptr)
 */
void PtrQueue::enqueue_known_active(void* ptr) {
  assert(0 <= _index && _index <= _sz, "Invariant.");
  assert(_index == 0 || _buf != NULL, "invariant");

  while (_index == 0) { // _index == 0代表当前的DCQ已经没有空间了
    handle_zero_index(); // 当前DCQ没有空间了，那么就将DCQ添加到DCQS中，并申请新的DCQ，具体实现搜索 PtrQueue::handle_zero_index
  }

  assert(_index > 0, "postcondition");
  /**
   * 在DirtyCardQueueSet::initialize 中调用了set_buffer_size()方法， 代表了一个DCQ的大小
   * 用户通过参数 G1UpdateBufferSize(默认256)设置了一个buffer的大小，假如oopSize=8（oop指的是 "ordinary object pointer"，即普通对象指针），那么_sz = 256 * 8 = 2048
   * 所以我们往DCQ中插入一个元素的时候，index = index - oopSize
   */
  _index -= oopSize; // 从这里可以看到，这个_index的值是随着插入的进行从大逐渐减小到0，因此 可以通过_index==0确定buffer已经满了
  _buf[byte_index_to_index((int)_index)] = ptr;
  assert(0 <= _index && _index <= _sz, "Invariant.");
}

/**
 * 这个方法会负责将当前的DCQ(buf指向的位置)添加到全局的DCQS中去
 * @param buf
 */
void PtrQueue::locking_enqueue_completed_buffer(void** buf) {
  assert(_lock->owned_by_self(), "Required.");

  // We have to unlock _lock (which may be Shared_DirtyCardQ_lock) before
  // we acquire DirtyCardQ_CBL_mon inside enqeue_complete_buffer as they
  // have the same rank and we may get the "possible deadlock" message
  _lock->unlock();

  // 关于qset()，搜索 PtrQueueSet* qset()
  qset()->enqueue_complete_buffer(buf); // 将当前的DCQ添加到全局的DCQS中去
  // We must relock only because the caller will unlock, for the normal
  // case.
  _lock->lock_without_safepoint_check();
}


PtrQueueSet::PtrQueueSet(bool notify_when_complete) :
  _max_completed_queue(0),
  _cbl_mon(NULL), _fl_lock(NULL),
  _notify_when_complete(notify_when_complete),
  _sz(0),
  _completed_buffers_head(NULL),
  _completed_buffers_tail(NULL),
  _n_completed_buffers(0),
  _process_completed_threshold(0), _process_completed(false),
  _buf_free_list(NULL), _buf_free_list_sz(0)
{
  _fl_owner = this;
}

/**
 * 分配一个DCQ, 返回这个DCQ的buf部分。在方法 PtrQueue::handle_zero_index 中调用
 *  这个DCQ可能是直接从_buf_free_list中来，也可能是现场创建出来的
 * @return
 */
void** PtrQueueSet::allocate_buffer() {
  assert(_sz > 0, "Didn't set a buffer size.");
  MutexLockerEx x(_fl_owner->_fl_lock, Mutex::_no_safepoint_check_flag);
  if (_fl_owner->_buf_free_list != NULL) { // 如果自由列表 _buf_free_list 不为空，
    void** res = BufferNode::make_buffer_from_node(_fl_owner->_buf_free_list); // 从自由列表中获取一个可用的缓冲区
    _fl_owner->_buf_free_list = _fl_owner->_buf_free_list->next(); // 之前的头节点已经被取出来占用了，更新头结点为之前头结点的next节点
    _fl_owner->_buf_free_list_sz--; // 计数器减去1
    return res;
  } else {  // 如果自由列表 _buf_free_list 为空，
    // Allocate space for the BufferNode in front of the buffer.
    // 将BufferNode对象分配在缓冲区前面，BufferNode对象相当于缓冲区的元数据，而把buffer区域放在BufferNode的后面，它俩共同组成了一个Block
    char *b =  NEW_C_HEAP_ARRAY(char, _sz + BufferNode::aligned_size(), mtGC); // 分配一块大小为 _sz + BufferNode::aligned_size() 字节的堆内存
    return BufferNode::make_buffer_from_block(b); // Block由两部分组成，前面是 BufferNode,后面是buffer
  }
}

void PtrQueueSet::deallocate_buffer(void** buf) {
  assert(_sz > 0, "Didn't set a buffer size.");
  MutexLockerEx x(_fl_owner->_fl_lock, Mutex::_no_safepoint_check_flag);
  BufferNode *node = BufferNode::make_node_from_buffer(buf);
  node->set_next(_fl_owner->_buf_free_list);
  _fl_owner->_buf_free_list = node;
  _fl_owner->_buf_free_list_sz++;
}

void PtrQueueSet::reduce_free_list() {
  assert(_fl_owner == this, "Free list reduction is allowed only for the owner");
  // For now we'll adopt the strategy of deleting half.
  MutexLockerEx x(_fl_lock, Mutex::_no_safepoint_check_flag);
  size_t n = _buf_free_list_sz / 2;
  while (n > 0) {
    assert(_buf_free_list != NULL, "_buf_free_list_sz must be wrong.");
    void* b = BufferNode::make_block_from_node(_buf_free_list);
    _buf_free_list = _buf_free_list->next();
    FREE_C_HEAP_ARRAY(char, b, mtGC);
    _buf_free_list_sz --;
    n--;
  }
}

/**
 * 查看调用方： PtrQueue::enqueue_known_active
 * 这个方法的调用发生在DCQ满了的的情况，这时候就将DCQ放到DCQS中去，并申请新的DCQ
 * 这个PtrQueue可能是普通的DCQ，也有可能是 DCQS中的 Shared Dirty Card Queue
 */
void PtrQueue::handle_zero_index() {
  assert(_index == 0, "Precondition.");

  // This thread records the full buffer and allocates a new one (while
  // holding the lock if there is one).
  if (_buf != NULL) {
    if (!should_enqueue_buffer()) { // 是否需要将当前的DCQ添加到DCQS中去
      assert(_index > 0, "the buffer can only be re-used if it's not full");
      return;
    }

    /**
     * 如果当前线程持有锁,PrtQueue持有锁的情况发生在DCQS中的共享的_shared_dirty_card_queue
     * _shared_dirty_card_queue在 G1SATBCardTableLoggingModRefBS::write_ref_field_work中添加了元素
     */

    if (_lock) {
      assert(_lock->owned_by_self(), "Required.");

      // The current PtrQ may be the shared dirty card queue and
      // may be being manipulated by more than one worker thread
      // during a pause. Since the enqueuing of the completed
      // buffer unlocks the Shared_DirtyCardQ_lock more than one
      // worker thread can 'race' on reading the shared queue attributes
      // (_buf and _index) and multiple threads can call into this
      // routine for the same buffer. This will cause the completed
      // buffer to be added to the CBL multiple times.

      // We "claim" the current buffer by caching value of _buf in
      // a local and clearing the field while holding _lock. When
      // _lock is released (while enqueueing the completed buffer)
      // the thread that acquires _lock will skip this code,
      // preventing the subsequent the multiple enqueue, and
      // install a newly allocated buffer below.

      void** buf = _buf;   // local pointer to completed buffer
      _buf = NULL;         // clear shared _buf field

      /**
       * 将当前的buf插入到全局的DCQS中去
       */
      locking_enqueue_completed_buffer(buf);  // enqueue completed buffer

      // While the current thread was enqueuing the buffer another thread
      // may have a allocated a new buffer and inserted it into this pointer
      // queue. If that happens then we just return so that the current
      // thread doesn't overwrite the buffer allocated by the other thread
      // and potentially losing some dirtied cards.

      if (_buf != NULL) return;
    } else {
      if (qset()->process_or_enqueue_complete_buffer(_buf)) {
        // Recycle the buffer. No allocation.
        _sz = qset()->buffer_size();
        _index = _sz;
        return;
      }
    }
  }
  // Reallocate the buffer
  _buf = qset()->allocate_buffer(); // 分配新的DCQ, 搜索 PtrQueueSet::allocate_buffer
  _sz = qset()->buffer_size(); // 搜索 PtrQueueSet::buffer_size
  _index = _sz; // _index指示的是_buf中待插入的位置，因此可以看到插入是从后往前插入的，因此可以通过_index==0判断_buf是否满了
  assert(0 <= _index && _index <= _sz, "Invariant.");
}

bool PtrQueueSet::process_or_enqueue_complete_buffer(void** buf) {
  if (Thread::current()->is_Java_thread()) { // 对于Java线程，如果当前已经完成的buffer超过了一定限度
    // We don't lock. It is fine to be epsilon-precise here.
    if (_max_completed_queue == 0 || _max_completed_queue > 0 &&
        _n_completed_buffers >= _max_completed_queue + _completed_queue_padding) {
     /**
      * 我们发现当前已经添加到DCQS中的buffer的数量已经大于_max_completed_queue + _completed_queue_padding， 那么这时候只能自己处理这个buf了
      * Mutator线程自己处理这个buffer，其实就是同步处理这个buf，而不是交给DCQS去异步处理，
      *     就是调用方法DirtyCardQueueSet::mut_process_buffer(void** buf)
      */
      bool b = mut_process_buffer(buf);
      if (b) {
        // True here means that the buffer hasn't been deallocated and the caller may reuse it.
        return true;
      }
    }
  }
  // The buffer will be enqueued. The caller will have to get a new one.
  enqueue_complete_buffer(buf);
  return false;
}

/**
 * PtrQueueSet是DirtyCardQueueSet的父类，这个方法其实就是DirtyCardQueueSet::enqueue_complete_buffer()
 *   用来将当前的DCQ添加到全局的DCQS中去
 * 这个方法的调用者是 PtrQueueSet::process_or_enqueue_complete_buffer
 * 查看enqueue_complete_buffer()的方法声明可以看到，用户客户已只提供第一个参数buf，这时候index会使用默认0的值
 */
void PtrQueueSet::enqueue_complete_buffer(void** buf, size_t index) {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  BufferNode* cbn = BufferNode::new_from_buffer(buf); // 搜索 BufferNode* new_from_buffer
  cbn->set_index(index);
  // 将这个BufferNode添加到链表的末尾
  if (_completed_buffers_tail == NULL) { // 第一个节点
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = cbn;
    _completed_buffers_tail = cbn;
  } else { // 不是第一个节点，将cbn append到链表的末尾
    _completed_buffers_tail->set_next(cbn);
    _completed_buffers_tail = cbn;
  }
  _n_completed_buffers++; // 更新已经添加到全局 DCQS 的 buffer数量的统计信息

  if (!_process_completed && _process_completed_threshold >= 0 &&
      _n_completed_buffers >= _process_completed_threshold) {
    _process_completed = true;
    if (_notify_when_complete)
      _cbl_mon->notify();
  }
  debug_only(assert_completed_buffer_list_len_correct_locked());
}

int PtrQueueSet::completed_buffers_list_length() {
  int n = 0;
  BufferNode* cbn = _completed_buffers_head;
  while (cbn != NULL) {
    n++;
    cbn = cbn->next();
  }
  return n;
}

void PtrQueueSet::assert_completed_buffer_list_len_correct() {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  assert_completed_buffer_list_len_correct_locked();
}

void PtrQueueSet::assert_completed_buffer_list_len_correct_locked() {
  guarantee(completed_buffers_list_length() ==  _n_completed_buffers,
            "Completed buffer length is wrong.");
}

/**
 * 在DirtyCardQueueSet::initialize 中调用了set_buffer_size()方法， 代表了一个DCQ的大小
 * 用户通过参数 G1UpdateBufferSize(默认256)设置了一个buffer的大小，假如oopSize=8（oop指的是 "ordinary object pointer"，即普通对象指针），那么_sz = 256 * 8 = 2048
 * 所以我们往DCQ中插入一个元素的时候，index = index - oopSize
 */
void PtrQueueSet::set_buffer_size(size_t sz) {
  assert(_sz == 0 && sz > 0, "Should be called only once.");
  _sz = sz * oopSize; // 用户
}

// Merge lists of buffers. Notify the processing threads.
// The source queue is emptied as a result. The queues
// must share the monitor.
void PtrQueueSet::merge_bufferlists(PtrQueueSet *src) {
  assert(_cbl_mon == src->_cbl_mon, "Should share the same lock");
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (_completed_buffers_tail == NULL) {
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = src->_completed_buffers_head;
    _completed_buffers_tail = src->_completed_buffers_tail;
  } else {
    assert(_completed_buffers_head != NULL, "Well formedness");
    if (src->_completed_buffers_head != NULL) {
      _completed_buffers_tail->set_next(src->_completed_buffers_head);
      _completed_buffers_tail = src->_completed_buffers_tail;
    }
  }
  _n_completed_buffers += src->_n_completed_buffers;

  src->_n_completed_buffers = 0;
  src->_completed_buffers_head = NULL;
  src->_completed_buffers_tail = NULL;

  assert(_completed_buffers_head == NULL && _completed_buffers_tail == NULL ||
         _completed_buffers_head != NULL && _completed_buffers_tail != NULL,
         "Sanity");
}

void PtrQueueSet::notify_if_necessary() {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (_n_completed_buffers >= _process_completed_threshold || _max_completed_queue == 0) {
    _process_completed = true;
    if (_notify_when_complete)
      _cbl_mon->notify();
  }
}
