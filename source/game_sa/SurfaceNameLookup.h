#pragma once

#include "Enums/eSurfaceType.h"

// The existing source name table is implemented once in SurfaceInfos_c.cpp.
// Case-sensitive; unrecognised names select SURFACE_DEFAULT, as in the source.
// name must be non-null and NUL-terminated.
eSurfaceType GetSourceSurfaceIdFromName(const char* name);
