// Check function entry and exit instrumentation of an x86-64 shared library.

// REQUIRES: system-linux,bolt-runtime

// RUN: split-file %s %t
// RUN: %clang %cflags -O2 -fno-inline -fPIC -c %t/lib.c -o %t/lib.o
// RUN: %clang -fPIC -shared %t/lib.o -Wl,-soname,libtarget.so -o %t/lib.so
// RUN: %clang %cflags -fPIC -c %t/hook.c -o %t/hook.o
// RUN: %clang -fPIC -shared %t/hook.o -o %t/hook.so
// RUN: llvm-bolt %t/lib.so -o %t/libtarget.so --relocs=0 \
// RUN:   --instrument=func-entry,func-exit --instrument-func-list='^target'
// RUN: llvm-objdump -t %t/libtarget.so | FileCheck %s --check-prefix=LIB-SYMS
// RUN: %clang %cflags -c %t/main.c -o %t/main.o
// RUN: %clang %t/main.o -L%t -ltarget -o %t/main
// RUN: llvm-bolt %t/main -o %t/main.bolt --relocs=0 \
// RUN:   --instrument=func-entry,func-exit \
// RUN:   --instrument-func-list='^(main|callback)$'
// RUN: llvm-objdump -t %t/main.bolt | FileCheck %s --check-prefix=MAIN-SYMS
// RUN: env LD_LIBRARY_PATH=%t LD_PRELOAD=%t/hook.so %t/main.bolt | FileCheck %s

// The instrumented body keeps the original name in .bolt.text. The .org.0
// symbol names the entry patch left at the original address in .text.
// LIB-SYMS-DAG: l F .text {{.*}} target.org.0
// LIB-SYMS-DAG: g F .bolt.text {{.*}} target
// LIB-SYMS-DAG: l F .text {{.*}} target_address.org.0
// LIB-SYMS-DAG: g F .bolt.text {{.*}} target_address
// MAIN-SYMS-DAG: l F .text {{.*}} main.org.0
// MAIN-SYMS-DAG: g F .bolt.text {{.*}} main
// MAIN-SYMS-DAG: l F .text {{.*}} callback.org.0
// MAIN-SYMS-DAG: g F .bolt.text {{.*}} callback
// MAIN-SYMS-DAG: l{{.*}}.bolt.text{{.*}}callback_label

// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}

//--- lib.c
typedef int (*function_t)(int);

__attribute__((noinline, visibility("protected"))) int target(int value) {
  __asm__ volatile(".rept 16\nnop\n.endr");
  return value + 1;
}

__attribute__((noinline, visibility("default"))) function_t
target_address(void) {
  function_t result;
  __asm__("leaq target(%%rip), %0" : "=r"(result));
  return result;
}

//--- main.c
typedef int (*function_t)(int);

extern int target(int value);
extern function_t target_address(void);

__attribute__((noinline)) int callback(int value) {
  if (value == 0)
    __asm__ volatile(".local callback_label\n"
                     ".type callback_label,@notype\n"
                     "callback_label:\n"
                     ".rept 16\nnop\n.endr");
  __asm__ volatile(".rept 16\nnop\n.endr");
  return value + 1;
}

function_t global_callback = callback;

int main(void) {
  if (global_callback != callback || target_address() != target ||
      global_callback(41) != 42)
    return 1;
  return target(41) == 42 ? 0 : 2;
}

//--- hook.c
#include <stdint.h>
#include <stdio.h>

void __bolt_probe_enter(uintptr_t function_pc) {
  printf("enter pc=0x%lx\n", (unsigned long)function_pc);
}

void __bolt_probe_exit(uintptr_t function_pc) {
  printf("exit pc=0x%lx\n", (unsigned long)function_pc);
}
