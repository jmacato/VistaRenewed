/*
 * libdxmt-native — the single native image.
 *
 * The D3D11/DXGI/D3D10 modules and the Metal backend are all linked into
 * this one dylib, so its exported entry points are theirs; nothing is
 * forwarded at runtime.
 *
 * Each module normally defines its own Logger instance, one log file per
 * DLL.  Those definitions are compiled out here (DXMT_NATIVE), leaving this
 * as the single definition for the whole image.
 */

#include "log/log.hpp"

namespace dxmt {

Logger Logger::s_instance("dxmt.log");

} // namespace dxmt
