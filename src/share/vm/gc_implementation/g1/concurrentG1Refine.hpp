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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTG1REFINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTG1REFINE_HPP

#include "gc_implementation/g1/g1HotCardCache.hpp"
#include "memory/allocation.hpp"
#include "runtime/thread.hpp"
#include "utilities/globalDefinitions.hpp"

// Forward decl
class ConcurrentG1RefineThread;
class G1CollectedHeap;
class G1HotCardCache;
class G1RegionToSpaceMapper;
class G1RemSet;
class DirtyCardQueue;

/**
 * ConcurrentG1Refine对象是在 jint G1CollectedHeap::initialize() 中构造的，
 *      在这个CocurrentG1Refine构造的时候，构造ConcurrentG1RefineThread线程池， 同时设置对应的DCQS的白绿黄红区域
 */
class ConcurrentG1Refine: public CHeapObj<mtGC> {
  ConcurrentG1RefineThread** _threads;
  uint _n_threads;
  uint _n_worker_threads;
 /*
  * The value of the update buffer queue length falls into one of 3 zones:
  * green, yellow, red. If the value is in [0, green) nothing is
  * done, the buffers are left unprocessed to enable the caching effect of the
  * dirtied cards. In the yellow zone [green, yellow) the concurrent refinement
  * threads are gradually activated. In [yellow, red) all threads are
  * running. If the length becomes red (max queue length) the mutators start
  * processing the buffers.
  *
  * There are some interesting cases (when G1UseAdaptiveConcRefinement
  * is turned off):
  * 1) green = yellow = red = 0. In this case the mutator will process all
  *    buffers. Except for those that are created by the deferred updates
  *    machinery during a collection.
  * 2) green = 0. Means no caching. Can be a good way to minimize the
  *    amount of time spent updating rsets during a collection.
  */
  int _green_zone;
  int _yellow_zone;
  int _red_zone;

  int _thread_threshold_step;

  // We delay the refinement of 'hot' cards using the hot card cache.
  // 缓存中的热卡片不会进行立刻refine
  G1HotCardCache _hot_card_cache;

  // Reset the threshold step value based of the current zone boundaries.
  void reset_threshold_step();

 public:
  ConcurrentG1Refine(G1CollectedHeap* g1h, CardTableEntryClosure* refine_closure);
  ~ConcurrentG1Refine();

  void init(G1RegionToSpaceMapper* card_counts_storage);
  void stop();

  void reinitialize_threads();

  // Iterate over all concurrent refinement threads
  void threads_do(ThreadClosure *tc);

  // Iterate over all worker refinement threads
  void worker_threads_do(ThreadClosure * tc);

  // The RS sampling thread
  ConcurrentG1RefineThread * sampling_thread() const;

  static uint thread_num();

  void print_worker_threads_on(outputStream* st) const;

  /**
   *  这里涉及到三个可以处理DCQ的线程，GC线程，Refine线程，以及用户线程Mutator
   * ·白区： [0， Green)， 对于该区， Refine线程并不处理， 交由GC线
            程来处理DCQ。
     ·绿区： [Green， Yellow)， 在该区中， Refine线程开始启动， 并且
            根据queue set数值的大小启动不同数量的Refine线程来处理DCQ。
     ·黄区： [Yellow， Red)， 在该区， 所有的Refine线程（ 除了抽样线
            程） 都参与DCQ处理。
     ·红区： [Red， +无穷)， 在该区， 不仅仅所有的Refine线程参与处
            理RSet， 而且连Mutator也参与处理dcq。
    这三个值通过三个参数设置： G1ConcRefinementGreenZone、
    G1ConcRefinementYellowZone、 G1ConcRefinementRedZone，
    默认值都是0。 如果没有设置这三个值， G1则自动推断这三个区的阈值大小
   * @param x
   */
  void set_green_zone(int x)  { _green_zone = x;  }
  void set_yellow_zone(int x) { _yellow_zone = x; }
  void set_red_zone(int x)    { _red_zone = x;    }

  int green_zone() const      { return _green_zone;  }
  int yellow_zone() const     { return _yellow_zone; }
  int red_zone() const        { return _red_zone;    }

  uint total_thread_num() const  { return _n_threads;        }
  uint worker_thread_num() const { return _n_worker_threads; }

  int thread_threshold_step() const { return _thread_threshold_step; }

  G1HotCardCache* hot_card_cache() { return &_hot_card_cache; }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_CONCURRENTG1REFINE_HPP
