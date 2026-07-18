// DX12 shim: HLSL prologue + platform-free kernel body. Compiled by dxc at
// build time (see CMakeLists.txt), one -E per entry point.
// Entry points: fs_guided_box, fs_guided_squares, fs_guided_ab, fs_guided_final
#include "../kernels/fs_prologue_hlsl.h"
#include "../kernels/fs_guided.kernel"
