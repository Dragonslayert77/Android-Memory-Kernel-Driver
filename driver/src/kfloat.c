// SPDX-License-Identifier: GPL-2.0-only
// Pure-integer IEEE-754 binary32. No float/double types, no FPU use.
// Fully self-contained — no dependency on sensor.c or any libm.

#include "kfloat.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#else
#include <errno.h>
#include <stdint.h>
#endif

/* ================= Constants ================= */
/* already in kfloat.h but kept here for clarity of the algorithm */
#ifndef KF_0P0
#define KF_0P0  0x00000000u
#endif
#ifndef KF_0P5
#define KF_0P5  0x3F000000u
#endif
#ifndef KF_1P0
#define KF_1P0  0x3F800000u
#endif
#ifndef KF_1P5
#define KF_1P5  0x3FC00000u
#endif

/* ================= Basic ops ================= */
u32 kf_neg(u32 a) { return a ^ 0x80000000u; }
u32 kf_abs(u32 a) { return a & 0x7FFFFFFFu; }

/* ================= Addition =================
 * IEEE-754 binary32 add, round-to-nearest-even.
 * Handles: normal × normal (exact), zeros, +/-Inf, NaN.
 * Subnormal inputs are flushed to zero (irrelevant for aim math).
 * Result subnormals also flush to zero.
 */
u32 kf_add(u32 a, u32 b)
{
    u32 sa, sb;
    int ea, eb;
    u32 ma, mb;
    u64 siga, sigb;
    u64 sum;
    int exp;
    u32 sign;
    int diff, nd;
    int msb, shift;
    u32 mant, g, r, s;

    /* (+-0) + x = x ; (+-0) + (+-0) = +0 */
    if ((a & 0x7FFFFFFFu) == 0) {
        if ((b & 0x7FFFFFFFu) == 0) return 0;
        return b;
    }
    if ((b & 0x7FFFFFFFu) == 0) return a;

    sa = a >> 31;
    sb = b >> 31;
    ea = (int)((a >> 23) & 0xFFu);
    eb = (int)((b >> 23) & 0xFFu);
    ma = a & 0x7FFFFFu;
    mb = b & 0x7FFFFFu;

    /* NaN */
    if (ea == 0xFF && ma) return 0x7FC00000u | (sa << 31);
    if (eb == 0xFF && mb) return 0x7FC00000u | (sb << 31);

    /* Inf */
    if (ea == 0xFF && eb == 0xFF && sa != sb) return 0x7FC00000u; /* Inf - Inf */
        if (ea == 0xFF) return a;
        if (eb == 0xFF) return b;

        /* Subnormal flush */
        if (ea == 0) return b;
        if (eb == 0) return a;

        siga = 0x800000ull | ma;
    sigb = 0x800000ull | mb;

    /* Align to larger exponent, tracking sticky */
    diff = ea - eb;
    if (diff > 0) {
        if (diff > 27) {
            sigb = 1;
        } else {
            u64 sticky = sigb & ((1ull << diff) - 1u);
            sigb >>= diff;
            if (sticky) sigb |= 1;
        }
        exp = ea;
    } else if (diff < 0) {
        nd = -diff;
        if (nd > 27) {
            siga = 1;
        } else {
            u64 sticky = siga & ((1ull << nd) - 1u);
            siga >>= nd;
            if (sticky) siga |= 1;
        }
        exp = eb;
    } else {
        exp = ea;
    }

    siga <<= 3;
    sigb <<= 3;

    if (sa == sb) {
        sum  = siga + sigb;
        sign = sa;
    } else if (siga > sigb) {
        sum  = siga - sigb;
        sign = sa;
    } else if (sigb > siga) {
        sum  = sigb - siga;
        sign = sb;
    } else {
        return 0;  /* exact cancellation */
    }

    if (sum == 0) return 0;

    /* Normalize: MSB → bit 26 (27-bit incl. 3 guard bits) */
    msb = 63;
    while (msb >= 0 && !(sum & (1ull << msb))) msb--;
    shift = msb - 26;

    if (shift > 0) {
        u64 low = sum & ((1ull << shift) - 1u);
        sum >>= shift;
        if (low) sum |= 1;
        exp += shift;
    } else if (shift < 0) {
        sum <<= -shift;
        exp += shift;
    }

    mant = (u32)(sum >> 3) & 0x7FFFFFu;
    g = (u32)(sum >> 2) & 1u;
    r = (u32)(sum >> 1) & 1u;
    s = (u32)(sum)      & 1u;

    /* Round to nearest, ties to even */
    if (g && (r || s || (mant & 1u))) {
        mant++;
        if (mant & 0x800000u) {
            mant = 0;
            exp++;
        }
    }

    if (exp >= 0xFF) return (sign << 31) | 0x7F800000u;
    if (exp < 1)     return sign << 31;
    return (sign << 31) | ((u32)exp << 23) | mant;
}

/* ================= Subtraction ================= */
u32 kf_sub(u32 a, u32 b) { return kf_add(a, kf_neg(b)); }

/* ================= Multiplication =================
 * IEEE-754 binary32 multiply, RNE. Normal operands only;
 * subnormals flushed to zero.
 *
 * The correction vs. the old code:
 *   if (prod & (1<<47)) { shift=24; exp += 1; }
 *   else                { shift=23; }  (exp unchanged)
 */
 u32 kf_mul(u32 a, u32 b)
 {
     u32 sa, sb, sign;
     int ea, eb;
     u32 ma, mb;
     u64 pa, pb, prod;
     int exp, shift;
     u32 mant, g, r, s;

     sa = a >> 31;
     sb = b >> 31;
     sign = sa ^ sb;
     ea = (int)((a >> 23) & 0xFFu);
     eb = (int)((b >> 23) & 0xFFu);
     ma = a & 0x7FFFFFu;
     mb = b & 0x7FFFFFu;

     /* NaN */
     if (ea == 0xFF && ma) return 0x7FC00000u;
     if (eb == 0xFF && mb) return 0x7FC00000u;

     /* Inf * 0 = NaN ; Inf * x = Inf */
     if (ea == 0xFF) {
         if ((b & 0x7FFFFFFFu) == 0) return 0x7FC00000u;
         return (sign << 31) | 0x7F800000u;
     }
     if (eb == 0xFF) {
         if ((a & 0x7FFFFFFFu) == 0) return 0x7FC00000u;
         return (sign << 31) | 0x7F800000u;
     }

     /* Zero or subnormal → result subnormal (flush to zero) */
     if (ea == 0 || eb == 0) return sign << 31;

     pa = 0x800000ull | ma;
     pb = 0x800000ull | mb;
     prod = pa * pb;

     exp = ea + eb - 127;

     if (prod & (1ull << 47)) {
         shift = 24;
         exp += 1;              /* carry: mantissa's implicit bit overflowed */
     } else {
         shift = 23;
     }

     mant = (u32)(prod >> shift)     & 0x7FFFFFu;
     g    = (u32)(prod >> (shift-1)) & 1u;
     r    = (u32)(prod >> (shift-2)) & 1u;
     s    = (prod & ((1ull << (shift - 2)) - 1u)) ? 1u : 0u;

     if (g && (r || s || (mant & 1u))) {
         mant++;
         if (mant & 0x800000u) {
             mant = 0;
             exp++;
         }
     }

     if (exp >= 0xFF) return (sign << 31) | 0x7F800000u;
     if (exp < 1)     return sign << 31;
     return (sign << 31) | ((u32)exp << 23) | mant;
 }

 /* ================= 1 / sqrt(x) =================
  * Fast inverse square root + 2 Newton iterations.
  * Relative error after 2 iterations: ~1e-6 — ample for aim math.
  */
 u32 kf_rsqrt(u32 x)
 {
     u32 abs_x = x & 0x7FFFFFFFu;
     u32 y, half_x;
     int i;

     if (abs_x == 0)             return 0x7F800000u;   /* 1/sqrt(+-0) = +Inf */
         if (abs_x > 0x7F800000u)    return 0x7FC00000u;   /* NaN */
             if (x & 0x80000000u)        return 0x7FC00000u;   /* 1/sqrt(neg) = NaN */
                 if (abs_x == 0x7F800000u)   return 0;             /* 1/sqrt(Inf) = +0 */

                     y = 0x5F3759DFu - (x >> 1);
     half_x = kf_mul(x, KF_0P5);

     for (i = 0; i < 2; i++) {
         u32 yy    = kf_mul(y, y);
         u32 xyy   = kf_mul(half_x, yy);
         u32 inner = kf_sub(KF_1P5, xyy);
         y = kf_mul(y, inner);
     }

     return y;
 }

 /* ================= Normalize 3-vector ================= */
 int kf_normalize3(u32 *vx, u32 *vy, u32 *vz)
 {
     u32 xx, yy, zz, len_sq, abs_ls, inv_len;

     xx = kf_mul(*vx, *vx);
     yy = kf_mul(*vy, *vy);
     zz = kf_mul(*vz, *vz);
     len_sq = kf_add(kf_add(xx, yy), zz);

     abs_ls = len_sq & 0x7FFFFFFFu;
     if (abs_ls == 0 || abs_ls >= 0x7F800000u) {
         return -ERANGE;   /* zero length or non-finite */
     }

     inv_len = kf_rsqrt(len_sq);

     *vx = kf_mul(*vx, inv_len);
     *vy = kf_mul(*vy, inv_len);
     *vz = kf_mul(*vz, inv_len);
     return 0;
 }
