// RUN: %clang_cc1 -Wno-header-shadowing -triple powerpc-ibm-aix -fsyntax-only -verify -internal-isystem %S/Inputs/include %s
// expected-no-diagnostics

#include <float.h>

_Static_assert(FLOAT_LOCAL_DEF, "");
