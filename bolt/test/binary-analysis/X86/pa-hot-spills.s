# REQUIRES: system-linux, x86-registered-target

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: printf 'no_lbr\n1 _start 7 100\n' > %t.fdata
# RUN: llvm-strip --strip-unneeded --keep-section=.debug_info --keep-section=.debug_abbrev \
# RUN:   --keep-section=.debug_line --keep-section=.debug_str --keep-section=.debug_line_str %t.o
# RUN: %clang %cflags %t.o -o %t.exe -Wl,-q -nostdlib
# RUN: llvm-bolt-binary-analysis %t.exe --perfdata %t.fdata --scanners=perf-spills \
# RUN:   --perf-advisor-report=%t.report --lite=0 2>&1 | FileCheck %s --check-prefix=STDOUT
# RUN: FileCheck %s --check-prefix=REPORT < %t.report

# STDOUT: BOLT-INFO: wrote performance advisor report to {{.*}}.report
# STDOUT-NOT: PERF-ADVISOR REPORT

# REPORT: PERF-ADVISOR REPORT
# REPORT: Function: _start
# REPORT: Function heat: {{[0-9]+}}
# REPORT: Hot stack slot traffic
# REPORT: Source: pa-hot-spills.c:11:3
# REPORT: Instruction: {{.*}}movq{{.*}}%rax{{.*}}-0x10(%rbp)
# REPORT: Stack traffic: stores RAX to [RBP - 16] (8 bytes).
# REPORT: Evidence: the same stack slot is later reloaded into RAX
# REPORT: Reload source: pa-hot-spills.c:12:3
# REPORT: Hint: this is a spill-like hot stack slot, not a proof that the compiler spilled a value.

  .file 1 "pa-hot-spills.c"
  .globl _start
  .type _start, %function
_start:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x0, %eax
  .loc 1 11 3
  movq  %rax, -0x10(%rbp)
  .loc 1 12 3
  movq  -0x10(%rbp), %rax
  .loc 1 13 3
  je done
  movl  $0x0, %eax
done:
  popq  %rbp
  retq
.size _start, .-_start
