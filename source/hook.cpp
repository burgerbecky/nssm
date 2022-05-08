/***************************************

	Thread hook manager

***************************************/

#include "hook.h"
#include "event.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "nssm_io.h"
#include "process.h"
#include "registry.h"
#include "service.h"

#include <wchar.h>

#include <stdint.h>
#include <strsafe.h>

struct hook_t {
	// Name of the hook (Allocated)
	wchar_t* m_pName;
	// Process handle for this hook
	HANDLE m_hProcess;
	// Process ID
	DWORD m_uPID;
	// Number of milliseconds before timeout
	DWORD m_uTimeoutDeadline;
	// Timestamp when the process was created
	FILETIME m_uCreationTime;
	// Process manager
	kill_t k;
};

/***************************************

	Thread callback, called from CreateThread()

***************************************/

static DWORD WINAPI await_hook(void* pArgument)
{
	// Sanity check
	hook_t* pHook = static_cast<hook_t*>(pArgument);
	if (!pHook) {
		return NSSM_HOOK_STATUS_ERROR;
	}

	// Wait for the process to complete
	DWORD uResult = 0;
	if (WaitForSingleObject(pHook->m_hProcess, pHook->m_uTimeoutDeadline) ==
		WAIT_TIMEOUT) {
		// It timed out
		uResult = NSSM_HOOK_STATUS_TIMEOUT;
	}

	// Tidy up hook process tree.
	if (pHook->m_pName) {
		pHook->k.m_pName = pHook->m_pName;
	} else {
		pHook->k.m_pName = L"hook";
	}
	// Copy over to the kill_t structure
	pHook->k.m_hProcess = pHook->m_hProcess;
	pHook->k.m_uPID = pHook->m_uPID;
	// Set all flags
	pHook->k.m_uStopMethodFlags = UINT32_MAX;
	pHook->k.m_uKillConsoleDelay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
	pHook->k.m_uKillWindowDelay = NSSM_KILL_WINDOW_GRACE_PERIOD;
	pHook->k.m_uKillThreadsDelay = NSSM_KILL_THREADS_GRACE_PERIOD;
	pHook->k.m_uCreationTime = pHook->m_uCreationTime;

	// Mark the exit time
	GetSystemTimeAsFileTime(&pHook->k.m_uExitTime);

	// Kill the process
	kill_process_tree(&pHook->k, pHook->m_uPID);

	// Did it timeout?
	if (!uResult) {

		// Get the exit code from the process
		DWORD uExitcode;
		GetExitCodeProcess(pHook->m_hProcess, &uExitcode);

		// Update the return value to non-zero if needed
		if (uExitcode == NSSM_HOOK_STATUS_ABORT) {
			// Aborted
			uResult = NSSM_HOOK_STATUS_ABORT;
		} else if (uExitcode) {

			// Failed
			uResult = NSSM_HOOK_STATUS_FAILED;
		}
	}

	// Dispose everything
	CloseHandle(pHook->m_hProcess);
	if (pHook->m_pName) {
		heap_free(pHook->m_pName);
	}
	heap_free(pHook);
	return uResult;
}

/***************************************

	Get the elapsed time and set the environment variable to the ascii string of
	the time. It will set it to "" if there is no time marks

***************************************/

static void set_hook_runtime(const wchar_t* pEnvVariableName,
	const FILETIME* pStartTime, const FILETIME* pCurrentTime)
{
	// Sanity check
	if (pStartTime && pCurrentTime) {

		// Capture the start time
		ULARGE_INTEGER uStart;
		uStart.LowPart = pStartTime->dwLowDateTime;
		uStart.HighPart = pStartTime->dwHighDateTime;

		// Was there a start time?
		if (uStart.QuadPart) {

			ULARGE_INTEGER uEndTime;
			uEndTime.LowPart = pCurrentTime->dwLowDateTime;
			uEndTime.HighPart = pCurrentTime->dwHighDateTime;

			// Is there an end time and time has elapsed?
			if (uEndTime.QuadPart && (uEndTime.QuadPart >= uStart.QuadPart)) {

				// Get the time difference
				uEndTime.QuadPart -= uStart.QuadPart;

				// Convert from 100ns granuality to ms granularity
				// https://docs.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime
				uEndTime.QuadPart /= 10000ULL;
				wchar_t TempNumber[32];
				StringCchPrintfW(TempNumber, 32, L"%llu", uEndTime.QuadPart);
				SetEnvironmentVariableW(pEnvVariableName, TempNumber);
				return;
			}
		}
	}
	SetEnvironmentVariableW(pEnvVariableName, L"");
}

/***************************************

	Add a thread to the process

***************************************/

static void add_thread_handle(
	hook_thread_t* pHookThread, HANDLE hThreadHandle, const wchar_t* pName)
{
	// Sanity check
	if (!pHookThread) {
		return;
	}

	// Prepare to increase the buffer by one
	uint32_t uNumThreads = pHookThread->m_uNumThreads;
	uint32_t uNewNumThreads = uNumThreads + 1U;
	hook_thread_data_t* pThreadData = static_cast<hook_thread_data_t*>(
		heap_alloc(uNewNumThreads * sizeof(hook_thread_data_t)));

	// Out of memory?
	if (!pThreadData) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"hook_thread_t", L"add_thread_handle()", NULL);
		return;
	}

	// Copy the old data to the new structure
	memmove(pThreadData, pHookThread->m_pThreadData,
		sizeof(*pThreadData) * uNumThreads);

	// Create the new entry
	memmove(
		pThreadData[uNumThreads].m_Name, pName, sizeof(pThreadData->m_Name));
	pThreadData[uNumThreads].m_hThreadHandle = hThreadHandle;

	// Release the old buffer
	if (pHookThread->m_pThreadData) {
		heap_free(pHookThread->m_pThreadData);
	}
	pHookThread->m_pThreadData = pThreadData;
	pHookThread->m_uNumThreads = uNewNumThreads;
}

/***************************************

	Verify that a hook name and event are valid

***************************************/

bool valid_hook_name(
	const wchar_t* pEventName, const wchar_t* pActionName, bool bQuiet)
{
	// Exit/Post
	if (str_equiv(pEventName, g_NSSMHookEventExit)) {
		if (str_equiv(pActionName, g_NSSMHookActionPost)) {
			return true;
		}
		if (!bQuiet) {
			print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_ACTION, pEventName);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPost);
		}
		return false;
	}

	// Power/{Change,Resume}
	if (str_equiv(pEventName, g_NSSMHookEventPower)) {
		if (str_equiv(pActionName, g_NSSMHookActionChange))
			return true;
		if (str_equiv(pActionName, g_NSSMHookActionResume))
			return true;
		if (!bQuiet) {
			print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_ACTION, pEventName);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionChange);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionResume);
		}
		return false;
	}

	// Rotate/{Pre,Post}
	if (str_equiv(pEventName, g_NSSMHookEventRotate)) {
		if (str_equiv(pActionName, g_NSSMHookActionPre))
			return true;
		if (str_equiv(pActionName, g_NSSMHookActionPost))
			return true;
		if (!bQuiet) {
			print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_ACTION, pEventName);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPre);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPost);
		}
		return false;
	}

	// Start/{Pre,Post}
	if (str_equiv(pEventName, g_NSSMHookEventStart)) {
		if (str_equiv(pActionName, g_NSSMHookActionPre))
			return true;
		if (str_equiv(pActionName, g_NSSMHookActionPost))
			return true;
		if (!bQuiet) {
			print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_ACTION, pEventName);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPre);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPost);
		}
		return false;
	}

	// Stop/Pre
	if (str_equiv(pEventName, g_NSSMHookEventStop)) {
		if (str_equiv(pActionName, g_NSSMHookActionPre)) {
			return true;
		}
		if (!bQuiet) {
			print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_ACTION, pEventName);
			fwprintf(stderr, L"%s\n", g_NSSMHookActionPre);
		}
		return false;
	}

	// Verbose output?
	if (!bQuiet) {
		print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_EVENT);
		fwprintf(stderr, L"%s\n", g_NSSMHookEventExit);
		fwprintf(stderr, L"%s\n", g_NSSMHookEventPower);
		fwprintf(stderr, L"%s\n", g_NSSMHookEventRotate);
		fwprintf(stderr, L"%s\n", g_NSSMHookEventStart);
		fwprintf(stderr, L"%s\n", g_NSSMHookEventStop);
	}
	return false;
}

/***************************************

	Yield to all the threads and remove any thread that completes

***************************************/

void await_hook_threads(hook_thread_t* pHookThreads,
	SERVICE_STATUS_HANDLE hStatusHandle, SERVICE_STATUS* pServiceStatus,
	uint32_t uTimeoutDeadline)
{
	// Sanity check
	if (!pHookThreads || !pHookThreads->m_uNumThreads) {
		return;
	}

	// Get the array of indexes of entries to retain
	uint32_t* pRetainArray = static_cast<uint32_t*>(
		heap_calloc(pHookThreads->m_uNumThreads * sizeof(uint32_t)));
	if (!pRetainArray) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"retain",
			L"await_hook_threads()", NULL);
		return;
	}

	/*
	  We could use WaitForMultipleObjects() but await_single_object() can update
	  the service status as well.
	*/

	// Number of threads still pending
	uint32_t uNewNumThreads = 0;
	uint32_t i;
	for (i = 0; i < pHookThreads->m_uNumThreads; i++) {
		// Is there a timeout?
		if (uTimeoutDeadline) {
			if (await_single_handle(hStatusHandle, pServiceStatus,
					pHookThreads->m_pThreadData[i].m_hThreadHandle,
					pHookThreads->m_pThreadData[i].m_Name,
					L"await_hook_threads", uTimeoutDeadline) != 1) {
				// Thread go bye bye
				CloseHandle(pHookThreads->m_pThreadData[i].m_hThreadHandle);
				continue;
			}

			// Wait, or until timeout
		} else if (WaitForSingleObject(
					   pHookThreads->m_pThreadData[i].m_hThreadHandle,
					   WAIT_OBJECT_0) != WAIT_TIMEOUT) {
			CloseHandle(pHookThreads->m_pThreadData[i].m_hThreadHandle);
			continue;
		}
		// Add to the list to retain
		pRetainArray[uNewNumThreads++] = i;
	}

	// Any threads to keep?
	if (uNewNumThreads) {
		hook_thread_data_t* pThreadData = static_cast<hook_thread_data_t*>(
			heap_alloc(uNewNumThreads * sizeof(hook_thread_data_t)));
		if (!pThreadData) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
				L"pThreadData", L"await_hook_threads()", NULL);

		} else {
			// Copy only the retained records

			for (i = 0; i < uNewNumThreads; i++) {
				memmove(&pThreadData[i],
					&pHookThreads->m_pThreadData[pRetainArray[i]],
					sizeof(*pThreadData));
			}
			// Dispose of the old data
			heap_free(pHookThreads->m_pThreadData);

			// Update the records
			pHookThreads->m_pThreadData = pThreadData;
			pHookThreads->m_uNumThreads = uNewNumThreads;
		}
	} else {
		// Dispose all the thread data and reset the structure
		heap_free(pHookThreads->m_pThreadData);
		ZeroMemory(pHookThreads, sizeof(*pHookThreads));
	}
	// Get rid of the array of indexes
	heap_free(pRetainArray);
}

/***************************************

	Execute a hook

	Returns:
	NSSM_HOOK_STATUS_SUCCESS	if the hook ran successfully.
	NSSM_HOOK_STATUS_NOTFOUND	if no hook was found.
	NSSM_HOOK_STATUS_ABORT		if the hook failed and we should cancel service
								start.
	NSSM_HOOK_STATUS_ERROR		on error.
	NSSM_HOOK_STATUS_NOTRUN		if the hook didn't run.
	NSSM_HOOK_STATUS_TIMEOUT	if the hook timed out.
	NSSM_HOOK_STATUS_FAILED		if the hook failed.

***************************************/

int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl, uint32_t uTimeoutDeadline, bool bAsyc)
{
	hook_t* pHook = static_cast<hook_t*>(heap_calloc(sizeof(hook_t)));
	if (!pHook) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"hook",
			L"nssm_hook()", NULL);
		return NSSM_HOOK_STATUS_ERROR;
	}

	// Mark the current time
	FILETIME uCurrentTime;
	GetSystemTimeAsFileTime(&uCurrentTime);

	// Lock the hook
	EnterCriticalSection(&pService->m_HookLock);

	// Set the environment.
	set_service_environment(pService);

	// ABI version.
	wchar_t TempNumber[32];
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", NSSM_HOOK_VERSION);
	SetEnvironmentVariableW(g_NSSMHookEnvVersion, TempNumber);

	// Event triggering this action.
	SetEnvironmentVariableW(g_NSSMHookEnvEvent, pEventName);

	// Hook action.
	SetEnvironmentVariableW(g_NSSMHookEnvAction, pActionName);

	// Control triggering this action.  May be empty.
	if (pHookControl) {
		SetEnvironmentVariableW(
			g_NSSMHookEnvTrigger, service_control_text(*pHookControl));
	} else {
		SetEnvironmentVariableW(g_NSSMHookEnvTrigger, L"");
	}

	// Last control handled.
	SetEnvironmentVariableW(
		g_NSSMHookEnvLastControl, service_control_text(pService->m_uLastControl));

	// Path to NSSM, unquoted for the environment.
	SetEnvironmentVariableW(g_NSSMHookEnvImagePath, nssm_unquoted_imagepath());

	// NSSM version.
	SetEnvironmentVariableW(
		g_NSSMHookEnvNSSMConfiguration, g_NSSMConfiguration);
	SetEnvironmentVariableW(g_NSSMHookEnvNSSMVersion, g_NSSMVersion);
	SetEnvironmentVariableW(g_NSSMHookEnvBuildDate, g_NSSMDate);

	// NSSM PID.
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", GetCurrentProcessId());
	SetEnvironmentVariableW(g_NSSMHookEnvPID, TempNumber);

	// NSSM runtime.
	set_hook_runtime(
		g_NSSMHookEnvRuntime, &pService->m_NSSMCreationTime, &uCurrentTime);

	// Application PID.
	if (pService->m_uPID) {
		StringCchPrintfW(
			TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", pService->m_uPID);
		SetEnvironmentVariableW(g_NSSMHookEnvApplicationPID, TempNumber);
		// Application runtime.
		set_hook_runtime(g_NSSMHookEnvApplicationRuntime,
			&pService->m_ProcessCreationTime, &uCurrentTime);
		// Exit code.
		SetEnvironmentVariableW(g_NSSMHookEnvExitCode, L"");
	} else {
		SetEnvironmentVariableW(g_NSSMHookEnvApplicationPID, L"");
		if (str_equiv(pEventName, g_NSSMHookEventStart) &&
			str_equiv(pActionName, g_NSSMHookActionPre)) {
			SetEnvironmentVariableW(g_NSSMHookEnvApplicationRuntime, L"");
			SetEnvironmentVariableW(g_NSSMHookEnvExitCode, L"");
		} else {
			set_hook_runtime(g_NSSMHookEnvApplicationRuntime,
				&pService->m_ProcessCreationTime, &pService->m_ProcessExitTime);
			// Exit code.
			StringCchPrintfW(TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu",
				pService->m_uExitcode);
			SetEnvironmentVariableW(g_NSSMHookEnvExitCode, TempNumber);
		}
	}

	// Deadline for this script.
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", uTimeoutDeadline);
	SetEnvironmentVariableW(g_NSSMHookEnvDeadline, TempNumber);

	// Service name.
	SetEnvironmentVariableW(g_NSSMHookEnvServiceName, pService->m_Name);
	SetEnvironmentVariableW(
		g_NSSMHookEnvServiceDisplayName, pService->m_DisplayName);

	// Times the service was asked to start.
	StringCchPrintfW(TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu",
		pService->m_uStartRequestedCount);
	SetEnvironmentVariableW(g_NSSMHookEnvStartRequestedCount, TempNumber);

	// Times the service actually did start.
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", pService->m_uStartCount);
	SetEnvironmentVariableW(g_NSSMHookEnvStartCount, TempNumber);

	// Times the service exited.
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", pService->m_uExitCount);
	SetEnvironmentVariableW(g_NSSMHookEnvExitCount, TempNumber);

	// Throttled count.
	StringCchPrintfW(
		TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu", pService->m_uThrottle);
	SetEnvironmentVariableW(g_NSSMHookEnvThrottleCount, TempNumber);

	// Command line.
	wchar_t BigBuffer[CMD_LENGTH];
	StringCchPrintfW(BigBuffer, RTL_NUMBER_OF(BigBuffer), L"\"%s\" %s",
		pService->m_ExecutablePath, pService->m_AppParameters);
	SetEnvironmentVariableW(g_NSSMHookEnvCommandLine, BigBuffer);

	if (get_hook(pService->m_Name, pEventName, pActionName, BigBuffer,
			sizeof(BigBuffer))) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_HOOK_FAILED, pEventName,
			pActionName, pService->m_Name, NULL);
		unset_service_environment(pService);
		LeaveCriticalSection(&pService->m_HookLock);
		heap_free(pHook);
		return NSSM_HOOK_STATUS_ERROR;
	}

	// No hook.
	if (!wcslen(BigBuffer)) {
		unset_service_environment(pService);
		LeaveCriticalSection(&pService->m_HookLock);
		heap_free(pHook);
		return NSSM_HOOK_STATUS_NOTFOUND;
	}

	// Run the command.
	STARTUPINFOW si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	if (pService->m_bHookShareOutputHandles) {
		use_output_handles(pService, &si);
	}
	bool bInheritHandles = false;
	if (si.dwFlags & STARTF_USESTDHANDLES) {
		bInheritHandles = true;
	}
	DWORD uFlags = CREATE_UNICODE_ENVIRONMENT;

	int iReturn = NSSM_HOOK_STATUS_NOTRUN;
	if (CreateProcessW(NULL, BigBuffer, NULL, NULL, bInheritHandles, uFlags,
			NULL, pService->m_WorkingDirectory, &si, &pi)) {
		close_output_handles(&si);
		pHook->m_pName = static_cast<wchar_t*>(
			heap_alloc(HOOK_NAME_LENGTH * sizeof(wchar_t)));
		if (pHook->m_pName) {
			StringCchPrintfW(pHook->m_pName, HOOK_NAME_LENGTH, L"%s (%s/%s)",
				pService->m_Name, pEventName, pActionName);
		}
		pHook->m_hProcess = pi.hProcess;
		pHook->m_uPID = pi.dwProcessId;
		pHook->m_uTimeoutDeadline = uTimeoutDeadline;
		if (get_process_creation_time(
				pHook->m_hProcess, &pHook->m_uCreationTime)) {
			GetSystemTimeAsFileTime(&pHook->m_uCreationTime);
		}

		// Start the thread
		DWORD uThreadID;
		HANDLE hThreadHandle =
			CreateThread(NULL, 0, await_hook, pHook, 0, &uThreadID);

		if (hThreadHandle) {
			if (bAsyc) {
				iReturn = 0;
				await_hook_threads(pHookThreads, pService->m_hStatusHandle,
					&pService->m_ServiceStatus, 0);
				add_thread_handle(pHookThreads, hThreadHandle, pHook->m_pName);
			} else {
				await_single_handle(pService->m_hStatusHandle, &pService->m_ServiceStatus,
					hThreadHandle, pHook->m_pName, L"nssm_hook",
					uTimeoutDeadline + NSSM_SERVICE_STATUS_DEADLINE);
				DWORD uExitCode;
				GetExitCodeThread(hThreadHandle, &uExitCode);
				iReturn = static_cast<int>(uExitCode);
				CloseHandle(hThreadHandle);
			}
		} else {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED,
				error_string(GetLastError()), NULL);
			await_hook(pHook);
			if (pHook->m_pName) {
				heap_free(pHook->m_pName);
			}
			heap_free(pHook);
		}
	} else {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_HOOK_CREATEPROCESS_FAILED,
			pEventName, pActionName, pService->m_Name, BigBuffer,
			error_string(GetLastError()), NULL);
		heap_free(pHook);
		close_output_handles(&si);
	}

	// Restore our environment.
	unset_service_environment(pService);

	// Release the lock for the hook
	LeaveCriticalSection(&pService->m_HookLock);

	return iReturn;
}

/***************************************

	Execute a hook, simple API

***************************************/

int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl, uint32_t uTimeoutDeadline)
{
	return nssm_hook(pHookThreads, pService, pEventName, pActionName,
		pHookControl, uTimeoutDeadline, true);
}

/***************************************

	Execute a hook, simple API

***************************************/

int nssm_hook(hook_thread_t* pHookThreads, nssm_service_t* pService,
	const wchar_t* pEventName, const wchar_t* pActionName,
	uint32_t* pHookControl)
{
	return nssm_hook(pHookThreads, pService, pEventName, pActionName,
		pHookControl, NSSM_HOOK_DEADLINE, true);
}
