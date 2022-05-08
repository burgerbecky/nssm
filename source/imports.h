/***************************************

	Imports class

***************************************/

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#ifndef RTL_SRWLOCK_INIT
typedef struct _RTL_CONDITION_VARIABLE {
	void* Ptr;
} RTL_CONDITION_VARIABLE;
typedef RTL_CONDITION_VARIABLE CONDITION_VARIABLE;
#endif

// Typedefs for all the calls

typedef BOOL(WINAPI* AttachConsole_ptr)(DWORD);
typedef BOOL(WINAPI* SleepConditionVariableCS_ptr)(
	_RTL_CONDITION_VARIABLE*, PCRITICAL_SECTION, DWORD);
typedef BOOL(WINAPI* QueryFullProcessImageNameW_ptr)(
	HANDLE, unsigned long, LPWSTR, unsigned long*);
typedef void(WINAPI* WakeConditionVariable_ptr)(_RTL_CONDITION_VARIABLE*);
typedef BOOL(WINAPI* CreateWellKnownSid_ptr)(
	WELL_KNOWN_SID_TYPE, SID*, SID*, unsigned long*);
typedef BOOL(WINAPI* IsWellKnownSid_ptr)(SID*, WELL_KNOWN_SID_TYPE);

struct imports_t {
	// Module for kernel32.dll
	HMODULE m_hKernel32;
	// Module for advapi32.dll
	HMODULE advapi32;

	// Functions from kernel32.dll
	AttachConsole_ptr AttachConsole;
	SleepConditionVariableCS_ptr SleepConditionVariableCS;
	QueryFullProcessImageNameW_ptr QueryFullProcessImageNameW;
	WakeConditionVariable_ptr WakeConditionVariable;

	// Functions from advapi32.dll
	CreateWellKnownSid_ptr CreateWellKnownSid;
	IsWellKnownSid_ptr IsWellKnownSid;
};

extern imports_t g_Imports;

extern int get_imports(void);
extern void free_imports(void);

#endif
