// Check resolving an instrumentation hook supplied only through LD_PRELOAD.

// REQUIRES: system-linux,bolt-runtime

// RUN: split-file %s %t
// RUN: cc -O2 -fno-inline -fPIE -pie %t/main.c -o %t.exe
// RUN: llvm-readelf -Ws %t.exe | not grep common_func
// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %t/hook.s -o %t-hook.o
// RUN: ld.lld -shared %t-hook.o -o %t-hook.so --hash-style=gnu \
// RUN:   -init=hook_init -fini=hook_fini
// RUN: not llvm-bolt %t.exe -o %t.invalid --instrument=func-probe 2>&1 | \
// RUN:   FileCheck %s --check-prefix=MISSING-HOOK
// RUN: not llvm-bolt %t.exe -o %t.invalid \
// RUN:   --instrument-func-probe-function=common_func 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ORPHAN-HOOK
// RUN: not llvm-bolt %t.exe -o %t.invalid \
// RUN:   --instrument-what-u-need=common_func 2>&1 | \
// RUN:   FileCheck %s --check-prefix=OLD-OPTION
// RUN: llvm-bolt %t.exe -o %t.bolt --relocs=0 --instrument=func-probe \
// RUN:   --instrument-func-probe-function=common_func
// RUN: %t.bolt | FileCheck %s --check-prefix=PLAIN --allow-empty
// RUN: env LD_PRELOAD=%t-hook.so %t.bolt | FileCheck %s --check-prefix=HOOK
// RUN: ld.lld -shared %t-hook.o -o %t-hook-sysv.so --hash-style=sysv \
// RUN:   -init=hook_init -fini=hook_fini
// RUN: env LD_PRELOAD=%t-hook-sysv.so %t.bolt | \
// RUN:   FileCheck %s --check-prefix=HOOK

// PLAIN-NOT: hooked
// HOOK: hooked
// MISSING-HOOK: --instrument=func-probe requires --instrument-func-probe-function
// ORPHAN-HOOK: --instrument-func-probe-function requires --instrument=func-probe
// OLD-OPTION: Unknown command line argument '--instrument-what-u-need=common_func'

//--- main.c
__attribute__((noinline)) static double foo(double value) {
  return value + 1.0;
}

int main(void) { return foo(41.0) == 42.0 ? 0 : 1; }

//--- hook.s
// clang-format off
.data
calls:
  .quad 0
message:
  .ascii "hooked\n"

.text
.globl common_func
.type common_func, @function
common_func:
  lock incq calls(%rip)
  xorl %eax, %eax
  xorl %ecx, %ecx
  xorl %edx, %edx
  xorl %esi, %esi
  xorl %edi, %edi
  xorl %r8d, %r8d
  xorl %r9d, %r9d
  xorl %r10d, %r10d
  xorl %r11d, %r11d
  pxor %xmm0, %xmm0
  retq
.size common_func, .-common_func

.globl hook_init
.type hook_init, @function
hook_init:
  retq
.size hook_init, .-hook_init

.globl hook_fini
.type hook_fini, @function
hook_fini:
  cmpq $0, calls(%rip)
  je .Ldone
  movl $1, %eax
  movl $1, %edi
  leaq message(%rip), %rsi
  movl $7, %edx
  syscall
.Ldone:
  retq
.size hook_fini, .-hook_fini
// clang-format on
