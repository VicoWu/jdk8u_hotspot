/*
 * Copyright (c) 2012, 2018, Oracle and/or its affiliates. All rights reserved.
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

// A ClassLoaderData identifies the full set of class types that a class
// loader's name resolution strategy produces for a given configuration of the
// class loader.
// Class types in the ClassLoaderData may be defined by from class file binaries
// provided by the class loader, or from other class loader it interacts with
// according to its name resolution strategy.
//
// Class loaders that implement a deterministic name resolution strategy
// (including with respect to their delegation behavior), such as the boot, the
// extension, and the system loaders of the JDK's built-in class loader
// hierarchy, always produce the same linkset for a given configuration.
//
// ClassLoaderData carries information related to a linkset (e.g.,
// metaspace holding its klass definitions).
// The System Dictionary and related data structures (e.g., placeholder table,
// loader constraints table) as well as the runtime representation of classes
// only reference ClassLoaderData.
//
// Instances of java.lang.ClassLoader holds a pointer to a ClassLoaderData that
// that represent the loader's "linking domain" in the JVM.
//
// The bootstrap loader (represented by NULL) also has a ClassLoaderData,
// the singleton class the_null_class_loader_data().

#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "memory/gcLocker.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/oopFactory.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/mutex.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"
#if INCLUDE_TRACE
#include "trace/tracing.hpp"
#endif

ClassLoaderData * ClassLoaderData::_the_null_class_loader_data = NULL;

/**
 * 我们通过方法void ClassLoaderData::add_class(Klass* k) 可以看到，
 *      一个ClassLoaderData通过_klass链接了自己所管理的各种klass对象，即各种不同的class
 *
 * 构造方法的调用者是 ClassLoaderData* ClassLoaderDataGraph::add
 * @param h_class_loader
 * @param is_anonymous
 * @param dependencies
 */
ClassLoaderData::ClassLoaderData(Handle h_class_loader, bool is_anonymous, Dependencies dependencies) :
  _class_loader(h_class_loader()),
  _is_anonymous(is_anonymous),
  // An anonymous class loader data doesn't have anything to keep
  // it from being unloaded during parsing of the anonymous class.
  // The null-class-loader should always be kept alive.
  /**
   * 空类加载器是一个特殊的类加载器，代表了一种无需加载的情况，通常用于系统类和基本类的加载。
     保持空类加载器活跃（即不被回收）可以确保在解析匿名类期间，相关的匿名类加载器数据不会被意外回收。
   */
  _keep_alive(is_anonymous || h_class_loader.is_null()),
  _metaspace(NULL), _unloading(false), _klasses(NULL),
  _claimed(0), _jmethod_ids(NULL), _handles(), _deallocate_list(NULL),
  _next(NULL), _dependencies(dependencies),
  _metaspace_lock(new Mutex(Monitor::leaf+1, "Metaspace allocation lock", true)) {
    // empty
}

void ClassLoaderData::init_dependencies(TRAPS) {
  assert(!Universe::is_fully_initialized(), "should only be called when initializing");
  assert(is_the_null_class_loader_data(), "should only call this for the null class loader");
  _dependencies.init(CHECK);
}

void ClassLoaderData::Dependencies::init(TRAPS) {
  // Create empty dependencies array to add to. CMS requires this to be
  // an oop so that it can track additions via card marks.  We think.
  _list_head = oopFactory::new_objectArray(2, CHECK);
}

ClassLoaderData::ChunkedHandleList::~ChunkedHandleList() {
  Chunk* c = _head;
  while (c != NULL) {
    Chunk* next = c->_next;
    delete c;
    c = next;
  }
}

oop* ClassLoaderData::ChunkedHandleList::add(oop o) {
  if (_head == NULL || _head->_size == Chunk::CAPACITY) {
    Chunk* next = new Chunk(_head);
    OrderAccess::release_store_ptr(&_head, next);
  }
  oop* handle = &_head->_data[_head->_size];
  *handle = o;
  OrderAccess::release_store(&_head->_size, _head->_size + 1);
  return handle;
}

inline void ClassLoaderData::ChunkedHandleList::oops_do_chunk(OopClosure* f, Chunk* c, const juint size) {
  for (juint i = 0; i < size; i++) {
    if (c->_data[i] != NULL) {
      f->do_oop(&c->_data[i]);
    }
  }
}

void ClassLoaderData::ChunkedHandleList::oops_do(OopClosure* f) {
  Chunk* head = (Chunk*) OrderAccess::load_ptr_acquire(&_head);
  if (head != NULL) {
    // Must be careful when reading size of head
    oops_do_chunk(f, head, OrderAccess::load_acquire(&head->_size));
    for (Chunk* c = head->_next; c != NULL; c = c->_next) {
      oops_do_chunk(f, c, c->_size);
    }
  }
}

bool ClassLoaderData::claim() {
  if (_claimed == 1) { // 已经被占用
    return false;
  }
  /**
   * 尝试将 _claimed 的值从 0 更改为 1。如果成功，表示当前线程已成功占用该类加载器数据，并返回 true。
   * 否则，表示在当前线程操作 _claimed 的过程中，其他线程已经将其设置为 1，当前线程无法占用该类加载器数据，因此返回 false。
   */
  return (int) Atomic::cmpxchg(1, &_claimed, 0) == 0;
}

/**
 * 调用位置在 G1CLDClosure(G1ParCopyClosure<G1BarrierNone, do_mark_object>* oop_closure 中。
 *
 *  从调用位置看到，f和klass_closure都是G1ParCopyClosure，只不过前者拦截器是G1BarrierNone，后者是G1BarrierKlass
 * @param f 这里的_oop_closure其实就是 G1ParCopyClosure，每一个G1CLDClosure都封装了一个 G1ParCopyClosure,拦截器是G1BarrierNone
 * @param klass_closure 这是一个G1KlassScanClosure，里面包含了一个G1ParCopyClosure，这个G1ParCopyClosure的拦截器是G1BarrierKlass
 * @param must_claim 是否需要主张占用对应的的ClassLoaderData
 */
void ClassLoaderData::oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim) {
  if (must_claim && !claim()) { // 并发标记阶段，must_claim = true, 但是我们发现这个CLD已经被其他线程占用，返回false
    return;
  }
  /**
   * f是G1ParCopyClosure, 对这个CLD对应的_classLoader对象去apply 没有拦截的G1ParCopyClosure
   * 对于一个ClassLoaderData，如果不是构造中的匿名类或者空类加载器，那么，它的keep_alive_object就是对应的class loader
   * 如果是一个匿名类，它的keep alive object就是对应的mirror对象
   * 查看 G1ParCopyClosure<barrier, do_mark_object>::do_oop_work
   */
  f->do_oop(&_class_loader);
  /**
   * 对这个CLD所依赖的所有的CLD去apply G1ParCopyClosure 没有拦截的G1ParCopyClosure
   */
  _dependencies.oops_do(f);
  /**
   * 对这个常量池数组 apply G1ParCopyClosure 没有拦截的G1ParCopyClosure
   */
  _handles.oops_do(f);
  if (klass_closure != NULL) {
      /**
       * 对对这个CLD以_klass为链表头指针链接起来的所有klass挨个去apply G1KlassScanClosure
       *
       * 方法实现搜索 搜索 void ClassLoaderData::classes_do
       */
    classes_do(klass_closure);
  }
}

void ClassLoaderData::Dependencies::oops_do(OopClosure* f) {
  f->do_oop((oop*)&_list_head);
}

/**
 * 调用者是 void ClassLoaderData::oops_do
 * 对这个ClassLoaderData中以_klasses链接起来的所有的klass 去apply klass_closure,
 * 即对于这个CLD的每一个klass，调用 G1KlassScanClosure的 do_klass方法（搜索g1CollectedHeap中的 void do_klass(Klass* klass) ）
 */
void ClassLoaderData::classes_do(KlassClosure* klass_closure) {
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) { // 对这个ClassLoaderData中以_klasses链接起来的所有的klass去apply klass_closure
    klass_closure->do_klass(k); //  class G1KlassScanClosure : public KlassClosure 查看对应的do_klass方法，调用 G1KlassScanClosure的do_klass()方法, 可以搜索
    assert(k != k->next_link(), "no loops!");
  }
}

/**
 * 对对这个CLD以_klass为链表头指针链接起来的所有klass挨个去apply G1KlassScanClosure
 * 其中 G1KlassScanClosure本质上还是封装了一个G1ParCopyClosure
 * 搜索 G1CLDClosure(G1ParCopyClosure<G1BarrierNone, do_mark_object>* oop_closure
 *
 *
 * @param f
 */
void ClassLoaderData::classes_do(void f(Klass * const)) {
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    f(k); // 搜索 G1KlassScanClosure::do_klass
  }
}

void ClassLoaderData::loaded_classes_do(KlassClosure* klass_closure) {
  // Lock to avoid classes being modified/added/removed during iteration
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    // Do not filter ArrayKlass oops here...
    if (k->oop_is_array() || (k->oop_is_instance() && InstanceKlass::cast(k)->is_loaded())) {
      klass_closure->do_klass(k);
    }
  }
}

void ClassLoaderData::classes_do(void f(InstanceKlass*)) {
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    if (k->oop_is_instance()) {
      f(InstanceKlass::cast(k));
    }
    assert(k != k->next_link(), "no loops!");
  }
}

void ClassLoaderData::record_dependency(Klass* k, TRAPS) {
  ClassLoaderData * const from_cld = this;
  ClassLoaderData * const to_cld = k->class_loader_data();

  // Dependency to the null class loader data doesn't need to be recorded
  // because the null class loader data never goes away.
  if (to_cld->is_the_null_class_loader_data()) {
    return;
  }

  oop to;
  if (to_cld->is_anonymous()) {
    // Anonymous class dependencies are through the mirror.
    to = k->java_mirror();
  } else {
    to = to_cld->class_loader();

    // If from_cld is anonymous, even if it's class_loader is a parent of 'to'
    // we still have to add it.  The class_loader won't keep from_cld alive.
    if (!from_cld->is_anonymous()) {
      // Check that this dependency isn't from the same or parent class_loader
      oop from = from_cld->class_loader();

      oop curr = from;
      while (curr != NULL) {
        if (curr == to) {
          return; // this class loader is in the parent list, no need to add it.
        }
        curr = java_lang_ClassLoader::parent(curr);
      }
    }
  }

  // It's a dependency we won't find through GC, add it. This is relatively rare
  // Must handle over GC point.
  Handle dependency(THREAD, to);
  from_cld->_dependencies.add(dependency, CHECK);
}


void ClassLoaderData::Dependencies::add(Handle dependency, TRAPS) {
  // Check first if this dependency is already in the list.
  // Save a pointer to the last to add to under the lock.
  objArrayOop ok = _list_head;
  objArrayOop last = NULL;
  while (ok != NULL) {
    last = ok;
    if (ok->obj_at(0) == dependency()) {
      // Don't need to add it
      return;
    }
    ok = (objArrayOop)ok->obj_at(1);
  }

  // Must handle over GC points
  assert (last != NULL, "dependencies should be initialized");
  objArrayHandle last_handle(THREAD, last);

  // Create a new dependency node with fields for (class_loader or mirror, next)
  objArrayOop deps = oopFactory::new_objectArray(2, CHECK);
  deps->obj_at_put(0, dependency());

  // Must handle over GC points
  objArrayHandle new_dependency(THREAD, deps);

  // Add the dependency under lock
  locked_add(last_handle, new_dependency, THREAD);
}

void ClassLoaderData::Dependencies::locked_add(objArrayHandle last_handle,
                                               objArrayHandle new_dependency,
                                               Thread* THREAD) {

  // Have to lock and put the new dependency on the end of the dependency
  // array so the card mark for CMS sees that this dependency is new.
  // Can probably do this lock free with some effort.
  ObjectLocker ol(Handle(THREAD, _list_head), THREAD);

  oop loader_or_mirror = new_dependency->obj_at(0);

  // Since the dependencies are only added, add to the end.
  objArrayOop end = last_handle();
  objArrayOop last = NULL;
  while (end != NULL) {
    last = end;
    // check again if another thread added it to the end.
    if (end->obj_at(0) == loader_or_mirror) {
      // Don't need to add it
      return;
    }
    end = (objArrayOop)end->obj_at(1);
  }
  assert (last != NULL, "dependencies should be initialized");
  // fill in the first element with the oop in new_dependency.
  if (last->obj_at(0) == NULL) {
    last->obj_at_put(0, new_dependency->obj_at(0));
  } else {
    last->obj_at_put(1, new_dependency());
  }
}

void ClassLoaderDataGraph::clear_claimed_marks() {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->clear_claimed();
  }
}

void ClassLoaderData::add_class(Klass* k) {
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  Klass* old_value = _klasses; // 临时保存kclasses
  k->set_next_link(old_value); // 通过设置 Klass 对象的 next_link 指针，将新的 Klass 对象链接到 ClassLoaderData 实例的 Klass 对象链表的头部中
  // link the new item into the list
  _klasses = k; // 将新的对象设置为链表头部

  if (TraceClassLoaderData && Verbose && k->class_loader_data() != NULL) {
    ResourceMark rm;
    tty->print_cr("[TraceClassLoaderData] Adding k: " PTR_FORMAT " %s to CLD: "
                  PTR_FORMAT " loader: " PTR_FORMAT " %s",
                  p2i(k),
                  k->external_name(),
                  p2i(k->class_loader_data()),
                  p2i((void *)k->class_loader()),
                  loader_name());
  }
}

// This is called by InstanceKlass::deallocate_contents() to remove the
// scratch_class for redefine classes.  We need a lock because there it may not
// be called at a safepoint if there's an error.
void ClassLoaderData::remove_class(Klass* scratch_class) {
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  Klass* prev = NULL;
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    if (k == scratch_class) {
      if (prev == NULL) {
        _klasses = k->next_link();
      } else {
        Klass* next = k->next_link();
        prev->set_next_link(next);
      }
      return;
    }
    prev = k;
    assert(k != k->next_link(), "no loops!");
  }
  ShouldNotReachHere();   // should have found this class!!
}

void ClassLoaderData::unload() {
  _unloading = true;

  // Tell serviceability tools these classes are unloading
  classes_do(InstanceKlass::notify_unload_class);

  if (TraceClassLoaderData) {
    ResourceMark rm;
    tty->print("[ClassLoaderData: unload loader data " INTPTR_FORMAT, p2i(this));
    tty->print(" for instance " INTPTR_FORMAT " of %s", p2i((void *)class_loader()),
               loader_name());
    if (is_anonymous()) {
      tty->print(" for anonymous class  " INTPTR_FORMAT " ", p2i(_klasses));
    }
    tty->print_cr("]");
  }
}

/**
 * 获取当前cld对应的class的keep_alive_object，这个object专门被G1GC用来跟踪对象的存活特性
 * @return
 */
oop ClassLoaderData::keep_alive_object() const {
  assert(!keep_alive(), "Don't use with CLDs that are artificially kept alive");
  /**
   * 如果是匿名类，那么使用它对应的mirror class来跟踪，如果是正常类，则使用class_loader来跟踪
   * 这里_klasses 是一个 Klass *，实际上一个CLD所管理的多个Klass具有相同的性质，比如是否是匿名的
   */
  return is_anonymous() ? _klasses->java_mirror() : class_loader();
}

/**
 * 在对CLD进行卸载的时候，检查这个ClassLoaderData 是否存活
 * 调用者 SystemDictionary::do_unloading -> bool ClassLoaderDataGraph::do_unloading
 * @param is_alive_closure
 * @return
 */
bool ClassLoaderData::is_alive(BoolObjectClosure* is_alive_closure) const {
    /**
     *  搜索 ClassLoaderData::keep_alive()
     *  如果是Null Class Loader加载的cld或者是一个当前还没有完全构造完成的匿名类，那么必须keep_alive()
     *
     *  null 类加载器被用来加载 Java 核心类库（如 java.lang 包中的类）和基本类（如原始数据类型的包装类），这些类在 JVM 启动时就已经预先加载到内存中了。
     *  因为这些类在 JVM 启动时就已经加载到内存中，所以它们不需要经过普通的类加载器加载，也不需要在加载时指定一个特定的类加载器。因此，对于这些系统类和基本类，它们的类加载器通常被设置为 null，表示它们是由 JVM 内部直接加载的。
     */
  bool alive = keep_alive() // null class loader and incomplete anonymous klasses.
      /**
       * G1CMIsAliveClosure::do_object_b
       * 可以看到，这里传入的参数是这个CLD对应的keep_alive_obj，对于匿名类，就是这个类的mirror，对于非匿名类，对应的是这个类的class loader
       */
      || is_alive_closure->do_object_b(keep_alive_object());

  return alive;
}


ClassLoaderData::~ClassLoaderData() {
  // Release C heap structures for all the classes.
  /**
   * 遍历了所有在这个类加载器中加载的类，并对每个类调用 InstanceKlass::release_C_heap_structures
   * 方法释放它们在 C 堆上的结构
   */
  classes_do(InstanceKlass::release_C_heap_structures);

  Metaspace *m = _metaspace;
  if (m != NULL) {
    _metaspace = NULL;
    // release the metaspace
    delete m; // 搜索 Metaspace::~Metaspace()
  }

  // Clear all the JNI handles for methods
  // These aren't deallocated and are going to look like a leak, but that's
  // needed because we can't really get rid of jmethodIDs because we don't
  // know when native code is going to stop using them.  The spec says that
  // they're "invalid" but existing programs likely rely on their being
  // NULL after class unloading.
  if (_jmethod_ids != NULL) {
      /**
       * 清除与这个类加载器相关联的所有 JNI 方法句柄。这样做是为了避免 JNI 方法句柄泄漏，
       * 即使这些句柄实际上无法完全释放，因为不能确定在哪个时刻原生代码将停止使用它们。
       */
    Method::clear_jmethod_ids(this); //
  }
  // Delete lock
  delete _metaspace_lock;

  // Delete free list
  if (_deallocate_list != NULL) {
    delete _deallocate_list;
  }
}

/**
 * Returns true if this class loader data is for the extension class loader.
 */
bool ClassLoaderData::is_ext_class_loader_data() const {
  return SystemDictionary::is_ext_class_loader(class_loader());
}

Metaspace* ClassLoaderData::metaspace_non_null() {
  assert(!DumpSharedSpaces, "wrong metaspace!");
  // If the metaspace has not been allocated, create a new one.  Might want
  // to create smaller arena for Reflection class loaders also.
  // The reason for the delayed allocation is because some class loaders are
  // simply for delegating with no metadata of their own.
  if (_metaspace == NULL) {
    MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
    // Check again if metaspace has been allocated while we were getting this lock.
    if (_metaspace != NULL) {
      return _metaspace;
    }
    if (this == the_null_class_loader_data()) {
      assert (class_loader() == NULL, "Must be");
      set_metaspace(new Metaspace(_metaspace_lock, Metaspace::BootMetaspaceType));
    } else if (is_anonymous()) {
      if (TraceClassLoaderData && Verbose && class_loader() != NULL) {
        tty->print_cr("is_anonymous: %s", class_loader()->klass()->internal_name());
      }
      set_metaspace(new Metaspace(_metaspace_lock, Metaspace::AnonymousMetaspaceType));
    } else if (class_loader()->is_a(SystemDictionary::reflect_DelegatingClassLoader_klass())) {
      if (TraceClassLoaderData && Verbose && class_loader() != NULL) {
        tty->print_cr("is_reflection: %s", class_loader()->klass()->internal_name());
      }
      set_metaspace(new Metaspace(_metaspace_lock, Metaspace::ReflectionMetaspaceType));
    } else {
      set_metaspace(new Metaspace(_metaspace_lock, Metaspace::StandardMetaspaceType));
    }
  }
  return _metaspace;
}

jobject ClassLoaderData::add_handle(Handle h) {
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  return (jobject) _handles.add(h());
}

// Add this metadata pointer to be freed when it's safe.  This is only during
// class unloading because Handles might point to this metadata field.
void ClassLoaderData::add_to_deallocate_list(Metadata* m) {
  // Metadata in shared region isn't deleted.
  if (!m->is_shared()) {
    MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
    if (_deallocate_list == NULL) {
      _deallocate_list = new (ResourceObj::C_HEAP, mtClass) GrowableArray<Metadata*>(100, true);
    }
    _deallocate_list->append_if_missing(m);
  }
}

// Deallocate free metadata on the free list.  How useful the PermGen was!
void ClassLoaderData::free_deallocate_list() {
  // Don't need lock, at safepoint
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
  if (_deallocate_list == NULL) {
    return;
  }
  // Go backwards because this removes entries that are freed.
  for (int i = _deallocate_list->length() - 1; i >= 0; i--) {
    Metadata* m = _deallocate_list->at(i);
    if (!m->on_stack()) {
      _deallocate_list->remove_at(i);
      // There are only three types of metadata that we deallocate directly.
      // Cast them so they can be used by the template function.
      if (m->is_method()) {
        MetadataFactory::free_metadata(this, (Method*)m);
      } else if (m->is_constantPool()) {
        MetadataFactory::free_metadata(this, (ConstantPool*)m);
      } else if (m->is_klass()) {
        MetadataFactory::free_metadata(this, (InstanceKlass*)m);
      } else {
        ShouldNotReachHere();
      }
    }
  }
}

// These anonymous class loaders are to contain classes used for JSR292
ClassLoaderData* ClassLoaderData::anonymous_class_loader_data(oop loader, TRAPS) {
  // Add a new class loader data to the graph.
  return ClassLoaderDataGraph::add(loader, true, THREAD); // 给匿名类创建一个ClassLoaderData
}

const char* ClassLoaderData::loader_name() {
  // Handles null class loader
  return SystemDictionary::loader_name(class_loader());
}

#ifndef PRODUCT
// Define to dump klasses
#undef CLD_DUMP_KLASSES

void ClassLoaderData::dump(outputStream * const out) {
  ResourceMark rm;
  out->print("ClassLoaderData CLD: " PTR_FORMAT ", loader: " PTR_FORMAT ", loader_klass: " PTR_FORMAT " %s {",
      p2i(this), p2i((void *)class_loader()),
      p2i(class_loader() != NULL ? class_loader()->klass() : NULL), loader_name());
  if (claimed()) out->print(" claimed ");
  if (is_unloading()) out->print(" unloading ");
  out->cr();
  if (metaspace_or_null() != NULL) {
    out->print_cr("metaspace: " INTPTR_FORMAT, p2i(metaspace_or_null()));
    metaspace_or_null()->dump(out);
  } else {
    out->print_cr("metaspace: NULL");
  }

#ifdef CLD_DUMP_KLASSES
  if (Verbose) {
    ResourceMark rm;
    Klass* k = _klasses;
    while (k != NULL) {
      out->print_cr("klass " PTR_FORMAT ", %s, CT: %d, MUT: %d", k, k->name()->as_C_string(),
          k->has_modified_oops(), k->has_accumulated_modified_oops());
      assert(k != k->next_link(), "no loops!");
      k = k->next_link();
    }
  }
#endif  // CLD_DUMP_KLASSES
#undef CLD_DUMP_KLASSES
  if (_jmethod_ids != NULL) {
    Method::print_jmethod_ids(this, out);
  }
  out->print_cr("}");
}
#endif // PRODUCT

void ClassLoaderData::verify() {
  oop cl = class_loader();

  guarantee(this == class_loader_data(cl) || is_anonymous(), "Must be the same");
  guarantee(cl != NULL || this == ClassLoaderData::the_null_class_loader_data() || is_anonymous(), "must be");

  // Verify the integrity of the allocated space.
  if (metaspace_or_null() != NULL) {
    metaspace_or_null()->verify();
  }

  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    guarantee(k->class_loader_data() == this, "Must be the same");
    k->verify();
    assert(k != k->next_link(), "no loops!");
  }
}

bool ClassLoaderData::contains_klass(Klass* klass) {
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    if (k == klass) return true;
  }
  return false;
}


// GC root of class loader data created.
ClassLoaderData* ClassLoaderDataGraph::_head = NULL;
ClassLoaderData* ClassLoaderDataGraph::_unloading = NULL;
ClassLoaderData* ClassLoaderDataGraph::_saved_unloading = NULL;
ClassLoaderData* ClassLoaderDataGraph::_saved_head = NULL;

bool ClassLoaderDataGraph::_should_purge = false;

// Add a new class loader data node to the list.  Assign the newly created
// ClassLoaderData into the java/lang/ClassLoader object as a hidden field
/**
 * 这是一个静态方法，搜索 ClassLoaderDataGraph::add 查看具体实现
 * 调用者
 *          ClassLoaderData* ClassLoaderData::anonymous_class_loader_data
 *          和
 *          ClassLoaderData *ClassLoaderDataGraph::find_or_create
 * @param loader
 * @param is_anonymous
 * @return
 */
ClassLoaderData* ClassLoaderDataGraph::add(Handle loader, bool is_anonymous, TRAPS) {
  // We need to allocate all the oops for the ClassLoaderData before allocating the
  // actual ClassLoaderData object.
  ClassLoaderData::Dependencies dependencies(CHECK_NULL);

  No_Safepoint_Verifier no_safepoints; // we mustn't GC until we've installed the
                                       // ClassLoaderData in the graph since the CLD
                                       // contains unhandled oops

  ClassLoaderData* cld = new ClassLoaderData(loader, is_anonymous, dependencies);

  /**
   *  如果不是给匿名类创建CLD
   */
  if (!is_anonymous) {
      /**
       * 获取对应的ClassLoader的地址
       * 搜索 ClassLoaderData** java_lang_ClassLoader::loader_data_addr 查看具体实现
       */
    ClassLoaderData** cld_addr = java_lang_ClassLoader::loader_data_addr(loader());
    // First, Atomically set it
    /**
     * cld_addr是一个指针的指针，即cld_addr指向的位置存放了一个指针，一个指向CLD的指针
     * 首先，比较 cld_addr 地址处存储的指针值 与 NULL 的值是否相等。
        如果相等，则将 cld 的值存储到 cld_addr 地址处，并返回原来存储在该地址处的指针值（即原来的 ClassLoaderData 对象的地址）。
        如果不相等，则不执行任何操作，并直接返回 cld_addr 地址处存储的指针值（即原来的 ClassLoaderData 对象的地址）。
     */
    ClassLoaderData* old = (ClassLoaderData*) Atomic::cmpxchg_ptr(cld, cld_addr, NULL);
    if (old != NULL) { // 如果原来这个cld_addr 存放的不是null
      delete cld;  // 删除刚刚创建的ClassLoaderData
      // Returns the data.
      return old; //依然使用旧的ClassLoaderData
    }
  }

  // We won the race, and therefore the task of adding the data to the list of
  // class loader data
  ClassLoaderData** list_head = &_head;
  ClassLoaderData* next = _head;

  do {
    cld->set_next(next);
    ClassLoaderData* exchanged = (ClassLoaderData*)Atomic::cmpxchg_ptr(cld, list_head, next);
    if (exchanged == next) {
      if (TraceClassLoaderData) {
        ResourceMark rm;
        tty->print("[ClassLoaderData: ");
        tty->print("create class loader data " INTPTR_FORMAT, p2i(cld));
        tty->print(" for instance " INTPTR_FORMAT " of %s", p2i((void *)cld->class_loader()),
                   cld->loader_name());
        tty->print_cr("]");
      }
      return cld;
    }
    next = exchanged;
  } while (true);

}

void ClassLoaderDataGraph::oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->oops_do(f, klass_closure, must_claim);
  }
}

void ClassLoaderDataGraph::keep_alive_oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    if (cld->keep_alive()) {
      cld->oops_do(f, klass_closure, must_claim);
    }
  }
}

void ClassLoaderDataGraph::always_strong_oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim) {
  if (ClassUnloading) {
    keep_alive_oops_do(f, klass_closure, must_claim);
  } else {
    oops_do(f, klass_closure, must_claim);
  }
}

void ClassLoaderDataGraph::cld_do(CLDClosure* cl) {
  for (ClassLoaderData* cld = _head; cl != NULL && cld != NULL; cld = cld->next()) {
    cl->do_cld(cld);
  }
}

/**
 * 方法在 G1RootProcessor::process_java_roots中被调用
 * 根据keep_alive，判断是使用strong 还是 weak的 CLDClosure
 * @param strong
 * @param weak
 */
void ClassLoaderDataGraph::roots_cld_do(CLDClosure* strong, CLDClosure* weak) {
  for (ClassLoaderData* cld = _head;  cld != NULL; cld = cld->_next) {
  /**
   * 遍历当前所有的ClassLoaderData，
   *    如果是keep_alive的，那么就使用 strong，
   *    如果不是keep_alive的，就使用strong
   * 一个CLD需要keep alive，意味着这个CLD对应的class loader是null class loader，或者，这个class是一个正在构造中的匿名类
   */
    CLDClosure* closure = cld->keep_alive() ? strong : weak;
    if (closure != NULL) {
        /**
         * 搜索 class G1CLDClosure : public CLDClosure,查看方法do_cld
         */
      closure->do_cld(cld);
    }
  }
}

void ClassLoaderDataGraph::keep_alive_cld_do(CLDClosure* cl) {
  roots_cld_do(cl, NULL);
}

void ClassLoaderDataGraph::always_strong_cld_do(CLDClosure* cl) {
  if (ClassUnloading) {
    keep_alive_cld_do(cl);
  } else {
    cld_do(cl);
  }
}

void ClassLoaderDataGraph::classes_do(KlassClosure* klass_closure) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->classes_do(klass_closure);
  }
}

void ClassLoaderDataGraph::classes_do(void f(Klass* const)) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->classes_do(f);
  }
}

void ClassLoaderDataGraph::loaded_classes_do(KlassClosure* klass_closure) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->loaded_classes_do(klass_closure);
  }
}

void ClassLoaderDataGraph::classes_unloading_do(void f(Klass* const)) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  // Only walk the head until any clds not purged from prior unloading
  // (CMS doesn't purge right away).
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    cld->classes_do(f);
  }
}

GrowableArray<ClassLoaderData*>* ClassLoaderDataGraph::new_clds() {
  assert(_head == NULL || _saved_head != NULL, "remember_new_clds(true) not called?");

  GrowableArray<ClassLoaderData*>* array = new GrowableArray<ClassLoaderData*>();

  // The CLDs in [_head, _saved_head] were all added during last call to remember_new_clds(true);
  ClassLoaderData* curr = _head;
  while (curr != _saved_head) {
    if (!curr->claimed()) {
      array->push(curr);

      if (TraceClassLoaderData) {
        tty->print("[ClassLoaderData] found new CLD: ");
        curr->print_value_on(tty);
        tty->cr();
      }
    }

    curr = curr->_next;
  }

  return array;
}

bool ClassLoaderDataGraph::unload_list_contains(const void* x) {
  assert(SafepointSynchronize::is_at_safepoint(), "only safe to call at safepoint");
  for (ClassLoaderData* cld = _unloading; cld != NULL; cld = cld->next()) {
    if (cld->metaspace_or_null() != NULL && cld->metaspace_or_null()->contains(x)) {
      return true;
    }
  }
  return false;
}

#ifndef PRODUCT
bool ClassLoaderDataGraph::contains_loader_data(ClassLoaderData* loader_data) {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    if (loader_data == data) {
      return true;
    }
  }

  return false;
}
#endif // PRODUCT


// Move class loader data from main list to the unloaded list for unloading
// and deallocation later.
/**
 * 这是一个静态方法
 * 在   SystemDictionary::do_unloading 中调用
 * @param is_alive_closure
 * @param clean_alive
 * @return
 */
bool ClassLoaderDataGraph::do_unloading(BoolObjectClosure* is_alive_closure, bool clean_alive) {
  ClassLoaderData* data = _head;
  ClassLoaderData* prev = NULL;
  bool seen_dead_loader = false;

  // Save previous _unloading pointer for CMS which may add to unloading list before
  // purging and we don't want to rewalk the previously unloaded class loader data.
  _saved_unloading = _unloading;

  while (data != NULL) {
      /**
       * 如果当前的类加载器依然存活，那么跳过当前的CLD，检查下一个cld
       * 搜索 bool ClassLoaderData::is_alive
       * 搜索 G1CMIsAliveClosure::do_object_b
       * 主要是查看这个cld在刚刚完成的标记的keep_alive_data是否已经被标记存活
       */
    if (data->is_alive(is_alive_closure)) {
      prev = data;
      data = data->next();
      continue;
    }
    seen_dead_loader = true;
    ClassLoaderData* dead = data;
    dead->unload(); // 对这个CLD执行unload操作
    data = data->next();
    // Remove from loader list.
    // This class loader data will no longer be found
    // in the ClassLoaderDataGraph.
    if (prev != NULL) {
      prev->set_next(data);
    } else {
      assert(dead == _head, "sanity check");
      _head = data;
    }
    dead->set_next(_unloading);
    _unloading = dead; // 将这个cld加入到_unloading这个链表中去
  }

  if (clean_alive) {
    // Clean previous versions and the deallocate list.
    ClassLoaderDataGraph::clean_metaspaces();
  }

  if (seen_dead_loader) {
    post_class_unload_events();
  }

  return seen_dead_loader;
}

void ClassLoaderDataGraph::clean_metaspaces() {
  // mark metadata seen on the stack and code cache so we can delete unneeded entries.
  bool has_redefined_a_class = JvmtiExport::has_redefined_a_class();
  MetadataOnStackMark md_on_stack(has_redefined_a_class);

  if (has_redefined_a_class) {
    // purge_previous_versions also cleans weak method links. Because
    // one method's MDO can reference another method from another
    // class loader, we need to first clean weak method links for all
    // class loaders here. Below, we can then free redefined methods
    // for all class loaders.
    for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
      data->classes_do(InstanceKlass::purge_previous_versions);
    }
  }

  // Need to purge the previous version before deallocating.
  free_deallocate_lists();
}

/**
 * 卸载所有的需要卸载的CLD（存放在_unloading链表中），同时调用Metaspace::purge()来卸载元数据空间
 * 这个_unloading链表是在刚刚的remark阶段(当前处于clean_up阶段)通过调用
 *      ConcurrentMarkThread::run -> CMCheckpointRootsFinalClosure::do_void()
 *                  -> ConcurrentMark::checkpointRootsFinal -> ConcurrentMark::weakRefsWork -> SystemDictionary::do_unloading
 * 来加进来的
 */
void ClassLoaderDataGraph::purge() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  ClassLoaderData* list = _unloading; // 需要进行卸载的CLD
  _unloading = NULL;
  ClassLoaderData* next = list;
  while (next != NULL) {
    ClassLoaderData* purge_me = next;
    next = purge_me->next();
    /**
     * 直接调用ClassLoaderData的析构函数，搜索 ClassLoaderData::~ClassLoaderData()
     */
    delete purge_me; //
  }
  /**
   * 卸载元数据空间
   */
  Metaspace::purge();
}

void ClassLoaderDataGraph::post_class_unload_events(void) {
#if INCLUDE_TRACE
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  if (Tracing::enabled()) {
    if (Tracing::is_event_enabled(TraceClassUnloadEvent)) {
      assert(_unloading != NULL, "need class loader data unload list!");
      _class_unload_time = Ticks::now();
      classes_unloading_do(&class_unload_event);
    }
    Tracing::on_unloading_classes();
  }
#endif
}

void ClassLoaderDataGraph::free_deallocate_lists() {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    // We need to keep this data until InstanceKlass::purge_previous_version has been
    // called on all alive classes. See the comment in ClassLoaderDataGraph::clean_metaspaces.
    cld->free_deallocate_list();
  }

  // In some rare cases items added to the unloading list will not be freed elsewhere.
  // To keep it simple, walk the _unloading list also.
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    cld->free_deallocate_list();
  }
}

// CDS support

// Global metaspaces for writing information to the shared archive.  When
// application CDS is supported, we may need one per metaspace, so this
// sort of looks like it.
/**
 *  这是两个静态变量，用来支持CDS(Class Data Sharing（类数据共享）), 忽略
 */
Metaspace* ClassLoaderData::_ro_metaspace = NULL;
Metaspace* ClassLoaderData::_rw_metaspace = NULL;
static bool _shared_metaspaces_initialized = false;

// Initialize shared metaspaces (change to call from somewhere not lazily)
void ClassLoaderData::initialize_shared_metaspaces() {
  assert(DumpSharedSpaces, "only use this for dumping shared spaces");
  assert(this == ClassLoaderData::the_null_class_loader_data(),
         "only supported for null loader data for now");
  assert (!_shared_metaspaces_initialized, "only initialize once");
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  _ro_metaspace = new Metaspace(_metaspace_lock, Metaspace::ROMetaspaceType);
  _rw_metaspace = new Metaspace(_metaspace_lock, Metaspace::ReadWriteMetaspaceType);
  _shared_metaspaces_initialized = true;
}

Metaspace* ClassLoaderData::ro_metaspace() {
  assert(_ro_metaspace != NULL, "should already be initialized");
  return _ro_metaspace;
}

Metaspace* ClassLoaderData::rw_metaspace() {
  assert(_rw_metaspace != NULL, "should already be initialized");
  return _rw_metaspace;
}

ClassLoaderDataGraphKlassIteratorAtomic::ClassLoaderDataGraphKlassIteratorAtomic()
    : _next_klass(NULL) {
  ClassLoaderData* cld = ClassLoaderDataGraph::_head;
  Klass* klass = NULL;

  // Find the first klass in the CLDG.
  while (cld != NULL) {
    klass = cld->_klasses;
    if (klass != NULL) {
      _next_klass = klass;
      return;
    }
    cld = cld->next();
  }
}

Klass* ClassLoaderDataGraphKlassIteratorAtomic::next_klass_in_cldg(Klass* klass) {
  Klass* next = klass->next_link();
  if (next != NULL) {
    return next;
  }

  // No more klasses in the current CLD. Time to find a new CLD.
  ClassLoaderData* cld = klass->class_loader_data();
  while (next == NULL) {
    cld = cld->next();
    if (cld == NULL) {
      break;
    }
    next = cld->_klasses;
  }

  return next;
}

Klass* ClassLoaderDataGraphKlassIteratorAtomic::next_klass() {
  Klass* head = _next_klass;

  while (head != NULL) {
    Klass* next = next_klass_in_cldg(head);

    Klass* old_head = (Klass*)Atomic::cmpxchg_ptr(next, &_next_klass, head);

    if (old_head == head) {
      return head; // Won the CAS.
    }

    head = old_head;
  }

  // Nothing more for the iterator to hand out.
  assert(head == NULL, err_msg("head is " PTR_FORMAT ", expected not null:", p2i(head)));
  return NULL;
}

ClassLoaderDataGraphMetaspaceIterator::ClassLoaderDataGraphMetaspaceIterator() {
  _data = ClassLoaderDataGraph::_head;
}

ClassLoaderDataGraphMetaspaceIterator::~ClassLoaderDataGraphMetaspaceIterator() {}

#ifndef PRODUCT
// callable from debugger
extern "C" int print_loader_data_graph() {
  ClassLoaderDataGraph::dump_on(tty);
  return 0;
}

void ClassLoaderDataGraph::verify() {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    data->verify();
  }
}

void ClassLoaderDataGraph::dump_on(outputStream * const out) {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    data->dump(out);
  }
  MetaspaceAux::dump(out);
}
#endif // PRODUCT

void ClassLoaderData::print_value_on(outputStream* out) const {
  if (class_loader() == NULL) {
    out->print("NULL class_loader");
  } else {
    out->print("class loader " INTPTR_FORMAT, p2i(this));
    class_loader()->print_value_on(out);
  }
}

#if INCLUDE_TRACE

Ticks ClassLoaderDataGraph::_class_unload_time;

void ClassLoaderDataGraph::class_unload_event(Klass* const k) {

  // post class unload event
  EventClassUnload event(UNTIMED);
  event.set_endtime(_class_unload_time);
  event.set_unloadedClass(k);
  oop defining_class_loader = k->class_loader();
  event.set_definingClassLoader(defining_class_loader != NULL ?
                                defining_class_loader->klass() : (Klass*)NULL);
  event.commit();
}

#endif // INCLUDE_TRACE
