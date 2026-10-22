# REQUIRES: system-linux, x86-registered-target

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: printf 'no_lbr\n1 _start 7 100\n' > %t.fdata
# RUN: llvm-strip --strip-unneeded --keep-section=.debug_info --keep-section=.debug_abbrev \
# RUN:   --keep-section=.debug_line --keep-section=.debug_str --keep-section=.debug_line_str %t.o
# RUN: %clang %cflags %t.o -o %t.exe -Wl,-q -nostdlib
# RUN: llvm-bolt-binary-analysis %t.exe --perfdata %t.fdata --scanners=perf-spills \
# RUN:   --perf-advisor-report=%t.report.json --lite=0 2>&1 | FileCheck %s --check-prefix=STDOUT
# RUN: FileCheck %s --check-prefix=REPORT < %t.report.json

# STDOUT: BOLT-INFO: wrote performance advisor report to {{.*}}.report
# STDOUT-NOT: PERF-ADVISOR REPORT

# REPORT: "report": "llvm-bolt-performance-advisor"
# REPORT: "schema_version": 2
# REPORT: "function_count": 1
# REPORT: "hint_count": 1
# REPORT: "hot_stack_slot_count": 1
# REPORT: "hot_loop_count": 0
# REPORT: "name": "_start"
# REPORT: "source": "pa-hot-spills.c:11:3"
# REPORT: "instruction": "{{.*}}movq{{.*}}%rax{{.*}}-0x10(%rbp)"
# REPORT: "register": "RAX"
# REPORT: "stack_base": "RBP"
# REPORT: "stack_offset": -16
# REPORT: "size": 8
# REPORT: "stack_slot": "[RBP - 16]"
# REPORT: "reload": {
# REPORT: "register": "RAX"
# REPORT: "source": "pa-hot-spills.c:12:3"

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
