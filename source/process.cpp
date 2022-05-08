/***************************************

	Process manager functions

***************************************/

#include "process.h"
#include "constants.h"
#include "event.h"
#include "imports.h"
#include "memorymanager.h"
#include "messages.h"
#include "service.h"

#include <wchar.h>

#include <psapi.h>
#include <strsafe.h>
#include <tlhelp32.h>

#ifndef ENDSESSION_CLOSEAPP
#define ENDSESSION_CLOSEAPP 0x00000001
#define ENDSESSION_CRITICAL 0x40000000
#endif

HANDLE get_debug_token(void)
{
	DWORD uError;
	HANDLE hToken;
	if (!OpenThreadToken(GetCurrentThread(),
			TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, false, &hToken)) {
		uError = GetLastError();
		if (uError == ERROR_NO_TOKEN) {
			ImpersonateSelf(SecurityImpersonation);
			OpenThreadToken(GetCurrentThread(),
				TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, false, &hToken);
		}
	}
	if (!hToken) {
		return INVALID_HANDLE_VALUE;
	}

	LUID luid;
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
		CloseHandle(hToken);
		return INVALID_HANDLE_VALUE;
	}

	TOKEN_PRIVILEGES privileges;
	privileges.PrivilegeCount = 1;
	privileges.Privileges[0].Luid = luid;
	privileges.Privileges[0].Attributes = 0;

	TOKEN_PRIVILEGES old;
	DWORD uPrivilegesLength = sizeof(TOKEN_PRIVILEGES);
	if (!AdjustTokenPrivileges(hToken, false, &privileges, uPrivilegesLength,
			&old, &uPrivilegesLength)) {
		CloseHandle(hToken);
		return INVALID_HANDLE_VALUE;
	}

	old.PrivilegeCount = 1;
	old.Privileges[0].Luid = luid;
	old.Privileges[0].Attributes |= SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(
			hToken, false, &old, uPrivilegesLength, NULL, NULL)) {
		CloseHandle(hToken);
		return INVALID_HANDLE_VALUE;
	}

	return hToken;
}

/***************************************

	Initialize a kill_t structure

***************************************/

void service_kill_t(nssm_service_t* pNSSMService, kill_t* pKill)
{
	if (!pNSSMService || !pKill) {
		return;
	}

	ZeroMemory(pKill, sizeof(*pKill));
	pKill->m_pName = pNSSMService->m_Name;
	pKill->m_hProcess = pNSSMService->m_hProcess;
	pKill->m_uPID = pNSSMService->m_uPID;
	pKill->m_uExitcode = pNSSMService->m_uExitcode;
	pKill->m_uStopMethodFlags = pNSSMService->m_uStopMethodFlags;
	pKill->m_uKillConsoleDelay = pNSSMService->m_uKillConsoleDelay;
	pKill->m_uKillWindowDelay = pNSSMService->m_uKillWindowDelay;
	pKill->m_uKillThreadsDelay = pNSSMService->m_uKillThreadsDelay;
	pKill->m_hService = pNSSMService->m_hStatusHandle;
	pKill->m_pStatus = &pNSSMService->m_ServiceStatus;
	pKill->m_uCreationTime = pNSSMService->m_ProcessCreationTime;
	pKill->m_uExitTime = pNSSMService->m_ProcessExitTime;
}

/***************************************

	Get a processes' creation timestamp

***************************************/

int get_process_creation_time(HANDLE hProcessHandle, FILETIME* pOutput)
{
	FILETIME uCreation_time;
	FILETIME uExitTime;
	FILETIME uKernelTime;
	FILETIME uUserTime;

	if (!GetProcessTimes(hProcessHandle, &uCreation_time, &uExitTime,
			&uKernelTime, &uUserTime)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETPROCESSTIMES_FAILED,
			error_string(GetLastError()), NULL);
		return 1;
	}
	memmove(pOutput, &uCreation_time, sizeof(uCreation_time));
	return 0;
}

int get_process_exit_time(HANDLE hProcessHandle, FILETIME* pOutput)
{
	FILETIME uCreation_time;
	FILETIME uExitTime;
	FILETIME uKernelTime;
	FILETIME uUserTime;

	if (!GetProcessTimes(hProcessHandle, &uCreation_time, &uExitTime,
			&uKernelTime, &uUserTime)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETPROCESSTIMES_FAILED,
			error_string(GetLastError()), NULL);
		return 1;
	}

	if (!(uExitTime.dwLowDateTime || uExitTime.dwHighDateTime)) {
		return 2;
	}
	memmove(pOutput, &uExitTime, sizeof(uExitTime));
	return 0;
}

int check_parent(
	kill_t* pKill, tagPROCESSENTRY32W* pProcessEntry, uint32_t uParentProcessID)
{
	/* Check parent process ID matches. */
	if (pProcessEntry->th32ParentProcessID != uParentProcessID) {
		return 1;
	}

	/*
	  Process IDs can be reused so do a sanity check by making sure the child
	  has been running for less time than the parent.
	  Though unlikely, it's possible that the parent exited and its process ID
	  was already reused, so we'll also compare against its exit time.
	*/
	HANDLE hProcess = OpenProcess(
		PROCESS_QUERY_INFORMATION, false, pProcessEntry->th32ProcessID);
	if (!hProcess) {
		wchar_t TempNumber[32];
		StringCchPrintfW(TempNumber, RTL_NUMBER_OF(TempNumber), L"%lu",
			pProcessEntry->th32ProcessID);
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENPROCESS_FAILED,
			TempNumber, pKill->m_pName, error_string(GetLastError()), NULL);
		return 2;
	}

	FILETIME uFileTime;
	if (get_process_creation_time(hProcess, &uFileTime)) {
		CloseHandle(hProcess);
		return 3;
	}

	CloseHandle(hProcess);

	/* Verify that the parent's creation time is not later. */
	if (CompareFileTime(&pKill->m_uCreationTime, &uFileTime) > 0) {
		return 4;
	}

	/* Verify that the parent's exit time is not earlier. */
	if (CompareFileTime(&pKill->m_uExitTime, &uFileTime) < 0) {
		return 5;
	}
	return 0;
}

/* Send some window messages and hope the window respects one or more. */
int CALLBACK kill_window(HWND hWindow, LPARAM pArg)
{
	kill_t* pKill = reinterpret_cast<kill_t*>(pArg);

	DWORD pid;
	if (!GetWindowThreadProcessId(hWindow, &pid)) {
		return 1;
	}
	if (pid != pKill->m_uPID) {
		return 1;
	}

	/* First try sending WM_CLOSE to request that the window close. */
	pKill->m_iSignalled |=
		PostMessageW(hWindow, WM_CLOSE, pKill->m_uExitcode, 0);

	/*
	  Then tell the window that the user is logging off and it should exit
	  without worrying about saving any data.
	*/
	pKill->m_iSignalled |= PostMessageW(hWindow, WM_ENDSESSION, 1,
		ENDSESSION_CLOSEAPP | ENDSESSION_CRITICAL | ENDSESSION_LOGOFF);

	return 1;
}

/*
  Try to post a message to the message queues of threads associated with the
  given process ID.  Not all threads have message queues so there's no
  guarantee of success, and we don't want to be left waiting for unsignalled
  processes so this function returns only true if at least one thread was
  successfully prodded.
*/
int kill_threads(nssm_service_t* /* pNSSMService */, kill_t* pKill)
{
	/* Get a snapshot of all threads in the system. */
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_CREATETOOLHELP32SNAPSHOT_THREAD_FAILED, pKill->m_pName,
			error_string(GetLastError()), NULL);
		return 0;
	}

	THREADENTRY32 ThreadEntry;
	ZeroMemory(&ThreadEntry, sizeof(ThreadEntry));
	ThreadEntry.dwSize = sizeof(ThreadEntry);

	if (!Thread32First(hSnapshot, &ThreadEntry)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_THREAD_ENUMERATE_FAILED,
			pKill->m_pName, error_string(GetLastError()), NULL);
		CloseHandle(hSnapshot);
		return 0;
	}

	int iResult = 0;
	/* This thread belongs to the doomed process so signal it. */
	if (ThreadEntry.th32OwnerProcessID == pKill->m_uPID) {
		iResult |= PostThreadMessageW(
			ThreadEntry.th32ThreadID, WM_QUIT, pKill->m_uExitcode, 0);
	}

	for (;;) {
		/* Try to get the next thread. */
		if (!Thread32Next(hSnapshot, &ThreadEntry)) {
			DWORD uError = GetLastError();
			if (uError == ERROR_NO_MORE_FILES) {
				break;
			}
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_THREAD_ENUMERATE_FAILED,
				pKill->m_pName, error_string(GetLastError()), NULL);
			CloseHandle(hSnapshot);
			return iResult;
		}

		if (ThreadEntry.th32OwnerProcessID == pKill->m_uPID) {
			iResult |= PostThreadMessageW(
				ThreadEntry.th32ThreadID, WM_QUIT, pKill->m_uExitcode, 0);
		}
	}

	CloseHandle(hSnapshot);

	return iResult;
}

int kill_threads(kill_t* pKill)
{
	return kill_threads(NULL, pKill);
}

/* Give the process a chance to die gracefully. */
int kill_process(nssm_service_t* /* pNSSMService */, kill_t* pKill)
{
	if (!pKill) {
		return 1;
	}

	DWORD uResult;
	if (GetExitCodeProcess(pKill->m_hProcess, &uResult)) {
		if (uResult != STILL_ACTIVE)
			return 1;
	}

	/* Try to send a Control-C event to the console. */
	if (pKill->m_uStopMethodFlags & NSSM_STOP_METHOD_CONSOLE) {
		if (!kill_console(pKill)) {
			return 1;
		}
	}

	/*
	  Try to post messages to the windows belonging to the given process ID.
	  If the process is a console application it won't have any windows so
	  there's no guarantee of success.
	*/
	if (pKill->m_uStopMethodFlags & NSSM_STOP_METHOD_WINDOW) {
		EnumWindows((WNDENUMPROC)kill_window, (LPARAM)pKill);
		if (pKill->m_iSignalled) {
			if (!await_single_handle(pKill->m_hService, pKill->m_pStatus,
					pKill->m_hProcess, pKill->m_pName, L"kill_process",
					pKill->m_uKillWindowDelay))
				return 1;
			pKill->m_iSignalled = 0;
		}
	}

	/*
	  Try to post messages to any thread message queues associated with the
	  process.  Console applications might have them (but probably won't) so
	  there's still no guarantee of success.
	*/
	if (pKill->m_uStopMethodFlags & NSSM_STOP_METHOD_THREADS) {
		if (kill_threads(pKill)) {
			if (!await_single_handle(pKill->m_hService, pKill->m_pStatus,
					pKill->m_hProcess, pKill->m_pName, L"kill_process",
					pKill->m_uKillThreadsDelay))
				return 1;
		}
	}

	/* We tried being nice.  Time for extreme prejudice. */
	if (pKill->m_uStopMethodFlags & NSSM_STOP_METHOD_TERMINATE) {
		return TerminateProcess(pKill->m_hProcess, pKill->m_uExitcode);
	}

	return 0;
}

int kill_process(kill_t* pKill)
{
	return kill_process(NULL, pKill);
}

/* Simulate a Control-C event to our console (shared with the app). */
int kill_console(nssm_service_t* /* pNSSMService */, kill_t* pKill)
{
	if (!pKill) {
		return 1;
	}

	/* Check we loaded AttachConsole(). */
	if (!g_Imports.AttachConsole) {
		return 4;
	}

	DWORD uResult;
	/* Try to attach to the process's console. */
	if (!g_Imports.AttachConsole(pKill->m_uPID)) {
		uResult = GetLastError();

		switch (uResult) {
		case ERROR_INVALID_HANDLE:
			/* The app doesn't have a console. */
			return 1;

		case ERROR_GEN_FAILURE:
			/* The app already exited. */
			return 2;

		case ERROR_ACCESS_DENIED:
		default:
			/* We already have a console. */
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ATTACHCONSOLE_FAILED,
				pKill->m_pName, error_string(uResult), NULL);
			return 3;
		}
	}

	/* Ignore the event ourselves. */
	uResult = 0;
	BOOL ignored = SetConsoleCtrlHandler(0, TRUE);
	if (!ignored) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETCONSOLECTRLHANDLER_FAILED,
			pKill->m_pName, error_string(GetLastError()), NULL);
		uResult = 4;
	}

	/* Send the event. */
	if (!uResult) {
		if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
			log_event(EVENTLOG_ERROR_TYPE,
				NSSM_EVENT_GENERATECONSOLECTRLEVENT_FAILED, pKill->m_pName,
				error_string(GetLastError()), NULL);
			uResult = 5;
		}
	}

	/* Detach from the console. */
	if (!FreeConsole()) {
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_FREECONSOLE_FAILED,
			pKill->m_pName, error_string(GetLastError()), NULL);
	}

	/* Wait for process to exit. */
	if (await_single_handle(pKill->m_hService, pKill->m_pStatus,
			pKill->m_hProcess, pKill->m_pName, L"kill_console",
			pKill->m_uKillConsoleDelay)) {
		uResult = 6;
	}

	/* Remove our handler. */
	if (ignored && !SetConsoleCtrlHandler(0, FALSE)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETCONSOLECTRLHANDLER_FAILED,
			pKill->m_pName, error_string(GetLastError()), NULL);
	}

	return static_cast<int>(uResult);
}

int kill_console(kill_t* pKill)
{
	return kill_console(NULL, pKill);
}

void walk_process_tree(nssm_service_t* pNSSMService, walk_function_t fn,
	kill_t* pKill, uint32_t uParentProcessID)
{
	if (!pKill) {
		return;
	}

	/* Shouldn't happen unless the service failed to start. */
	/* XXX: needed? */
	if (!pKill->m_uPID) {
		return;
	}

	uint32_t uPID = pKill->m_uPID;
	// Save the depth
	uint32_t uDepth = pKill->m_uDepth;

	wchar_t pid_string[32];
	StringCchPrintfW(pid_string, RTL_NUMBER_OF(pid_string), L"%lu", uPID);

	if (fn == static_cast<walk_function_t>(kill_process)) {
		wchar_t exitcode_string[32];
		StringCchPrintfW(exitcode_string, RTL_NUMBER_OF(exitcode_string),
			L"%lu", pKill->m_uExitcode);
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_KILLING, pKill->m_pName,
			pid_string, exitcode_string, NULL);
	}

	/* We will need a process handle in order to call TerminateProcess() later.
	 */
	HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION |
			PROCESS_VM_READ | PROCESS_TERMINATE,
		false, uPID);
	if (hProcess) {

		// Kill this process first, then its descendents.
		if (fn == static_cast<walk_function_t>(kill_process)) {
			wchar_t ppid_string[32];
			StringCchPrintfW(ppid_string, RTL_NUMBER_OF(ppid_string), L"%lu",
				uParentProcessID);
			log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_KILL_PROCESS_TREE,
				pid_string, ppid_string, pKill->m_pName, NULL);
		}

		// XXX: open directly?
		pKill->m_hProcess = hProcess;
		if (!fn(pNSSMService, pKill)) {
			// Maybe it already died.
			DWORD uNewExitCode;
			if (!GetExitCodeProcess(hProcess, &uNewExitCode) ||
				uNewExitCode == STILL_ACTIVE) {
				if (pKill->m_uStopMethodFlags & NSSM_STOP_METHOD_TERMINATE) {
					log_event(EVENTLOG_ERROR_TYPE,
						NSSM_EVENT_TERMINATEPROCESS_FAILED, pid_string,
						pKill->m_pName, error_string(GetLastError()), NULL);
				} else {
					log_event(EVENTLOG_WARNING_TYPE,
						NSSM_EVENT_PROCESS_STILL_ACTIVE, pKill->m_pName,
						pid_string, g_NSSM, g_NSSMRegStopMethodSkip, NULL);
				}
			}
		}
		CloseHandle(hProcess);

	} else {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENPROCESS_FAILED,
			pid_string, pKill->m_pName, error_string(GetLastError()), NULL);
	}

	/* Get a snapshot of all processes in the system. */
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_CREATETOOLHELP32SNAPSHOT_PROCESS_FAILED, pKill->m_pName,
			error_string(GetLastError()), NULL);
		return;
	}

	PROCESSENTRY32W pe;
	ZeroMemory(&pe, sizeof(pe));
	pe.dwSize = sizeof(pe);

	if (!Process32FirstW(hSnapshot, &pe)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_PROCESS_ENUMERATE_FAILED,
			pKill->m_pName, error_string(GetLastError()), NULL);
		CloseHandle(hSnapshot);
		return;
	}

	/* This is a child of the doomed process so kill it. */
	pKill->m_uDepth++;
	if (!check_parent(pKill, &pe, uPID)) {
		pKill->m_uPID = pe.th32ProcessID;
		walk_process_tree(pNSSMService, fn, pKill, uParentProcessID);
	}
	pKill->m_uPID = uPID;

	for (;;) {
		/* Try to get the next process. */
		if (!Process32NextW(hSnapshot, &pe)) {
			DWORD uError = GetLastError();
			if (uError == ERROR_NO_MORE_FILES) {
				break;
			}
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_PROCESS_ENUMERATE_FAILED,
				pKill->m_pName, error_string(GetLastError()), NULL);
			CloseHandle(hSnapshot);
			pKill->m_uDepth = uDepth;
			return;
		}

		if (!check_parent(pKill, &pe, uPID)) {
			pKill->m_uPID = pe.th32ProcessID;
			walk_process_tree(pNSSMService, fn, pKill, uParentProcessID);
		}
		pKill->m_uPID = uPID;
	}
	pKill->m_uDepth = uDepth;

	CloseHandle(hSnapshot);
}

void kill_process_tree(kill_t* pKill, uint32_t uParentProcessID)
{
	return walk_process_tree(NULL, kill_process, pKill, uParentProcessID);
}

int print_process(nssm_service_t* /* pNSSMService */, kill_t* pKill)
{
	wchar_t exe[EXE_LENGTH];
	wchar_t* pBuffer = NULL;
	if (pKill->m_uDepth) {
		pBuffer = static_cast<wchar_t*>(
			heap_alloc((pKill->m_uDepth + 1) * sizeof(wchar_t)));
		if (pBuffer) {
			uint32_t i;
			for (i = 0; i < pKill->m_uDepth; i++) {
				pBuffer[i] = L' ';
			}
			pBuffer[i] = 0;
		}
	}

	unsigned long size = RTL_NUMBER_OF(exe);
	if (!g_Imports.QueryFullProcessImageNameW ||
		!g_Imports.QueryFullProcessImageNameW(
			pKill->m_hProcess, 0, exe, &size)) {
		/*
		  Fall back to GetModuleFileNameEx(), which won't work for WOW64
		  processes.
		*/
		if (!GetModuleFileNameExW(
				pKill->m_hProcess, NULL, exe, RTL_NUMBER_OF(exe))) {
			DWORD uError = GetLastError();
			if (uError == ERROR_PARTIAL_COPY) {
				StringCchPrintfW(exe, RTL_NUMBER_OF(exe), L"[WOW64]");
			} else {
				StringCchPrintfW(exe, RTL_NUMBER_OF(exe), L"???");
			}
		}
	}

	wprintf(L"% 8lu %s%s\n", pKill->m_uPID, pBuffer ? pBuffer : L"", exe);

	if (pBuffer) {
		heap_free(pBuffer);
	}
	return 1;
}

int print_process(kill_t* pKill)
{
	return print_process(NULL, pKill);
}
