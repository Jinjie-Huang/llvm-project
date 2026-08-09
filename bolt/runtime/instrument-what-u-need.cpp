//===- bolt/runtime/instrument-what-u-need.cpp ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if (defined(__x86_64__) || defined(__aarch64__) || defined(__arm64__)) &&     \
    !defined(__APPLE__)

using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using int64_t = long long;

#if defined(__x86_64__)
#define IWYN_SAVE_ALL                                                          \
  "push %%rax\n"                                                               \
  "push %%rbx\n"                                                               \
  "push %%rcx\n"                                                               \
  "push %%rdx\n"                                                               \
  "push %%rdi\n"                                                               \
  "push %%rsi\n"                                                               \
  "push %%rbp\n"                                                               \
  "push %%r8\n"                                                                \
  "push %%r9\n"                                                                \
  "push %%r10\n"                                                               \
  "push %%r11\n"                                                               \
  "push %%r12\n"                                                               \
  "push %%r13\n"                                                               \
  "push %%r14\n"                                                               \
  "push %%r15\n"                                                               \
  "sub $8, %%rsp\n"
#define IWYN_RESTORE_ALL                                                       \
  "add $8, %%rsp\n"                                                            \
  "pop %%r15\n"                                                                \
  "pop %%r14\n"                                                                \
  "pop %%r13\n"                                                                \
  "pop %%r12\n"                                                                \
  "pop %%r11\n"                                                                \
  "pop %%r10\n"                                                                \
  "pop %%r9\n"                                                                 \
  "pop %%r8\n"                                                                 \
  "pop %%rbp\n"                                                                \
  "pop %%rsi\n"                                                                \
  "pop %%rdi\n"                                                                \
  "pop %%rdx\n"                                                                \
  "pop %%rcx\n"                                                                \
  "pop %%rbx\n"                                                                \
  "pop %%rax\n"
#elif defined(__aarch64__) || defined(__arm64__)
#define IWYN_SAVE_ALL                                                          \
  "stp x0, x1, [sp, #-16]!\n"                                                  \
  "stp x2, x3, [sp, #-16]!\n"                                                  \
  "stp x4, x5, [sp, #-16]!\n"                                                  \
  "stp x6, x7, [sp, #-16]!\n"                                                  \
  "stp x8, x9, [sp, #-16]!\n"                                                  \
  "stp x10, x11, [sp, #-16]!\n"                                                \
  "stp x12, x13, [sp, #-16]!\n"                                                \
  "stp x14, x15, [sp, #-16]!\n"                                                \
  "stp x16, x17, [sp, #-16]!\n"                                                \
  "stp x18, x19, [sp, #-16]!\n"                                                \
  "stp x20, x21, [sp, #-16]!\n"                                                \
  "stp x22, x23, [sp, #-16]!\n"                                                \
  "stp x24, x25, [sp, #-16]!\n"                                                \
  "stp x26, x27, [sp, #-16]!\n"                                                \
  "stp x28, x29, [sp, #-16]!\n"                                                \
  "mrs x29, nzcv\n"                                                            \
  "stp x29, x30, [sp, #-16]!\n"
#define IWYN_RESTORE_ALL                                                       \
  "ldp x29, x30, [sp], #16\n"                                                  \
  "msr nzcv, x29\n"                                                            \
  "ldp x28, x29, [sp], #16\n"                                                  \
  "ldp x26, x27, [sp], #16\n"                                                  \
  "ldp x24, x25, [sp], #16\n"                                                  \
  "ldp x22, x23, [sp], #16\n"                                                  \
  "ldp x20, x21, [sp], #16\n"                                                  \
  "ldp x18, x19, [sp], #16\n"                                                  \
  "ldp x16, x17, [sp], #16\n"                                                  \
  "ldp x14, x15, [sp], #16\n"                                                  \
  "ldp x12, x13, [sp], #16\n"                                                  \
  "ldp x10, x11, [sp], #16\n"                                                  \
  "ldp x8, x9, [sp], #16\n"                                                    \
  "ldp x6, x7, [sp], #16\n"                                                    \
  "ldp x4, x5, [sp], #16\n"                                                    \
  "ldp x2, x3, [sp], #16\n"                                                    \
  "ldp x0, x1, [sp], #16\n"
#endif

#pragma GCC visibility push(hidden)

extern "C" {
extern int64_t __bolt_iwyn_dispatch_offset;
extern char __bolt_iwyn_hook_name[];
int64_t __bolt_iwyn_dynamic_offset;
void *__bolt_iwyn_resolved_hook;
}

namespace {

struct LinkMap {
  uint64_t Address;
  const char *Name;
  uint64_t *Dynamic;
  LinkMap *Next;
  LinkMap *Prev;
};

struct RDebug {
  int Version;
  LinkMap *Map;
};

struct ElfSymbol {
  uint32_t Name;
  uint8_t Info;
  uint8_t Other;
  uint16_t SectionIndex;
  uint64_t Value;
  uint64_t Size;
};

enum : uint64_t {
  DynNull = 0,
  DynHash = 4,
  DynStrTab = 5,
  DynSymTab = 6,
  DynSymEnt = 11,
  DynDebug = 21,
  DynGNUHash = 0x6ffffef5,
};

enum : uint8_t {
  SymbolBindingLocal = 0,
  SymbolVisibilityMask = 0x3,
  SymbolVisibilityHidden = 2,
  SymbolVisibilityInternal = 1,
};

static uint32_t gnuHash(const char *Name) {
  uint32_t Hash = 5381;
  for (; *Name; ++Name)
    Hash = Hash * 33 + static_cast<uint8_t>(*Name);
  return Hash;
}

static uint32_t elfHash(const char *Name) {
  uint32_t Hash = 0;
  for (; *Name; ++Name) {
    Hash = (Hash << 4) + static_cast<uint8_t>(*Name);
    const uint32_t High = Hash & 0xf0000000;
    if (High)
      Hash ^= High >> 24;
    Hash &= ~High;
  }
  return Hash;
}

static bool equalString(const char *Left, const char *Right) {
  while (*Left && *Left == *Right) {
    ++Left;
    ++Right;
  }
  return *Left == *Right;
}

static uint64_t dynamicPointer(const LinkMap *Map, uint64_t Value) {
  if (Map->Address && Value < Map->Address)
    return Map->Address + Value;
  return Value;
}

static const ElfSymbol *lookupGNUHash(const uint32_t *Table,
                                      const ElfSymbol *Symbols,
                                      const char *Strings, const char *Name) {
  const uint32_t NumBuckets = Table[0];
  const uint32_t SymbolOffset = Table[1];
  const uint32_t BloomSize = Table[2];
  const uint32_t BloomShift = Table[3];
  if (!NumBuckets || !BloomSize)
    return nullptr;

  const uint64_t *Bloom = reinterpret_cast<const uint64_t *>(Table + 4);
  const uint32_t *Buckets =
      reinterpret_cast<const uint32_t *>(Bloom + BloomSize);
  const uint32_t *Chains = Buckets + NumBuckets;
  const uint32_t Hash = gnuHash(Name);
  const uint64_t Word = Bloom[(Hash / 64) % BloomSize];
  const uint64_t Mask = (uint64_t(1) << (Hash % 64)) |
                        (uint64_t(1) << ((Hash >> BloomShift) % 64));
  if ((Word & Mask) != Mask)
    return nullptr;

  uint32_t Index = Buckets[Hash % NumBuckets];
  if (Index < SymbolOffset)
    return nullptr;
  for (;;) {
    const uint32_t Chain = Chains[Index - SymbolOffset];
    if ((Chain | 1) == (Hash | 1) &&
        equalString(Strings + Symbols[Index].Name, Name))
      return &Symbols[Index];
    if (Chain & 1)
      return nullptr;
    ++Index;
  }
}

static const ElfSymbol *lookupELFHash(const uint32_t *Table,
                                      const ElfSymbol *Symbols,
                                      const char *Strings, const char *Name) {
  const uint32_t NumBuckets = Table[0];
  if (!NumBuckets)
    return nullptr;
  const uint32_t *Buckets = Table + 2;
  const uint32_t *Chains = Buckets + NumBuckets;
  for (uint32_t Index = Buckets[elfHash(Name) % NumBuckets]; Index;
       Index = Chains[Index])
    if (equalString(Strings + Symbols[Index].Name, Name))
      return &Symbols[Index];
  return nullptr;
}

static void *lookupInObject(LinkMap *Map, const char *Name) {
  const char *Strings = nullptr;
  const ElfSymbol *Symbols = nullptr;
  const uint32_t *GNUHash = nullptr;
  const uint32_t *ELFHash = nullptr;
  uint64_t SymbolSize = sizeof(ElfSymbol);

  for (uint64_t *Dyn = Map->Dynamic; Dyn && Dyn[0] != DynNull; Dyn += 2) {
    switch (Dyn[0]) {
    case DynStrTab:
      Strings = reinterpret_cast<const char *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynSymTab:
      Symbols =
          reinterpret_cast<const ElfSymbol *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynSymEnt:
      SymbolSize = Dyn[1];
      break;
    case DynGNUHash:
      GNUHash = reinterpret_cast<const uint32_t *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynHash:
      ELFHash = reinterpret_cast<const uint32_t *>(dynamicPointer(Map, Dyn[1]));
      break;
    }
  }

  if (!Strings || !Symbols || SymbolSize != sizeof(ElfSymbol))
    return nullptr;
  const ElfSymbol *Symbol =
      GNUHash ? lookupGNUHash(GNUHash, Symbols, Strings, Name) : nullptr;
  if (!Symbol && ELFHash)
    Symbol = lookupELFHash(ELFHash, Symbols, Strings, Name);
  const uint8_t Binding = Symbol ? Symbol->Info >> 4 : SymbolBindingLocal;
  const uint8_t Visibility =
      Symbol ? Symbol->Other & SymbolVisibilityMask : SymbolVisibilityHidden;
  if (!Symbol || !Symbol->SectionIndex || Binding == SymbolBindingLocal ||
      Visibility == SymbolVisibilityHidden ||
      Visibility == SymbolVisibilityInternal)
    return nullptr;
  return reinterpret_cast<void *>(Map->Address + Symbol->Value);
}

static void *resolveHook() {
  uint64_t *Dynamic = reinterpret_cast<uint64_t *>(
      reinterpret_cast<uint64_t>(&__bolt_iwyn_dynamic_offset) +
      __bolt_iwyn_dynamic_offset);
  RDebug *Debug = nullptr;
  for (uint64_t *Dyn = Dynamic; Dyn[0] != DynNull; Dyn += 2)
    if (Dyn[0] == DynDebug) {
      Debug = reinterpret_cast<RDebug *>(Dyn[1]);
      break;
    }
  if (!Debug)
    return nullptr;

  for (LinkMap *Map = Debug->Map; Map; Map = Map->Next)
    if (void *Address = lookupInObject(Map, __bolt_iwyn_hook_name))
      return Address;
  return nullptr;
}

extern "C" void *__bolt_iwyn_resolve() {
  void *Address = __atomic_load_n(&__bolt_iwyn_resolved_hook, __ATOMIC_ACQUIRE);
  if (!Address) {
    Address = resolveHook();
    if (!Address)
      return nullptr;
    __atomic_store_n(&__bolt_iwyn_resolved_hook, Address, __ATOMIC_RELEASE);
  }
  return Address;
}

} // namespace

extern "C" __attribute((naked)) void __bolt_iwyn_dispatch() {
  // clang-format off
#if defined(__x86_64__)
  __asm__ __volatile__("mov __bolt_iwyn_resolved_hook(%%rip), %%r11\n"
                       "test %%r11, %%r11\n"
                       "jnz 2f\n"
                       IWYN_SAVE_ALL
                       "call __bolt_iwyn_resolve\n"
                       "test %%rax, %%rax\n"
                       "jz 1f\n"
                       "mov %%rax, 40(%%rsp)\n"
                       IWYN_RESTORE_ALL
                       "push %%r11\n"
                       "ret\n"
                       "1:\n"
                       IWYN_RESTORE_ALL
                       "ret\n"
                       "2:\n"
                       "jmp *%%r11\n"
                       :::);
#elif defined(__aarch64__) || defined(__arm64__)
  __asm__ __volatile__("adrp x16, __bolt_iwyn_resolved_hook\n"
                       "ldr x16, [x16, #:lo12:__bolt_iwyn_resolved_hook]\n"
                       "cbnz x16, 2f\n"
                       IWYN_SAVE_ALL
                       "bl __bolt_iwyn_resolve\n"
                       "cbz x0, 1f\n"
                       "str x0, [sp, #112]\n"
                       IWYN_RESTORE_ALL
                       "br x16\n"
                       "1:\n"
                       IWYN_RESTORE_ALL
                       "ret\n"
                       "2:\n"
                       "br x16\n"
                       :::);
#endif
  // clang-format on
}

#pragma GCC visibility pop

#endif
