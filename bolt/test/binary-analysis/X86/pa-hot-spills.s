# REQUIRES: system-linux, x86-registered-target

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: link_fdata --no-lbr %s %t.o %t.fdata
# RUN: llvm-strip --strip-unneeded %t.o
# RUN: %clang %cflags %t.o -o %t.exe -Wl,-q -nostdlib
# RUN: llvm-bolt-binary-analysis %t.exe --perfdata %t.fdata --scanners=perf-spills \
# RUN:   --lite=0 2>&1 | FileCheck %s

# CHECK: PERF-ADVISOR: hot stack spill in function _start
# CHECK: The instruction is {{.*}}movq{{.*}}%rax{{.*}}-0x10(%rbp)
# CHECK: Spill: stores RAX to [RBP - 16] (8 bytes).
# CHECK: Hint: this hot register-to-stack store is a likely spill/reload cost.

  .globl _start
  .type _start, %function
_start:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x0, %eax
hot_spill:
# FDATA: 1 _start #hot_spill# 100
  movq  %rax, -0x10(%rbp)
  movq  -0x10(%rbp), %rax
  je done
  movl  $0x0, %eax
done:
  popq  %rbp
  retq
.size _start, .-_start
