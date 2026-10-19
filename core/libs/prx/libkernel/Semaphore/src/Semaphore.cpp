#include "prx/libkernel/Semaphore/include/Semaphore.hpp"
#include "prx/libkernel/Pthread/include/JobDiag.hpp"
#include "prx/libc/include/general/LogMacros.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <chrono>

#include <stdexcept>
#include <string>
#include <utility>

KernelSemaPrivate::KernelSemaPrivate(std::int32_t initCount, std::int32_t maxCount, std::string name, bool isFifo)
 : name(std::move(name)), tokenCount(initCount), initCount(initCount), maxCount(maxCount), isFifo(isFifo) {
}

extern "C" {

int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt) {
 (void)opt;
 if (sem == nullptr || name == nullptr || attr > 2 || init < 0 || max <= 0 || init > max) {
  APS5_INVALID_ARG_EX;
 }

 *sem = new KernelSemaPrivate(init, max, std::string(name), attr == 1);
 if (aps5::jobdiag::DiagEnabled()) { char e[64]; std::snprintf(e, sizeof(e), "init=%d max=%d", init, max); aps5::jobdiag::Event("sema-create", reinterpret_cast<std::uintptr_t>(*sem), e); }
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelPollSema(KernelSema sem, int need) {
    APS5_TRACE("sceKernelPollSema", (uintptr_t)sem);
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (sem->tokenCount < need) {
  return SCE_KERNEL_ERROR_EBUSY;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelSignalSema(KernelSema sem, int count) {
    APS5_TRACE("sceKernelSignalSema", (uintptr_t)sem);
 if (sem == nullptr || count < 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount - sem->tokenCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 sem->tokenCount += count;
 if (aps5::jobdiag::DiagEnabled()) { char e[64]; std::snprintf(e, sizeof(e), "+%d -> %d", count, sem->tokenCount); aps5::jobdiag::Event("sema-signal", reinterpret_cast<std::uintptr_t>(sem), e); }
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time) {
    APS5_TRACE("sceKernelWaitSema", (uintptr_t)sem);
    aps5::jobdiag::NoteLastWait("sema", (uintptr_t)sem);
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 const std::uint64_t generation = sem->cancelGeneration;
 if (aps5::jobdiag::DiagEnabled()) { char e[80]; std::snprintf(e, sizeof(e), "need=%d have=%d to=%d", need, sem->tokenCount, time ? static_cast<int>(*time) : -1); aps5::jobdiag::Event("sema-wait", reinterpret_cast<std::uintptr_t>(sem), e); }
 ++sem->waiterCount;
 ++sem->cancelableCount;
 struct WaiterGuard {
  KernelSemaPrivate* sem;
  std::uint64_t generation;
  ~WaiterGuard() {
   --sem->waiterCount;
   if (sem->cancelGeneration == generation) {
    --sem->cancelableCount;
   }
   sem->condition.NotifyAll();
  }
 } waiterGuard{sem, generation};

 const auto waitStart = std::chrono::steady_clock::now();
 const auto traceWait = [&](bool timedOut) {
  KernelTraceWait_nid_postfix("sema", __builtin_return_address(0), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), timedOut);
 };
 const auto released = [&] { return sem->tokenCount >= need || sem->deleted || sem->cancelGeneration != generation; };
 if (time == nullptr) {
  sem->condition.Wait(lock, released);
  traceWait(false);
  if (sem->deleted) {
   return SCE_KERNEL_ERROR_EACCES;
  }
  if (sem->cancelGeneration != generation) {
   return SCE_KERNEL_ERROR_ECANCELED;
  }
  sem->tokenCount -= need;
  return KERNEL_SEMA_OK;
 }

 const std::uint64_t deadline = TimedWait::DeadlineNanos(*time);
 const bool acquired = sem->condition.WaitUntil(lock, deadline, released);
 *time = static_cast<KernelUseconds>(TimedWait::RemainingMicros(deadline));
 traceWait(!acquired);
 if (sem->deleted) {
  return SCE_KERNEL_ERROR_EACCES;
 }
 if (sem->cancelGeneration != generation) {
  return SCE_KERNEL_ERROR_ECANCELED;
 }
 if (!acquired) {
  return SCE_KERNEL_ERROR_ETIMEDOUT;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelCancelSema(KernelSema sem, int count, int* threads) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 if (threads != nullptr) {
  *threads = sem->cancelableCount;
 }
 sem->tokenCount = count < 0 ? sem->initCount : count;
 sem->cancelableCount = 0;
 ++sem->cancelGeneration;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelDeleteSema(KernelSema sem) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 sem->deleted = true;
 sem->condition.NotifyAll();
 sem->condition.Wait(lock, [&] { return sem->waiterCount == 0; });
 lock.unlock();
 delete sem;
 return KERNEL_SEMA_OK;
}

}
