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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_PTRQUEUE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_PTRQUEUE_HPP

#include "memory/allocation.hpp"
#include "utilities/sizes.hpp"

// There are various techniques that require threads to be able to log
// addresses.  For example, a generational write barrier might log
// the addresses of modified old-generation objects.  This type supports
// this operation.

// The definition of placement operator new(size_t, void*) in the <new>.
#include <new>

class PtrQueueSet;
/**
 * PtrQueue有两个子类，ObjPtrQueue和DirtyCardQueue，分别负责SATB和转移专用写屏障的写操作的线程本地队列，并且都有对应的QueueSet来负责全局的队列
 */
class PtrQueue VALUE_OBJ_CLASS_SPEC {
  friend class VMStructs;

protected:
  // The ptr queue set to which this queue belongs.
  PtrQueueSet* _qset;

  // Whether updates should be logged.
  bool _active;

  // The buffer.
  void** _buf;
  // The index at which an object was last enqueued.  Starts at "_sz"
  // (indicating an empty buffer) and goes towards zero.
  size_t _index;  // 最近入队列的元素的位置。元素从_sz进入队列，然后往0的位置逐渐推进

  // The size of the buffer.
  size_t _sz; // 这个DCQ的大小，

  // If true, the queue is permanent, and doesn't need to deallocate
  // its buffer in the destructor (since that obtains a lock which may not
  // be legally locked by then.
  bool _perm;

  // If there is a lock associated with this buffer, this is that lock.
  Mutex* _lock;

  PtrQueueSet* qset() { return _qset; }
  bool is_permanent() const { return _perm; }

  // Process queue entries and release resources, if not permanent.
  void flush_impl();

public:
  // Initialize this queue to contain a null buffer, and be part of the
  // given PtrQueueSet.
  PtrQueue(PtrQueueSet* qset, bool perm = false, bool active = false);

  // Requires queue flushed or permanent.
  ~PtrQueue();

  // Associate a lock with a ptr queue.
  void set_lock(Mutex* lock) { _lock = lock; }

  void reset() { if (_buf != NULL) _index = _sz; }

  void enqueue(volatile void* ptr) {
    enqueue((void*)(ptr));
  }

  /**
   * 调用类查看 G1SATBCardTableLoggingModRefBS::write_ref_field_work
   */
  // Enqueues the given "obj".
  void enqueue(void* ptr) { // 将对象放入到DCQ中
    if (!_active) return;
    else enqueue_known_active(ptr); // 查看 PtrQueue::enqueue_known_active. enqueue_known_active()负责将对象放入到一个已知是active的DCQ中去。这个DCQ可能已经满了，这时候就需要把DCQ添加到DCQS中，并申请新的DCQ
  }

  // This method is called when we're doing the zero index handling
  // and gives a chance to the queues to do any pre-enqueueing
  // processing they might want to do on the buffer. It should return
  // true if the buffer should be enqueued, or false if enough
  // entries were cleared from it so that it can be re-used. It should
  // not return false if the buffer is still full (otherwise we can
  // get into an infinite loop).
  virtual bool should_enqueue_buffer() { return true; }
  void handle_zero_index();
  void locking_enqueue_completed_buffer(void** buf);

  void enqueue_known_active(void* ptr); // 查看 PtrQueue::enqueue_known_active

  size_t size() {
    assert(_sz >= _index, "Invariant.");
    return _buf == NULL ? 0 : _sz - _index;
  }

  bool is_empty() {
    return _buf == NULL || _sz == _index;
  }

  // Set the "active" property of the queue to "b".  An enqueue to an
  // inactive thread is a no-op.  Setting a queue to inactive resets its
  // log to the empty state.
  void set_active(bool b) {
    _active = b;
    if (!b && _buf != NULL) {
      _index = _sz;
    } else if (b && _buf != NULL) {
      assert(_index == _sz, "invariant: queues are empty when activated.");
    }
  }

  bool is_active() { return _active; }

  static int byte_index_to_index(int ind) {
    assert((ind % oopSize) == 0, "Invariant.");
    return ind / oopSize;
  }

  static int index_to_byte_index(int byte_ind) {
    return byte_ind * oopSize;
  }

  // To support compiler.
  static ByteSize byte_offset_of_index() {
    return byte_offset_of(PtrQueue, _index);
  }
  static ByteSize byte_width_of_index() { return in_ByteSize(sizeof(size_t)); }

  static ByteSize byte_offset_of_buf() {
    return byte_offset_of(PtrQueue, _buf);
  }
  static ByteSize byte_width_of_buf() { return in_ByteSize(sizeof(void*)); }

  static ByteSize byte_offset_of_active() {
    return byte_offset_of(PtrQueue, _active);
  }
  static ByteSize byte_width_of_active() { return in_ByteSize(sizeof(bool)); }

};

class BufferNode {
  size_t _index;
  BufferNode* _next;
public:
  BufferNode() : _index(0), _next(NULL) { }
  BufferNode* next() const     { return _next;  }
  void set_next(BufferNode* n) { _next = n;     }
  size_t index() const         { return _index; }
  void set_index(size_t i)     { _index = i;    }

  // Align the size of the structure to the size of the pointer
  // 将BufferNode对象的大小对齐到当前系统的指针大小的整数倍， 例如，如果sizeof(BufferNode) = 13， sizeof(void *) = 4
  //    调用 round_to(13, 4)，表示将 13 向上舍入到最接近的 4 的倍数，结果为 16。
  static size_t aligned_size() {
    static const size_t alignment = round_to(sizeof(BufferNode), sizeof(void*));
    return alignment;
  }

  // BufferNode is allocated before the buffer.
  // The chunk of memory that holds both of them is a block.

  // Produce a new BufferNode given a buffer.
  /**
   * BufferNode 在buffer之前分配。 容纳它们的内存区域是一个Block。
   * 根据提供的buf来构造一个BufferNode对象，这个BufferNode对象将添加到全局的DCQS中去
   * BufferNode对象是在buf前面的，因此需要调用make_block_from_buffer(buf)，返回BufferNode的其实地址，并
   *    在这个地址上面构造BufferNode，从而，BufferNode对象和buf数据区相邻
   * @param buf
   * @return
   */
  static BufferNode* new_from_buffer(void** buf) { //
    return new (make_block_from_buffer(buf)) BufferNode; // 在由 make_block_from_buffer(buf) 返回的地址上创建一个 BufferNode 对象。
  }

  // The following are the required conversion routines:
  static BufferNode* make_node_from_buffer(void** buf) {
    return (BufferNode*)make_block_from_buffer(buf);
  }
  /**
   * 从一个BufferNode指针，返回其后面的buffer。
   * 上面说过，BufferNode和其对应的buf在内存上是连续的
   */
  static void** make_buffer_from_node(BufferNode *node) {
    return make_buffer_from_block(node); // 返回node对应的buf的地址
  }

  /**
   * 将BufferNode指针直接转换成void *指针，这可以说明，Block的位置和BufferNode的起始位置是同一个位置
   *    但是这个位置往后移动aligned_size()才是真正的数据区Buffer的位置
   * @param node
   * @return
   */
  static void* make_block_from_node(BufferNode *node) {
    return (void*)node;
  }
  /**
   * 接受一个指向缓冲区起始位置的指针 p，然后将其向后偏移 aligned_size() 个字节，以找到缓冲区中实际数据存储区域的起始地址
   * @param p
   * @return
   */
  static void** make_buffer_from_block(void* p) {
    return (void**)((char*)p + aligned_size());
  }
  /**
   * 接受一个指向缓冲区中实际数据存储区域的起始位置的指针 p，然后将其向前偏移 aligned_size() 个字节，以找到缓冲区的起始地址
   * @param p
   * @return
   */
  static void* make_block_from_buffer(void** p) {
    return (void*)((char*)p - aligned_size());
  }
};

// A PtrQueueSet represents resources common to a set of pointer queues.
// In particular, the individual queues allocate buffers from this shared
// set, and return completed buffers to the set.
// All these variables are are protected by the TLOQ_CBL_mon. XXX ???
class PtrQueueSet VALUE_OBJ_CLASS_SPEC {
protected:
  Monitor* _cbl_mon;  // Protects the fields below.
  BufferNode* _completed_buffers_head;
  BufferNode* _completed_buffers_tail;
  int _n_completed_buffers; // 搜索 void PtrQueueSet::enqueue_complete_buffer查看更新_n_completed_buffers的值
  int _process_completed_threshold;
  volatile bool _process_completed;

  // This (and the interpretation of the first element as a "next"
  // pointer) are protected by the TLOQ_FL_lock.
  Mutex* _fl_lock;
  BufferNode* _buf_free_list;
  size_t _buf_free_list_sz;
  // Queue set can share a freelist. The _fl_owner variable
  // specifies the owner. It is set to "this" by default.
  PtrQueueSet* _fl_owner;

  // The size of all buffers in the set.
  size_t _sz;

  bool _all_active;

  // If true, notify_all on _cbl_mon when the threshold is reached.
  bool _notify_when_complete;

  // Maximum number of elements allowed on completed queue: after that,
  // enqueuer does the work itself.  Zero indicates no maximum.
  int _max_completed_queue;
  int _completed_queue_padding;

  int completed_buffers_list_length();
  void assert_completed_buffer_list_len_correct_locked();
  void assert_completed_buffer_list_len_correct();

protected:
  // A mutator thread does the the work of processing a buffer.
  // Returns "true" iff the work is complete (and the buffer may be
  // deallocated).
  virtual bool mut_process_buffer(void** buf) {
    ShouldNotReachHere();
    return false;
  }

public:
  // Create an empty ptr queue set.
  PtrQueueSet(bool notify_when_complete = false);

  // Because of init-order concerns, we can't pass these as constructor
  // arguments.
  void initialize(Monitor* cbl_mon, Mutex* fl_lock,
                  int process_completed_threshold,
                  int max_completed_queue,
                  PtrQueueSet *fl_owner = NULL) {
    _max_completed_queue = max_completed_queue;
    _process_completed_threshold = process_completed_threshold;
    _completed_queue_padding = 0;
    assert(cbl_mon != NULL && fl_lock != NULL, "Init order issue?");
    _cbl_mon = cbl_mon;
    _fl_lock = fl_lock;
    _fl_owner = (fl_owner != NULL) ? fl_owner : this;
  }

  // Return an empty oop array of size _sz (required to be non-zero).
  void** allocate_buffer();

  // Return an empty buffer to the free list.  The "buf" argument is
  // required to be a pointer to the head of an array of length "_sz".
  void deallocate_buffer(void** buf);

  // Declares that "buf" is a complete buffer.
  void enqueue_complete_buffer(void** buf, size_t index = 0);

  // To be invoked by the mutator.
  bool process_or_enqueue_complete_buffer(void** buf);

  bool completed_buffers_exist_dirty() {
    return _n_completed_buffers > 0;
  }

  bool process_completed_buffers() { return _process_completed; }
  void set_process_completed(bool x) { _process_completed = x; }

  bool is_active() { return _all_active; }

  // Set the buffer size.  Should be called before any "enqueue" operation
  // can be called.  And should only be called once.
  void set_buffer_size(size_t sz);

  // Get the buffer size.
  size_t buffer_size() { return _sz; }

  // Get/Set the number of completed buffers that triggers log processing.
  void set_process_completed_threshold(int sz) { _process_completed_threshold = sz; }
  int process_completed_threshold() const { return _process_completed_threshold; }

  // Must only be called at a safe point.  Indicates that the buffer free
  // list size may be reduced, if that is deemed desirable.
  void reduce_free_list();

  int completed_buffers_num() { return _n_completed_buffers; }

  void merge_bufferlists(PtrQueueSet* src);

  void set_max_completed_queue(int m) { _max_completed_queue = m; }
  int max_completed_queue() { return _max_completed_queue; }

  void set_completed_queue_padding(int padding) { _completed_queue_padding = padding; }
  int completed_queue_padding() { return _completed_queue_padding; }

  // Notify the consumer if the number of buffers crossed the threshold
  void notify_if_necessary();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_PTRQUEUE_HPP
