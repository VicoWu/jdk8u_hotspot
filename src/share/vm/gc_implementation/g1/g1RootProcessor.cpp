/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
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

#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc_implementation/g1/bufferingOopClosure.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1GCPhaseTimes.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/g1RootProcessor.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/fprofiler.hpp"
#include "runtime/mutex.hpp"
#include "services/management.hpp"

class G1CodeBlobClosure : public CodeBlobClosure {
  class HeapRegionGatheringOopClosure : public OopClosure {
    G1CollectedHeap* _g1h;
    OopClosure* _work;  // 封装了一个G1ParCopyClosure的实现
    nmethod* _nm;

    template <typename T>
    void do_oop_work(T* p) {
      _work->do_oop(p);
      T oop_or_narrowoop = oopDesc::load_heap_oop(p);
      if (!oopDesc::is_null(oop_or_narrowoop)) {
        oop o = oopDesc::decode_heap_oop_not_null(oop_or_narrowoop);
        HeapRegion* hr = _g1h->heap_region_containing_raw(o);
        assert(!_g1h->obj_in_cs(o) || hr->rem_set()->strong_code_roots_list_contains(_nm), "if o still in CS then evacuation failed and nm must already be in the remset");
        hr->add_strong_code_root(_nm);
      }
    }

  public:
    HeapRegionGatheringOopClosure(OopClosure* oc) : _g1h(G1CollectedHeap::heap()), _work(oc), _nm(NULL) {}

    void do_oop(oop* o) {
      do_oop_work(o);
    }

    void do_oop(narrowOop* o) {
      do_oop_work(o);
    }

    void set_nm(nmethod* nm) {
      _nm = nm;
    }
  };

  HeapRegionGatheringOopClosure _oc;
public:
  G1CodeBlobClosure(OopClosure* oc) : _oc(oc) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = cb->as_nmethod_or_null();
    if (nm != NULL) {
      if (!nm->test_set_oops_do_mark()) {
        _oc.set_nm(nm);
        nm->oops_do(&_oc);
        nm->fix_oop_relocations();
      }
    }
  }
};


/**
 * 只有当我们enable了类卸载的时候UC爱会调用这个方法
 */
void G1RootProcessor::worker_has_discovered_all_strong_classes() {
  uint n_workers = _g1h->n_par_threads();
  assert(ClassUnloadingWithConcurrentMark, "Currently only needed when doing G1 Class Unloading");
  /**
   * 有并发线程正在运行，那么有必要进行通知
   */
  if (n_workers > 0) {
      /**
       * 使用原子操作 Atomic::add(1, &_n_workers_discovered_strong_classes) 将 _n_workers_discovered_strong_classes 的值增加 1，
       * 并将结果存储在 new_value 中。这个变量用于记录已经发现强引用类的工作线程数量。
       */
    uint new_value = (uint)Atomic::add(1, &_n_workers_discovered_strong_classes);
    if (new_value == n_workers) { // 每一个worker都会执行上面的原子交换，但是只有最后一个线程的new_value
                // 会等于总的worker数量，因此通知其他线程，在wait_until_all_strong_classes_discovered 方法出unblock
      // This thread is last. Notify the others.
      MonitorLockerEx ml(&_lock, Mutex::_no_safepoint_check_flag);
      _lock.notify_all();
    }
  }
}

void G1RootProcessor::wait_until_all_strong_classes_discovered() {
  uint n_workers = _g1h->n_par_threads();
  assert(ClassUnloadingWithConcurrentMark, "Currently only needed when doing G1 Class Unloading");

  if (n_workers > 0 && (uint)_n_workers_discovered_strong_classes != n_workers) {
    MonitorLockerEx ml(&_lock, Mutex::_no_safepoint_check_flag);
    while ((uint)_n_workers_discovered_strong_classes != n_workers) {
      _lock.wait(Mutex::_no_safepoint_check_flag, 0, false);
    }
  }
}

G1RootProcessor::G1RootProcessor(G1CollectedHeap* g1h) :
    _g1h(g1h),
    _process_strong_tasks(G1RP_PS_NumElements),
    _srs(g1h),
    _lock(Mutex::leaf, "G1 Root Scanning barrier lock", false),
    _n_workers_discovered_strong_classes(0) {}

/**
 * 搜 _root_processor->evacuate_roots 查看调用位置
 *
 *       _root_processor->evacuate_roots(strong_root_cl,
                                      weak_root_cl, // non_heap_weak_root
                                      strong_cld_cl,
                                      weak_cld_cl, // heap_weak_root
                                      trace_metadata,  // 如果设置了ClassUnloadingWithConcurrentMark，那么trace_metadata=true
                                      worker_id // 当前的gc线程的id);
 */
void G1RootProcessor::evacuate_roots(OopClosure* scan_non_heap_roots,
                                     OopClosure* scan_non_heap_weak_roots,
                                     CLDClosure* scan_strong_clds,
                                     CLDClosure* scan_weak_clds,
                                     bool trace_metadata,
                                     uint worker_i) {
  // First scan the shared roots.
  double ext_roots_start = os::elapsedTime();
  G1GCPhaseTimes* phase_times = _g1h->g1_policy()->phase_times();

  /**
   * BufferingOopClosure closure只是对其他closure的一种封装，
   * 用来通过batch的方式加速迭代，将迭代和处理两件事情在时间上分开
   */
  BufferingOopClosure buf_scan_non_heap_roots(scan_non_heap_roots);
  BufferingOopClosure buf_scan_non_heap_weak_roots(scan_non_heap_weak_roots);

  OopClosure* const weak_roots = &buf_scan_non_heap_weak_roots;
  OopClosure* const strong_roots = &buf_scan_non_heap_roots;

  // CodeBlobClosures are not interoperable with BufferingOopClosures
  G1CodeBlobClosure root_code_blobs(scan_non_heap_roots);

  /**
   * ·Java根：主要指类加载器和线程栈。
        ·类加载器主要是遍历这个类加载器中所有存活的Klass并复制（copy）到Survivor或者晋升到老生代。
        ·线程栈既会处理普通的Java线程栈分配的局部变量，也会处理本地方法栈访问的堆对象，
            在介绍线程栈的时候已经介绍了如何把栈对象和堆对象进行关联。
     ·JVM根：通常是全局对象，比如Universe、JNIHandles、
            ObjectSynchronizer、FlatProfiler、Management、JvmtiExport、
            SystemDictionary、StringTable
   */
  // 处理Java 根
  /**
   * 搜索 G1RootProcessor::process_java_roots 查看具体实现
   * 只有当trace_metadata = true， weak_cld_cl和 strong_cld_cl才会使用，否则为null
   */
  process_java_roots(strong_roots,
                      /**
                       *  如果需要跟踪metadata（发生在需要进行标记（当前正在初始标记，并且需要卸载class）），
                       *  那么用来处理线程栈的cld的闭包就是scan_strong_clds，否则是null
                       */
                     trace_metadata ? scan_strong_clds : NULL,
                     scan_strong_clds,
                        /**
                        *  如果需要跟踪metadata（发生在需要进行标记（当前正在初始标记，并且需要卸载class）），
                        *  那么用来处理线程栈的cld的闭包就是scan_weak_clds，否则是null
                        */
                     trace_metadata ? NULL : scan_weak_clds,
                     &root_code_blobs,
                     phase_times,
                     worker_i);

  // This is the point where this worker thread will not find more strong CLDs/nmethods.
  // Report this so G1 can synchronize the strong and weak CLDs/nmethods processing.
  if (trace_metadata) {
      /**
       * 调用这个方法，代表自己这个worker已经处理完了所有的强根
       */
    worker_has_discovered_all_strong_classes();
  }
  /**
   * 处理 VM的根, 传入进来的参数是strong_roots和weak_roots, 和CLD无关
   * 搜索 G1RootProcessor::process_vm_roots
   */
  process_vm_roots(strong_roots, weak_roots, phase_times, worker_i);
  // 处理string table 根
  process_string_table_roots(weak_roots, phase_times, worker_i);
  {
    // Now the CM ref_processor roots.
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::CMRefRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_refProcessor_oops_do)) {
      // We need to treat the discovered reference lists of the
      // concurrent mark ref processor as roots and keep entries
      // (which are added by the marking threads) on them live
      // until they can be processed at the end of marking.
      _g1h->ref_processor_cm()->weak_oops_do(&buf_scan_non_heap_roots);
    }
  }

  if (trace_metadata) {
    {
      G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::WaitForStrongCLD, worker_i);
      // Barrier to make sure all workers passed
      // the strong CLD and strong nmethods phases.
      /**
       * 如果启动了类卸载，那么在并发多worker的情况下，必须等待所有的强class被发现(即所有其他worker线程执行完成)，才能继续往下进行弱根的扫描
       * 参考 worker_has_discovered_all_strong_classes
       */
      wait_until_all_strong_classes_discovered();
    }

    // Now take the complement of the strong CLDs.
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::WeakCLDRoots, worker_i);
      /**
       * 所有的强根的处理(注意当前方法只是根扫描，因此不包括对那些根 所 指向的对象的间接扫描)开始进行弱根的扫描
       */
    ClassLoaderDataGraph::roots_cld_do(NULL, scan_weak_clds); // 不再处理强应用，只处理弱引用，只处理is_weak=true的那些cld
  } else {
    phase_times->record_time_secs(G1GCPhaseTimes::WaitForStrongCLD, worker_i, 0.0);
    phase_times->record_time_secs(G1GCPhaseTimes::WeakCLDRoots, worker_i, 0.0);
  }

  // Finish up any enqueued closure apps (attributed as object copy time).
  buf_scan_non_heap_roots.done();
  buf_scan_non_heap_weak_roots.done();

  double obj_copy_time_sec = buf_scan_non_heap_roots.closure_app_seconds()
      + buf_scan_non_heap_weak_roots.closure_app_seconds();

  phase_times->record_time_secs(G1GCPhaseTimes::ObjCopy, worker_i, obj_copy_time_sec);

  double ext_root_time_sec = os::elapsedTime() - ext_roots_start - obj_copy_time_sec;

  phase_times->record_time_secs(G1GCPhaseTimes::ExtRootScan, worker_i, ext_root_time_sec);

  // During conc marking we have to filter the per-thread SATB buffers
  // to make sure we remove any oops into the CSet (which will show up
  // as implicitly live).
  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::SATBFiltering, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_filter_satb_buffers) && _g1h->mark_in_progress()) {
      JavaThread::satb_mark_queue_set().filter_thread_buffers();
    }
  }

  _process_strong_tasks.all_tasks_completed();
}

void G1RootProcessor::process_strong_roots(OopClosure* oops,
                                           CLDClosure* clds,
                                           CodeBlobClosure* blobs) {

  process_java_roots(oops, clds, clds, NULL, blobs, NULL, 0);
  process_vm_roots(oops, NULL, NULL, 0);

  _process_strong_tasks.all_tasks_completed();
}

void G1RootProcessor::process_all_roots(OopClosure* oops,
                                        CLDClosure* clds,
                                        CodeBlobClosure* blobs,
                                        bool process_string_table) {

  process_java_roots(oops, NULL, clds, clds, NULL, NULL, 0);
  process_vm_roots(oops, oops, NULL, 0);

  if (process_string_table) {
    process_string_table_roots(oops, NULL, 0);
   }
  process_code_cache_roots(blobs, NULL, 0);

  _process_strong_tasks.all_tasks_completed();
}

void G1RootProcessor::process_all_roots(OopClosure* oops,
                                        CLDClosure* clds,
                                        CodeBlobClosure* blobs) {
  process_all_roots(oops, clds, blobs, true);
}

void G1RootProcessor::process_all_roots_no_string_table(OopClosure* oops,
                                                        CLDClosure* clds,
                                                        CodeBlobClosure* blobs) {
  assert(!ClassUnloading, "Should only be used when class unloading is disabled");
  process_all_roots(oops, clds, blobs, false);
}

/**
 *   调用者是 G1RootProcessor::evacuate_roots
 *   strong_roots：一个用于处理强根的 OopClosure。
 *   thread_stack_clds：一个用于处理线程栈上的类加载器数据（CLD）根的 CLDClosure。
 *   strong_clds：一个用于处理强 CLD 的 CLDClosure。
 *   weak_clds：一个用于处理弱 CLD 的 CLDClosure。
 *   strong_code：一个用于处理强代码块的 CodeBlobClosure。
 *   phase_times：一个指向 G1GCPhaseTimes 的指针，用于跟踪阶段时间。
 *   worker_i：一个工作线程的标识符。
 */
void G1RootProcessor::process_java_roots(OopClosure* strong_roots, // 一个用于处理强根的 OopClosure。
                                         /**
                                          * trace_metadata ? scan_strong_clds : NULL,
                                          * 即如果需要trace_metadata，那么我们需要扫描thread_stack_clds, 而如果不进行trace_metadata，那么我们不需要扫描thread_stack_clds
                                          */
                                         CLDClosure* thread_stack_clds, // 一个用于处理线程栈上的类加载器数据（CLD）根的 CLDClosure，如果需要trace metadata(即处于初始标记阶段并且用户配置了在标记阶段进行类卸载), 那么这个cld就是 scan_strong_clds
                                         CLDClosure* strong_clds, // 一个用于处理强 CLD 的 CLDClosure， 如果CLD对象的keep_alive()是true，那么就apply strong_clds，否则，apply weak_clds
                                         /**
                                          * trace_metadata ? NULL : scan_weak_clds
                                          * 即如果需要trace_metadata，那么我们在这里不扫描和处理弱的cld(cld.is_weak() = true)，在所有线程完成了对强cld的扫描以后，我们再处理弱的cld
                                          */
                                         CLDClosure* weak_clds, // 一个用于处理弱 CLD 的 CLDClosure
                                         CodeBlobClosure* strong_code, // 一个用于处理强代码块的 CodeBlobClosure
                                         G1GCPhaseTimes* phase_times,
                                         uint worker_i // 工作线程的id) {
  assert(thread_stack_clds == NULL || weak_clds == NULL, "There is overlap between those, only one may be set");
  // Iterating over the CLDG and the Threads are done early to allow us to
  // first process the strong CLDs and nmethods and then, after a barrier,
  // let the thread process the weak CLDs and nmethods.
  /**
   * 我们先通过下面的两个方法首先处理强的CLD和nmethods，然后，在一个栅栏 （ wait_until_all_strong_classes_discovered(); ）以后，
   * 这个线程就可以处理弱的CLD和nmethods
   */
  {
      /**
       * 这里处理类加载数据图上面的类加载器数据
       */
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::CLDGRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_ClassLoaderDataGraph_oops_do)) {
        /**
         * 调用 ClassLoaderDataGraph::roots_cld_do 静态方法来迭代 CLDG 根，并根据提供的闭包（strong_clds 和 weak_clds）进行应用。
         * 在整个CLDG上的每一个ClassLoaderData上apply G1CLDClosure，其实是针对每一个CLD调用void do_cld(ClassLoaderData* cld)
         * 搜索 ClassLoaderDataGraph::roots_cld_do 查看方法的具体实现
         * 查看静态方法的具体实现，可以看到，如果CLD对象的keep_alive()是true，那么就apply strong_clds，否则，apply weak_clds
         * 我们从 调用者的构造方法可以看到，strong_clds和 weak_clds的区别是他们的参数中的 G1ParCopyClosure _oop_closure 是对应的strong还是weak
         */
      ClassLoaderDataGraph::roots_cld_do(strong_clds, weak_clds); // 如果trace metadata，那么weak_clds = null
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::ThreadRoots, worker_i);
    /**
     * 处理线程根：在此阶段，函数处理在线程栈上发现的根。它利用 Threads::possibly_parallel_oops_do 方法，
     * 可能并行处理强根（strong_roots）、CLD（thread_stack_clds）和代码块（strong_code）。
     * 如果 trace_metadata = true，那么thread_stack_clds就是strong_roots_cld，否则为null
     *
     * 搜索 void Threads::possibly_parallel_oops_do 查看具体实现
     *
     * void Threads::possibly_parallel_oops_do -> JavaThread::oops_do -> frame::oops_do_internal -> frame::oops_interpreted_do
     */
    Threads::possibly_parallel_oops_do(strong_roots, thread_stack_clds, strong_code);
  }
}

/**
 * 处理虚拟机root，比如全局的Universe、JNIHandles、
    ObjectSynchronizer、FlatProfiler、Management、JvmtiExport、
    SystemDictionary、StringTable
    strong_roots和weak_roots根据参数不同，都会进行标记或者不进行标记
    其中只有 SystemDictionary需要同时传入 strong_roots 和 weak_roots
 * @param strong_roots
 * @param weak_roots
 * @param phase_times
 * @param worker_i
 */
void G1RootProcessor::process_vm_roots(OopClosure* strong_roots,
                                       OopClosure* weak_roots,
                                       G1GCPhaseTimes* phase_times,
                                       uint worker_i) {
  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::UniverseRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_Universe_oops_do)) {
      Universe::oops_do(strong_roots);
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::JNIRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_JNIHandles_oops_do)) {
      JNIHandles::oops_do(strong_roots);
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::ObjectSynchronizerRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_ObjectSynchronizer_oops_do)) {
      ObjectSynchronizer::oops_do(strong_roots);
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::FlatProfilerRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_FlatProfiler_oops_do)) {
      FlatProfiler::oops_do(strong_roots);
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::ManagementRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_Management_oops_do)) {
      Management::oops_do(strong_roots);
    }
  }

  {
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::JVMTIRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_jvmti_oops_do)) {
      JvmtiExport::oops_do(strong_roots);
    }
  }

  {
      /**
       * 在JVM（Java虚拟机）中，系统字典（System Dictionary）是一个重要的数据结构，用于管理Java类的加载、解析和链接。它的作用包括：
        类加载器委派：系统字典负责实现类加载器的委派模型。当需要加载一个类时，系统字典会按照委派规则，逐级查询类加载器，
            直到找到需要加载的类或者确定该类不存在。这种委派模型可以保证类加载的顺序和一致性，确保类的唯一性和正确性。
        类的解析和链接：系统字典负责解析类的名称、定位类文件、加载类字节码并进行链接（验证、准备、解析）。
            在解析阶段，系统字典会将类与其他类及其成员进行关联，建立符号引用与实际引用之间的映射关系，以便在运行时能够正确地访问类及其成员。
        类的存储和管理：系统字典维护了已加载的类的信息，包括类的元数据、类的静态变量和常量池等。
            它提供了对类的存储和管理功能，例如查找类、添加类、移除类、更新类等。
        类的查找和访问：系统字典提供了各种方法来查找和访问已加载的类，包括根据类名、类加载器、类路径等条件进行类的查找和定位。这
            使得Java程序能够在运行时动态地加载和访问类，实现了Java的灵活性和动态性。
       */
    G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::SystemDictionaryRoots, worker_i);
    if (!_process_strong_tasks.is_task_claimed(G1RP_PS_SystemDictionary_oops_do)) {
      SystemDictionary::roots_oops_do(strong_roots, weak_roots); // 搜索 void SystemDictionary::roots_oops_do
    }
  }
}

void G1RootProcessor::process_string_table_roots(OopClosure* weak_roots, G1GCPhaseTimes* phase_times,
                                                 uint worker_i) {
  assert(weak_roots != NULL, "Should only be called when all roots are processed");

  G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::StringTableRoots, worker_i);
  // All threads execute the following. A specific chunk of buckets
  // from the StringTable are the individual tasks.
  StringTable::possibly_parallel_oops_do(weak_roots);
}

void G1RootProcessor::process_code_cache_roots(CodeBlobClosure* code_closure,
                                               G1GCPhaseTimes* phase_times,
                                               uint worker_i) {
  if (!_process_strong_tasks.is_task_claimed(G1RP_PS_CodeCache_oops_do)) {
    CodeCache::blobs_do(code_closure);
  }
}

/**
 * 搜搜 _root_processor->scan_remembered_sets 查看调用位置
 * 将 G1ParPushHeapRSClosure scan_rs 闭包应用于回收集合中的所有Region的Rset的并集中的所有位置（已完成“set_region”以指示根所在的区域）
 */
void G1RootProcessor::scan_remembered_sets(G1ParPushHeapRSClosure* scan_rs,
                                           OopClosure* scan_non_heap_weak_roots,
                                           uint worker_i) {
  G1GCPhaseTimes* phase_times = _g1h->g1_policy()->phase_times();
  G1GCParPhaseTimesTracker x(phase_times, G1GCPhaseTimes::CodeCacheRoots, worker_i);

  // Now scan the complement of the collection set.
  G1CodeBlobClosure scavenge_cs_nmethods(scan_non_heap_weak_roots);
  /**
   * 搜 G1RemSet::oops_into_collection_set_do 查看方法实现
   */
  _g1h->g1_rem_set()->oops_into_collection_set_do(scan_rs, &scavenge_cs_nmethods, worker_i);
}

void G1RootProcessor::set_num_workers(int active_workers) {
  _process_strong_tasks.set_n_threads(active_workers);
}
