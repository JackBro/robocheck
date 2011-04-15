
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#include "../lib/librobocheck.h"

struct rbc_output **
load_module(struct rbc_input *input, rbc_errset_t flags, int *err_count, char * libmodule, char *func_name)
{
        char *error;
	void *handle;
	struct rbc_output **output = NULL;
        struct rbc_output ** (* run_tool_ptr) (struct rbc_input *, rbc_errset_t flags, int *);

        handle = dlopen (libmodule, RTLD_LAZY);
        if (!handle)
	{
		log_message (dlerror(), NULL);
		goto exit_function;
        }

        run_tool_ptr = dlsym(handle, func_name);
        if ((error = dlerror()) != NULL)
	{
		log_message (error, NULL);
		goto exit_function;
        }
	
	*err_count = 0;
	if (run_tool_ptr != NULL)
	{
		output = run_tool_ptr(input, flags, err_count);
	}

        dlclose(handle);

exit_function:
	return output;
}

