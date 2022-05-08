/***************************************

	Thread hook manager

***************************************/

#ifndef __HOOK_H__
#define __HOOK_H__

#ifndef __CONSTANTS_H__
#include "constants.h"
#endif

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

// Version 1.
#define NSSM_HOOK_VERSION 1

// Hook ran successfully.
#define NSSM_HOOK_STATUS_SUCCESS 0
// No hook configured.
#define NSSM_HOOK_STATUS_NOTFOUND 1
// Hook requested abort.
#define NSSM_HOOK_STATUS_ABORT 99
// Internal error launching hook.
#define NSSM_HOOK_STATUS_ERROR 100
// Hook was not run.
#define NSSM_HOOK_STATUS_NOTRUN 101
// Hook timed out.
#define NSSM_HOOK_STATUS_TIMEOUT 102
// Hook returned non-zero.
#define NSSM_HOOK_STATUS_FAILED 111

struct nssm_service_t;

// Data for a hook's thread
struct hook_thread_data_t {
	// Name of the thread
	wchar_t m_Name[HOOK_NAME_LENGTH];
	// Handle for the thread
	HANDLE m_hThreadHandle;
};

// List of threads
struct hook_thread_t {
	// Array of thread records
	hook_thread_data_t* m_pThreadData;
	// Size of the array in elements
	uint32_t m_uNumThreads;
};

extern bool valid_hook_name(
	const wchar_t* pEventName, const wchar_t* pActionName, bool bQuiet);
extern void await_hook_threads(hook_thread_t* pHookThreads,
	SERVICE_STATUS_HANDLE hStatusHandle, SERVICE_STATUS* pServiceStatus,
	uint32_t uTimeoutDeadline);
extern int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl, uint32_t uTimeoutDeadline, bool bAsyc);
extern int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl, uint32_t uTimeoutDeadline);
extern int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl);

#endif
