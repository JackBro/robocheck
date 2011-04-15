
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "../lib/rbc_constants.h"
#include "../lib/rbc_utils.h"

static FILE * FileLogger = NULL;
static char LoggerBuff[2 * MAX_BUFF_SIZE];

void
create_log_message (char * message)
{
	int status = 0;
	int max_length = 0, current_length = 0;
	struct timeval time_val; struct tm *time_struct = NULL;
	time_t *crt_time = NULL;

	// adjust message pointer and maximum length
	message = (message != NULL) ? message : NULL_STRING;
	max_length = MAX(2 * MAX_BUFF_SIZE, strlen(message) + 1);
	
	// reset the buffer
	memset (LoggerBuff, 2 * MAX_BUFF_SIZE, 0);
	
	status = gettimeofday (&time_val, NULL);
	if (status == 0)
	{
		crt_time = &(time_val.tv_sec);
	}
	
	// set time stamp
	if (crt_time != NULL)
	{
		time_struct = localtime(crt_time);
		if (time_struct != NULL)
		{
			current_length = strftime (LoggerBuff, max_length, "%d-%m-%Y %T - ", time_struct);
			if (current_length == 0)
			{
				memset (LoggerBuff, 2 * MAX_BUFF_SIZE, 0);
			}
		}
	}
	max_length -= current_length;
	
	// append the robocheck tag
	strncat (LoggerBuff, RBC_TAG, max_length);
	current_length = strlen (LoggerBuff);
	max_length -= current_length;
	
	// append the message contents
	snprintf (LoggerBuff + current_length, max_length - 1, " %s\n", message);
}

int
log_message (char *message, FILE *f_ptr)
{
	int status = 0;
	
	create_log_message (message);	
	
	if (f_ptr != NULL)
	{
		fprintf(f_ptr, "%s", LoggerBuff);
	}
	else if (FileLogger != NULL)
	{
		fprintf(FileLogger, "%s", LoggerBuff);
	}
	else
	{
		status = -1;
	}
	
	return status;
}

int
init_robocheck (void)
{
	// TODO
	FileLogger = stdout;
	
	return 0;
}

void
close_robocheck (void)
{
	int ret_value = 0;
	
	ret_value = is_internal_stream (FileLogger);
	
	if (ret_value != 0)
	{
		fclose (FileLogger);
	}
}

void *
rbc_get_mem (size_t count, size_t element_size)
{
	void *mem_ptr = NULL;
	
	mem_ptr = malloc (count * element_size);
	if (mem_ptr == NULL)
	{
		log_message (NOMEM_ERR, FileLogger);
	}
	
	return mem_ptr;
}

void
rbc_free_mem (void **mem_ptr)
{
	if (*mem_ptr != NULL)
	{
		free (*mem_ptr);
		*mem_ptr = NULL;
	}
}

int
is_internal_stream (FILE *stream)
{
	return  (stream == STDERR_HANDLER) ||
			(stream == STDIN_HANDLER)  ||
			(stream == STDOUT_HANDLER);
}

char *
make_comm_string (int argc, char **args)
{
	int i = 0;
	int ret_string_len = 0, temp_len = 0, max_len = 0;
	char *ret_string = NULL, *realloc_string = NULL;
	
	// alocate initial chunk for return string value
	ret_string = (char *) rbc_get_mem(MAX_BUFF_SIZE, sizeof *ret_string);
	
	if (ret_string != NULL && argc > 0 && args != NULL)
	{
		// if allocation is succesfull
		// fill string with '0'`s
		max_len = MAX_BUFF_SIZE;
		memset (ret_string, MAX_BUFF_SIZE, 0);
		
		for (i = 0; i < argc; i++)
		{
			// if current argument is a valid one
			if (args[i] != NULL && (temp_len = strlen(args[i])) > 0)
			{
				// if exceded allocated size
				// reallocate buffer
				if (temp_len + ret_string_len + 3 > max_len)
				{
					max_len += MAX_BUFF_SIZE;
					
					realloc_string = (char *) realloc(ret_string, 
													  max_len * (sizeof (char))
													 );
					if (realloc_string == NULL)
					{
						rbc_free_mem ((void **)&ret_string);
						log_message (NOMEM_ERR, NULL);
						break;
					}
					else
					{
						ret_string = realloc_string;
					}
				}
				
				// copy argument into string
				strncpy(ret_string + ret_string_len, args[i], temp_len);
				ret_string_len += temp_len;
				
				// add a space between current argument 
				// (and possibly next one)
				strncpy(ret_string + ret_string_len, " ", 1);
				ret_string_len++;
			}
		}
	}
	
	return ret_string;
}
