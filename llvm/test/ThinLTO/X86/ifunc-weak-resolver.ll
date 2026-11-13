; REQUIRES: x86-registered-target
; RUN: split-file %s %t
; RUN: opt -module-summary %t/a.ll -o %t/a.bc
; RUN: opt -module-summary %t/b.ll -o %t/b.bc
; RUN: llvm-lto2 run %t/a.bc %t/b.bc -o %t/out -save-temps \
; RUN:   -r %t/a.bc,dispatch,plx -r %t/a.bc,resolver,pl -r %t/a.bc,call_a,plx \
; RUN:   -r %t/a.bc,condition,l -r %t/a.bc,implementation,l -r %t/a.bc,alternative,l \
; RUN:   -r %t/b.bc,dispatch,l -r %t/b.bc,resolver,l -r %t/b.bc,call_b,plx \
; RUN:   -r %t/b.bc,unique_dispatch,plx \
; RUN:   -r %t/a.bc,ordinary,plx -r %t/b.bc,ordinary,l \
; RUN:   -r %t/b.bc,condition,l -r %t/b.bc,implementation,l -r %t/b.bc,alternative,l
; RUN: llvm-dis %t/out.2.3.import.bc -o - | FileCheck %s --check-prefix=IMPORT --implicit-check-not=comdat
; RUN: llvm-nm %t/out.2 | FileCheck %s --check-prefix=NM --implicit-check-not=ordinary
;
; An ifunc requires a resolver definition for the linker even when another
; module has the prevailing copy of the weak resolver. Do not replace ifuncs
; with declarations based on their resolver: unique_dispatch is defined only
; in B and shares the non-prevailing resolver with dispatch.
;
; IMPORT-DAG: @dispatch = weak_odr{{.*}} ifunc i32 (), ptr @resolver
; IMPORT-DAG: @unique_dispatch = weak_odr{{.*}} ifunc i32 (), ptr @resolver
; IMPORT: define weak_odr{{.*}} ptr @resolver()
; IMPORT: define available_externally{{.*}} i32 @ordinary()
; NM-DAG: i dispatch
; NM-DAG: W resolver
; NM-DAG: i unique_dispatch

;--- a.ll
target triple = "x86_64-unknown-linux-gnu"
$resolver = comdat any
@condition = external global i32
@dispatch = weak_odr ifunc i32 (), ptr @resolver
declare i32 @implementation()
declare i32 @alternative()
define weak_odr ptr @resolver() comdat {
  %value = load i32, ptr @condition
  %zero = icmp eq i32 %value, 0
  %result = select i1 %zero, ptr @implementation, ptr @alternative
  ret ptr %result
}
define i32 @call_a() {
  %result = call i32 @dispatch()
  ret i32 %result
}
define weak_odr i32 @ordinary() {
  ret i32 42
}

;--- b.ll
target triple = "x86_64-unknown-linux-gnu"
$resolver = comdat any
@condition = external global i32
@dispatch = weak_odr ifunc i32 (), ptr @resolver
@unique_dispatch = weak_odr ifunc i32 (), ptr @resolver
declare i32 @implementation()
declare i32 @alternative()
define weak_odr ptr @resolver() comdat {
  %value = load i32, ptr @condition
  %zero = icmp eq i32 %value, 0
  %result = select i1 %zero, ptr @implementation, ptr @alternative
  ret ptr %result
}
define i32 @call_b() {
  %first = call i32 @dispatch()
  %second = call i32 @unique_dispatch()
  %result = add i32 %first, %second
  ret i32 %result
}
define weak_odr i32 @ordinary() {
  ret i32 42
}
