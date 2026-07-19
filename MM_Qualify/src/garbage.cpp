/*  garbage.cpp — see garbage.h. Bilinear sample + channel extraction over
    the unpacked garbage plane. The UV mapping and channel math come from
    qk_core.h so the GPU kernel (qk_garbage.kernel) runs literally the same
    functions; only the fetch loop is written twice, and the parity test
    diffs the two.
*/

#include "qk_cpu_prologue.h"
#include "qk_core.h"
#include "garbage.h"

namespace qk {

float SampleGarbage(const QK_GarbageField &f, int ox, int oy)
{
	if (!f.active || !f.data || f.W <= 0 || f.H <= 0) return 1.f;

	float u, v;
	QK_GarbageUV((float)ox, (float)oy,
	             (float)f.outOriginX, (float)f.outOriginY,
	             (float)f.srcRefW, (float)f.srcRefH,
	             (float)f.mRefW, (float)f.mRefH,
	             (float)f.mOriginX, (float)f.mOriginY,
	             f.dsx, f.dsy, u, v);

	// Bilinear with clamp-to-edge (see garbage.h). Weights from the
	// unclamped fractional position so the edge row/column is flat, not
	// mirrored — matches the kernel transcription exactly.
	const float fu = QK_FLOOR(u);
	const float fv = QK_FLOOR(v);
	const float du = u - fu;
	const float dv = v - fv;

	const int x0 = QK_ClampI((int)fu,     0, f.W - 1);
	const int x1 = QK_ClampI((int)fu + 1, 0, f.W - 1);
	const int y0 = QK_ClampI((int)fv,     0, f.H - 1);
	const int y1 = QK_ClampI((int)fv + 1, 0, f.H - 1);

	const float *p00 = f.data + ((size_t)y0 * f.W + x0) * 4;
	const float *p10 = f.data + ((size_t)y0 * f.W + x1) * 4;
	const float *p01 = f.data + ((size_t)y1 * f.W + x0) * 4;
	const float *p11 = f.data + ((size_t)y1 * f.W + x1) * 4;

	const float w00 = (1.f - du) * (1.f - dv);
	const float w10 = du * (1.f - dv);
	const float w01 = (1.f - du) * dv;
	const float w11 = du * dv;

	const float a = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
	const float r = w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
	const float g = w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];
	const float b = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];

	return QK_GarbageChannel(a, r, g, b, f.channel);
}

} // namespace qk
