# Instrument What You Need

`--instrument=func-probe` inserts a call to the function named by
`--instrument-func-probe-function` at every discovered function entry and
before every normal function exit (`ret` or tail call). It is supported for
x86-64 and AArch64 ELF binaries in both relocation and non-relocation modes.
The input does not need `.rel.text` or `.rela.text`.

The existing counter instrumentation remains the default mode for the
value-less `--instrument` option. It can also be selected explicitly with
`--instrument=counter`.

The hook may be defined in the input binary or supplied by an `LD_PRELOAD`
library. It must not throw. It can use the platform's ordinary C calling
convention:

```c
__attribute__((noinline))
void common_func(void) {
  // Record a timestamp or update thread-local profiling state.
}
```

BOLT preserves caller-visible state around the call. On x86-64 this includes
the caller-saved general-purpose registers, RFLAGS, x87/SSE state, stack
alignment, and the red zone. On AArch64 this includes X0-X18, LR, Q0-Q31,
NZCV, FPCR, and FPSR.

Sites call an in-binary hook directly. External hooks go through a cached local
dispatch stub; use an explicit event protocol or unwind through that stub if
the hook needs to distinguish entry and exit sites.

The instrumented function body is emitted in BOLT's new text area. All
patchable original primary and secondary entry points are replaced with
architecture-specific long jumps to the rewritten entry symbols. The normal
BOLT `PatchEntries`, `LongJmp`, and JITLink relocation paths perform this
redirection. In non-relocation mode, BOLT emits each modified function into a
newly allocated code section while leaving the original bytes in place except
for the entry patches.

Relocations for the new hook calls and entry jumps exist only in BOLT's
temporary output object. JITLink resolves them while producing the final
binary, so neither the input nor the output binary needs to retain relocation
records.

If the hook is not present in the input, BOLT links a local resolver. On its
first invocation the resolver walks the dynamic loader's `link_map`, resolves
the configured symbol from the GNU or SysV hash table of loaded objects, and
caches the address. The main executable does not need a dynamic symbol, PLT
entry, or link-time dependency for the hook:

```sh
llvm-bolt app --relocs=0 \
  --instrument=func-probe \
  --instrument-func-probe-function=common_func -o app.bolt
LD_PRELOAD=./libhooks.so ./app.bolt
```

The preload library exports the same C symbol:

```c
extern "C"
void common_func(void) {
  // Record this event.
}
```

External hooks require a dynamically linked ELF executable because static
executables do not support `LD_PRELOAD`. If no loaded object defines the hook,
the resolver returns without invoking anything. The ELF process entry point is
not instrumented when using an external hook; loader startup must finish before
the resolver inspects `DT_DEBUG`.

Functions without a CFG are skipped and included in the summary diagnostic.
The hook and its split fragments are excluded to prevent recursion. Split
fragments receive exit hooks but not extra entry hooks because entering a
fragment does not start a new function invocation.

Exception unwinding and abnormal termination instructions are not normal
function exits and do not receive exit hooks. The x86-64 wrapper preserves
x87/SSE state but not the upper halves of AVX or AVX-512 registers. The
AArch64 wrapper preserves the base AAPCS64 scalar and 128-bit SIMD state but
not SVE/SME state.
