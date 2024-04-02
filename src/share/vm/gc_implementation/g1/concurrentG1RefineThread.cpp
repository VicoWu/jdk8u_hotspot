/*
 * Copyright (c) 2001, 2010, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"

ConcurrentG1RefineThread::
ConcurrentG1RefineThread(ConcurrentG1Refine* cg1r, ConcurrentG1RefineThread *next,
                         CardTableEntryClosure* refine_closure,
                         uint worker_id_offset, uint worker_id) :
  ConcurrentGCThread(),
  _refine_closure(refine_closure), // RefineCardTableEntryClosure
  _worker_id_offset(worker_id_offset),
  _worker_id(worker_id),
  _active(false),
  _next(next),
  _monitor(NULL),
  _cg1r(cg1r),
  _vtime_accum(0.0)
{

  // Each thread has its own monitor. The i-th thread is responsible for signalling
  // to thread i+1 if the number of buffers in the queue exceeds a threashold for this
  // thread. Monitors are also used to wake up the threads during termination.
  // The 0th worker in notified by mutator threads and has a special monitor.
  // The last worker is used for young gen rset size sampling.
  if (worker_id > 0) { // 对于非0号线程，这个_monitor是每个ConcurrentG1RefineThread的本地变量
    _monitor = new Monitor(Mutex::nonleaf, "Refinement monitor", true);
  } else {
    _monitor = DirtyCardQ_CBL_mon; // 对于0号线程，这个_monitor是一个全局静态变量，因为这个_monitor需要和用户线程共享，即用户线程需要通过这个_monitor来通知0号Refine线程运行
  }
  initialize();
  create_and_start();
}

void ConcurrentG1RefineThread::initialize() {
  if (_worker_id < cg1r()->worker_thread_num()) {
    // Current thread activation threshold
    _threshold = MIN2<int>(cg1r()->thread_threshold_step() * (_worker_id + 1) + cg1r()->green_zone(),
                           cg1r()->yellow_zone());
    // A thread deactivates once the number of buffer reached a deactivation threshold
    _deactivation_threshold = MAX2<int>(_threshold - cg1r()->thread_threshold_step(), cg1r()->green_zone());
  } else {
    set_active(true);
  }
}

/**
 * 这是Refine线程池的最后一个线程
 * 对新生代的region的转移专用记忆集合进行采样，获取一个预测值，然后基于预测值来调整新生代的region的数量
 */
void ConcurrentG1RefineThread::sample_young_list_rs_lengths() {
  SuspendibleThreadSetJoiner sts;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1CollectorPolicy* g1p = g1h->g1_policy();
  // // 如果新生代内存的region list长度是可以动态调整的，我们看代码可以发现，只要newSize和maxNewSize不一样，那么就可以动态调整
  if (g1p->adaptive_young_list_length()) {
    int regions_visited = 0;
    g1h->young_list()->rs_length_sampling_init(); // 初始化开始采样的cursor指针位置，准备开始采样
    while (g1h->young_list()->rs_length_sampling_more()) { // 遍历YoungList中的每一个Region，逐个采样

        /**
         *  判断当前的region是否在新生代回收集合CSet中，如果在，则将这个Region的RSet大小也就是它的PRT的分区个数加入到新生代回收需要处理的分区数目中
         */
      g1h->young_list()->rs_length_sampling_next(); // 对当前的Region进行采样，搜索YoungList::rs_length_sampling_next()查看实现
      ++regions_visited;

      // we try to yield every time we visit 10 regions
      if (regions_visited == 10) { // 每处理10个region，尝试让出cpu时间，避免干扰用户线程的正常执行
        if (sts.should_yield()) {
          sts.yield();
          // we just abandon the iteration
          break;
        }
        regions_visited = 0;
      }
    }
    // 完成了采样，采样的结果放在了YoungList对象中的_last_sampled_rs_lengths变量中
    /**
     * 检查年轻列表 RSet 长度的当前值并将其与上次预测进行比较。 如果当前值较高，则重新计算年轻列表目标长度预测。
     */
    g1p->revise_young_list_target_length_if_necessary();
  }
}

void ConcurrentG1RefineThread::run_young_rs_sampling() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  _vtime_start = os::elapsedVTime();
  while(!_should_terminate) {
    sample_young_list_rs_lengths();

    if (os::supports_vtime()) {
      _vtime_accum = (os::elapsedVTime() - _vtime_start);
    } else {
      _vtime_accum = 0.0;
    }

    MutexLockerEx x(_monitor, Mutex::_no_safepoint_check_flag);
    if (_should_terminate) {
      break;
    }
    _monitor->wait(Mutex::_no_safepoint_check_flag, G1ConcRefinementServiceIntervalMillis);
  }
}

void ConcurrentG1RefineThread::wait_for_completed_buffers() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  MutexLockerEx x(_monitor, Mutex::_no_safepoint_check_flag);
  while (!_should_terminate && !is_active()) {
    _monitor->wait(Mutex::_no_safepoint_check_flag);// 等待通知
  }
}

bool ConcurrentG1RefineThread::is_active() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  return _worker_id > 0 ? _active : dcqs.process_completed_buffers();
}

/**
 * 通过_monitor来通知一个Refine线程池的线程启动
 */
void ConcurrentG1RefineThread::activate() {
  MutexLockerEx x(_monitor, Mutex::_no_safepoint_check_flag);
  if (_worker_id > 0) { //  如果不是0号线程
    if (G1TraceConcRefinement) {
      DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
      gclog_or_tty->print_cr("G1-Refine-activated worker %d, on threshold %d, current %d",
                             _worker_id, _threshold, (int)dcqs.completed_buffers_num());
    }
    set_active(true); // 当前线程状态设置为active
  } else { // 如果是0号线程
    DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();// 获取全局的DCQS
    dcqs.set_process_completed(true);
  }
  _monitor->notify(); // 注意，0号线程和非0号线程的_monitor的创建方式完全不同
}

void ConcurrentG1RefineThread::deactivate() {
  MutexLockerEx x(_monitor, Mutex::_no_safepoint_check_flag);
  if (_worker_id > 0) {
    if (G1TraceConcRefinement) {
      DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
      gclog_or_tty->print_cr("G1-Refine-deactivated worker %d, off threshold %d, current %d",
                             _worker_id, _deactivation_threshold, (int)dcqs.completed_buffers_num());
    }
    set_active(false);
  } else {
    DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
    dcqs.set_process_completed(false);
  }
}

/**
 * 线程池的线程(可能是采样线程，可能是DCQ处理线程)运行主方法
 */
void ConcurrentG1RefineThread::run() {
  initialize_in_thread();
  wait_for_universe_init();

  // 如果 _worker_id是worker_thread_num，那么这就是最后一个线程，by design，Refine线程池的最后一个线程是采样线程
  if (_worker_id >= cg1r()->worker_thread_num()) {
    run_young_rs_sampling();
    terminate(); // 采样结束，退出
    return;
  }

  // 不是采样线程而是DCQ的处理线程，开始根据DCQ和DCQS的当前状态进行相应处理
  _vtime_start = os::elapsedVTime();
  while (!_should_terminate) {
    DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set(); // 获取DCQS全局变量，即DCQ的优先级队列

    // Wait for work 循环等待前驱线程的通知以解除阻塞
    // 搜索_monitor = DirtyCardQ_CBL_mon 查看0号和非0号线程的_monitor构建方式的区别
    wait_for_completed_buffers();

    if (_should_terminate) {
      break;
    }

    {
      SuspendibleThreadSetJoiner sts;

      do {
          /**
           * 当前线程的buffer数量(_n_completed_buffers),这个值的value是在 PtrQueueSet::enqueue_complete_buffer中增加的
           */
        int curr_buffer_num = (int)dcqs.completed_buffers_num();
        // If the number of the buffers falls down into the yellow zone,
        // that means that the transition period after the evacuation pause has ended.
        /**
         *  ·黄区： [Yellow， Red)， 在该区， 所有的Refine线程（ 除了抽样线程） 都参与DCQ处理。
         */
        if (dcqs.completed_queue_padding() > 0 && curr_buffer_num <= cg1r()->yellow_zone()) {
          dcqs.set_completed_queue_padding(0);
        }

        // 如果当前线程不是0号线程，并且当前线程的buffer数量不大于deactive的阈值，那么就deactivate当前线程
        if (_worker_id > 0 && curr_buffer_num <= _deactivation_threshold) {
          // If the number of the buffer has fallen below our threshold
          // we should deactivate. The predecessor will reactivate this
          // thread should the number of the buffers cross the threshold again.
          deactivate();
          break;
        }

        // 如果当前线程不是最后一个线程，如果当前线程的buffer_num大于下一个现成的_threshold，那么就需要启动下一个线程
        if (_next != NULL && !_next->is_active() && curr_buffer_num > _next->_threshold) {
          _next->activate();// 向_next现成发送通知，_next现成收到通知以后，就会在wait_for_completed_buffers()中解除阻塞
        }
        /**
         *  这里的closure是 RefineCardTableEntryClosure ，搜索 _refine_cte_cl = new RefineCardTableEntryClosure();
         */
      } while (dcqs.apply_closure_to_completed_buffer(_refine_closure, _worker_id + _worker_id_offset, cg1r()->green_zone()));

      // We can exit the loop above while being active if there was a yield request.
      if (is_active()) {
        deactivate();
      }
    }

    if (os::supports_vtime()) {
      _vtime_accum = (os::elapsedVTime() - _vtime_start);
    } else {
      _vtime_accum = 0.0;
    }
  }
  assert(_should_terminate, "just checking");
  terminate();
}

void ConcurrentG1RefineThread::stop() {
  // it is ok to take late safepoints here, if needed
  {
    MutexLockerEx mu(Terminator_lock);
    _should_terminate = true;
  }

  {
    MutexLockerEx x(_monitor, Mutex::_no_safepoint_check_flag);
    _monitor->notify();
  }

  {
    MutexLockerEx mu(Terminator_lock);
    while (!_has_terminated) {
      Terminator_lock->wait();
    }
  }
  if (G1TraceConcRefinement) {
    gclog_or_tty->print_cr("G1-Refine-stop");
  }
}

void ConcurrentG1RefineThread::print() const {
  print_on(tty);
}

void ConcurrentG1RefineThread::print_on(outputStream* st) const {
  st->print("\"G1 Concurrent Refinement Thread#%d\" ", _worker_id);
  Thread::print_on(st);
  st->cr();
}
