// DX12 shim: HLSL prologue + platform-free kernel body. Compiled by dxc at
// build time (see CMakeLists.txt), one -E per entry point.
// Entry points: fs_matte
#include "../kernels/fs_prologue_hlsl.h"
#include "../kernels/fs_matte.kernel"
