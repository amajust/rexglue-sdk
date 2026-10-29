/**
 * @file        rex/core/ucontext_android.cpp
 * @brief       Minimal getcontext/makecontext/swapcontext/setcontext for
 *              aarch64 Android (bionic provides ucontext_t but not the
 *              deprecated ucontext API functions).
 *
 * Scope note: the runtime's only consumer is Fiber (fiber_posix.cpp), which
 * uses the no-argument makecontext form (the trampoline pulls its arguments
 * from TLS) and never relies on signal-mask save/restore. This file
 * implements exactly that subset:
 *   - callee-saved integer registers x19-x30, sp and pc
 *   - caller-saved registers are left unspecified on resume, which is ABI
 *     legal: a resumed getcontext/swapcontext behaves like the call returned
 *     0 (regs[0] is forced to zero before the saved pc is entered)
 *   - makecontext switches pc to the given function and sp to the top of
 *     uc_stack (16-byte aligned); uc_link is ignored (the fiber entry never
 *     returns)
 *
 * Floating point callee-saved registers (d8-d15) are NOT preserved. The
 * fiber switch sites in the runtime are plain C++ control flow (no active
 * FP values live across SwitchTo), matching the upstream ucontext backend's
 * assumptions; if that ever changes, extend the save/restore blocks.
 *
 * Field offsets are pinned by static_asserts against the bionic/arm64
 * layout (uc_mcontext is the kernel's struct sigcontext): any layout drift
 * fails the build loudly instead of corrupting stacks.
 */

#include <rex/platform.h>

#if defined(__ANDROID__) && defined(__aarch64__)

#include <stddef.h>
#include <signal.h>
#include <ucontext.h>

extern "C" {

// --- Layout pins (bionic arm64: uc_mcontext = kernel struct sigcontext) ----
// Derivation (bionic sys/ucontext.h, aarch64, LP64):
//   uc_flags 0 | uc_link 8 | uc_stack 16 (stack_t 24B) | uc_sigmask union 40
//   (kernel LP64 sigset_t = 8B) | __padding[128-8] -> 168 | uc_mcontext is
//   16-byte aligned (mcontext __reserved has aligned(16)) -> ** 176 **.
static_assert(offsetof(ucontext_t, uc_flags) == 0, "bionic ucontext_t drift");
static_assert(offsetof(ucontext_t, uc_link) == 8, "bionic ucontext_t drift");
static_assert(offsetof(ucontext_t, uc_stack) == 16, "bionic ucontext_t drift");
static_assert(offsetof(ucontext_t, uc_sigmask) == 40, "bionic ucontext_t drift");
static_assert(offsetof(ucontext_t, uc_mcontext) == 176, "bionic ucontext_t drift");
static_assert(offsetof(mcontext_t, fault_address) == 0, "bionic mcontext_t drift");
static_assert(offsetof(mcontext_t, regs) == 8, "bionic mcontext_t drift");
static_assert(offsetof(mcontext_t, sp) == 256, "bionic mcontext_t drift");
static_assert(offsetof(mcontext_t, pc) == 264, "bionic mcontext_t drift");
static_assert(offsetof(mcontext_t, pstate) == 272, "bionic mcontext_t drift");

// Register slots inside ucontext_t (uc_mcontext at 176, regs[] at +8):
//   regs[i]  -> 184 + 8*i;  sp -> 432;  pc -> 440;  pstate -> 448
#define REX_UC_REGS_BASE 184
#define REX_UC_SP 432
#define REX_UC_PC 440
#define REX_UC_PSTATE 448

__attribute__((naked)) int getcontext(ucontext_t* ucp) {
  __asm__ volatile(
      // x0 = ucp
      "mov     x2, x30\n"                       // resume pc = return address
      "add     x3, x0, %0\n"
      "stp     x19, x20, [x3, %1]\n"            // regs[19..20]
      "stp     x21, x22, [x3, %2]\n"
      "stp     x23, x24, [x3, %3]\n"
      "stp     x25, x26, [x3, %4]\n"
      "stp     x27, x28, [x3, %5]\n"
      "stp     x29, x30, [x3, %6]\n"            // regs[29..30]
      "mov     x4, sp\n"
      "str     x4, [x3, %7]\n"                  // sp
      "str     x2, [x3, %8]\n"                  // pc
      "str     xzr, [x0, %9]\n"                 // regs[0] = 0: resume -> return 0
      "str     xzr, [x0, %10]\n"                // pstate = 0
      "mov     w0, #0\n"
      "ret\n"
      :
      : "I"(REX_UC_REGS_BASE), "I"(8 * 19), "I"(8 * 21), "I"(8 * 23),
        "I"(8 * 25), "I"(8 * 27), "I"(8 * 29), "I"(REX_UC_SP - REX_UC_REGS_BASE),
        "I"(REX_UC_PC - REX_UC_REGS_BASE), "I"(REX_UC_REGS_BASE),
        "I"(REX_UC_PSTATE));
}

void makecontext(ucontext_t* ucp, void (*func)(), int argc, ...) {
  // Fiber trampolines take no ucontext arguments (args ride in TLS).
  ucp->uc_mcontext.pc = reinterpret_cast<uint64_t>(func);
  ucp->uc_mcontext.sp = (reinterpret_cast<uint64_t>(ucp->uc_stack.ss_sp) +
                         ucp->uc_stack.ss_size) &
                        ~static_cast<uint64_t>(0xF);
}

__attribute__((naked)) int setcontext(const ucontext_t* ucp) {
  __asm__ volatile(
      // x0 = ucp
      "str     xzr, [x0, %0]\n"                 // regs[0] = 0: resume -> return 0
      "add     x3, x0, %1\n"
      "ldp     x19, x20, [x3, %2]\n"
      "ldp     x21, x22, [x3, %3]\n"
      "ldp     x23, x24, [x3, %4]\n"
      "ldp     x25, x26, [x3, %5]\n"
      "ldp     x27, x28, [x3, %6]\n"
      "ldp     x29, x30, [x3, %7]\n"
      "ldr     x4, [x3, %8]\n"
      "mov     sp, x4\n"
      "ldr     x16, [x3, %9]\n"
      "br      x16\n"
      :
      : "I"(REX_UC_REGS_BASE), "I"(REX_UC_REGS_BASE), "I"(8 * 19),
        "I"(8 * 21), "I"(8 * 23), "I"(8 * 25), "I"(8 * 27), "I"(8 * 29),
        "I"(REX_UC_SP - REX_UC_REGS_BASE), "I"(REX_UC_PC - REX_UC_REGS_BASE));
}

__attribute__((naked)) int swapcontext(ucontext_t* oucp, const ucontext_t* ucp) {
  __asm__ volatile(
      // x0 = oucp, x1 = ucp
      "mov     x2, x30\n"                       // resume pc = return address
      "add     x3, x0, %0\n"
      "stp     x19, x20, [x3, %1]\n"
      "stp     x21, x22, [x3, %2]\n"
      "stp     x23, x24, [x3, %3]\n"
      "stp     x25, x26, [x3, %4]\n"
      "stp     x27, x28, [x3, %5]\n"
      "stp     x29, x30, [x3, %6]\n"
      "mov     x4, sp\n"
      "str     x4, [x3, %7]\n"                  // sp
      "str     x2, [x3, %8]\n"                  // pc
      "str     xzr, [x3, #0]\n"                 // regs[0] = 0
      "str     xzr, [x3, %9]\n"                 // pstate = 0
      // Restore ucp
      "str     xzr, [x1, %0]\n"                 // resume returns 0
      "add     x3, x1, %0\n"
      "ldp     x19, x20, [x3, %1]\n"
      "ldp     x21, x22, [x3, %2]\n"
      "ldp     x23, x24, [x3, %3]\n"
      "ldp     x25, x26, [x3, %4]\n"
      "ldp     x27, x28, [x3, %5]\n"
      "ldp     x29, x30, [x3, %6]\n"
      "ldr     x4, [x3, %7]\n"
      "mov     sp, x4\n"
      "ldr     x16, [x3, %8]\n"
      "br      x16\n"
      :
      : "I"(REX_UC_REGS_BASE), "I"(8 * 19), "I"(8 * 21), "I"(8 * 23),
        "I"(8 * 25), "I"(8 * 27), "I"(8 * 29), "I"(REX_UC_SP - REX_UC_REGS_BASE),
        "I"(REX_UC_PC - REX_UC_REGS_BASE), "I"(REX_UC_PSTATE));
}

}  // extern "C"

#endif  // __ANDROID__ && __aarch64__
