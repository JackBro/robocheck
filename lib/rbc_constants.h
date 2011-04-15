
#ifndef RBC_CONSTANTS_H_
#define RBC_CONSTANTS_H_

#include "rbc_err_codes.h"

#define BYTE_SIZE 		8
#define RBC_ERRSET_COUNT 	(ERR_MAX / UINT_SIZE + 1)
#define UINT_SIZE 		(sizeof (unsigned int) * BYTE_SIZE)

#define RBC_MAX_ERR_INPUT	10
#define PENALTY_COUNT		ERR_MAX

#define MAX_BUFF_SIZE		512

#define STDERR_HANDLER		stderr
#define STDIN_HANDLER		stdin
#define STDOUT_HANDLER		stdout

#define NULL_STRING 		"[NULL]"
#define RBC_TAG 		"[robocheck]"

#define NOMEM_ERR 		"Inssuficient memory space."
#define INVALID_PROC_STARTED	"Invalid shell command for starting the process."
#define INVALID_PROC_STOPED	"Invalid process stoped."

#endif

