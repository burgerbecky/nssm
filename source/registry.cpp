/***************************************

	Registry handler

***************************************/

#include "registry.h"
#include "constants.h"
#include "env.h"
#include "event.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "nssm_io.h"
#include "service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <wchar.h>

#include <Shlwapi.h>
#include <strsafe.h>

/***************************************

	Create the path to the registry entry

	"SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters"

***************************************/

static int service_registry_path(const wchar_t* pServiceName, bool bParameters,
	const wchar_t* pSub, wchar_t* pBuffer, uint32_t uBufferLength)
{
	int iResult;

	if (bParameters) {
		if (pSub) {
			iResult = StringCchPrintfW(pBuffer, uBufferLength,
				g_NSSMRegistryParameters2, pServiceName, pSub);
		} else {
			iResult = StringCchPrintfW(
				pBuffer, uBufferLength, g_NSSMRegistryParameters, pServiceName);
		}
	} else {
		iResult = StringCchPrintfW(
			pBuffer, uBufferLength, g_NSSMRegistry, pServiceName);
	}
	return iResult;
}

/***************************************

	Open a registry key and return the Windows error, if any

***************************************/

static LONG open_registry_key(
	const wchar_t* pRegistry, REGSAM uAccessMask, HKEY* pKey, bool bMustExist)
{
	LONG iResult;

	if (uAccessMask & KEY_SET_VALUE) {
		iResult = RegCreateKeyExW(HKEY_LOCAL_MACHINE, pRegistry, 0, NULL,
			REG_OPTION_NON_VOLATILE, uAccessMask, NULL, pKey, NULL);
		if (iResult != ERROR_SUCCESS) {
			*pKey = NULL;
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, pRegistry,
				error_string(static_cast<uint32_t>(iResult)), NULL);
		}
	} else {
		iResult =
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, pRegistry, 0, uAccessMask, pKey);
		if (iResult != ERROR_SUCCESS) {
			*pKey = NULL;
			if ((iResult != ERROR_FILE_NOT_FOUND) || bMustExist) {
				log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED,
					pRegistry, error_string(static_cast<uint32_t>(iResult)),
					NULL);
			}
		}
	}

	return iResult;
}

/***************************************

	Open a registry key and return key handle or NULL

***************************************/

static HKEY open_registry_key(
	const wchar_t* pRegistry, REGSAM uAccessMask, bool bMustExist)
{
	HKEY hKey;
	open_registry_key(pRegistry, uAccessMask, &hKey, bMustExist);
	return hKey;
}

/***************************************

	Open a registry key and return key handle or NULL

***************************************/

int create_messages(void)
{
	wchar_t RegistryName[KEY_LENGTH];
	if (StringCchPrintfW(RegistryName, RTL_NUMBER_OF(RegistryName),
			L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s",
			g_NSSM) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"eventlog registry", L"create_messages()", NULL);
		return 1;
	}

	HKEY hKey;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RegistryName, 0, NULL,
			REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
			NULL) != ERROR_SUCCESS) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, RegistryName,
			error_string(GetLastError()), NULL);
		return 2;
	}

	// Get path of this program
	const wchar_t* pPath = nssm_unquoted_imagepath();

	// Try to register the module but don't worry so much on failure
	RegSetValueExW(hKey, L"EventMessageFile", 0, REG_SZ,
		reinterpret_cast<const BYTE*>(pPath),
		static_cast<uint32_t>((wcslen(pPath) + 1) * sizeof(wchar_t)));

	uint32_t uTypes =
		EVENTLOG_INFORMATION_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_ERROR_TYPE;
	RegSetValueExW(hKey, L"TypesSupported", 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&uTypes), sizeof(uTypes));

	// Close the key
	RegCloseKey(hKey);
	return 0;
}

/***************************************

	Enumate a registry key

***************************************/

int enumerate_registry_values(
	HKEY hKey, uint32_t* pIndex, wchar_t* pName, uint32_t uNameLength)
{
	DWORD uType;
	DWORD uDataLength = uNameLength;
	LONG iError = RegEnumValueW(
		hKey, *pIndex, pName, &uDataLength, NULL, &uType, NULL, NULL);
	if (iError == ERROR_SUCCESS) {
		++*pIndex;
	}
	return static_cast<int>(iError);
}

/***************************************

	Create registry keys from a service

***************************************/

int create_parameters(nssm_service_t* pNSSMService, bool bEditing)
{
	/* Try to open the registry */
	HKEY hKey = open_registry(pNSSMService->m_Name, KEY_WRITE);
	if (!hKey) {
		return 1;
	}

	// Remember parameters in case we need to delete them.
	wchar_t RegistryName[KEY_LENGTH];
	int iResult = service_registry_path(pNSSMService->m_Name, true, 0,
		RegistryName, RTL_NUMBER_OF(RegistryName));

	// Try to create the parameters
	if (set_expand_string(hKey, g_NSSMRegExe, pNSSMService->m_ExecutablePath)) {
		if (iResult > 0) {
			RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryName);
		}
		RegCloseKey(hKey);
		return 2;
	}
	if (set_expand_string(
			hKey, g_NSSMRegFlags, pNSSMService->m_AppParameters)) {
		if (iResult > 0) {
			RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryName);
		}
		RegCloseKey(hKey);
		return 3;
	}
	if (set_expand_string(
			hKey, g_NSSMRegDir, pNSSMService->m_WorkingDirectory)) {
		if (iResult > 0) {
			RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryName);
		}
		RegCloseKey(hKey);
		return 4;
	}

	// Other non-default parameters. May fail.
	if (pNSSMService->m_uPriority != NORMAL_PRIORITY_CLASS) {
		set_number(hKey, g_NSSMRegPriority, pNSSMService->m_uPriority);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegPriority);
	}

	if (pNSSMService->m_uAffinity) {
		wchar_t* pString;
		if (!affinity_mask_to_string(pNSSMService->m_uAffinity, &pString)) {
			if (RegSetValueExW(hKey, g_NSSMRegAffinity, 0, REG_SZ,
					reinterpret_cast<const BYTE*>(pString),
					static_cast<DWORD>((wcslen(pString) + 1) *
						sizeof(wchar_t))) != ERROR_SUCCESS) {
				log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
					g_NSSMRegAffinity, error_string(GetLastError()), NULL);
				heap_free(pString);
				return 5;
			}
		}
		if (pString) {
			heap_free(pString);
		}
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegAffinity);
	}

	uint32_t uStopMethodSkip = ~pNSSMService->m_uStopMethodFlags;
	if (uStopMethodSkip) {
		set_number(hKey, g_NSSMRegStopMethodSkip, uStopMethodSkip);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegStopMethodSkip);
	}

	if (pNSSMService->m_uDefaultExitAction < NSSM_NUM_EXIT_ACTIONS) {
		create_exit_action(pNSSMService->m_Name,
			g_ExitActionStrings[pNSSMService->m_uDefaultExitAction], bEditing);
	}

	if (pNSSMService->m_uRestartDelay) {
		set_number(hKey, g_NSSMRegRestartDelay, pNSSMService->m_uRestartDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRestartDelay);
	}

	if (pNSSMService->m_uThrottleDelay != NSSM_RESET_THROTTLE_RESTART) {
		set_number(hKey, g_NSSMRegThrottle, pNSSMService->m_uThrottleDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegThrottle);
	}

	if (pNSSMService->m_uKillConsoleDelay != NSSM_KILL_CONSOLE_GRACE_PERIOD) {
		set_number(hKey, g_NSSMRegKillConsoleGracePeriod,
			pNSSMService->m_uKillConsoleDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegKillConsoleGracePeriod);
	}

	if (pNSSMService->m_uKillWindowDelay != NSSM_KILL_WINDOW_GRACE_PERIOD) {
		set_number(hKey, g_NSSMRegKillWindowGracePeriod,
			pNSSMService->m_uKillWindowDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegKillWindowGracePeriod);
	}

	if (pNSSMService->m_uKillThreadsDelay != NSSM_KILL_THREADS_GRACE_PERIOD) {
		set_number(hKey, g_NSSMRegKillThreadsGracePeriod,
			pNSSMService->m_uKillThreadsDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegKillThreadsGracePeriod);
	}

	if (!pNSSMService->m_bKillProcessTree) {
		set_number(hKey, g_NSSMRegKillProcessTree, 0);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegKillProcessTree);
	}

	if (pNSSMService->m_StdinPathname[0] || bEditing) {
		if (pNSSMService->m_StdinPathname[0]) {
			set_expand_string(
				hKey, g_NSSMRegStdIn, pNSSMService->m_StdinPathname);
		} else if (bEditing) {
			RegDeleteValueW(hKey, g_NSSMRegStdIn);
		}

		if (pNSSMService->m_uStdinSharing != NSSM_STDIN_SHARING) {
			set_createfile_parameter(hKey, g_NSSMRegStdIn,
				g_NSSMRegStdIOSharing, pNSSMService->m_uStdinSharing);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdIn, g_NSSMRegStdIOSharing);
		}

		if (pNSSMService->m_uStdinDisposition != NSSM_STDIN_DISPOSITION) {
			set_createfile_parameter(hKey, g_NSSMRegStdIn,
				g_NSSMRegStdIODisposition, pNSSMService->m_uStdinDisposition);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdIn, g_NSSMRegStdIODisposition);
		}

		if (pNSSMService->m_uStdinFlags != NSSM_STDIN_FLAGS) {
			set_createfile_parameter(hKey, g_NSSMRegStdIn, g_NSSMRegStdIOFlags,
				pNSSMService->m_uStdinFlags);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdIn, g_NSSMRegStdIOFlags);
		}
	}

	if (pNSSMService->m_StdoutPathname[0] || bEditing) {
		if (pNSSMService->m_StdoutPathname[0]) {
			set_expand_string(
				hKey, g_NSSMRegStdOut, pNSSMService->m_StdoutPathname);
		} else if (bEditing) {
			RegDeleteValueW(hKey, g_NSSMRegStdOut);
		}

		if (pNSSMService->m_uStdoutSharing != NSSM_STDOUT_SHARING) {
			set_createfile_parameter(hKey, g_NSSMRegStdOut,
				g_NSSMRegStdIOSharing, pNSSMService->m_uStdoutSharing);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdOut, g_NSSMRegStdIOSharing);
		}

		if (pNSSMService->m_uStdoutDisposition != NSSM_STDOUT_DISPOSITION) {
			set_createfile_parameter(hKey, g_NSSMRegStdOut,
				g_NSSMRegStdIODisposition, pNSSMService->m_uStdoutDisposition);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdOut, g_NSSMRegStdIODisposition);
		}

		if (pNSSMService->m_uStdoutFlags != NSSM_STDOUT_FLAGS) {
			set_createfile_parameter(hKey, g_NSSMRegStdOut, g_NSSMRegStdIOFlags,
				pNSSMService->m_uStdoutFlags);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdOut, g_NSSMRegStdIOFlags);
		}

		if (pNSSMService->m_bStdoutCopyAndTruncate) {
			set_createfile_parameter(
				hKey, g_NSSMRegStdOut, g_NSSMRegStdIOCopyAndTruncate, 1);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdOut, g_NSSMRegStdIOCopyAndTruncate);
		}
	}

	if (pNSSMService->m_StderrPathname[0] || bEditing) {
		if (pNSSMService->m_StderrPathname[0]) {
			set_expand_string(
				hKey, g_NSSMRegStdErr, pNSSMService->m_StderrPathname);
		} else if (bEditing) {
			RegDeleteValueW(hKey, g_NSSMRegStdErr);
		}

		if (pNSSMService->m_uStderrSharing != NSSM_STDERR_SHARING) {
			set_createfile_parameter(hKey, g_NSSMRegStdErr,
				g_NSSMRegStdIOSharing, pNSSMService->m_uStderrSharing);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdErr, g_NSSMRegStdIOSharing);
		}

		if (pNSSMService->m_uStderrDisposition != NSSM_STDERR_DISPOSITION) {
			set_createfile_parameter(hKey, g_NSSMRegStdErr,
				g_NSSMRegStdIODisposition, pNSSMService->m_uStderrDisposition);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdErr, g_NSSMRegStdIODisposition);
		}

		if (pNSSMService->m_uStderrFlags != NSSM_STDERR_FLAGS) {
			set_createfile_parameter(hKey, g_NSSMRegStdErr, g_NSSMRegStdIOFlags,
				pNSSMService->m_uStderrFlags);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdErr, g_NSSMRegStdIOFlags);
		}

		if (pNSSMService->m_bStderrCopyAndTruncate) {
			set_createfile_parameter(
				hKey, g_NSSMRegStdErr, g_NSSMRegStdIOCopyAndTruncate, 1);
		} else if (bEditing) {
			delete_createfile_parameter(
				hKey, g_NSSMRegStdErr, g_NSSMRegStdIOCopyAndTruncate);
		}
	}

	if (pNSSMService->m_bTimestampLog) {
		set_number(hKey, g_NSSMRegTimeStampLog, 1);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegTimeStampLog);
	}

	if (pNSSMService->m_bHookShareOutputHandles) {
		set_number(hKey, g_NSSMRegHookShareOutputHandles, 1);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegHookShareOutputHandles);
	}

	if (pNSSMService->m_bRotateFiles) {
		set_number(hKey, g_NSSMRegRotate, 1);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotate);
	}

	if (pNSSMService->m_uRotateStdoutOnline) {
		set_number(hKey, g_NSSMRegRotateOnline, 1);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotateOnline);
	}

	if (pNSSMService->m_uRotateSeconds) {
		set_number(
			hKey, g_NSSMRegRotateSeconds, pNSSMService->m_uRotateSeconds);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotateSeconds);
	}

	if (pNSSMService->m_uRotateBytesLow) {
		set_number(
			hKey, g_NSSMRegRotateBytesLow, pNSSMService->m_uRotateBytesLow);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotateBytesLow);
	}

	if (pNSSMService->m_uRotateBytesHigh) {
		set_number(
			hKey, g_NSSMRegRotateBytesHigh, pNSSMService->m_uRotateBytesHigh);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotateBytesHigh);
	}

	if (pNSSMService->m_uRotateDelay != NSSM_ROTATE_DELAY) {
		set_number(hKey, g_NSSMRegRotateDelay, pNSSMService->m_uRotateDelay);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegRotateDelay);
	}

	if (pNSSMService->m_bDontSpawnConsole) {
		set_number(hKey, g_NSSMRegNoConsole, 1);
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegNoConsole);
	}

	// Environment
	if (pNSSMService->m_pEnvironmentVariables) {
		if (RegSetValueExW(hKey, g_NSSMRegEnv, 0, REG_MULTI_SZ,
				reinterpret_cast<const BYTE*>(
					pNSSMService->m_pEnvironmentVariables),
				static_cast<DWORD>(pNSSMService->m_uEnvironmentVariablesLength *
					sizeof(wchar_t))) != ERROR_SUCCESS) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
				g_NSSMRegEnv, error_string(GetLastError()), NULL);
		}
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegEnv);
	}

	if (pNSSMService->m_pExtraEnvironmentVariables) {
		if (RegSetValueExW(hKey, g_NSSMRegEnvExtra, 0, REG_MULTI_SZ,
				reinterpret_cast<const BYTE*>(
					pNSSMService->m_pExtraEnvironmentVariables),
				static_cast<DWORD>(
					pNSSMService->m_uExtraEnvironmentVariablesLength *
					sizeof(wchar_t))) != ERROR_SUCCESS) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
				g_NSSMRegEnvExtra, error_string(GetLastError()), NULL);
		}
	} else if (bEditing) {
		RegDeleteValueW(hKey, g_NSSMRegEnvExtra);
	}

	// Close registry.
	RegCloseKey(hKey);

	return 0;
}

/***************************************

	Create registry keys for exit actions

***************************************/

int create_exit_action(
	const wchar_t* pServiceName, const wchar_t* pActionString, bool bEditing)
{
	/* Get registry */
	wchar_t RegistryName[KEY_LENGTH];
	if (service_registry_path(pServiceName, true, g_NSSMRegExit, RegistryName,
			RTL_NUMBER_OF(RegistryName)) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"NSSM_REG_EXIT", L"create_exit_action()", NULL);
		return 1;
	}

	// Try to open the registry
	HKEY hKey;
	DWORD uDisposition;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RegistryName, 0, 0,
			REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
			&uDisposition) != ERROR_SUCCESS) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, RegistryName,
			error_string(GetLastError()), NULL);
		return 2;
	}

	// Do nothing if the key already existed
	if ((uDisposition == REG_OPENED_EXISTING_KEY) && !bEditing) {
		RegCloseKey(hKey);
		return 0;
	}

	// Create the default value
	if (RegSetValueExW(hKey, NULL, 0, REG_SZ,
			reinterpret_cast<const BYTE*>(pActionString),
			static_cast<DWORD>((wcslen(pActionString) + 1) *
				sizeof(wchar_t))) != ERROR_SUCCESS) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
			g_NSSMRegExit, error_string(GetLastError()), NULL);
		RegCloseKey(hKey);
		return 3;
	}

	// Close registry
	RegCloseKey(hKey);

	return 0;
}

/***************************************

	Get environment variables from the registry

***************************************/

int get_environment(const wchar_t* pServiceName, HKEY hKey,
	const wchar_t* pValueName, wchar_t** ppEnvironmentVariables,
	uintptr_t* pEnvironmentVariablesLength)
{
	// Previously initialised?
	if (*ppEnvironmentVariables) {
		heap_free(*ppEnvironmentVariables);
		*ppEnvironmentVariables = NULL;
	}
	*pEnvironmentVariablesLength = 0;

	// Dummy test to find buffer size
	DWORD uType = REG_MULTI_SZ;
	DWORD uEnvironmentSize;
	LONG iResult =
		RegQueryValueExW(hKey, pValueName, 0, &uType, NULL, &uEnvironmentSize);
	if (iResult != ERROR_SUCCESS) {
		// The service probably doesn't have any environment configured
		if (iResult == ERROR_FILE_NOT_FOUND) {
			return 0;
		}
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED, pValueName,
			error_string(static_cast<uint32_t>(iResult)), NULL);
		return 1;
	}

	if (uType != REG_MULTI_SZ) {
		log_event(EVENTLOG_WARNING_TYPE,
			NSSM_EVENT_INVALID_ENVIRONMENT_STRING_TYPE, pValueName,
			pServiceName, NULL);
		return 2;
	}

	// Minimum usable environment would be A= NULL NULL.
	if (uEnvironmentSize < (4 * sizeof(wchar_t))) {
		return 3;
	}

	*ppEnvironmentVariables =
		static_cast<wchar_t*>(heap_alloc(uEnvironmentSize));
	if (!*ppEnvironmentVariables) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, pValueName,
			L"get_environment()", NULL);
		return 4;
	}

	// Actually get the strings.
	iResult = RegQueryValueExW(hKey, pValueName, 0, &uType,
		reinterpret_cast<BYTE*>(*ppEnvironmentVariables), &uEnvironmentSize);
	if (iResult != ERROR_SUCCESS) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED, pValueName,
			error_string(static_cast<uint32_t>(iResult)), NULL);
		heap_free(*ppEnvironmentVariables);
		*ppEnvironmentVariables = 0;
		return 5;
	}

	// Value retrieved by RegQueryValueEx() is size in bytes, not elements
	*pEnvironmentVariablesLength = environment_length(*ppEnvironmentVariables);

	return 0;
}

/***************************************

	Get a string from the registry, with or without expansion

***************************************/

int get_string(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bExpand, bool bSanitize, bool bMustExist)
{
	// Create a duplicate buffer so expansion can happen
	wchar_t* pTempBuffer = static_cast<wchar_t*>(heap_alloc(uBufferLength));
	if (!pTempBuffer) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, pValueName,
			L"get_string()", NULL);
		return 1;
	}
	// Clear the output
	ZeroMemory(pBuffer, uBufferLength);

	DWORD uType = REG_EXPAND_SZ;
	DWORD uTempBufferLength = uBufferLength;

	long iResult = RegQueryValueExW(hKey, pValueName, 0, &uType,
		reinterpret_cast<BYTE*>(pTempBuffer), &uTempBufferLength);
	if (iResult != ERROR_SUCCESS) {
		heap_free(pTempBuffer);

		if (iResult == ERROR_FILE_NOT_FOUND) {
			if (!bMustExist) {
				return 0;
			}
		}

		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED, pValueName,
			error_string(static_cast<uint32_t>(iResult)), NULL);
		return 2;
	}

	// Paths aren't allowed to contain quotes.
	if (bSanitize) {
		PathUnquoteSpacesW(pTempBuffer);
	}

	// Do we want to expand the string?
	if (!bExpand) {
		// Force the registry to NOT expand the string
		if (uType == REG_EXPAND_SZ) {
			uType = REG_SZ;
		}
	}

	// Technically we shouldn't expand environment strings from REG_SZ values
	if (uType != REG_EXPAND_SZ) {
		memmove(pBuffer, pTempBuffer, uTempBufferLength);
		heap_free(pTempBuffer);
		return 0;
	}

	DWORD uRet = ExpandEnvironmentStringsW(pTempBuffer, pBuffer, uBufferLength);
	if (!uRet || (uRet > uBufferLength)) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_EXPANDENVIRONMENTSTRINGS_FAILED, pTempBuffer,
			error_string(GetLastError()), NULL);
		heap_free(pTempBuffer);
		return 3;
	}

	heap_free(pTempBuffer);
	return 0;
}

/***************************************

	Get a string from the registry, without expansion

***************************************/

int get_string(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bSanitize)
{
	return get_string(
		hKey, pValueName, pBuffer, uBufferLength, false, bSanitize, true);
}

/***************************************

	Get a string from the registry, with expansion

***************************************/

int expand_parameter(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bSanitize, bool bMustExist)
{
	return get_string(
		hKey, pValueName, pBuffer, uBufferLength, true, bSanitize, bMustExist);
}

/***************************************

	Get a string from the registry, with expansion that must exist

***************************************/

int expand_parameter(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bSanitize)
{
	return expand_parameter(
		hKey, pValueName, pBuffer, uBufferLength, bSanitize, true);
}

/***************************************

	Sets a string in the registry.
	Returns:0 if it was set.
			1 on error.

***************************************/

int set_string(
	HKEY hKey, const wchar_t* pValueName, const wchar_t* pString, bool bExpand)
{
	// Type of string to save
	uint32_t uType = bExpand ? static_cast<uint32_t>(REG_EXPAND_SZ) :
                               static_cast<uint32_t>(REG_SZ);

	if (RegSetValueExW(hKey, pValueName, 0, uType,
			reinterpret_cast<const BYTE*>(pString),
			static_cast<DWORD>((wcslen(pString) + 1) * sizeof(wchar_t))) ==
		ERROR_SUCCESS) {
		return 0;
	}
	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED, pValueName,
		error_string(GetLastError()), NULL);
	return 1;
}

/***************************************

	Sets a literal string in the registry

***************************************/

int set_string(HKEY hKey, const wchar_t* pValueName, const wchar_t* pString)
{
	return set_string(hKey, pValueName, pString, false);
}

/***************************************

	Sets an expandable string in the registry

***************************************/

int set_expand_string(
	HKEY hKey, const wchar_t* pValueName, const wchar_t* pString)
{
	return set_string(hKey, pValueName, pString, true);
}

/***************************************

	Set an unsigned long in the registry.
	Returns:0 if it was set.
			1 on error.

***************************************/

int set_number(HKEY hKey, const wchar_t* pValueName, uint32_t uNumber)
{
	if (RegSetValueExW(hKey, pValueName, 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&uNumber),
			sizeof(uNumber)) == ERROR_SUCCESS) {
		return 0;
	}

	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED, pValueName,
		error_string(GetLastError()), NULL);
	return 1;
}

/***************************************

	Query an unsigned long from the registry.
	Returns:1 if a number was retrieved.
			0 if none was found and must_exist is false.
			-1 if none was found and must_exist is true.
			-2 otherwise.

***************************************/

int get_number(
	HKEY hKey, const wchar_t* pValueName, uint32_t* pNumber, bool bMustExist)
{
	DWORD uType = REG_DWORD;
	DWORD uNumberLength = sizeof(uint32_t);

	LONG iResult = RegQueryValueExW(hKey, pValueName, 0, &uType,
		reinterpret_cast<BYTE*>(pNumber), &uNumberLength);
	if (iResult == ERROR_SUCCESS) {
		return 1;
	}

	if (iResult == ERROR_FILE_NOT_FOUND) {
		if (!bMustExist) {
			return 0;
		}
	}

	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED, pValueName,
		error_string(static_cast<uint32_t>(iResult)), NULL);
	if (iResult == ERROR_FILE_NOT_FOUND) {
		return -1;
	}

	return -2;
}

/***************************************

	Get an unsigned 32 bit integer from the registry. It must exist

***************************************/

int get_number(HKEY hKey, const wchar_t* pValueName, uint32_t* pNumber)
{
	return get_number(hKey, pValueName, pNumber, true);
}

/***************************************

	Replace NULL with CRLF. Leave NULL NULL as the end marker.

***************************************/

int format_double_null(const wchar_t* pInput, uintptr_t uInputLength,
	wchar_t** ppFormatted, uintptr_t* pFormattedLength)
{

	*pFormattedLength = uInputLength;

	if (!*pFormattedLength) {
		*ppFormatted = NULL;
		return 0;
	}

	uintptr_t i;
	for (i = 0; i < uInputLength; i++) {
		if (!pInput[i] && pInput[i + 1]) {
			++*pFormattedLength;
		}
	}

	*ppFormatted =
		static_cast<wchar_t*>(heap_calloc(*pFormattedLength * sizeof(wchar_t)));
	if (!*ppFormatted) {
		*pFormattedLength = 0;
		return 1;
	}

	uintptr_t j;
	for (i = 0, j = 0; i < uInputLength; i++) {
		(*ppFormatted)[j] = pInput[i];
		if (!pInput[i]) {
			if (pInput[i + 1]) {
				(*ppFormatted)[j] = L'\r';
				(*ppFormatted)[++j] = L'\n';
			}
		}
		j++;
	}

	return 0;
}

/***************************************

	Strip CR and replace LF with NULL.

	Note: Will damage data pointed by pFormatted

***************************************/

int unformat_double_null(wchar_t* pFormatted, uintptr_t uFormattedLength,
	wchar_t** ppParsed, uintptr_t* pParsedLength)
{

	*pParsedLength = 0;

	// Don't count trailing NULLs.
	uintptr_t i;
	for (i = 0; i < uFormattedLength; i++) {
		if (!pFormatted[i]) {
			uFormattedLength = i;
			break;
		}
	}

	if (!uFormattedLength) {
		*ppParsed = NULL;
		return 0;
	}

	for (i = 0; i < uFormattedLength; i++) {
		if (pFormatted[i] != L'\r') {
			++*pParsedLength;
		}
	}

	// Skip blank lines.
	for (i = 0; i < uFormattedLength; i++) {
		if ((pFormatted[i] == L'\r') && (pFormatted[i + 1] == L'\n')) {
			// This is the last CRLF.
			if (i >= (uFormattedLength - 2)) {
				break;
			}

			/*
			  Strip at the start of the block or if the next characters are
			  CRLF too.
			*/
			if (!i ||
				((pFormatted[i + 2] == L'\r') &&
					(pFormatted[i + 3] == L'\n'))) {
				uintptr_t j;
				for (j = i + 2; j < uFormattedLength; j++) {
					pFormatted[j - 2] = pFormatted[j];
				}
				pFormatted[uFormattedLength--] = 0;
				pFormatted[uFormattedLength--] = 0;
				i--;
				--*pParsedLength;
			}
		}
	}

	// Must end with two NULLs.
	*pParsedLength += 2;

	*ppParsed =
		static_cast<wchar_t*>(heap_calloc(*pParsedLength * sizeof(wchar_t)));
	if (!*ppParsed) {
		return 1;
	}

	// Ignore \r, and replace \n with null
	uintptr_t k;
	for (i = 0, k = 0; i < uFormattedLength; i++) {
		if (pFormatted[i] == L'\r') {
			continue;
		}
		if (pFormatted[i] == L'\n') {
			(*ppParsed)[k] = 0;
		} else {
			(*ppParsed)[k] = pFormatted[i];
		}
		++k;
	}

	return 0;
}

/***************************************

	Copy a block.

***************************************/

int copy_double_null(
	const wchar_t* pInput, uintptr_t uInputLength, wchar_t** ppOutput)
{
	if (!ppOutput) {
		return 1;
	}

	*ppOutput = NULL;
	if (!pInput) {
		return 0;
	}

	*ppOutput =
		static_cast<wchar_t*>(heap_alloc(uInputLength * sizeof(wchar_t)));
	if (!*ppOutput) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"pInput",
			L"copy_double_null()", NULL);
		return 2;
	}

	memmove(*ppOutput, pInput, uInputLength * sizeof(wchar_t));
	return 0;
}

/***************************************

	Create a new block with all the strings of the first block plus a new
	string.

	The new string may be specified as <key> <delimiter> <value> and the keylen
	gives the offset into the string to compare against existing entries.

	If the key is already present its value will be overwritten in place.

	If the key is blank or empty the new block will still be allocated and have
	non-zero length.

***************************************/

int append_to_double_null(const wchar_t* pInput, uintptr_t uInputLength,
	wchar_t** ppOutput, uintptr_t* pOutputLength, const wchar_t* pAppend,
	uintptr_t uKeyLength, bool bCaseSensitive)
{
	// Nothing to append?
	if (!pAppend || !pAppend[0]) {
		return copy_double_null(pInput, uInputLength, ppOutput);
	}

	uintptr_t uAppendLength = wcslen(pAppend);
	int (*fn)(const wchar_t*, const wchar_t*, size_t) =
		(bCaseSensitive) ? wcsncmp : _wcsnicmp;

	// Identify the key, if any, or treat the whole string as the key.
	if (!uKeyLength || (uKeyLength > uAppendLength)) {
		uKeyLength = uAppendLength;
	}

	// Make a buffer for the key for comparisons
	wchar_t* pKey =
		static_cast<wchar_t*>(heap_alloc((uKeyLength + 1) * sizeof(wchar_t)));
	if (!pKey) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"key",
			L"append_to_double_null()", NULL);
		return 1;
	}
	memmove(pKey, pAppend, uKeyLength * sizeof(wchar_t));
	pKey[uKeyLength] = 0;

	// Find the length of the block not including any existing key.
	uintptr_t uLength = 0;
	const wchar_t* s;
	for (s = pInput; *s; s++) {
		if (fn(s, pKey, uKeyLength)) {
			uLength += wcslen(s) + 1;
		}
		for (; *s; s++) {
		}
	}

	// Account for new entry and trailing null
	uLength += wcslen(pAppend) + 2;

	// Allocate a new block.
	*ppOutput = static_cast<wchar_t*>(heap_calloc(uLength * sizeof(wchar_t)));
	if (!*ppOutput) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"newdn",
			L"append_to_double_null()", NULL);
		heap_free(pKey);
		return 2;
	}

	// Copy existing entries.
	*pOutputLength = uLength;
	wchar_t* pOutput = *ppOutput;
	const wchar_t* u;

	// Set to true if data was replaced
	bool bReplaced = false;
	for (s = pInput; *s; s++) {
		if (fn(s, pKey, uKeyLength)) {
			u = s;
		} else {
			u = pAppend;
			bReplaced = true;
		}
		uLength = wcslen(u) + 1;
		memmove(pOutput, u, uLength * sizeof(wchar_t));
		pOutput += uLength;
		for (; *s; s++) {
		}
	}

	// Add the entry if it wasn't already replaced.  The buffer was zeroed.
	if (!bReplaced) {
		memmove(pOutput, pAppend, wcslen(pAppend) * sizeof(wchar_t));
	}
	heap_free(pKey);
	return 0;
}

/***************************************

	Create a new block with all the string of the first block minus the given
	string.

	The keylen parameter gives the offset into the string to compare against
	existing entries. If a substring of existing value matches the string to
	the given length it will be removed.

	If the last entry is removed the new block will still be allocated and
	have non-zero length.

***************************************/

int remove_from_double_null(const wchar_t* pInput, uintptr_t uInputLength,
	wchar_t** ppOutput, uintptr_t* pOutputLength, const wchar_t* pRemove,
	uintptr_t uKeyLength, bool bCaseSensitive)
{
	// Nothing to remove? Just copy
	if (!pRemove || !pRemove[0]) {
		return copy_double_null(pInput, uInputLength, ppOutput);
	}

	uintptr_t uRemoveLength = wcslen(pRemove);

	// Type of comparison
	int (*fn)(const wchar_t*, const wchar_t*, size_t) =
		bCaseSensitive ? wcsncmp : _wcsnicmp;

	// Identify the key, if any, or treat the whole string as the key.
	if (!uKeyLength || (uKeyLength > uRemoveLength)) {
		uKeyLength = uRemoveLength;
	}
	wchar_t* pKey = (wchar_t*)heap_alloc((uKeyLength + 1) * sizeof(wchar_t));
	if (!pKey) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"key",
			L"remove_from_double_null()", NULL);
		return 1;
	}
	memmove(pKey, remove, uKeyLength * sizeof(wchar_t));
	pKey[uKeyLength] = 0;

	// Find the length of the block not including any existing key.
	uintptr_t uOutputLength = 0;
	const wchar_t* s;
	for (s = pInput; *s; s++) {
		if (fn(s, pKey, uKeyLength)) {
			uOutputLength += wcslen(s) + 1;
		}
		for (; *s; s++) {
		}
	}

	// Account for trailing NULL.
	if (++uOutputLength < 2) {
		uOutputLength = 2;
	}

	// Allocate a new block.
	*ppOutput =
		static_cast<wchar_t*>(heap_calloc(uOutputLength * sizeof(wchar_t)));
	if (!*ppOutput) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"newdn",
			L"remove_from_double_null()", NULL);
		heap_free(pKey);
		return 2;
	}

	// Copy existing entries.
	*pOutputLength = uOutputLength;
	wchar_t* t = *ppOutput;
	for (s = pInput; *s; s++) {
		if (fn(s, pKey, uKeyLength)) {
			uOutputLength = wcslen(s) + 1;
			memmove(t, s, uOutputLength * sizeof(wchar_t));
			t += uOutputLength;
		}
		for (; *s; s++) {
		}
	}

	heap_free(pKey);
	return 0;
}

void override_milliseconds(const wchar_t* pServiceName, HKEY hKey,
	const wchar_t* pValueName, uint32_t* pNumber, uint32_t uDefaultValue,
	uint32_t uLogEvent)
{
	DWORD uType = REG_DWORD;
	DWORD uBufferLength = sizeof(DWORD);
	bool bOK = false;
	LONG iResult = RegQueryValueExW(hKey, pValueName, 0, &uType,
		reinterpret_cast<BYTE*>(pNumber), &uBufferLength);
	if (iResult != ERROR_SUCCESS) {
		if (iResult != ERROR_FILE_NOT_FOUND) {
			if (uType != REG_DWORD) {
				wchar_t MillisecondString[16];
				StringCchPrintfW(MillisecondString,
					RTL_NUMBER_OF(MillisecondString), L"%lu", uDefaultValue);
				log_event(EVENTLOG_WARNING_TYPE, uLogEvent, pServiceName,
					pValueName, MillisecondString, NULL);
			} else {
				log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED,
					pValueName, error_string(static_cast<uint32_t>(iResult)),
					NULL);
			}
		}
	} else {
		bOK = true;
	}
	if (!bOK) {
		*pNumber = uDefaultValue;
	}
}

/***************************************

	Open the key of the service itself Services\<service_name>.

***************************************/

HKEY open_service_registry(
	const wchar_t* pServiceName, REGSAM uAccessMask, bool bMustExist)
{
	// Get registry key name
	wchar_t RegistryName[KEY_LENGTH];
	if (service_registry_path(pServiceName, false, 0, RegistryName,
			RTL_NUMBER_OF(RegistryName)) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, g_NSSMRegistry,
			"open_service_registry()", NULL);
		return 0;
	}
	// Open the key
	return open_registry_key(RegistryName, uAccessMask, bMustExist);
}

/***************************************

	Open a subkey of the service Services\<service_name>\<sub>.

***************************************/

long open_registry(const wchar_t* pServiceName, const wchar_t* pSub,
	REGSAM uAccessMask, HKEY* pKey, bool bMustExist)
{
	// Get registry
	wchar_t RegistryName[KEY_LENGTH];
	if (service_registry_path(pServiceName, true, pSub, RegistryName,
			RTL_NUMBER_OF(RegistryName)) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, g_NSSMRegistry,
			L"open_registry()", NULL);
		return 0;
	}

	return open_registry_key(RegistryName, uAccessMask, pKey, bMustExist);
}

HKEY open_registry(const wchar_t* pServiceName, const wchar_t* pSub,
	REGSAM uAccessMask, bool bMustExist)
{
	HKEY hKey;
	open_registry(pServiceName, pSub, uAccessMask, &hKey, bMustExist);
	return hKey;
}

/***************************************

	Open a subkey of the service Services\<service_name>\<sub>.
	Must exist

***************************************/

HKEY open_registry(
	const wchar_t* pServiceName, const wchar_t* pSub, REGSAM uAccessMask)
{
	return open_registry(pServiceName, pSub, uAccessMask, true);
}

HKEY open_registry(const wchar_t* pServiceName, REGSAM uAccessMask)
{
	return open_registry(pServiceName, NULL, uAccessMask, true);
}

/***************************************

	Pull the stdin, stderr, stdout values from the registry

***************************************/

int get_io_parameters(nssm_service_t* pNSSMService, HKEY hKey)
{
	// stdin
	if (get_createfile_parameters(hKey, g_NSSMRegStdIn,
			pNSSMService->m_StdinPathname, &pNSSMService->m_uStdinSharing,
			NSSM_STDIN_SHARING, &pNSSMService->m_uStdinDisposition,
			NSSM_STDIN_DISPOSITION, &pNSSMService->m_uStdinFlags,
			NSSM_STDIN_FLAGS, 0)) {
		pNSSMService->m_uStdinSharing = pNSSMService->m_uStdinDisposition =
			pNSSMService->m_uStdinFlags = 0;
		ZeroMemory(pNSSMService->m_StdinPathname,
			RTL_NUMBER_OF(pNSSMService->m_StdinPathname) * sizeof(wchar_t));
		return 1;
	}

	// stdout
	if (get_createfile_parameters(hKey, g_NSSMRegStdOut,
			pNSSMService->m_StdoutPathname, &pNSSMService->m_uStdoutSharing,
			NSSM_STDOUT_SHARING, &pNSSMService->m_uStdoutDisposition,
			NSSM_STDOUT_DISPOSITION, &pNSSMService->m_uStdoutFlags,
			NSSM_STDOUT_FLAGS, &pNSSMService->m_bStdoutCopyAndTruncate)) {
		pNSSMService->m_uStdoutSharing = pNSSMService->m_uStdoutDisposition =
			pNSSMService->m_uStdoutFlags = 0;
		ZeroMemory(pNSSMService->m_StdoutPathname,
			RTL_NUMBER_OF(pNSSMService->m_StdoutPathname) * sizeof(wchar_t));
		return 2;
	}

	// stderr
	if (get_createfile_parameters(hKey, g_NSSMRegStdErr,
			pNSSMService->m_StderrPathname, &pNSSMService->m_uStderrSharing,
			NSSM_STDERR_SHARING, &pNSSMService->m_uStderrDisposition,
			NSSM_STDERR_DISPOSITION, &pNSSMService->m_uStderrFlags,
			NSSM_STDERR_FLAGS, &pNSSMService->m_bStderrCopyAndTruncate)) {
		pNSSMService->m_uStderrSharing = pNSSMService->m_uStderrDisposition =
			pNSSMService->m_uStderrFlags = 0;
		ZeroMemory(pNSSMService->m_StderrPathname,
			RTL_NUMBER_OF(pNSSMService->m_StderrPathname) * sizeof(wchar_t));
		return 3;
	}

	return 0;
}

/***************************************

	Get all the parameters from the registry

***************************************/

int get_parameters(
	nssm_service_t* pNSSMService, const STARTUPINFOW* pStartupInfo)
{
	// Try to open the registry
	HKEY hKey = open_registry(pNSSMService->m_Name, KEY_READ);
	if (!hKey) {
		return 1;
	}

	// Don't expand parameters when retrieving for the GUI.
	bool bExpand = pStartupInfo ? true : false;

	// Try to get environment variables - may fail
	get_environment(pNSSMService->m_Name, hKey, g_NSSMRegEnv,
		&pNSSMService->m_pEnvironmentVariables,
		&pNSSMService->m_uEnvironmentVariablesLength);

	// Environment variables to add to existing rather than replace - may fail.
	get_environment(pNSSMService->m_Name, hKey, g_NSSMRegEnvExtra,
		&pNSSMService->m_pExtraEnvironmentVariables,
		&pNSSMService->m_uExtraEnvironmentVariablesLength);

	// Set environment if we are starting the service.
	if (pStartupInfo) {
		set_service_environment(pNSSMService);
	}

	// Try to get executable file - MUST succeed
	if (get_string(hKey, g_NSSMRegExe, pNSSMService->m_ExecutablePath,
			sizeof(pNSSMService->m_ExecutablePath), bExpand, false, true)) {
		RegCloseKey(hKey);
		return 3;
	}

	// Try to get flags - may fail and we don't care
	if (get_string(hKey, g_NSSMRegFlags, pNSSMService->m_AppParameters,
			sizeof(pNSSMService->m_AppParameters), bExpand, false, true)) {
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_NO_FLAGS, g_NSSMRegFlags,
			pNSSMService->m_Name, pNSSMService->m_ExecutablePath, NULL);
		ZeroMemory(pNSSMService->m_AppParameters,
			sizeof(pNSSMService->m_AppParameters));
	}

	// Try to get startup directory - may fail and we fall back to a default
	if (get_string(hKey, g_NSSMRegDir, pNSSMService->m_WorkingDirectory,
			sizeof(pNSSMService->m_WorkingDirectory), bExpand, true, true) ||
		!pNSSMService->m_WorkingDirectory[0]) {

		StringCchPrintfW(pNSSMService->m_WorkingDirectory,
			RTL_NUMBER_OF(pNSSMService->m_WorkingDirectory), L"%s",
			pNSSMService->m_ExecutablePath);

		strip_basename(pNSSMService->m_WorkingDirectory);

		if (pNSSMService->m_WorkingDirectory[0] == 0) {
			// Help!
			UINT uRet = GetWindowsDirectoryW(pNSSMService->m_WorkingDirectory,
				sizeof(pNSSMService->m_WorkingDirectory));
			if (!uRet || (uRet > sizeof(pNSSMService->m_WorkingDirectory))) {
				log_event(EVENTLOG_ERROR_TYPE,
					NSSM_EVENT_NO_DIR_AND_NO_FALLBACK, g_NSSMRegDir,
					pNSSMService->m_Name, NULL);
				RegCloseKey(hKey);
				return 4;
			}
		}
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_NO_DIR, g_NSSMRegDir,
			pNSSMService->m_Name, pNSSMService->m_WorkingDirectory, NULL);
	}

	// Try to get processor affinity - may fail.
	wchar_t buffer[512];
	if (get_string(hKey, g_NSSMRegAffinity, buffer, sizeof(buffer), false,
			false, false) ||
		!buffer[0]) {
		pNSSMService->m_uAffinity = 0LL;
	} else if (affinity_string_to_mask(buffer, &pNSSMService->m_uAffinity)) {
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_BOGUS_AFFINITY_MASK,
			pNSSMService->m_Name, buffer, NULL);
		pNSSMService->m_uAffinity = 0LL;
	} else {
		DWORD_PTR uAffinity;
		DWORD_PTR uSystemAffinity;

		if (GetProcessAffinityMask(
				GetCurrentProcess(), &uAffinity, &uSystemAffinity)) {
			uint64_t effective_affinity =
				pNSSMService->m_uAffinity & uSystemAffinity;
			if (effective_affinity != pNSSMService->m_uAffinity) {
				wchar_t* pSystem = NULL;
				if (!affinity_mask_to_string(uSystemAffinity, &pSystem)) {
					wchar_t* pEffective = NULL;
					if (!affinity_mask_to_string(
							effective_affinity, &pEffective)) {
						log_event(EVENTLOG_WARNING_TYPE,
							NSSM_EVENT_EFFECTIVE_AFFINITY_MASK,
							pNSSMService->m_Name, buffer, pSystem, pEffective,
							NULL);
					}
					heap_free(pEffective);
				}
				heap_free(pSystem);
			}
		}
	}

	// Try to get priority - may fail.
	uint32_t uPriority;
	if (get_number(hKey, g_NSSMRegPriority, &uPriority, false) == 1) {
		if (uPriority == (uPriority & priority_mask())) {
			pNSSMService->m_uPriority = uPriority;
		} else {
			log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_BOGUS_PRIORITY,
				pNSSMService->m_Name, g_NSSMRegPriority, NULL);
		}
	}

	// Try to get hook I/O sharing - may fail.
	uint32_t uHookShareOutputHandles;
	if (get_number(hKey, g_NSSMRegHookShareOutputHandles,
			&uHookShareOutputHandles, false) == 1) {
		if (uHookShareOutputHandles) {
			pNSSMService->m_bHookShareOutputHandles = true;
		} else {
			pNSSMService->m_bHookShareOutputHandles = false;
		}
	} else {
		pNSSMService->m_bHookShareOutputHandles = false;
	}

	// Try to get file rotation settings - may fail.
	uint32_t uRotateFiles;
	if (get_number(hKey, g_NSSMRegRotate, &uRotateFiles, false) == 1) {
		if (uRotateFiles) {
			pNSSMService->m_bRotateFiles = true;
		} else {
			pNSSMService->m_bRotateFiles = false;
		}
	} else {
		pNSSMService->m_bRotateFiles = false;
	}

	if (get_number(hKey, g_NSSMRegRotateOnline, &uRotateFiles, false) == 1) {
		if (uRotateFiles) {
			pNSSMService->m_uRotateStdoutOnline =
				pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_ONLINE;
		} else {
			pNSSMService->m_uRotateStdoutOnline =
				pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_OFFLINE;
		}
	} else {
		pNSSMService->m_uRotateStdoutOnline =
			pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_OFFLINE;
	}

	// Log timestamping requires a logging thread.
	uint32_t uTimestampLog;
	if (get_number(hKey, g_NSSMRegTimeStampLog, &uTimestampLog, false) == 1) {
		if (uTimestampLog) {
			pNSSMService->m_bTimestampLog = true;
		} else {
			pNSSMService->m_bTimestampLog = false;
		}
	} else {
		pNSSMService->m_bTimestampLog = false;
	}

	// Hook I/O sharing and online rotation need a pipe.
	pNSSMService->m_bUseStdoutPipe = pNSSMService->m_uRotateStdoutOnline ||
		pNSSMService->m_bTimestampLog || uHookShareOutputHandles;
	pNSSMService->m_bUseStderrPipe = pNSSMService->m_uRotateStderrOnline ||
		pNSSMService->m_bTimestampLog || uHookShareOutputHandles;

	if (get_number(hKey, g_NSSMRegRotateSeconds,
			&pNSSMService->m_uRotateSeconds, false) != 1)
		pNSSMService->m_uRotateSeconds = 0;

	if (get_number(hKey, g_NSSMRegRotateBytesLow,
			&pNSSMService->m_uRotateBytesLow, false) != 1) {
		pNSSMService->m_uRotateBytesLow = 0;
	}
	if (get_number(hKey, g_NSSMRegRotateBytesHigh,
			&pNSSMService->m_uRotateBytesHigh, false) != 1) {
		pNSSMService->m_uRotateBytesHigh = 0;
	}

	override_milliseconds(pNSSMService->m_Name, hKey, g_NSSMRegRotateDelay,
		&pNSSMService->m_uRotateDelay, NSSM_ROTATE_DELAY,
		NSSM_EVENT_BOGUS_THROTTLE);

	// Try to get force new console setting - may fail.
	if (get_number(hKey, g_NSSMRegNoConsole, &pNSSMService->m_bDontSpawnConsole,
			false) != 1) {
		pNSSMService->m_bDontSpawnConsole = false;
	}

	// Change to startup directory in case stdout/stderr are relative paths.
	wchar_t cwd[PATH_LENGTH];
	GetCurrentDirectoryW(RTL_NUMBER_OF(cwd), cwd);
	SetCurrentDirectoryW(pNSSMService->m_WorkingDirectory);

	// Try to get stdout and stderr
	if (get_io_parameters(pNSSMService, hKey)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_OUTPUT_HANDLES_FAILED,
			pNSSMService->m_Name, NULL);
		RegCloseKey(hKey);
		SetCurrentDirectoryW(cwd);
		return 5;
	}

	// Change back in case the startup directory needs to be deleted.
	SetCurrentDirectoryW(cwd);

	// Try to get mandatory restart delay
	override_milliseconds(pNSSMService->m_Name, hKey, g_NSSMRegRestartDelay,
		&pNSSMService->m_uRestartDelay, 0, NSSM_EVENT_BOGUS_RESTART_DELAY);

	// Try to get throttle restart delay
	override_milliseconds(pNSSMService->m_Name, hKey, g_NSSMRegThrottle,
		&pNSSMService->m_uThrottleDelay, NSSM_RESET_THROTTLE_RESTART,
		NSSM_EVENT_BOGUS_THROTTLE);

	// Try to get service stop flags.
	DWORD uType = REG_DWORD;
	uint32_t uStopMethodSkip;
	DWORD uBufferLength = sizeof(uStopMethodSkip);
	bool bStopOK = false;
	LONG iResult = RegQueryValueExW(hKey, g_NSSMRegStopMethodSkip, 0, &uType,
		reinterpret_cast<BYTE*>(&uStopMethodSkip), &uBufferLength);
	if (iResult != ERROR_SUCCESS) {
		if (iResult != ERROR_FILE_NOT_FOUND) {
			if (uType != REG_DWORD) {
				log_event(EVENTLOG_WARNING_TYPE,
					NSSM_EVENT_BOGUS_STOP_METHOD_SKIP, pNSSMService->m_Name,
					g_NSSMRegStopMethodSkip, g_NSSM, NULL);
			} else {
				log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED,
					g_NSSMRegStopMethodSkip,
					error_string(static_cast<uint32_t>(iResult)), NULL);
			}
		}
	} else {
		bStopOK = true;
	}

	// Try all methods except those requested to be skipped.
	pNSSMService->m_uStopMethodFlags = UINT32_MAX;
	if (bStopOK) {
		pNSSMService->m_uStopMethodFlags &= ~uStopMethodSkip;
	}

	// Try to get kill delays - may fail.
	override_milliseconds(pNSSMService->m_Name, hKey,
		g_NSSMRegKillConsoleGracePeriod, &pNSSMService->m_uKillConsoleDelay,
		NSSM_KILL_CONSOLE_GRACE_PERIOD,
		NSSM_EVENT_BOGUS_KILL_CONSOLE_GRACE_PERIOD);
	override_milliseconds(pNSSMService->m_Name, hKey,
		g_NSSMRegKillWindowGracePeriod, &pNSSMService->m_uKillWindowDelay,
		NSSM_KILL_WINDOW_GRACE_PERIOD,
		NSSM_EVENT_BOGUS_KILL_WINDOW_GRACE_PERIOD);
	override_milliseconds(pNSSMService->m_Name, hKey,
		g_NSSMRegKillThreadsGracePeriod, &pNSSMService->m_uKillThreadsDelay,
		NSSM_KILL_THREADS_GRACE_PERIOD,
		NSSM_EVENT_BOGUS_KILL_THREADS_GRACE_PERIOD);

	// Try to get process tree settings - may fail.
	uint32_t uKillProcessTree;
	if (get_number(hKey, g_NSSMRegKillProcessTree, &uKillProcessTree, false) ==
		1) {
		if (uKillProcessTree) {
			pNSSMService->m_bKillProcessTree = true;
		} else {
			pNSSMService->m_bKillProcessTree = false;
		}
	} else {
		pNSSMService->m_bKillProcessTree = true;
	}

	// Try to get default exit action.
	bool bDefaultAction;
	pNSSMService->m_uDefaultExitAction = NSSM_EXIT_RESTART;
	wchar_t ActionString[ACTION_LEN];
	if (!get_exit_action(
			pNSSMService->m_Name, 0, ActionString, &bDefaultAction)) {
		for (uint32_t i = 0; g_ExitActionStrings[i]; i++) {
			if (!_wcsnicmp(ActionString, g_ExitActionStrings[i], ACTION_LEN)) {
				pNSSMService->m_uDefaultExitAction = i;
				break;
			}
		}
	}

	// Close registry
	RegCloseKey(hKey);

	return 0;
}

/***************************************

	Sets the string for the exit action corresponding to the exit code.

	pExitcode is a pointer to an unsigned long containing the exit code.

	If ret is NULL, we retrieve the default exit action unconditionally.

	pAction is a buffer at least ACTION_LEN in size which receives the string.

	pDefaultAction is a pointer to a bool which is set to false if there
	was an explicit string for the given exit code, or true if we are
	returning the default action.

	Returns:0 on success.
			1 on error.

***************************************/

int get_exit_action(const wchar_t* pServiceName, uint32_t* pExitcode,
	wchar_t* pAction, bool* pDefaultAction)
{
	// Are we returning the default action or a status-specific one?
	*pDefaultAction = !pExitcode;

	// Try to open the registry
	HKEY hKey = open_registry(pServiceName, g_NSSMRegExit, KEY_READ);
	if (!hKey) {
		return 1;
	}

	DWORD uType = REG_SZ;
	DWORD uActionLength = ACTION_LEN;

	wchar_t CodeString[16];
	if (!pExitcode) {
		CodeString[0] = 0;
	} else if (StringCchPrintfW(CodeString, RTL_NUMBER_OF(CodeString), L"%lu",
				   *pExitcode) < 0) {
		RegCloseKey(hKey);
		return get_exit_action(pServiceName, 0, pAction, pDefaultAction);
	}
	if (RegQueryValueExW(hKey, CodeString, 0, &uType, (unsigned char*)pAction,
			&uActionLength) != ERROR_SUCCESS) {
		RegCloseKey(hKey);
		// Try again with * as the key if an exit code was defined
		if (pExitcode) {
			return get_exit_action(pServiceName, 0, pAction, pDefaultAction);
		}
		return 0;
	}

	// Close registry
	RegCloseKey(hKey);

	return 0;
}

int set_hook(const wchar_t* pServiceName, const wchar_t* pHookEvent,
	const wchar_t* pHookAction, const wchar_t* pCommandLine)
{
	// Try to open the registry
	wchar_t RegistryString[KEY_LENGTH];
	if (StringCchPrintfW(RegistryString, RTL_NUMBER_OF(RegistryString),
			L"%s\\%s", g_NSSMRegHook, pHookEvent) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"hook registry", L"set_hook()", NULL);
		return 1;
	}

	HKEY hKey;
	long iError;

	// Don't create keys needlessly.
	if (!wcslen(pCommandLine)) {
		hKey = open_registry(pServiceName, RegistryString, KEY_READ, false);
		if (!hKey) {
			return 0;
		}
		iError = RegQueryValueExW(hKey, pHookAction, 0, 0, 0, 0);
		RegCloseKey(hKey);
		if (iError == ERROR_FILE_NOT_FOUND) {
			return 0;
		}
	}

	hKey = open_registry(pServiceName, RegistryString, KEY_WRITE);
	if (!hKey) {
		return 1;
	}

	int iResult = 1;
	if (wcslen(pCommandLine)) {
		iResult = set_string(hKey, pHookAction, pCommandLine, true);
	} else {
		iError = RegDeleteValueW(hKey, pHookAction);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			iResult = 0;
		}
	}

	// Close registry
	RegCloseKey(hKey);

	return iResult;
}

int get_hook(const wchar_t* pServiceName, const wchar_t* pHookEvent,
	const wchar_t* pHookAction, wchar_t* pBuffer, uint32_t uBufferLength)
{
	// Try to open the registry
	wchar_t RegistryString[KEY_LENGTH];
	if (StringCchPrintfW(RegistryString, RTL_NUMBER_OF(RegistryString),
			L"%s\\%s", g_NSSMRegHook, pHookEvent) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"hook registry", L"get_hook()", NULL);
		return 1;
	}
	HKEY hKey;
	long iError =
		open_registry(pServiceName, RegistryString, KEY_READ, &hKey, false);
	if (!hKey) {
		if (iError == ERROR_FILE_NOT_FOUND) {
			ZeroMemory(pBuffer, uBufferLength);
			return 0;
		}
		return 1;
	}

	int iResult = expand_parameter(
		hKey, pHookAction, pBuffer, uBufferLength, true, false);

	// Close registry
	RegCloseKey(hKey);

	return iResult;
}
