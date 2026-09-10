#pragma once

#ifndef ASTER_LANCZOS_USE_LUT
#define ASTER_LANCZOS_USE_LUT 0
#endif

#if ASTER_LANCZOS_USE_LUT != 0 && ASTER_LANCZOS_USE_LUT != 1
#error ASTER_LANCZOS_USE_LUT must be 0 or 1
#endif
