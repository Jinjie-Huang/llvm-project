# Check function entry/exit instrumentation and original entry patching.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe
# RUN: llvm-readelf -r %t.exe | FileCheck %s --check-prefix=NO-RELOCS
# RUN: llvm-bolt %t.exe -o %t.bolt --instrument=func-probe \
# RUN:   --instrument-func-probe-function=common_func --relocs=0 --lite=0 \
# RUN:   2>&1 | FileCheck %s --check-prefix=BOLT
# RUN: llvm-readelf -r %t.bolt | FileCheck %s --check-prefix=NO-RELOCS
# RUN: %t.bolt
# RUN: llvm-objdump -d --disassemble-symbols=foo,common_func %t.bolt | \
# RUN:   FileCheck %s --check-prefix=NEW
# RUN: llvm-objdump -d --disassemble-symbols=foo.org.0 %t.bolt | \
# RUN:   FileCheck %s --check-prefix=OLD
# RUN: llvm-objdump -d --disassemble-symbols=secondary_entry.org.0 %t.bolt | \
# RUN:   FileCheck %s --check-prefix=OLD-SECONDARY
# RUN: ld.lld -pie %t.o -o %t.pie
# RUN: llvm-readelf -r %t.pie | FileCheck %s --check-prefix=NO-RELOCS
# RUN: llvm-bolt %t.pie -o %t.pie.bolt --instrument=func-probe \
# RUN:   --instrument-func-probe-function=common_func --relocs=0 --lite=0
# RUN: %t.pie.bolt

# BOLT: func-probe inserted 3 entry call(s) and 3 exit call(s) in 2 function(s)
# NO-RELOCS: There are no relocations in this file.

# NEW-LABEL: <common_func>:
# NEW-NOT: callq
# NEW: retq

# NEW-LABEL: <foo>:
# NEW: callq {{.*}} <common_func>
# NEW: testq
# NEW: callq {{.*}} <common_func>
# NEW: fxrstor64
# NEW: movq %r11, %rsp
# NEW: retq
# NEW: callq {{.*}} <common_func>
# NEW: fxrstor64
# NEW: movq %r11, %rsp
# NEW: retq

# OLD-LABEL: <foo.org.0>:
# OLD-NEXT: {{.*}} jmp {{.*}} <foo>

# OLD-SECONDARY-LABEL: <secondary_entry.org.0>:
# OLD-SECONDARY-NEXT: {{.*}} jmp {{.*}} <foo+0x{{[1-9a-f][0-9a-f]*}}>

.data
.globl counter
counter:
  .quad 0

.text
.globl common_func
.type common_func, @function
common_func:
  incq counter(%rip)
  retq
.size common_func, .-common_func

.globl foo
.type foo, @function
foo:
  movq %rdi, %rax
  testq %rdi, %rdi
  je .Lzero
  addq $1, %rax
  retq
.Lzero:
.globl secondary_entry
secondary_entry:
  movl $7, %eax
  retq
  .rept 32
  nop
  .endr
.size foo, .-foo

.globl _start
.type _start, @function
_start:
  movq $41, %rdi
  callq foo
  cmpq $42, %rax
  jne .Lfail
  cmpq $3, counter(%rip)
  jne .Lfail
  xorl %edi, %edi
  jmp .Lexit
.Lfail:
  movl $1, %edi
.Lexit:
  movl $60, %eax
  syscall
  .rept 32
  nop
  .endr
.size _start, .-_start
