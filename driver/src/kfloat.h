// SPDX-License-Identifier: GPL-2.0
#ifndef DRIVER_KFLOAT_H
#define DRIVER_KFLOAT_H

#include <linux/errno.h>
#include <linux/types.h>

/* Pure-integer IEEE-754 binary32. No FP registers touched — safe in
 * exception context and under -mgeneral-regs-only. */

u32 kf_add(u32 a, u32 b);          /* delegates to sensor.c::fadd */
u32 kf_sub(u32 a, u32 b);          /* a + (-b) */
u32 kf_mul(u32 a, u32 b);          /* IEEE-754 binary32 multiply */
u32 kf_neg(u32 a);                 /* flip sign bit */
u32 kf_abs(u32 a);                 /* clear sign bit */

/* Fast 1/sqrt(x); ~1e-6 relative error after 2 Newton iterations. */
u32 kf_rsqrt(u32 x);

/* Normalize (vx, vy, vz) in place. Returns 0 on success, -ERANGE if
 * magnitude is 0 or non-finite. */
int kf_normalize3(u32 *vx, u32 *vy, u32 *vz);

/* IEEE-754 bit patterns for common constants. */
#define KF_0P0  0x00000000u
#define KF_0P5  0x3F000000u
#define KF_1P0  0x3F800000u
#define KF_1P5  0x3FC00000u

static inline int kf_is_zero(u32 a) { return (a & 0x7FFFFFFFu) == 0; }

#endif /* DRIVER_KFLOAT_H */
