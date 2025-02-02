// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Ken Ashcraft <opensource@google.com>

#include <config.h>
#include "static_vars.h"
#include <stddef.h>
#include <new>                          // for operator new
#ifndef _WIN32
#include <pthread.h>                    // for pthread_atfork
#endif
#include "internal_logging.h"  // for CHECK_CONDITION
#include "common.h"
#include "sampler.h"           // for Sampler
#include "getenv_safe.h"       // TCMallocGetenvSafe
#include "base/googleinit.h"

#include "thread_cache_ptr.h"
#include "system-alloc.h"

namespace tcmalloc {

bool Static::inited_;
SizeMap Static::sizemap_;
CentralFreeList Static::central_cache_[kClassSizesMax];
PageHeapAllocator<Span> Static::span_allocator_;
PageHeapAllocator<StackTrace> Static::stacktrace_allocator_;
Span Static::sampled_objects_;
std::atomic<StackTrace*> Static::growth_stacks_;
StaticStorage<PageHeap> Static::pageheap_;

void Static::InitStaticVars() {
  sizemap_.Init();
  span_allocator_.Init();
  span_allocator_.New(); // Reduce cache conflicts
  span_allocator_.New(); // Reduce cache conflicts
  stacktrace_allocator_.Init();

  for (int i = 0; i < num_size_classes(); ++i) {
    central_cache_[i].Init(i);
  }

  new (pageheap()) PageHeap(sizemap_.min_span_size_in_pages());

#if defined(ENABLE_AGGRESSIVE_DECOMMIT_BY_DEFAULT)
  const bool kDefaultAggressiveDecommit = true;
#else
  const bool kDefaultAggressiveDecommit = false;
#endif


  bool aggressive_decommit =
    tcmalloc::commandlineflags::StringToBool(
      TCMallocGetenvSafe("TCMALLOC_AGGRESSIVE_DECOMMIT"),
                         kDefaultAggressiveDecommit);

  pageheap()->SetAggressiveDecommit(aggressive_decommit);

  inited_ = true;

  DLL_Init(&sampled_objects_);
}

// These following two functions are registered via pthread_atfork to
// make sure the central_cache locks remain in a consisten state in
// the forked version of the thread. Also our OSX integration uses it
// for mi_force_lock.

void CentralCacheLockAll() NO_THREAD_SAFETY_ANALYSIS
{
  Static::pageheap_lock()->Lock();
  for (int i = 0; i < Static::num_size_classes(); ++i)
    Static::central_cache()[i].Lock();
  ThreadCachePtr::GetSlowTLSLock()->Lock();
  GetSysAllocLock()->Lock();
}

void CentralCacheUnlockAll() NO_THREAD_SAFETY_ANALYSIS
{
  GetSysAllocLock()->Unlock();
  ThreadCachePtr::GetSlowTLSLock()->Unlock();
  for (int i = 0; i < Static::num_size_classes(); ++i)
    Static::central_cache()[i].Unlock();
  Static::pageheap_lock()->Unlock();
}

void Static::InitLateMaybeRecursive() {
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(TCMALLOC_NO_ATFORK) \
  && !defined(__FreeBSD__) && !defined(_AIX)
  // OSX has it's own way of handling atfork in malloc (see
  // libc_override_osx.h).
  //
  // Windows doesn't do fork.
  //
  // FreeBSD and AIX seemingly cannot handle early pthread_atfork
  // calls. So we don't.
  //
  // For other OSes we do pthread_atfork even if standard seemingly
  // discourages pthread_atfork, asking apps to do only
  // async-signal-safe calls between fork and exec.
  //
  // We're deliberately attempting to register atfork handlers as part
  // of malloc initialization. So very early. This ensures that our
  // handler is called last and that means fork will try to grab
  // tcmalloc locks last avoiding possible issues with many other
  // locks that are held around calls to malloc. I.e. if we don't do
  // that, fork() grabbing malloc lock before such other lock would be
  // prone to deadlock, if some other thread holds other lock and
  // calls malloc.
  //
  // We still leave some way of disabling it via
  // -DTCMALLOC_NO_ATFORK. It looks like on glibc even with fully
  // static binaries malloc is really initialized very early. But I
  // can see how combination of static linking and other libc-s could
  // be less fortunate and allow some early app constructors to run
  // before malloc is ever called.

  pthread_atfork(
    CentralCacheLockAll,    // parent calls before fork
    CentralCacheUnlockAll,  // parent calls after fork
    CentralCacheUnlockAll); // child calls after fork
#endif
}

}  // namespace tcmalloc
