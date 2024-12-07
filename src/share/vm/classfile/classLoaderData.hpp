/*
 * Copyright (c) 2012, 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP
#define SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceCounters.hpp"
#include "runtime/mutex.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_TRACE
#include "utilities/ticks.hpp"
#endif

//
// A class loader represents a linkset. Conceptually, a linkset identifies
// the complete transitive closure of resolved links that a dynamic linker can
// produce.
//
// A ClassLoaderData also encapsulates the allocation space, called a metaspace,
// used by the dynamic linker to allocate the runtime representation of all
// the types it defines.
//
// ClassLoaderData are stored in the runtime representation of classes and the
// system dictionary, are roots of garbage collection, and provides iterators
// for root tracing and other GC operations.

class ClassLoaderData;
class JNIMethodBlock;
class Metadebug;

// GC root for walking class loader data created

class ClassLoaderDataGraph : public AllStatic {
  friend class ClassLoaderData;
  friend class ClassLoaderDataGraphMetaspaceIterator;
  friend class ClassLoaderDataGraphKlassIteratorAtomic;
  friend class VMStructs;
 private:
  // All CLDs (except the null CLD) can be reached by walking _head->_next->...
  static ClassLoaderData* _head;
  static ClassLoaderData* _unloading;
  // CMS support.
  static ClassLoaderData* _saved_head;
  static ClassLoaderData* _saved_unloading;
  static bool _should_purge;

  static ClassLoaderData* add(Handle class_loader, bool anonymous, TRAPS);
  static void post_class_unload_events(void);
  static void clean_metaspaces();
 public:
  static ClassLoaderData* find_or_create(Handle class_loader, TRAPS);
  static void purge();
  static void clear_claimed_marks();
  // oops do
  static void oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim);
  static void keep_alive_oops_do(OopClosure* blk, KlassClosure* klass_closure, bool must_claim);
  static void always_strong_oops_do(OopClosure* blk, KlassClosure* klass_closure, bool must_claim);
  // cld do
  static void cld_do(CLDClosure* cl);
  static void roots_cld_do(CLDClosure* strong, CLDClosure* weak);
  static void keep_alive_cld_do(CLDClosure* cl);
  static void always_strong_cld_do(CLDClosure* cl);
  // klass do
  static void classes_do(KlassClosure* klass_closure);
  static void classes_do(void f(Klass* const));
  static void loaded_classes_do(KlassClosure* klass_closure);
  static void classes_unloading_do(void f(Klass* const));
  static bool do_unloading(BoolObjectClosure* is_alive, bool clean_alive);

  // CMS support.
  static void remember_new_clds(bool remember) { _saved_head = (remember ? _head : NULL); }
  static GrowableArray<ClassLoaderData*>* new_clds();

  static void set_should_purge(bool b) { _should_purge = b; }
  static void purge_if_needed() {
    // Only purge the CLDG for CMS if concurrent sweep is complete.
    if (_should_purge) {
      purge();
      // reset for next time.
      set_should_purge(false);
    }
  }

  static void free_deallocate_lists();

  static void dump_on(outputStream * const out) PRODUCT_RETURN;
  static void dump() { dump_on(tty); }
  static void verify();

  static bool unload_list_contains(const void* x);
#ifndef PRODUCT
  static bool contains_loader_data(ClassLoaderData* loader_data);
#endif

#if INCLUDE_TRACE
 private:
  static Ticks _class_unload_time;
  static void class_unload_event(Klass* const k);
#endif
};

// ClassLoaderData class

class ClassLoaderData : public CHeapObj<mtClass> {
  friend class VMStructs;
 private:
  class Dependencies VALUE_OBJ_CLASS_SPEC {
    objArrayOop _list_head;
    void locked_add(objArrayHandle last,
                    objArrayHandle new_dependency,
                    Thread* THREAD);
   public:
    Dependencies() : _list_head(NULL) {}
    Dependencies(TRAPS) : _list_head(NULL) {
      init(CHECK);
    }
    void add(Handle dependency, TRAPS);
    void init(TRAPS);
    void oops_do(OopClosure* f); // 具体实现搜索 void ClassLoaderData::Dependencies::oops_do(OopClosure* f)
  };

  class ChunkedHandleList VALUE_OBJ_CLASS_SPEC {
    struct Chunk : public CHeapObj<mtClass> {
      static const size_t CAPACITY = 32;

      oop _data[CAPACITY];
      volatile juint _size;
      Chunk* _next;

      Chunk(Chunk* c) : _next(c), _size(0) { }
    };

    Chunk* _head;

    void oops_do_chunk(OopClosure* f, Chunk* c, const juint size);

   public:
    ChunkedHandleList() : _head(NULL) {}
    ~ChunkedHandleList();

    // Only one thread at a time can add, guarded by ClassLoaderData::metaspace_lock().
    // However, multiple threads can execute oops_do concurrently with add.
    oop* add(oop o);
    void oops_do(OopClosure* f);
  };

  friend class ClassLoaderDataGraph;
  friend class ClassLoaderDataGraphKlassIteratorAtomic;
  friend class ClassLoaderDataGraphMetaspaceIterator;
  friend class MetaDataFactory;
  friend class Method;

  static ClassLoaderData * _the_null_class_loader_data;
  /**
   * oop 类型的变量，用于唯一标识一个类加载器或规范的类路径。它可以用于在Java虚拟机中唯一地识别一个类加载器或类路径。
   * 当且仅当一个class loader 被卸载，这个class loader下面的类才可以被卸载。
   * 对于一个普通的类，G1GC将这个_class_loader作为这个CLD 的keep_alive_object ，对于一个匿名类，则将_mirror作为它的keep_alive_object
   */
  oop _class_loader;          // oop used to uniquely identify a class loader
                              // class loader or a canonical class path
  /**
   * 保存该类加载器数据与其他类加载器数据之间的依赖关系。它记录了该类加载器数据所依赖的其他类加载器数据。
   */
  Dependencies _dependencies; // holds dependencies from this class loader
                              // data to others.
  /**
   * 指向分配类加载器中的类所需的元空间。元空间是用于存储类元数据的内存区域。
   */
  Metaspace * _metaspace;  // Meta-space where meta-data defined by the
                           // classes in the class loader are allocated.
  Mutex* _metaspace_lock;  // Locks the metaspace for allocations and setup.
  /**
   * 是否正在卸载这个ClassLoader
   */
  bool _unloading;         // true if this class loader goes away
  /**
   * 是否是一个必须keep_alive的ClassLoader
   */
  bool _keep_alive;        // if this CLD is kept alive without a keep_alive_object().
  /**
   * 是否是匿名类的ClassLoader
   */
  bool _is_anonymous;      // if this CLD is for an anonymous class
  volatile int _claimed;   // true if claimed, for example during GC traces.
                           // To avoid applying oop closure more than once.
                           // Has to be an int because we cas it
  /**
   *  这个ClassLoader下面挂载的所有的类，显然，这是一个数组。
   *  很显然，这些类具有相同的_is_anoymous性质
   */
  Klass* _klasses;         // The classes defined by the class loader.
  /**
   * _handles 是一个 ChunkedHandleList 类型的对象，用于存储对常量池数组等的句柄，其生命周期与相应的类加载器相同。
   */
  ChunkedHandleList _handles; // Handles to constant pool arrays, etc, which
                              // have the same life cycle of the corresponding ClassLoader.

  // These method IDs are created for the class loader and set to NULL when the
  // class loader is unloaded.  They are rarely freed, only for redefine classes
  // and if they lose a data race in InstanceKlass.
  JNIMethodBlock*                  _jmethod_ids;

  // Metadata to be deallocated when it's safe at class unloading, when
  // this class loader isn't unloaded itself.
  GrowableArray<Metadata*>*      _deallocate_list;

  // Support for walking class loader data objects
  /**
   * 用来给ClassLoaderDataGraph维护的ClassLoaderData对象的链表的next指针
   */
  ClassLoaderData* _next; /// Next loader_datas created

  // ReadOnly and ReadWrite metaspaces (static because only on the null
  // class loader for now).
  static Metaspace* _ro_metaspace;
  static Metaspace* _rw_metaspace;

  void set_next(ClassLoaderData* next) { _next = next; }
  ClassLoaderData* next() const        { return _next; }

  ClassLoaderData(Handle h_class_loader, bool is_anonymous, Dependencies dependencies);
  ~ClassLoaderData();

  void set_metaspace(Metaspace* m) { _metaspace = m; }

  Mutex* metaspace_lock() const { return _metaspace_lock; }

  // GC interface.
  void clear_claimed()          { _claimed = 0; }
  bool claimed() const          { return _claimed == 1; }
  bool claim();

  void unload();
  /**
   * ClassLoaderData::keep_alive() 方法
   * if this CLD is kept alive without a keep_alive_object().
   * 默认是true，搜索 set_keep_alive 方法 查看其设置为false的情况
   * 搜索 ClassLoaderData::ClassLoaderData 查看其初始化的过程
   * @return
   */
  bool keep_alive() const       { return _keep_alive; }
  void classes_do(void f(Klass*));
  void loaded_classes_do(KlassClosure* klass_closure);
  void classes_do(void f(InstanceKlass*));

  // Deallocate free list during class unloading.
  void free_deallocate_list();

  // Allocate out of this class loader data
  MetaWord* allocate(size_t size);

 public:

  bool is_alive(BoolObjectClosure* is_alive_closure) const;

  // Accessors
  Metaspace* metaspace_or_null() const     { return _metaspace; }

  static ClassLoaderData* the_null_class_loader_data() {
    return _the_null_class_loader_data;
  }

  bool is_anonymous() const { return _is_anonymous; }

  static void init_null_class_loader_data() {
    assert(_the_null_class_loader_data == NULL, "cannot initialize twice");
    assert(ClassLoaderDataGraph::_head == NULL, "cannot initialize twice");

    // We explicitly initialize the Dependencies object at a later phase in the initialization
    _the_null_class_loader_data = new ClassLoaderData((oop)NULL, false, Dependencies());
    ClassLoaderDataGraph::_head = _the_null_class_loader_data;
    assert(_the_null_class_loader_data->is_the_null_class_loader_data(), "Must be");
    if (DumpSharedSpaces) {
      _the_null_class_loader_data->initialize_shared_metaspaces();
    }
  }

  bool is_the_null_class_loader_data() const {
    return this == _the_null_class_loader_data;
  }
  bool is_ext_class_loader_data() const;

  // The Metaspace is created lazily so may be NULL.  This
  // method will allocate a Metaspace if needed.
  Metaspace* metaspace_non_null();

    /**
     * oop 类型的变量，用于唯一标识一个类加载器或规范的类路径。它可以用于在Java虚拟机中唯一地识别一个类加载器或类路径。
     * loader 被设计为一个 oop（ordinary object pointer），因为它是一个对 Java 对象的引用，
     * 而类加载器（java.lang.ClassLoader 的实例）本质上就是一个普通的 Java 对象。这种设计背后有几个重要的原因和逻辑：
     * 当且仅当一个class loader 被卸载，这个class loader下面的类才可以被卸载。
     * 对于一个普通的类，G1GC将这个_class_loader作为这个CLD 的keep_alive_object ，
     * 对于一个匿名类，则将_mirror作为它的keep_alive_object
     */
  oop class_loader() const      { return _class_loader; }

  /**
   * The object the GC is using to keep this ClassLoaderData alive.
   * 搜索 ClassLoaderData::keep_alive_object
   */

  /**
   * 专门给G1GC使用的这个ClassLoader的keep_alive_object
   *    对于一个普通的类，G1GC将这个_class_loader作为这个CLD 的keep_alive_object ，
   *    对于一个匿名类，则将_mirror作为它的keep_alive_object
   * @return
   */
  oop keep_alive_object() const;

  // Returns true if this class loader data is for a loader going away.
  bool is_unloading() const     {
    assert(!(is_the_null_class_loader_data() && _unloading), "The null class loader can never be unloaded");
    return _unloading;
  }

  // Used to make sure that this CLD is not unloaded.
  void set_keep_alive(bool value) { _keep_alive = value; }

  unsigned int identity_hash() {
    return _class_loader == NULL ? 0 : _class_loader->identity_hash();
  }

  // Used when tracing from klasses.
  void oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim);

  void classes_do(KlassClosure* klass_closure);

  JNIMethodBlock* jmethod_ids() const              { return _jmethod_ids; }
  void set_jmethod_ids(JNIMethodBlock* new_block)  { _jmethod_ids = new_block; }

  void print_value() { print_value_on(tty); }
  void print_value_on(outputStream* out) const;
  void dump(outputStream * const out) PRODUCT_RETURN;
  void verify();
  const char* loader_name();

  jobject add_handle(Handle h);
  void add_class(Klass* k);
  void remove_class(Klass* k);
  bool contains_klass(Klass* k);
  void record_dependency(Klass* to, TRAPS);
  void init_dependencies(TRAPS);

  void add_to_deallocate_list(Metadata* m);

  static ClassLoaderData* class_loader_data(oop loader);
  static ClassLoaderData* class_loader_data_or_null(oop loader);
  static ClassLoaderData* anonymous_class_loader_data(oop loader, TRAPS);
  static void print_loader(ClassLoaderData *loader_data, outputStream *out);

  // CDS support
  Metaspace* ro_metaspace();
  Metaspace* rw_metaspace();
  void initialize_shared_metaspaces();
};

// An iterator that distributes Klasses to parallel worker threads.
class ClassLoaderDataGraphKlassIteratorAtomic : public StackObj {
 Klass* volatile _next_klass;
 public:
  ClassLoaderDataGraphKlassIteratorAtomic();
  Klass* next_klass();
 private:
  static Klass* next_klass_in_cldg(Klass* klass);
};

class ClassLoaderDataGraphMetaspaceIterator : public StackObj {
  ClassLoaderData* _data;
 public:
  ClassLoaderDataGraphMetaspaceIterator();
  ~ClassLoaderDataGraphMetaspaceIterator();
  bool repeat() { return _data != NULL; }
  Metaspace* get_next() {
    assert(_data != NULL, "Should not be NULL in call to the iterator");
    Metaspace* result = _data->metaspace_or_null();
    _data = _data->next();
    // This result might be NULL for class loaders without metaspace
    // yet.  It would be nice to return only non-null results but
    // there is no guarantee that there will be a non-null result
    // down the list so the caller is going to have to check.
    return result;
  }
};
#endif // SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP
