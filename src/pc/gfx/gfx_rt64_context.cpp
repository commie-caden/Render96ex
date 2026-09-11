#ifdef RAPI_RT64

#include "gfx_rt64_context.h"

RT64Context &gfx_rt64_context(void) {
	static RT64Context context;
	return context;
}

#endif