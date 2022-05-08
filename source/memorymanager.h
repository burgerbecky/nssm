/***************************************

	Memory allocation functions

***************************************/

#ifndef __MEMORYMANAGER_H__
#define __MEMORYMANAGER_H__

#ifndef _STDINT
#include <stdint.h>
#endif

extern void* heap_alloc(uintptr_t uSize);
extern void* heap_calloc(uintptr_t uSize);
extern int heap_free(void* pInput);

#endif
