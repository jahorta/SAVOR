#pragma once

#include "Core/DolphinWrapper.h"
struct ScopedEmu {
	savor::DolphinWrapper w;
	~ScopedEmu() { w.shutdownAll(); } // guarantees teardown on any exit path
};
