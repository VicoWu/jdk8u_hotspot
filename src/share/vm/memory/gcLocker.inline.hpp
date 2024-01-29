/*
 * Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP
#define SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP

#include "memory/gcLocker.hpp"

/**
 * 需要将 lock_critical，enter_critical和jni_lock区分开
 * @param thread
 */
inline void GC_locker::lock_critical(JavaThread* thread) {
  if (!thread->in_critical()) { // 如果线程当前不在临界区(这并不意味着其他线程不在临界区，但是我们确定当前给这个thread调用lock_critical是该thread第一次进入临界区用)，并且需要gc，那么，
    if (needs_gc()) { // 线程当前不在临界区（第一次尝试进入），并且需要gc，那么，暂时不要进入关键区
      // jni_lock call calls enter_critical under the lock so that the
      // global lock count and per thread count are in agreement.
      // 慢速加锁的方式增加关键区计数，不要进入关键区，而是在JNICritical_lock上等待，等gc完成以后，会收到这个锁上的通知，然后进入关键区
      jni_lock(thread); // 这个慢路径方法会一直block waiting，直到gc完成收到锁通知，才会进入关键区，然后退出
      return;
    }

    increment_debug_jni_lock_count();
  }
    // 现成当前不在关键区，或者，虽然在关键区，但是当前没有gc的需求，那么，就
  thread->enter_critical(); // 快速方式进入临界区，即只是将对应线程的临界区计数加1
}

inline void GC_locker::unlock_critical(JavaThread* thread) {
  if (thread->in_last_critical()) { // 当前准备离开的临界区是该线程的最后一个临界区，即一旦离开这个临界区，该线程则不再处于临界区
    if (needs_gc()) { // 如果有gc正在等待线程离开临界区
      // jni_unlock call calls exit_critical under the lock so that
      // the global lock count and per thread count are in agreement.
      jni_unlock(thread); // 通过慢路径离开临界区
      return;
    }
    decrement_debug_jni_lock_count();
  }
  thread->exit_critical(); // 快速方式离开临界区，即只是将对应线程的临界区计数减去1
}

#endif // SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP
