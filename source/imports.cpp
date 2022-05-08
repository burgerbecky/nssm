/***************************************

	Imports class

***************************************/

#include "imports.h"
#include "event.h"
#include "memorymanager.h"
#include "messages.h"
#include "utf8.h"

// Global imports
imports_t g_Imports;

/***************************************

	Try to set up function pointers.
	In this first implementation it is not an error if we can't load them
	because we aren't currently trying to load any functions which we
	absolutely need.  If we later add some indispensible imports we can
	return non-zero here to force an application exit.

***************************************/

/***************************************

	Load the DLL

***************************************/

static HMODULE get_dll(const wchar_t* pDLLName, uint32_t* pError)
{
	// No error yet
	DWORD uError = 0;

	HMODULE hModule = LoadLibraryW(pDLLName);
	if (!hModule) {
		// Error? Save it
		uError = GetLastError();

		// Log if it's not a PROC not found error
		if (uError != ERROR_PROC_NOT_FOUND) {
			log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_LOADLIBRARY_FAILED,
				pDLLName, error_string(uError), NULL);
		}
	}
	*pError = uError;
	return hModule;
}

/***************************************

	Find a function in the DLL

***************************************/

static FARPROC get_import(
	HMODULE hLibrary, const char* pFunctionName, uint32_t* pError)
{
	// No error yet
	DWORD uError = 0;

	FARPROC pProc = GetProcAddress(hLibrary, pFunctionName);
	if (!pProc) {
		uError = GetLastError();
		if (uError != ERROR_PROC_NOT_FOUND) {
			wchar_t* pWideName;
			if (!from_utf8(pFunctionName, &pWideName, NULL)) {
				log_event(EVENTLOG_WARNING_TYPE,
					NSSM_EVENT_GETPROCADDRESS_FAILED, pWideName,
					error_string(uError), NULL);
				heap_free(pWideName);
			}
		}
	}
	*pError = uError;
	return pProc;
}

/***************************************

	Import all of the requested functions

***************************************/

int get_imports(void)
{
	// Make sure we don't leak
	free_imports();

	uint32_t uError;

	// Load functions found in kernel32.dll
	g_Imports.m_hKernel32 = get_dll(L"kernel32.dll", &uError);
	if (g_Imports.m_hKernel32) {

		// Get all the functions
		g_Imports.AttachConsole = reinterpret_cast<AttachConsole_ptr>(
			get_import(g_Imports.m_hKernel32, "AttachConsole", &uError));
		if (!g_Imports.AttachConsole) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 2;
			}
		}

		g_Imports.QueryFullProcessImageNameW =
			reinterpret_cast<QueryFullProcessImageNameW_ptr>(get_import(
				g_Imports.m_hKernel32, "QueryFullProcessImageNameW", &uError));
		if (!g_Imports.QueryFullProcessImageNameW) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 3;
			}
		}

		g_Imports.SleepConditionVariableCS =
			reinterpret_cast<SleepConditionVariableCS_ptr>(get_import(
				g_Imports.m_hKernel32, "SleepConditionVariableCS", &uError));
		if (!g_Imports.SleepConditionVariableCS) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 4;
			}
		}

		g_Imports.WakeConditionVariable =
			reinterpret_cast<WakeConditionVariable_ptr>(get_import(
				g_Imports.m_hKernel32, "WakeConditionVariable", &uError));
		if (!g_Imports.WakeConditionVariable) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 5;
			}
		}

		// Should never trigger
	} else if (uError != ERROR_MOD_NOT_FOUND) {
		return 1;
	}

	// Get the vista functions from advapi32.dll

	g_Imports.advapi32 = get_dll(L"advapi32.dll", &uError);
	if (g_Imports.advapi32) {
		g_Imports.CreateWellKnownSid = reinterpret_cast<CreateWellKnownSid_ptr>(
			get_import(g_Imports.advapi32, "CreateWellKnownSid", &uError));
		if (!g_Imports.CreateWellKnownSid) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 7;
			}
		}

		g_Imports.IsWellKnownSid = reinterpret_cast<IsWellKnownSid_ptr>(
			get_import(g_Imports.advapi32, "IsWellKnownSid", &uError));
		if (!g_Imports.IsWellKnownSid) {
			if (uError != ERROR_PROC_NOT_FOUND) {
				return 8;
			}
		}
	} else if (uError != ERROR_MOD_NOT_FOUND) {
		return 6;
	}

	return 0;
}

/***************************************

	Release all the shared libraries

***************************************/

void free_imports(void)
{
	// Release the DLLs
	if (g_Imports.m_hKernel32) {
		FreeLibrary(g_Imports.m_hKernel32);
	}
	if (g_Imports.advapi32) {
		FreeLibrary(g_Imports.advapi32);
	}
	// Clear everything
	ZeroMemory(&g_Imports, sizeof(g_Imports));
}
