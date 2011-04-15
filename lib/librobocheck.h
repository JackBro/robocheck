
#ifndef LIB_RBC_H_
#define LIB_RBC_H_

#include "../export/static_tool.h"
#include "../export/dynamic_tool.h"

#include "rbc_utils.h"

struct rbc_output **
load_module (struct rbc_input *, rbc_errset_t flags, int *err_count, char * libmodule, char *func_name);

#endif
