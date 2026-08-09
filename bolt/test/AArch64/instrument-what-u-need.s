# Check function entry/exit instrumentation and original entry patching.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld -m aarch64elf %t.o -o %t.exe
# RUN: llvm-readelf -r %t.exe | FileCheck %s --check-prefix=NO-RELOCS
# RUN: llvm-bolt %t.exe -o %t.bolt --instrument=func-probe \
# RUN:   --instrument-func-probe-function=common_func --relocs=0 --lite=0 \
# RUN:   2>&1 | FileCheck %s --check-prefix=BOLT
# RUN: llvm-readelf -r %t.bolt | FileCheck %s --check-prefix=NO-RELOCS
# RUN: llvm-objdump -d --disassemble-symbols=foo,common_func %t.bolt | \
# RUN:   FileCheck %s --check-prefix=NEW
# RUN: llvm-objdump -d --disassemble-symbols=foo.org.0 %t.bolt | \
# RUN:   FileCheck %s --check-prefix=OLD
# RUN: llvm-objdump -d --disassemble-symbols=secondary_entry.org.0 %t.bolt | \
# RUN:   FileCheck %s --check-prefix=OLD-SECONDARY

# BOLT: func-probe inserted 3 entry call(s) and 3 exit call(s) in 2 function(s)
# NO-RELOCS: There are no relocations in this file.

# NEW-LABEL: <common_func>:
# NEW-NOT: bl
# NEW: ret

# NEW-LABEL: <foo>:
# NEW: adrp x16
# NEW: add x16
# NEW: blr x16
# NEW: cbz
# NEW: adrp x9
# NEW: add x9
# NEW: adrp x16
# NEW: add x16
# NEW: blr x16
# NEW: ret
# NEW: adrp x16
# NEW: add x16
# NEW: blr x16
# NEW: ret

# OLD-LABEL: <foo.org.0>:
# OLD-NEXT: {{.*}} adrp x16
# OLD-NEXT: {{.*}} add x16
# OLD-NEXT: {{.*}} br x16

# OLD-SECONDARY-LABEL: <secondary_entry.org.0>:
# OLD-SECONDARY-NEXT: {{.*}} adrp x16
# OLD-SECONDARY-NEXT: {{.*}} add x16
# OLD-SECONDARY-NEXT: {{.*}} br x16

.data
.globl counter
counter:
  .quad 0

.text
.globl common_func
.type common_func, %function
common_func:
  adrp x16, counter
  add x16, x16, :lo12:counter
  ldr x17, [x16]
  add x17, x17, #1
  str x17, [x16]
  ret
.size common_func, .-common_func

.globl foo
.type foo, %function
foo:
  cbz x0, .Lzero
  adrp x9, counter
  add x9, x9, :lo12:counter
  ldr x10, [x9]
  add x0, x0, #1
  ret
.Lzero:
.globl secondary_entry
secondary_entry:
  mov x0, #7
  ret
  .rept 32
  nop
  .endr
.size foo, .-foo

.globl _start
.type _start, %function
_start:
  mov x0, #41
  bl foo
  ret
  .rept 32
  nop
  .endr
.size _start, .-_start
