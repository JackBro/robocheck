
#ifndef RBC_TYPES_H_
#define RBC_TYPES_H_

#include "rbc_constants.h"
#include "rbc_errset.h"

enum EN_data_type
{
	RBC_NONE,
	
	RBC_INT,
	RBC_SHORT,
	RBC_LONG,
	RBC_CHAR,
	RBC_PCHAR,
	
	RBC_TYPE_MAX
};

enum EN_rbc_access
{ 
	RBC_R, 
	RBC_W, 
	RBC_RW
};

typedef struct 
{
	enum EN_data_type type;
	void *value;
	const char *xml_name;
} __rbc_data_type_t;

typedef struct
{
	int count;
	__rbc_data_type_t data_type[RBC_MAX_ERR_INPUT];
} __rbc_entry_t;

typedef struct
{
	unsigned int bit_set[RBC_ERRSET_COUNT];
} rbc_errset_t;

typedef struct
{
	enum EN_rbc_access access;
	FILE *task_output;
} rbc_task_t;

struct rbc_out_info
{
	enum EN_err_type err_type;
	void *aux;
};

__rbc_entry_t rbc_sys_entries[PENALTY_COUNT];

#endif

