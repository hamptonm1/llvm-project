//===-- asan_memory_profile.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
//
// This file implements __sanitizer_print_memory_profile.
//===----------------------------------------------------------------------===//

#include "asan/asan_allocator.h"
#include "lsan/lsan_common.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_platform.h"
#include "sanitizer_common/sanitizer_stackdepot.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "sanitizer_common/sanitizer_stoptheworld.h"

#if CAN_SANITIZE_LEAKS
#  if SANITIZER_LINUX || SANITIZER_NETBSD
#    include <link.h>
#  endif

namespace __asan {

struct AllocationSite {
  u32 id;
  uptr total_size;
  uptr count;
};

class HeapProfile {
 public:
  HeapProfile() { allocations_.reserve(1024); }

  void ProcessChunk(const AsanChunkView &cv) {
    if (cv.IsAllocated()) {
      total_allocated_user_size_ += cv.UsedSize();
      total_allocated_count_++;
      u32 id = cv.GetAllocStackId();
      if (id)
        Insert(id, cv.UsedSize());
    } else if (cv.IsQuarantined()) {
      total_quarantined_user_size_ += cv.UsedSize();
      total_quarantined_count_++;
    } else {
      total_other_count_++;
    }
  }

  void Print(uptr top_percent, uptr max_number_of_contexts) {
    Sort(allocations_.data(), allocations_.size(),
         [](const AllocationSite &a, const AllocationSite &b) {
           return a.total_size > b.total_size;
         });
    CHECK(total_allocated_user_size_);
    uptr total_shown = 0;
    Printf("Live Heap Allocations: %zd bytes in %zd chunks; quarantined: "
           "%zd bytes in %zd chunks; %zd other chunks; total chunks: %zd; "
           "showing top %zd%% (at most %zd unique contexts)\n",
           total_allocated_user_size_, total_allocated_count_,
           total_quarantined_user_size_, total_quarantined_count_,
           total_other_count_, total_allocated_count_ +
           total_quarantined_count_ + total_other_count_, top_percent,
           max_number_of_contexts);
    for (uptr i = 0; i < Min(allocations_.size(), max_number_of_contexts);
         i++) {
      auto &a = allocations_[i];
      Printf("%zd byte(s) (%zd%%) in %zd allocation(s)\n", a.total_size,
             a.total_size * 100 / total_allocated_user_size_, a.count);
      StackDepotGet(a.id).Print();
      total_shown += a.total_size;
      if (total_shown * 100 / total_allocated_user_size_ > top_percent)
        break;
    }
  }

 private:
  uptr total_allocated_user_size_ = 0;
  uptr total_allocated_count_ = 0;
  uptr total_quarantined_user_size_ = 0;
  uptr total_quarantined_count_ = 0;
  uptr total_other_count_ = 0;
  InternalMmapVector<AllocationSite> allocations_;

  void Insert(u32 id, uptr size) {
    // Linear lookup will be good enough for most cases (although not all).
    for (uptr i = 0; i < allocations_.size(); i++) {
      if (allocations_[i].id == id) {
        allocations_[i].total_size += size;
        allocations_[i].count++;
        return;
      }
    }
    allocations_.push_back({id, size, 1});
  }
};

static void ChunkCallback(uptr chunk, void *arg) {
  reinterpret_cast<HeapProfile*>(arg)->ProcessChunk(
      FindHeapChunkByAllocBeg(chunk));
}

static void MemoryProfileCB(const SuspendedThreadsList &suspended_threads_list,
                            void *argument) {
  HeapProfile hp;
  __lsan::ForEachChunk(ChunkCallback, &hp);
  uptr *Arg = reinterpret_cast<uptr*>(argument);
  hp.Print(Arg[0], Arg[1]);

  if (Verbosity())
    __asan_print_accumulated_stats();
}

struct DoStopTheWorldParam {
  StopTheWorldCallback callback;
  void *argument;
};

static void LockDefStuffAndStopTheWorld(DoStopTheWorldParam *param) {
  __lsan::LockThreadRegistry();
  __lsan::LockAllocator();
  __sanitizer::StopTheWorld(param->callback, param->argument);
  __lsan::UnlockAllocator();
  __lsan::UnlockThreadRegistry();
}

#if SANITIZER_LINUX || SANITIZER_NETBSD
static int LockStuffAndStopTheWorldCallback(struct dl_phdr_info *info,
                                            size_t size, void *data) {
  DoStopTheWorldParam *param = reinterpret_cast<DoStopTheWorldParam *>(data);
  LockDefStuffAndStopTheWorld(param);
  return 1;
}
#endif

static void LockStuffAndStopTheWorld(StopTheWorldCallback callback,
                                     void *argument) {
  DoStopTheWorldParam param = {callback, argument};

#  if SANITIZER_LINUX || SANITIZER_NETBSD
  // For libc dep systems, symbolization uses dl_iterate_phdr, which acquire a
  // dl write lock. It could deadlock if the lock is already acquired by one of
  // suspended. So calling stopTheWorld inside dl_iterate_phdr, first wait for
  // that lock to be released (if acquired) and than suspend all threads
  dl_iterate_phdr(LockStuffAndStopTheWorldCallback, &param);
#  else
  LockDefStuffAndStopTheWorld(&param);
#  endif
}
}  // namespace __asan

#endif  // CAN_SANITIZE_LEAKS

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE
void __sanitizer_print_memory_profile(uptr top_percent,
                                      uptr max_number_of_contexts) {
#if CAN_SANITIZE_LEAKS
  uptr Arg[2];
  Arg[0] = top_percent;
  Arg[1] = max_number_of_contexts;
  __asan::LockStuffAndStopTheWorld(__asan::MemoryProfileCB, Arg);
#endif  // CAN_SANITIZE_LEAKS
}
}  // extern "C"
