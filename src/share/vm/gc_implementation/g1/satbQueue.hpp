/*
 * Copyright (c) 2001, 2015, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_SATBQUEUE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_SATBQUEUE_HPP

#include "memory/allocation.hpp"
#include "gc_implementation/g1/ptrQueue.hpp"

class JavaThread;
class SATBMarkQueueSet;

// Base class for processing the contents of a SATB buffer.
class SATBBufferClosure : public StackObj {
protected:
  ~SATBBufferClosure() { }

public:
  // Process the SATB entries in the designated buffer range.
  virtual void do_buffer(void** buffer, size_t size) = 0;
};

// A ptrQueue whose elements are "oops", pointers to object heads.
/**
 * 一个指向对象头不的ordinary object pointers，即指向对象头部的普通对象头部指针
 * PtrQueue有两个子类，ObjPtrQueue和DirtyCardQueue，分别负责SATB和转移专用写屏障的写操作的线程本地队列，
 * 他们都有对应的全局Queue Set， 分别叫做 SATBMarkQueueSet 和 DirtyCardQueueSet，这些QueueSet里面都有对应的一个全局的PtrQueue的对象
 *     并且都有对应的QueueSet来负责全局的队列
 */
class ObjPtrQueue: public PtrQueue {
  friend class SATBMarkQueueSet;

private:
  // Filter out unwanted entries from the buffer.
  void filter();

public:
  ObjPtrQueue(PtrQueueSet* qset, bool perm = false) :
    // SATB queues are only active during marking cycles. We create
    // them with their active field set to false. If a thread is
    // created during a cycle and its SATB queue needs to be activated
    // before the thread starts running, we'll need to set its active
    // field to true. This is done in JavaThread::initialize_queues().
    PtrQueue(qset, perm, false /* active */) { }

  // Process queue entries and free resources.
  void flush();

  // Apply cl to the active part of the buffer.
  // Prerequisite: Must be at a safepoint.
  void apply_closure_and_empty(SATBBufferClosure* cl);

  // Overrides PtrQueue::should_enqueue_buffer(). See the method's
  // definition for more information.
  virtual bool should_enqueue_buffer();

#ifndef PRODUCT
  // Helpful for debugging
  void print(const char* name);
  static void print(const char* name, void** buf, size_t index, size_t sz);
#endif // PRODUCT
};

class SATBMarkQueueSet: public PtrQueueSet {
  ObjPtrQueue _shared_satb_queue;

#ifdef ASSERT
  void dump_active_states(bool expected_active);
  void verify_active_states(bool expected_active);
#endif // ASSERT

public:
  SATBMarkQueueSet();

  void initialize(Monitor* cbl_mon, Mutex* fl_lock,
                  int process_completed_threshold,
                  Mutex* lock);

  static void handle_zero_index_for_thread(JavaThread* t);

  // Apply "set_active(active)" to all SATB queues in the set. It should be
  // called only with the world stopped. The method will assert that the
  // SATB queues of all threads it visits, as well as the SATB queue
  // set itself, has an active value same as expected_active.
  void set_active_all_threads(bool active, bool expected_active);

  // Filter all the currently-active SATB buffers.
  void filter_thread_buffers();

  // If there exists some completed buffer, pop and process it, and
  // return true.  Otherwise return false.  Processing a buffer
  // consists of applying the closure to the buffer range starting
  // with the first non-NULL entry to the end of the buffer; the
  // leading entries may be NULL due to filtering.
  bool apply_closure_to_completed_buffer(SATBBufferClosure* cl);

#ifndef PRODUCT
  // Helpful for debugging
  void print_all(const char* msg);
#endif // PRODUCT

  ObjPtrQueue* shared_satb_queue() { return &_shared_satb_queue; }

  // If a marking is being abandoned, reset any unprocessed log buffers.
  void abandon_partial_marking();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_SATBQUEUE_HPP
