; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=riscv64 -mattr=+m,+zve32x,+zvl1024b -O2 | FileCheck %s -check-prefix=ZVE32X
; RUN: llc < %s -mtriple=riscv64 -mattr=+m,+zve64x,+zvl1024b -O2 | FileCheck %s -check-prefix=ZVE64X

; TODO: Currently we don't lower interleaved accesses of ptr types if XLEN isn't
; a supported SEW. We should improve this with a wide load and a set of shuffles.
define <4 x i1> @load_large_vector(ptr %p) {
; ZVE32X-LABEL: load_large_vector:
; ZVE32X:       # %bb.0:
; ZVE32X-NEXT:    ld a1, 0(a0)
; ZVE32X-NEXT:    ld a2, 8(a0)
; ZVE32X-NEXT:    ld a3, 24(a0)
; ZVE32X-NEXT:    ld a4, 32(a0)
; ZVE32X-NEXT:    ld a5, 48(a0)
; ZVE32X-NEXT:    ld a6, 56(a0)
; ZVE32X-NEXT:    ld a7, 72(a0)
; ZVE32X-NEXT:    ld a0, 80(a0)
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmv.s.x v8, zero
; ZVE32X-NEXT:    vmv.v.i v9, 0
; ZVE32X-NEXT:    xor a3, a3, a4
; ZVE32X-NEXT:    xor a1, a1, a2
; ZVE32X-NEXT:    xor a2, a5, a6
; ZVE32X-NEXT:    xor a0, a7, a0
; ZVE32X-NEXT:    snez a3, a3
; ZVE32X-NEXT:    snez a1, a1
; ZVE32X-NEXT:    vmv.s.x v10, a3
; ZVE32X-NEXT:    vmv.s.x v11, a1
; ZVE32X-NEXT:    vsetivli zero, 1, e8, mf4, ta, ma
; ZVE32X-NEXT:    vand.vi v10, v10, 1
; ZVE32X-NEXT:    vmsne.vi v0, v10, 0
; ZVE32X-NEXT:    vand.vi v10, v11, 1
; ZVE32X-NEXT:    vmerge.vim v11, v8, 1, v0
; ZVE32X-NEXT:    vmsne.vi v0, v10, 0
; ZVE32X-NEXT:    snez a1, a2
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmerge.vim v10, v9, 1, v0
; ZVE32X-NEXT:    vsetivli zero, 2, e8, mf4, tu, ma
; ZVE32X-NEXT:    vslideup.vi v10, v11, 1
; ZVE32X-NEXT:    vmv.s.x v11, a1
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmsne.vi v0, v10, 0
; ZVE32X-NEXT:    vsetivli zero, 1, e8, mf4, ta, ma
; ZVE32X-NEXT:    vand.vi v10, v11, 1
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmerge.vim v11, v9, 1, v0
; ZVE32X-NEXT:    vsetivli zero, 1, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmsne.vi v0, v10, 0
; ZVE32X-NEXT:    snez a0, a0
; ZVE32X-NEXT:    vmerge.vim v10, v8, 1, v0
; ZVE32X-NEXT:    vsetivli zero, 3, e8, mf4, tu, ma
; ZVE32X-NEXT:    vslideup.vi v11, v10, 2
; ZVE32X-NEXT:    vmv.s.x v10, a0
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmsne.vi v0, v11, 0
; ZVE32X-NEXT:    vsetivli zero, 1, e8, mf4, ta, ma
; ZVE32X-NEXT:    vand.vi v10, v10, 1
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmerge.vim v9, v9, 1, v0
; ZVE32X-NEXT:    vsetivli zero, 1, e8, mf4, ta, ma
; ZVE32X-NEXT:    vmsne.vi v0, v10, 0
; ZVE32X-NEXT:    vmerge.vim v8, v8, 1, v0
; ZVE32X-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; ZVE32X-NEXT:    vslideup.vi v9, v8, 3
; ZVE32X-NEXT:    vmsne.vi v0, v9, 0
; ZVE32X-NEXT:    ret
;
; ZVE64X-LABEL: load_large_vector:
; ZVE64X:       # %bb.0:
; ZVE64X-NEXT:    vsetivli zero, 4, e64, m1, ta, ma
; ZVE64X-NEXT:    vlseg3e64.v v8, (a0)
; ZVE64X-NEXT:    vmsne.vv v0, v8, v9
; ZVE64X-NEXT:    ret
  %l = load <12 x ptr>, ptr %p
  %s1 = shufflevector <12 x ptr> %l, <12 x ptr> poison, <4 x i32> <i32 0, i32 3, i32 6, i32 9>
  %s2 = shufflevector <12 x ptr> %l, <12 x ptr> poison, <4 x i32> <i32 1, i32 4, i32 7, i32 10>
  %ret = icmp ne <4 x ptr> %s1, %s2
  ret <4 x i1> %ret
}
