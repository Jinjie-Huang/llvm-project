# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs --split-relocs --build-id=sha1 %t.o -o %t.exe
# RUN: llvm-readobj -S --notes %t.exe > %t.log
# RUN: llvm-readobj -h -S --notes %t.exe.reloc >> %t.log
# RUN: FileCheck %s --check-prefixes=MAIN,RELOC < %t.log

# MAIN:      NoteSection {
# MAIN:        Name: .note.gnu.build-id
# MAIN:        Build ID: [[BUILD_ID:[0-9a-f]+]]
## Check .rela.* section is not present in the main file.
# MAIN-NOT:  Name: .rela.text

# RELOC:      ElfHeader {
# RELOC:        Type: Relocatable (0x1)

# RELOC:      Section {
# RELOC:        Name: .note.gnu.build-id
# RELOC:        Type: SHT_NOTE
# RELOC:      Section {
# RELOC:        Name: .rela.text{{.*}}
# RELOC-NEXT:   Type: SHT_RELA
# RELOC-NEXT:   Flags [ (0x0)
# RELOC-NEXT:   ]
# RELOC-NEXT:   Address: 0x0
# RELOC-NEXT:   Offset:
# RELOC-NEXT:   Size:
# RELOC-NEXT:   Link: 0
# RELOC-NEXT:   Info: 0

## Check note section and build-id
# RELOC:      NoteSections [
# RELOC:        NoteSection {
# RELOC:          Name: .note.gnu.build-id
# RELOC:          Owner: GNU
# RELOC:          Type: NT_GNU_BUILD_ID{{.*}}
# RELOC:          Build ID: [[BUILD_ID]]

.section .text.fn,"ax",@progbits,unique,0
.globl fn
.type fn,@function
fn:
 nop

bar:
  movl $bar, %edx
  callq fn@PLT
  nop

.section .text.fn2,"ax",@progbits,unique,1
.globl fn2
.type fn2,@function
fn2:
 nop

foo:
  movl $foo, %edx
  callq fn2@PLT
  nop
