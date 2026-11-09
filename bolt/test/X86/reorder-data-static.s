## Estimate data heat from execution profiles without memory samples. The loop
## reads hotA three times, but absolute and large only once. Counts per byte put hotA
## first, absolute second, large third, and the rarely accessed cold last.
## Address-taking, stores, indirect/indexed loads, and unprofiled functions must
## not heat their referenced objects. Check the emitted addresses and execution.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux --defsym ABS=1 %s -o %t.o
# RUN: link_fdata %s %t.o %t.fdata
# RUN: link_fdata %s %t.o %t.cycles.fdata CYCLES --no-lbr
# RUN: sed -i 's/^no_lbr$/no_lbr cycles:u/' %t.cycles.fdata
# RUN: llvm-strip --strip-unneeded %t.o
# RUN: ld.lld --emit-relocs -Ttext=0x200000 -Tdata=0x300000 %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --data %t.fdata --reorder-data=.data \
# RUN:   --reorder-data-algo=static --print-reordered-data > %t.log
# RUN: FileCheck %s --check-prefix=HEAT --input-file=%t.log
# RUN: llvm-objdump -d --disassemble-symbols=foo %t.bolt | FileCheck %s --check-prefixes=CODE,ABS
# RUN: llvm-readelf -sW %t.bolt | FileCheck %s --check-prefix=UNCHANGED
# RUN: %t.bolt

## YAML profiles carry execution counts and need no memory profile extension.
# RUN: llvm-bolt %t.exe -o %t.save --data %t.fdata -w %t.yaml
# RUN: llvm-bolt %t.exe -o %t.yaml.bolt --data %t.yaml --reorder-data=.data \
# RUN:   --reorder-data-algo=static
# RUN: llvm-objdump -d --disassemble-symbols=foo %t.yaml.bolt | FileCheck %s --check-prefixes=CODE,ABS
# RUN: %t.yaml.bolt

## Basic samples can also supply the estimated block frequencies.
# RUN: llvm-bolt %t.exe -o %t.cycles.bolt --data %t.cycles.fdata \
# RUN:   --reorder-data=.data --reorder-data-algo=static
# RUN: llvm-objdump -d --disassemble-symbols=foo %t.cycles.bolt | FileCheck %s --check-prefixes=CODE,ABS
# RUN: %t.cycles.bolt

## The memory-profile algorithms retain their behavior with a pure LBR input.
# RUN: llvm-bolt %t.exe -o %t.count --data %t.fdata --reorder-data=.data
# RUN: llvm-readelf -sW %t.count | FileCheck %s --check-prefix=NOHEAT
# RUN: llvm-bolt %t.exe -o %t.noprofile --reorder-data=.data --reorder-data-algo=static
# RUN: llvm-readelf -sW %t.noprofile | FileCheck %s --check-prefix=NOHEAT

## The memory-profile algorithm also moves referenced child symbols correctly.
# RUN: echo "4 foo 0 4 hotA_sub 0 100" > %t.mem.fdata
# RUN: llvm-bolt %t.exe -o %t.mem.bolt --data %t.mem.fdata --reorder-data=.data
# RUN: llvm-objdump -d --disassemble-symbols=foo %t.mem.bolt | FileCheck %s --check-prefix=CHILD
# RUN: %t.mem.bolt

## Also exercise direct RIP-relative loads in a PIE.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.pie.o
# RUN: link_fdata %s %t.pie.o %t.pie.fdata
# RUN: llvm-strip --strip-unneeded %t.pie.o
# RUN: ld.lld --emit-relocs -pie -Ttext=0x200000 -Tdata=0x300000 %t.pie.o -o %t.pie
# RUN: llvm-bolt %t.pie -o %t.pie.bolt --data %t.pie.fdata \
# RUN:   --reorder-data=.data --reorder-data-algo=static --print-reordered-data > %t.pie.log
# RUN: FileCheck %s --check-prefix=HEAT --input-file=%t.pie.log
# RUN: llvm-objdump -d --disassemble-symbols=foo %t.pie.bolt | FileCheck %s --check-prefixes=CODE,RIP

# HEAT:      Hot global symbols for .data:
# HEAT-NEXT: (object: hotA, {{.*}}weight=187.50000
# HEAT-NEXT: (object: absolute, {{.*}}weight=62.50000
# HEAT-NEXT: (object: large, {{.*}}weight=3.90625
# HEAT-NEXT: (object: cold, {{.*}}weight=0.06250
# HEAT:      reorder-data: 5001/5001 (100.0%) weighted loads, 304 hot bytes

# CODE-LABEL: <foo>:
# CODE: movl {{.*}}# 0x[[#%x,HOT:]] <hotA>
# CODE: addl {{.*}}# 0x[[#HOT+4]] <hotA_sub>
# CODE: addl {{.*}}# 0x[[#HOT+8]]
# CODE: addl {{.*}}# 0x[[#HOT+32]] <large>
# ABS: movl 0x[[#HOT+16]], %ecx
# ABS: movl 0x300150, %ecx
# RIP: movl {{.*}}%ecx {{.*}}# 0x[[#HOT+16]] <absolute>
# CODE: leaq {{.*}}# 0x300110 <indirect>
# CODE: movl (%rdx), %ecx
# CODE: leaq {{.*}}# 0x300120 <indexed>
# CODE: movl (%r8,%rcx,4), %ecx
# CODE: movl $0x0, {{.*}}# 0x300130 <written>
# CODE: leaq {{.*}}# 0x300140 <address_only>
# CODE: movl {{.*}}# 0x[[#HOT+288]] <cold>

# CHILD-LABEL: <foo>:
# CHILD: movl {{.*}}# 0x[[#%x,MEMHOT:]] <hotA>
# CHILD: addl {{.*}}# 0x[[#MEMHOT+4]] <hotA_sub>

# UNCHANGED-DAG: 0000000000300110 16 OBJECT {{.*}} indirect
# UNCHANGED-DAG: 0000000000300120 16 OBJECT {{.*}} indexed
# UNCHANGED-DAG: 0000000000300130 16 OBJECT {{.*}} written
# UNCHANGED-DAG: 0000000000300140 16 OBJECT {{.*}} address_only
# UNCHANGED-DAG: 0000000000300150 16 OBJECT {{.*}} unprofiled
# NOHEAT-DAG: 0000000000300000 16 OBJECT {{.*}} cold
# NOHEAT-DAG: 0000000000300010 256 OBJECT {{.*}} large
# NOHEAT-DAG: 0000000000300160 16 OBJECT {{.*}} absolute
# NOHEAT-DAG: 0000000000300170 16 OBJECT {{.*}} hotA

# FDATA: 0 [unknown] 0 1 foo 0 0 10
# FDATA: 1 foo #entry_branch# 1 foo #rare# 0 1
# FDATA: 1 foo #entry_branch# 1 foo #loop# 0 9
# FDATA: 1 foo #loop_branch# 1 foo #loop# 0 991
# FDATA: 1 foo #loop_branch# 1 foo #loop_exit# 0 9
# CYCLES: 1 foo #foo# 100
# CYCLES: 1 foo #loop# 10000
# CYCLES: 1 foo #rare# 10

.text
.globl _start
.type _start,@function
_start:
  movl $2, %edi
  call foo
  cmpl $15, %eax
  jne fail
  xorl %edi, %edi
  movl $60, %eax
  syscall
fail:
  movl $1, %edi
  movl $60, %eax
  syscall
.size _start, .-_start

.globl foo
.type foo,@function
foo:
  testl %edi, %edi
entry_branch:
  je rare
loop:
  movl hotA(%rip), %eax
  addl hotA_sub(%rip), %eax
  addl hotA+8(%rip), %eax
  addl large(%rip), %eax
.ifdef ABS
  movl absolute, %ecx
  # No relocation or symbolic operand: this reference cannot follow moved data.
  movl 0x300150, %ecx
.else
  movl absolute(%rip), %ecx
.endif
  leaq indirect(%rip), %rdx
  movl (%rdx), %ecx
  leaq indexed(%rip), %r8
  movl (%r8,%rcx,4), %ecx
  movl $0, written(%rip)
  leaq address_only(%rip), %rdx
  decl %edi
loop_branch:
  jne loop
loop_exit:
  ret
rare:
  movl cold(%rip), %eax
  ret
.size foo, .-foo

.globl unprofiled_function
.type unprofiled_function,@function
unprofiled_function:
  movl unprofiled(%rip), %eax
  ret
.size unprofiled_function, .-unprofiled_function

.data
.balign 16
.globl cold
.type cold,@object
cold:
  .long 777
  .zero 12
.size cold, .-cold

.globl large
.type large,@object
large:
  .long 5
  .zero 252
.size large, .-large

.globl indirect
.type indirect,@object
indirect:
  .zero 16
.size indirect, .-indirect

.globl indexed
.type indexed,@object
indexed:
  .zero 16
.size indexed, .-indexed

.globl written
.type written,@object
written:
  .long 1
  .zero 12
.size written, .-written

.globl address_only
.type address_only,@object
address_only:
  .zero 16
.size address_only, .-address_only

.globl unprofiled
.type unprofiled,@object
unprofiled:
  .zero 16
.size unprofiled, .-unprofiled

.globl absolute
.type absolute,@object
absolute:
  .zero 16
.size absolute, .-absolute

.globl hotA
.type hotA,@object
hotA:
  .long 6
.globl hotA_sub
.type hotA_sub,@object
hotA_sub:
  .long 4
.size hotA_sub, .-hotA_sub
  .zero 8
.size hotA, .-hotA

.section .note.GNU-stack,"",@progbits
