#pragma once

//#define TASKSYSTEM_USE_TBB 1

#ifdef TASKSYSTEM_USE_TBB
#include "TaskSystemTBB.h"
#else
#include "TaskSystemEnki.h"
#endif
