#pragma once

//#define TASKSYSTEM_USE_TBB 1

#ifdef TASKSYSTEM_USE_TBB
#include "core/task/TaskSystemTBB.h"
#else
#include "core/task/TaskSystemEnki.h"
#endif
