// SPDX-License-Identifier: GPL-2.0-only
// Pure-integer binary32 arithmetic for the silent-aim HWBP handler.

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "kfloat.h"
#include "sensor.h"   /* fadd() lives here — reuse it */

/* fadd is already IEEE-754-correct in sensor.c; alias it. */
u32 kf_add(u32 a, u32 b) { return fadd(a, b); }

u32 kf_neg(u32 a) { return a ^ 0x80000000u; }
u32 kf_abs(u32 a) { return a & 0x7FFFFFFFu; }

u32 kf_sub(u32 a, u32 b) { return kf_add(a, kf_neg(b)); }

/* IEEE-754 binary32 multiply. Normal × Normal only; subnormals flush to 0.
 * That's fine for aiming: all values are in ~[1e-6, 1e8]. */
u32 kf_mul(u32 a, u32 b)
{
    u32 sign_a = a >> 31, sign_b = b >> 31;
    u32 sign   = sign_a ^ sign_b;
    u32 exp_a  = (a >> 23) & 0xFFu;
    u32 exp_b  = (b >> 23) & 0xFFu;
    u32 mant_a = a & 0x7FFFFFu;
    u32 mant_b = b & 0x7FFFFFu;
    u64 prod, sig_a, sig_b;
    int exp, shift;
    u32 mant, round_bit, sticky;

    if ((exp_a == 0xFFu && mant_a) || (exp_b == 0xFFu && mant_b))
        return 0x7FC00000u;

    if (exp_a == 0xFFu || exp_b == 0xFFu) {
        if ((exp_a == 0xFFu && (b & 0x7FFFFFFFu) == 0) ||
            (exp_b == 0xFFu && (a & 0x7FFFFFFFu) == 0))
            return 0x7FC00000u;
        return (sign << 31) | 0x7F800000u;
    }

    if ((a & 0x7FFFFFFFu) == 0 || (b & 0x7FFFFFFFu) == 0)
        return sign << 31;

    /* Subnormal inputs: flush to zero. */
    if (exp_a == 0u || exp_b == 0u)
        return sign << 31;

    sig_a = 0x800000ull | mant_a;
    sig_b = 0x800000ull | mant_b;
    prod  = sig_a * sig_b;             /* in [2^46, 2^48) */

    exp = (int)exp_a + (int)exp_b - 127;

    if (prod & (1ull << 47)) {
        shift = 24;
    } else {
        shift = 23;
        exp -= 1;
    }

    mant      = (u32)(prod >> shift) & 0x7FFFFFu;
    round_bit = (u32)(prod >> (shift - 1)) & 1u;
    sticky    = (prod & ((1ull << (shift - 1)) - 1ull)) ? 1u : 0u;

    /* Round-to-nearest-even. */
    if (round_bit && (sticky || (mant & 1u))) {
        mant++;
        if (mant > 0x7FFFFFu) {
            mant = 0;
            exp++;
        }
    }

    if (exp >= 0xFF) return (sign << 31) | 0x7F800000u;
    if (exp <= 0)    return sign << 31;

    return (sign << 31) | ((u32)exp << 23) | mant;
}

u32 kf_rsqrt(u32 x)
{
    /* Quake III initial estimate for 1/sqrt(x). */
    u32 y = 0x5f3759dfu - (x >> 1);
    u32 half_x = kf_mul(x, KF_0P5);
    int i;

    for (i = 0; i < 2; i++) {
        u32 yy   = kf_mul(y, y);
        u32 xyy  = kf_mul(half_x, yy);
        u32 inner = kf_sub(KF_1P5, xyy);
        y = kf_mul(y, inner);
    }
    return y;
}

int kf_normalize3(u32 *vx, u32 *vy, u32 *vz)
{
    u32 xx = kf_mul(*vx, *vx);
    u32 yy = kf_mul(*vy, *vy);
    u32 zz = kf_mul(*vz, *vz);
    u32 len_sq = kf_add(kf_add(xx, yy), zz);
    u32 inv_len;

    if (kf_is_zero(len_sq))
        return -ERANGE;

    inv_len = kf_rsqrt(len_sq);
    *vx = kf_mul(*vx, inv_len);
    *vy = kf_mul(*vy, inv_len);
    *vz = kf_mul(*vz, inv_len);
    return 0;
}
