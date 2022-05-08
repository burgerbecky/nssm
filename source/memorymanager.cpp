/***************************************

	Memory allocation functions

***************************************/

#include "memorymanager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

/***************************************

	Replace the functions below to change the memory allocation system

***************************************/

/***************************************

	Allocate memory from the heap

***************************************/

void* heap_alloc(uintptr_t uSize)
{
	return HeapAlloc(GetProcessHeap(), 0, uSize);
}

/***************************************

	Allocate memory from the heap

***************************************/

void* heap_calloc(uintptr_t uSize)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, uSize);
}

/***************************************

	Release memory to the heap

***************************************/

int heap_free(void* pInput)
{
	// NULL is acceptable
	return HeapFree(GetProcessHeap(), 0, pInput);
}
