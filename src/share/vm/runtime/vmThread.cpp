/*
 * Copyright (c) 1998, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "compiler/compileBroker.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/resourceArea.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vm_operations.hpp"
#include "services/runtimeService.hpp"
#include "trace/tracing.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/events.hpp"
#include "utilities/xmlstream.hpp"

#ifndef USDT2
HS_DTRACE_PROBE_DECL3(hotspot, vmops__request, char *, uintptr_t, int);
HS_DTRACE_PROBE_DECL3(hotspot, vmops__begin, char *, uintptr_t, int);
HS_DTRACE_PROBE_DECL3(hotspot, vmops__end, char *, uintptr_t, int);
#endif /* !USDT2 */

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC

// Dummy VM operation to act as first element in our circular double-linked list
class VM_Dummy: public VM_Operation {
  VMOp_Type type() const { return VMOp_Dummy; }
  void  doit() {};
};

VMOperationQueue::VMOperationQueue() {
  // The queue is a circular doubled-linked list, which always contains
  // one element (i.e., one element means empty).
  for(int i = 0; i < nof_priorities; i++) {
    _queue_length[i] = 0;
    _queue_counter = 0;
    _queue[i] = new VM_Dummy();
    _queue[i]->set_next(_queue[i]);
    _queue[i]->set_prev(_queue[i]);
  }
  _drain_list = NULL;
}


bool VMOperationQueue::queue_empty(int prio) {
  // It is empty if there is exactly one element
  bool empty = (_queue[prio] == _queue[prio]->next());
  assert( (_queue_length[prio] == 0 && empty) ||
          (_queue_length[prio] > 0  && !empty), "sanity check");
  return _queue_length[prio] == 0;
}

// Inserts an element to the right of the q element
void VMOperationQueue::insert(VM_Operation* q, VM_Operation* n) {
  assert(q->next()->prev() == q && q->prev()->next() == q, "sanity check");
  n->set_prev(q);
  n->set_next(q->next());
  q->next()->set_prev(n);
  q->set_next(n);
}

void VMOperationQueue::queue_add_front(int prio, VM_Operation *op) {
  _queue_length[prio]++;
  insert(_queue[prio]->next(), op);
}

void VMOperationQueue::queue_add_back(int prio, VM_Operation *op) {
  _queue_length[prio]++;
  insert(_queue[prio]->prev(), op);
}


void VMOperationQueue::unlink(VM_Operation* q) {
  assert(q->next()->prev() == q && q->prev()->next() == q, "sanity check");
  q->prev()->set_next(q->next());
  q->next()->set_prev(q->prev());
}

VM_Operation* VMOperationQueue::queue_remove_front(int prio) {
  if (queue_empty(prio)) return NULL;
  assert(_queue_length[prio] >= 0, "sanity check");
  _queue_length[prio]--;
  VM_Operation* r = _queue[prio]->next();
  assert(r != _queue[prio], "cannot remove base element");
  unlink(r);
  return r;
}

/**
 * 搜索 enum Priorities 查看 queue_drain的prio参数
 * 该参数返回一个 VM_Operation ,其实是对应的priority的VM_Operation链表的头结点，通过next指针获取这个链表中的所有VM_Operation
 * @param prio
 * @return
 */
VM_Operation* VMOperationQueue::queue_drain(int prio) {
  if (queue_empty(prio)) return NULL; // 指定优先级的队列是否为空。如果队列为空，则直接返回 NULL，表示没有可供处理的操作。
  DEBUG_ONLY(int length = _queue_length[prio];);
  assert(length >= 0, "sanity check");
  _queue_length[prio] = 0;
    // 从队列中获取第一个操作对象 r，即通过 next() 方法获取 _queue[prio] 的下一个元素。
  // 此处 _queue[prio] 是队列的基准元素（基准节点），确保它不会作为普通的操作对象被移除。
  VM_Operation* r = _queue[prio]->next();
  assert(r != _queue[prio], "cannot remove base element");
  // remove links to base element from head and tail
  r->set_prev(NULL); // 将第一个操作对象的前驱指针设置为 NULL，断开其与基准节点的连接。
  _queue[prio]->prev()->set_next(NULL); // 将队列末尾的操作对象的 next() 指针也设置为 NULL，断开尾部与基准节点的连接
  // restore queue to empty state
  _queue[prio]->set_next(_queue[prio]); // 将 _queue[prio]（即基准节点）的 next() 和 prev() 指针都重新指向自身，表示队列现在为空。
  _queue[prio]->set_prev(_queue[prio]);
  assert(queue_empty(prio), "drain corrupted queue");
#ifdef ASSERT
  int len = 0;
  VM_Operation* cur;
  for(cur = r; cur != NULL; cur=cur->next()) len++;
  assert(len == length, "drain lost some ops");
#endif
  return r;
}

void VMOperationQueue::queue_oops_do(int queue, OopClosure* f) {
  VM_Operation* cur = _queue[queue];
  cur = cur->next();
  while (cur != _queue[queue]) {
    cur->oops_do(f);
    cur = cur->next();
  }
}

void VMOperationQueue::drain_list_oops_do(OopClosure* f) {
  VM_Operation* cur = _drain_list;
  while (cur != NULL) {
    cur->oops_do(f);
    cur = cur->next();
  }
}

//-----------------------------------------------------------------
// High-level interface
bool VMOperationQueue::add(VM_Operation *op) {

#ifndef USDT2
  HS_DTRACE_PROBE3(hotspot, vmops__request, op->name(), strlen(op->name()),
                   op->evaluation_mode());
#else /* USDT2 */
  HOTSPOT_VMOPS_REQUEST(
                   (char *) op->name(), strlen(op->name()),
                   op->evaluation_mode());
#endif /* USDT2 */

  // Encapsulates VM queue policy. Currently, that
  // only involves putting them on the right list
  if (op->evaluate_at_safepoint()) { // 需要在safepoint模式下执行
    queue_add_back(SafepointPriority, op); // 以SafepointPriority添加这个VMOperation
    return true;
  }

  queue_add_back(MediumPriority, op); // 以普通Priority添加Operation
  return true;
}

VM_Operation* VMOperationQueue::remove_next() {
  // Assuming VMOperation queue is two-level priority queue. If there are
  // more than two priorities, we need a different scheduling algorithm.
  assert(SafepointPriority == 0 && MediumPriority == 1 && nof_priorities == 2,
         "current algorithm does not work");

  // simple counter based scheduling to prevent starvation of lower priority
  // queue. -- see 4390175
  int high_prio, low_prio;
  if (_queue_counter++ < 10) {
      high_prio = SafepointPriority;
      low_prio  = MediumPriority;
  } else {
      _queue_counter = 0;
      high_prio = MediumPriority;
      low_prio  = SafepointPriority;
  }

  return queue_remove_front(queue_empty(high_prio) ? low_prio : high_prio);
}

void VMOperationQueue::oops_do(OopClosure* f) {
  for(int i = 0; i < nof_priorities; i++) {
    queue_oops_do(i, f);
  }
  drain_list_oops_do(f);
}


//------------------------------------------------------------------------------------------------------------------
// Implementation of VMThread stuff

bool                VMThread::_should_terminate   = false;
bool              VMThread::_terminated         = false;
Monitor*          VMThread::_terminate_lock     = NULL;
VMThread*         VMThread::_vm_thread          = NULL;
VM_Operation*     VMThread::_cur_vm_operation   = NULL;
VMOperationQueue* VMThread::_vm_queue           = NULL;
PerfCounter*      VMThread::_perf_accumulated_vm_operation_time = NULL;

/**
 * 创建VMThread对象
 * 在Threads::create_vm(JavaVMInitArgs* args, bool* canTryAgain)中调用
 */
void VMThread::create() {
  assert(vm_thread() == NULL, "we can only allocate one VMThread");
  _vm_thread = new VMThread();

  // Create VM operation queue
  _vm_queue = new VMOperationQueue();
  guarantee(_vm_queue != NULL, "just checking");

  _terminate_lock = new Monitor(Mutex::safepoint, "VMThread::_terminate_lock", true);

  if (UsePerfData) {
    // jvmstat performance counters
    Thread* THREAD = Thread::current();
    _perf_accumulated_vm_operation_time =
                 PerfDataManager::create_counter(SUN_THREADS, "vmOperationTime",
                                                 PerfData::U_Ticks, CHECK);
  }
}


VMThread::VMThread() : NamedThread() {
  set_name("VM Thread");
}

void VMThread::destroy() {
  if (_vm_thread != NULL) {
    delete _vm_thread;
    _vm_thread = NULL;      // VM thread is gone
  }
}

/**
 * 查看 Threads::create_vm ;
 * search:  Thread* vmthread = VMThread::vm_thread();
 */
void VMThread::run() {
  assert(this == vm_thread(), "check");

  this->initialize_thread_local_storage();
  this->set_native_thread_name(this->name());
  this->record_stack_base_and_size();
  // Notify_lock wait checks on active_handles() to rewait in
  // case of spurious wakeup, it should wait on the last
  // value set prior to the notify
  this->set_active_handles(JNIHandleBlock::allocate_block());

  {
    MutexLocker ml(Notify_lock);
    Notify_lock->notify();
  }
  // Notify_lock is destroyed by Threads::create_vm()

  int prio = (VMThreadPriority == -1)
    ? os::java_to_os_priority[NearMaxPriority]
    : VMThreadPriority;
  // Note that I cannot call os::set_priority because it expects Java
  // priorities and I am *explicitly* using OS priorities so that it's
  // possible to set the VM thread priority higher than any Java thread.
  os::set_native_priority( this, prio );

  // Wait for VM_Operations until termination
  this->loop(); // 循环等待执行所有的task

  // Note the intention to exit before safepointing.
  // 6295565  This has the effect of waiting for any large tty
  // outputs to finish.
  if (xtty != NULL) {
    ttyLocker ttyl;
    xtty->begin_elem("destroy_vm");
    xtty->stamp();
    xtty->end_elem();
    assert(should_terminate(), "termination flag must be set");
  }

  // 4526887 let VM thread exit at Safepoint
  SafepointSynchronize::begin();

  if (VerifyBeforeExit) {
    HandleMark hm(VMThread::vm_thread());
    // Among other things, this ensures that Eden top is correct.
    Universe::heap()->prepare_for_verify();
    os::check_heap();
    // Silent verification so as not to pollute normal output,
    // unless we really asked for it.
    Universe::verify(!(PrintGCDetails || Verbose) || VerifySilently);
  }

  CompileBroker::set_should_block();

  // wait for threads (compiler threads or daemon threads) in the
  // _thread_in_native state to block.
  VM_Exit::wait_for_threads_in_native_to_block();

  // signal other threads that VM process is gone
  {
    // Note: we must have the _no_safepoint_check_flag. Mutex::lock() allows
    // VM thread to enter any lock at Safepoint as long as its _owner is NULL.
    // If that happens after _terminate_lock->wait() has unset _owner
    // but before it actually drops the lock and waits, the notification below
    // may get lost and we will have a hang. To avoid this, we need to use
    // Mutex::lock_without_safepoint_check().
    MutexLockerEx ml(_terminate_lock, Mutex::_no_safepoint_check_flag);
    _terminated = true;
    _terminate_lock->notify();
  }

  // Thread destructor usually does this.
  ThreadLocalStorage::set_thread(NULL);

  // Deletion must be done synchronously by the JNI DestroyJavaVM thread
  // so that the VMThread deletion completes before the main thread frees
  // up the CodeHeap.

}


// Notify the VMThread that the last non-daemon JavaThread has terminated,
// and wait until operation is performed.
void VMThread::wait_for_vm_thread_exit() {
  { MutexLocker mu(VMOperationQueue_lock);
    _should_terminate = true;
    VMOperationQueue_lock->notify();
  }

  // Note: VM thread leaves at Safepoint. We are not stopped by Safepoint
  // because this thread has been removed from the threads list. But anything
  // that could get blocked by Safepoint should not be used after this point,
  // otherwise we will hang, since there is no one can end the safepoint.

  // Wait until VM thread is terminated
  // Note: it should be OK to use Terminator_lock here. But this is called
  // at a very delicate time (VM shutdown) and we are operating in non- VM
  // thread at Safepoint. It's safer to not share lock with other threads.
  { MutexLockerEx ml(_terminate_lock, Mutex::_no_safepoint_check_flag);
    while(!VMThread::is_terminated()) {
        _terminate_lock->wait(Mutex::_no_safepoint_check_flag);
    }
  }
}

void VMThread::print_on(outputStream* st) const {
  st->print("\"%s\" ", name());
  Thread::print_on(st);
  st->cr();
}

/**
 * 这里会执行对应的VMOperation的evaluate()方法
 * @param op
 */
void VMThread::evaluate_operation(VM_Operation* op) {
  ResourceMark rm;

  {
    PerfTraceTime vm_op_timer(perf_accumulated_vm_operation_time());
#ifndef USDT2
    HS_DTRACE_PROBE3(hotspot, vmops__begin, op->name(), strlen(op->name()),
                     op->evaluation_mode());
#else /* USDT2 */
    HOTSPOT_VMOPS_BEGIN(
                     (char *) op->name(), strlen(op->name()),
                     op->evaluation_mode());
#endif /* USDT2 */

    EventExecuteVMOperation event;

    op->evaluate(); // 为什么在execute()静态方法中也会执行？

    if (event.should_commit()) {
      bool is_concurrent = op->evaluate_concurrently();
      event.set_operation(op->type());
      event.set_safepoint(op->evaluate_at_safepoint());
      event.set_blocking(!is_concurrent);
      // Only write caller thread information for non-concurrent vm operations.
      // For concurrent vm operations, the thread id is set to 0 indicating thread is unknown.
      // This is because the caller thread could have exited already.
      event.set_caller(is_concurrent ? 0 : op->calling_thread()->osthread()->thread_id());
      event.commit();
    }

#ifndef USDT2
    HS_DTRACE_PROBE3(hotspot, vmops__end, op->name(), strlen(op->name()),
                     op->evaluation_mode());
#else /* USDT2 */
    HOTSPOT_VMOPS_END(
                     (char *) op->name(), strlen(op->name()),
                     op->evaluation_mode());
#endif /* USDT2 */
  }

  // Last access of info in _cur_vm_operation!
  bool c_heap_allocated = op->is_cheap_allocated();

  // Mark as completed
  if (!op->evaluate_concurrently()) { // 只有当非concurrent的VMOperation才需要在执行完成的时候增加_vm_operation_completed_count以实现non-concurrent任务的同步
    op->calling_thread()->increment_vm_operation_completed_count();
  }
  // It is unsafe to access the _cur_vm_operation after the 'increment_vm_operation_completed_count' call,
  // since if it is stack allocated the calling thread might have deallocated
  if (c_heap_allocated) {
    delete _cur_vm_operation;
  }
}

/**
 * 执行对应的VM_Operation，执行的时候会进行安全点的检查
 *  如果需要进入安全点，那么就通过调用SafepointSynchronize::begin()进入安全点（其实是设置安全点标记，其他线程检测到标记会进入安全点）
 *      设置了安全点标记以后，其他各种线程(编译线程，执行线程，native代码，G1GC的3个线程，都会进行各自不同的进入安全点的方式，但是都是被动检查安全点标记，然后进入安全点)
 *  执行结束，就离开安全点SafepointSynchronize::end()
 *
 */
void VMThread::loop() {
  assert(_cur_vm_operation == NULL, "no current one should be executing");

  while(true) {
    VM_Operation* safepoint_ops = NULL;
    //
    // Wait for VM operation
    //
    // use no_safepoint_check to get lock without attempting to "sneak"
    { MutexLockerEx mu_queue(VMOperationQueue_lock,
                             Mutex::_no_safepoint_check_flag);

      // Look for new operation
      assert(_cur_vm_operation == NULL, "no current one should be executing");
      _cur_vm_operation = _vm_queue->remove_next();

      // Stall time tracking code
      if (PrintVMQWaitTime && _cur_vm_operation != NULL &&
          !_cur_vm_operation->evaluate_concurrently()) {
        long stall = os::javaTimeMillis() - _cur_vm_operation->timestamp();
        if (stall > 0)
          tty->print_cr("%s stall: %Ld",  _cur_vm_operation->name(), stall);
      }

      while (!should_terminate() && _cur_vm_operation == NULL) { // 反复循环，希望能够从队列中取到一个operation
        // wait with a timeout to guarantee safepoints at regular intervals
        bool timedout =
          VMOperationQueue_lock->wait(Mutex::_no_safepoint_check_flag,
                                      GuaranteedSafepointInterval); // 有新的VMOperation插入到队列，结束等待

        // Support for self destruction
        if ((SelfDestructTimer != 0) && !is_error_reported() &&
            (os::elapsedTime() > SelfDestructTimer * 60)) {
          tty->print_cr("VM self-destructed");
          exit(-1);
        }

        if (timedout && (SafepointALot ||
                         SafepointSynchronize::is_cleanup_needed())) {
          MutexUnlockerEx mul(VMOperationQueue_lock,
                              Mutex::_no_safepoint_check_flag);
          // Force a safepoint since we have not had one for at least
          // 'GuaranteedSafepointInterval' milliseconds.  This will run all
          // the clean-up processing that needs to be done regularly at a
          // safepoint
          SafepointSynchronize::begin();
          #ifdef ASSERT
            if (GCALotAtAllSafepoints) InterfaceSupport::check_gc_alot();
          #endif
          SafepointSynchronize::end();
        }

        _cur_vm_operation = _vm_queue->remove_next(); // 提取一个vm_thread

        // If we are at a safepoint we will evaluate all the operations that
        // follow that also require a safepoint
        if (_cur_vm_operation != NULL &&
            _cur_vm_operation->evaluate_at_safepoint()) {
          safepoint_ops = _vm_queue->drain_at_safepoint_priority(); // 获取 SafepointPriority 中的所有Operation
        }
      } // 退出循环，说明 _cur_vm_operation  ！= null了，说明有其他的thread通过VMThread::execute()方法进行了提交

      if (should_terminate()) break;
    } // Release mu_queue_lock

    //
    // 开始执行这个VMOperation
    //
    { HandleMark hm(VMThread::vm_thread());

      EventMark em("Executing VM operation: %s", vm_operation()->name());
      assert(_cur_vm_operation != NULL, "we should have found an operation to execute");

      // Give the VM thread an extra quantum.  Jobs tend to be bursty and this
      // helps the VM thread to finish up the job.
      // FIXME: When this is enabled and there are many threads, this can degrade
      // performance significantly.
      if( VMThreadHintNoPreempt )
        os::hint_no_preempt();

      // If we are at a safepoint we will evaluate all the operations that
      // follow that also require a safepoint
      // 如果当前的VMOperation是一个safepoint 的 VMOperation，那么干脆将所有的safepoint的VMOperation全部取出来执行
      if (_cur_vm_operation->evaluate_at_safepoint()) { // 这是一个需要在安全点执行的VM_Operation

        _vm_queue->set_drain_list(safepoint_ops); // ensure ops can be scanned

        SafepointSynchronize::begin(); // 开启安全点，是由对应的VMThread负责的
        evaluate_operation(_cur_vm_operation); // 执行这个VMOperation
        // now process all queued safepoint ops, iteratively draining
        // the queue until there are none left
        /**
         * 处理所有队列中的安全点操作
         */
        do {
          _cur_vm_operation = safepoint_ops;
          if (_cur_vm_operation != NULL) {
            do {
              EventMark em("Executing coalesced safepoint VM operation: %s", _cur_vm_operation->name());
              // evaluate_operation deletes the op object so we have
              // to grab the next op now
              VM_Operation* next = _cur_vm_operation->next();
              _vm_queue->set_drain_list(next); // 将drain list指针移动到下一个节点
              evaluate_operation(_cur_vm_operation);// 为什么在execute()静态方法中也会执行？
              _cur_vm_operation = next; // 下一个VM_Operation
              if (PrintSafepointStatistics) {
                SafepointSynchronize::inc_vmop_coalesced_count();
              }
            } while (_cur_vm_operation != NULL);
          }
          // There is a chance that a thread enqueued a safepoint op
          // since we released the op-queue lock and initiated the safepoint.
          // So we drain the queue again if there is anything there, as an
          // optimization to try and reduce the number of safepoints.
          // As the safepoint synchronizes us with JavaThreads we will see
          // any enqueue made by a JavaThread, but the peek will not
          // necessarily detect a concurrent enqueue by a GC thread, but
          // that simply means the op will wait for the next major cycle of the
          // VMThread - just as it would if the GC thread lost the race for
          // the lock.
          if (_vm_queue->peek_at_safepoint_priority()) { // 如果队列中还有safepoint优先级的操作
            // must hold lock while draining queue
            MutexLockerEx mu_queue(VMOperationQueue_lock,
                                     Mutex::_no_safepoint_check_flag);
            safepoint_ops = _vm_queue->drain_at_safepoint_priority(); // 取出所有的safepoint优先级的vmoperation
          } else {
            safepoint_ops = NULL;
          }
        } while(safepoint_ops != NULL);

        _vm_queue->set_drain_list(NULL);

        // Complete safepoint synchronization
        SafepointSynchronize::end();

      } else {  // not a safepoint operation
        if (TraceLongCompiles) {
          elapsedTimer t;
          t.start();
          evaluate_operation(_cur_vm_operation);
          t.stop();
          double secs = t.seconds();
          if (secs * 1e3 > LongCompileThreshold) {
            // XXX - _cur_vm_operation should not be accessed after
            // the completed count has been incremented; the waiting
            // thread may have already freed this memory.
            tty->print_cr("vm %s: %3.7f secs]", _cur_vm_operation->name(), secs);
          }
        } else {
          evaluate_operation(_cur_vm_operation); // 执行这个非Safepoint Operation
        }

        _cur_vm_operation = NULL;
      }
    }

    //
    //  Notify (potential) waiting Java thread(s) - lock without safepoint
    //  check so that sneaking is not possible
    { MutexLockerEx mu(VMOperationRequest_lock,
                       Mutex::_no_safepoint_check_flag);
      VMOperationRequest_lock->notify_all();
    }

    //
    // We want to make sure that we get to a safepoint regularly.
    //
    if (SafepointALot || SafepointSynchronize::is_cleanup_needed()) {
      long interval          = SafepointSynchronize::last_non_safepoint_interval();
      bool max_time_exceeded = GuaranteedSafepointInterval != 0 && (interval > GuaranteedSafepointInterval);
      if (SafepointALot || max_time_exceeded) {
        HandleMark hm(VMThread::vm_thread());
        SafepointSynchronize::begin();
        SafepointSynchronize::end();
      }
    }
  }
}

/**
 * 将对应的VM_Operation实现添加到一个队列中，然后在loop()方法中会从队列中取出任务然后执行
 * 我们从vmThread.hpp中可以看到execute()是一个静态方法，比如我们使用VMThread::execute(task)添加一个operation的时候，
 * 这个VMThread::execute(task)所处的线程可能并不一定是VMThread，而且，从VMThread::execute(task)的代码可以看到，只有当不是VMThread的时候，
 * 才会执行doit_prologue()和doit_epilogue()
 *
    这两个分支处理了不同类型的线程，确保了适当的逻辑被执行。
        在第一个分支中，通过检查线程类型，进行了一些初始化操作，并且将VM操作添加到VM操作队列中。4
        在第二个分支中，直接执行VM操作。
    看下面的代码会发现，如果当前线程不是VM_Thread，那么是加入到队列中异步执行的，如果是VM_thread，那么是直接执行的

    调用者有
        void G1CollectedHeap::collect
        G1CollectedHeap::mem_allocate
        ConcurrentMarkThread::run
        G1CollectedHeap::do_collection_pause

 * @param op
 */
void VMThread::execute(VM_Operation* op) {
  Thread* t = Thread::current();
  /**
   * 可以看到，如果当前线程不是vm_thread，那么当前的op不会立刻执行，而是先进行doit_prologue()，检查成功，就把这个op加入到_vm_queue
   * 而如果当前线程已经是vm_thread了，
   */
  if (!t->is_VM_thread()) { // 如果当前线程不是VMThread，那么执行相应的逻辑。这个分支主要处理Java线程或Watcher线程等情况。
    SkipGCALot sgcalot(t);    // avoid re-entrant attempts to gc-a-lot
    // JavaThread or WatcherThread
    // 大部分跟gc相关的operation，concurrently都是false，
    // 因为他们的evaluation_mode都是safepoint，即加锁和安全点都同时需要满足
    bool concurrent = op->evaluate_concurrently();
    // only blocking VM operations need to verify the call111er's safepoint state:
    if (!concurrent) { // 只有当VM_Operation为 非并发模式的时候，我们需要校验safepoint状态
      t->check_for_valid_safepoint_state(true);
    }

    // New request from Java thread, evaluate prologue
    /**
     * doit_prologue的执行是无条件的，只要调用线程不是VM_thread,
     * 但是doit_epilogue()是由条件的，要求 !op->is_cheap_allocated()
     */
    if (!op->doit_prologue()) {
      return;   // op was cancelled
    }

    // Setup VM_operations for execution
    op->set_calling_thread(t, Thread::get_priority(t)); // 将当前的VMThread设置到VMOperation中

    // It does not make sense to execute the epilogue, if the VM operation object is getting
    // deallocated by the VM thread.
    bool execute_epilog = !op->is_cheap_allocated(); // 大部分的gc线程的is_cheap_allocated()都是false，除了VM_ForceAsyncSafepoint和VM_ThreadStop两个特殊operation
    /**
     * 这里的意思是不可能出现concurrent==true，但是op->is_cheap_allocated()==false
     * 即如果是concurrent，那么一定是 cheap_allocated
     */
    assert(!concurrent || op->is_cheap_allocated(), "concurrent => cheap_allocated");

    // Get ticket number for non-concurrent VM operations
    int ticket = 0;
    if (!concurrent) { // 如果不是并发，比如Mode._no_safepoint或者Mode._safepoint，那么所有的operation一定要串行获取结果
      ticket = t->vm_operation_ticket();
    }

    // Add VM operation to list of waiting threads. We are guaranteed not to block while holding the
    // VMOperationQueue_lock, so we can block without a safepoint check. This allows vm operation requests
    // to be queued up during a safepoint synchronization.
    {
      VMOperationQueue_lock->lock_without_safepoint_check();
      /**
       * 将op添加到静态的等待队列_vm_queue中去。可以看到只有当调用线程不是VM_Thread的时候，
       * 才会放到_vm_queue中，否则在下面的else中直接执行
       */
      bool ok = _vm_queue->add(op);
      op->set_timestamp(os::javaTimeMillis());
      VMOperationQueue_lock->notify(); // 在VMOperationQueue上进行通知，便于在这个队列上等待获取新的VMOperation的循环从block waiting状态唤醒
      VMOperationQueue_lock->unlock();
      // VM_Operation got skipped
      if (!ok) {
        assert(concurrent, "can only skip concurrent tasks");
        if (op->is_cheap_allocated()) delete op;
        return;
      }
    }
    // 不是并发的，那么必须等待当前线程
    if (!concurrent) { // 如果这个线程不可以并发执行，那么一直block waiting直到这个op执行结束
      // Wait for completion of request (non-concurrent)
      // Note: only a JavaThread triggers the safepoint check when locking
      MutexLocker mu(VMOperationRequest_lock);
      while(t->vm_operation_completed_count() < ticket) { // 串行等待一共ticket个操作执行完毕
          // 当t是java线程，wait(false)表示不能跳过safepoint检查，即需要进行safepoint检查
          // 当t不是java线程，wait(true)表示跳过safepoint检查
        VMOperationRequest_lock->wait(!t->is_Java_thread()); // 搜索Monitor::wait
      }
    }

    if (execute_epilog) {
      op->doit_epilogue(); // 和doit_prologue()一样，doit_epilogue()也是在原来的调用者线程中执行的
    }
  } else { // 如果当前线程是VMThread，那么也有相应的逻辑。
    // invoked by VM thread; usually nested VM operation
    // 当前线程是VM现成，因此大多数情况下都是一个嵌套的VM操作
    assert(t->is_VM_thread(), "must be a VM thread");
    VM_Operation* prev_vm_operation = vm_operation(); //取出当前的VM_Operation,即_cur_vm_operation
    if (prev_vm_operation != NULL) {
      // Check the VM operation allows nested VM operation. This normally not the case, e.g., the compiler
      // does not allow nested scavenges or compiles.
      if (!prev_vm_operation->allow_nested_vm_operations()) {
        fatal(err_msg("Nested VM operation %s requested by operation %s",
                      op->name(), vm_operation()->name()));
      }
      // 设置当前的调用线程为上一个operation的调用线程
      op->set_calling_thread(prev_vm_operation->calling_thread(), prev_vm_operation->priority());
    }

    EventMark em("Executing %s VM operation: %s", prev_vm_operation ? "nested" : "", op->name());

    // Release all internal handles after operation is evaluated
    HandleMark hm(t);
    _cur_vm_operation = op; // 更新 _cur_vm_operation
    // 如果当前线程需要进入safepoint，但是并不在safepoint中
    if (op->evaluate_at_safepoint() && !SafepointSynchronize::is_at_safepoint()) {
      SafepointSynchronize::begin();
      op->evaluate(); // 最终执行对应的op的evaluate()方法。为什么在loop中也会执行
      SafepointSynchronize::end();
    } else {
      op->evaluate();
    }

    // Free memory if needed
    if (op->is_cheap_allocated()) delete op;

    _cur_vm_operation = prev_vm_operation;
  }
}


void VMThread::oops_do(OopClosure* f, CLDClosure* cld_f, CodeBlobClosure* cf) {
  Thread::oops_do(f, cld_f, cf);
  _vm_queue->oops_do(f);
}

//------------------------------------------------------------------------------------------------------------------
#ifndef PRODUCT

void VMOperationQueue::verify_queue(int prio) {
  // Check that list is correctly linked
  int length = _queue_length[prio];
  VM_Operation *cur = _queue[prio];
  int i;

  // Check forward links
  for(i = 0; i < length; i++) {
    cur = cur->next();
    assert(cur != _queue[prio], "list to short (forward)");
  }
  assert(cur->next() == _queue[prio], "list to long (forward)");

  // Check backwards links
  cur = _queue[prio];
  for(i = 0; i < length; i++) {
    cur = cur->prev();
    assert(cur != _queue[prio], "list to short (backwards)");
  }
  assert(cur->prev() == _queue[prio], "list to long (backwards)");
}

#endif

void VMThread::verify() {
  oops_do(&VerifyOopClosure::verify_oop, NULL, NULL);
}
