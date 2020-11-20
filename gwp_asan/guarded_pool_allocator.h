//===-- guarded_pool_allocator.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GWP_ASAN_GUARDED_POOL_ALLOCATOR_H_
#define GWP_ASAN_GUARDED_POOL_ALLOCATOR_H_

#include "gwp_asan/common.h"
#include "gwp_asan/definitions.h"
#include "gwp_asan/mutex.h"
#include "gwp_asan/options.h"
#include "gwp_asan/stack_trace_compressor.h"

#include <stddef.h>
#include <stdint.h>

namespace gwp_asan {
// This class is the primary implementation of the allocator portion of GWP-
// ASan. It is the sole owner of the pool of sequentially allocated guarded
// slots. It should always be treated as a singleton.

// Functions in the public interface of this class are thread-compatible until
// init() is called, at which point they become thread-safe (unless specified
// otherwise).
class GuardedPoolAllocator {
public:
  // Name of the GWP-ASan mapping that for `Metadata`.
  static constexpr const char *kGwpAsanMetadataName = "GWP-ASan Metadata";

  // During program startup, we must ensure that memory allocations do not land
  // in this allocation pool if the allocator decides to runtime-disable
  // GWP-ASan. The constructor value-initialises the class such that if no
  // further initialisation takes place, calls to shouldSample() and
  // pointerIsMine() will return false.
  constexpr GuardedPoolAllocator() {}
  GuardedPoolAllocator(const GuardedPoolAllocator &) = delete;
  GuardedPoolAllocator &operator=(const GuardedPoolAllocator &) = delete;

  // Note: This class is expected to be a singleton for the lifetime of the
  // program. If this object is initialised, it will leak the guarded page pool
  // and metadata allocations during destruction. We can't clean up these areas
  // as this may cause a use-after-free on shutdown.
  ~GuardedPoolAllocator() = default;

  // Initialise the rest of the members of this class. Create the allocation
  // pool using the provided options. See options.inc for runtime configuration
  // options.
  void init(const options::Options &Opts);
  void uninitTestOnly();

  // Functions exported for libmemunreachable's use on Android. disable()
  // installs a lock in the allocator that prevents any thread from being able
  // to allocate memory, until enable() is called.
  void disable();
  void enable();

  typedef void (*iterate_callback)(uintptr_t base, size_t size, void *arg);
  // Execute the callback Cb for every allocation the lies in [Base, Base +
  // Size). Must be called while the allocator is disabled. The callback can not
  // allocate.
  void iterate(void *Base, size_t Size, iterate_callback Cb, void *Arg);

  // This function is used to signal the allocator to indefinitely stop
  // functioning, as a crash has occurred. This stops the allocator from
  // servicing any further allocations permanently.
  void stop();

  // Return whether the allocation should be randomly chosen for sampling.
  GWP_ASAN_ALWAYS_INLINE bool shouldSample() {
    // NextSampleCounter == 0 means we "should regenerate the counter".
    //                   == 1 means we "should sample this allocation".
    // AdjustedSampleRatePlusOne is designed to intentionally underflow. This
    // class must be valid when zero-initialised, and we wish to sample as
    // infrequently as possible when this is the case, hence we underflow to
    // UINT32_MAX.
    if (GWP_ASAN_UNLIKELY(ThreadLocals.NextSampleCounter == 0))
      ThreadLocals.NextSampleCounter =
          (getRandomUnsigned32() % (AdjustedSampleRatePlusOne - 1)) + 1;

    return GWP_ASAN_UNLIKELY(--ThreadLocals.NextSampleCounter == 0);
  }

  // Returns whether the provided pointer is a current sampled allocation that
  // is owned by this pool.
  GWP_ASAN_ALWAYS_INLINE bool pointerIsMine(const void *Ptr) const {
    return State.pointerIsMine(Ptr);
  }

  // Allocate memory in a guarded slot, and return a pointer to the new
  // allocation. Returns nullptr if the pool is empty, the requested size is too
  // large for this pool to handle, or the requested size is zero.
  void *allocate(size_t Size);

  // Deallocate memory in a guarded slot. The provided pointer must have been
  // allocated using this pool. This will set the guarded slot as inaccessible.
  void deallocate(void *Ptr);

  // Returns the size of the allocation at Ptr.
  size_t getSize(const void *Ptr);

  // Returns a pointer to the Metadata region, or nullptr if it doesn't exist.
  const AllocationMetadata *getMetadataRegion() const { return Metadata; }

  // Returns a pointer to the AllocatorState region.
  const AllocatorState *getAllocatorState() const { return &State; }

private:
  // Name of actively-occupied slot mappings.
  static constexpr const char *kGwpAsanAliveSlotName = "GWP-ASan Alive Slot";
  // Name of the guard pages. This includes all slots that are not actively in
  // use (i.e. were never used, or have been free()'d).)
  static constexpr const char *kGwpAsanGuardPageName = "GWP-ASan Guard Page";
  // Name of the mapping for `FreeSlots`.
  static constexpr const char *kGwpAsanFreeSlotsName = "GWP-ASan Metadata";

  static constexpr size_t kInvalidSlotID = SIZE_MAX;

  // These functions anonymously map memory or change the permissions of mapped
  // memory into this process in a platform-specific way. Pointer and size
  // arguments are expected to be page-aligned. These functions will never
  // return on error, instead electing to kill the calling process on failure.
  // Note that memory is initially mapped inaccessible. In order for RW
  // mappings, call mapMemory() followed by markReadWrite() on the returned
  // pointer. Each mapping is named on platforms that support it, primarily
  // Android. This name must be a statically allocated string, as the Android
  // kernel uses the string pointer directly.
  void *mapMemory(size_t Size, const char *Name) const;
  void unmapMemory(void *Ptr, size_t Size, const char *Name) const;
  void markReadWrite(void *Ptr, size_t Size, const char *Name) const;
  void markInaccessible(void *Ptr, size_t Size, const char *Name) const;

  // Get the page size from the platform-specific implementation. Only needs to
  // be called once, and the result should be cached in PageSize in this class.
  static size_t getPlatformPageSize();

  // Returns a pointer to the metadata for the owned pointer. If the pointer is
  // not owned by this pool, the result is undefined.
  AllocationMetadata *addrToMetadata(uintptr_t Ptr) const;

  // Reserve a slot for a new guarded allocation. Returns kInvalidSlotID if no
  // slot is available to be reserved.
  size_t reserveSlot();

  // Unreserve the guarded slot.
  void freeSlot(size_t SlotIndex);

  // Raise a SEGV and set the corresponding fields in the Allocator's State in
  // order to tell the crash handler what happened. Used when errors are
  // detected internally (Double Free, Invalid Free).
  void trapOnAddress(uintptr_t Address, Error E);

  static GuardedPoolAllocator *getSingleton();

  // Install a pthread_atfork handler.
  void installAtFork();

  gwp_asan::AllocatorState State;

  // A mutex to protect the guarded slot and metadata pool for this class.
  Mutex PoolMutex;
  // Record the number allocations that we've sampled. We store this amount so
  // that we don't randomly choose to recycle a slot that previously had an
  // allocation before all the slots have been utilised.
  size_t NumSampledAllocations = 0;
  // Pointer to the allocation metadata (allocation/deallocation stack traces),
  // if any.
  AllocationMetadata *Metadata = nullptr;

  // Pointer to an array of free slot indexes.
  size_t *FreeSlots = nullptr;
  // The current length of the list of free slots.
  size_t FreeSlotsLength = 0;

  // See options.{h, inc} for more information.
  bool PerfectlyRightAlign = false;

  // Backtrace function provided by the supporting allocator. See `options.h`
  // for more information.
  options::Backtrace_t Backtrace = nullptr;

  // The adjusted sample rate for allocation sampling. Default *must* be
  // nonzero, as dynamic initialisation may call malloc (e.g. from libstdc++)
  // before GPA::init() is called. This would cause an error in shouldSample(),
  // where we would calculate modulo zero. This value is set UINT32_MAX, as when
  // GWP-ASan is disabled, we wish to never spend wasted cycles recalculating
  // the sample rate.
  uint32_t AdjustedSampleRatePlusOne = 0;

  // Pack the thread local variables into a struct to ensure that they're in
  // the same cache line for performance reasons. These are the most touched
  // variables in GWP-ASan.
  struct alignas(8) ThreadLocalPackedVariables {
    constexpr ThreadLocalPackedVariables()
        : RandomState(0xff82eb50), NextSampleCounter(0), RecursiveGuard(false) {
    }
    // Initialised to a magic constant so that an uninitialised GWP-ASan won't
    // regenerate its sample counter for as long as possible. The xorshift32()
    // algorithm used below results in getRandomUnsigned32(0xff82eb50) ==
    // 0xfffffea4.
    uint32_t RandomState;
    // Thread-local decrementing counter that indicates that a given allocation
    // should be sampled when it reaches zero.
    uint32_t NextSampleCounter : 31;
    // Guard against recursivity. Unwinders often contain complex behaviour that
    // may not be safe for the allocator (i.e. the unwinder calls dlopen(),
    // which calls malloc()). When recursive behaviour is detected, we will
    // automatically fall back to the supporting allocator to supply the
    // allocation.
    bool RecursiveGuard : 1;
  };
  static_assert(sizeof(ThreadLocalPackedVariables) == sizeof(uint64_t),
                "thread local data does not fit in a uint64_t");

  class ScopedRecursiveGuard {
  public:
    ScopedRecursiveGuard() { ThreadLocals.RecursiveGuard = true; }
    ~ScopedRecursiveGuard() { ThreadLocals.RecursiveGuard = false; }
  };

  // Initialise the PRNG, platform-specific.
  void initPRNG();

  // xorshift (32-bit output), extremely fast PRNG that uses arithmetic
  // operations only. Seeded using platform-specific mechanisms by initPRNG().
  uint32_t getRandomUnsigned32();

  static GWP_ASAN_TLS_INITIAL_EXEC ThreadLocalPackedVariables ThreadLocals;
};
} // namespace gwp_asan

#endif // GWP_ASAN_GUARDED_POOL_ALLOCATOR_H_
