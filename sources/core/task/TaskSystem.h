#pragma once

//#define TASKSYSTEM_USE_TBB 1
//#define TASKSYSTEM_USE_LOCKFREE 1

#if defined(TASKSYSTEM_USE_TBB)
#include "core/task/TaskSystemTBB.h"
#elif defined(TASKSYSTEM_USE_LOCKFREE)
#include "core/task/TaskSystemLockFree.h"
#else
#include "core/task/TaskSystemEnki.h"
#endif
