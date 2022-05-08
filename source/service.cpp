/***************************************

	Windows services manager

***************************************/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include "service.h"
#include "account.h"
#include "constants.h"
#include "env.h"
#include "event.h"
#include "gui.h"
#include "hook.h"
#include "imports.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "nssm_io.h"
#include "process.h"
#include "registry.h"
#include "resource.h"
#include "settings.h"

#include <wchar.h>

#include <strsafe.h>

// If compiling against an old Windows SDK...

#ifndef SERVICE_CONFIG_DELAYED_AUTO_START_INFO

//
// Info levels for ChangeServiceConfig2 and QueryServiceConfig2
//
#define SERVICE_CONFIG_DELAYED_AUTO_START_INFO 3
#define SERVICE_CONFIG_FAILURE_ACTIONS_FLAG 4
#define SERVICE_CONFIG_SERVICE_SID_INFO 5
#define SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO 6
#define SERVICE_CONFIG_PRESHUTDOWN_INFO 7
#define SERVICE_CONFIG_TRIGGER_INFO 8
#define SERVICE_CONFIG_PREFERRED_NODE 9

typedef struct _SERVICE_DELAYED_AUTO_START_INFO {
	BOOL fDelayedAutostart; // Delayed autostart flag
} SERVICE_DELAYED_AUTO_START_INFO, *LPSERVICE_DELAYED_AUTO_START_INFO;

typedef struct _SERVICE_FAILURE_ACTIONS_FLAG {
	BOOL fFailureActionsOnNonCrashFailures; // Failure actions flag
} SERVICE_FAILURE_ACTIONS_FLAG, *LPSERVICE_FAILURE_ACTIONS_FLAG;

#endif

struct list_t {
	// First entry
	int m_iFirst;
	// Last entry
	int m_iLast;
};

// Set to true if critical section is initialized
static bool g_bUseCriticalSection;

// All the threads being managed
static hook_thread_t g_HookThreads = {NULL, 0};

/*
  Check the status in response to a control.
  Returns:  1 if the status is expected, eg STOP following CONTROL_STOP.
			0 if the status is desired, eg STOPPED following CONTROL_STOP.
		   -1 if the status is undesired, eg STOPPED following CONTROL_START.
*/
static inline int service_control_response(uint32_t uControl, uint32_t uStatus)
{
	switch (uControl) {
	case NSSM_SERVICE_CONTROL_START:
		switch (uStatus) {
		case SERVICE_START_PENDING:
			return 1;

		case SERVICE_RUNNING:
			return 0;

		default:
			return -1;
		}

	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		switch (uStatus) {
		case SERVICE_RUNNING:
		case SERVICE_STOP_PENDING:
			return 1;

		case SERVICE_STOPPED:
			return 0;

		default:
			return -1;
		}

	case SERVICE_CONTROL_PAUSE:
		switch (uStatus) {
		case SERVICE_PAUSE_PENDING:
			return 1;

		case SERVICE_PAUSED:
			return 0;

		default:
			return -1;
		}

	case SERVICE_CONTROL_CONTINUE:
		switch (uStatus) {
		case SERVICE_CONTINUE_PENDING:
			return 1;

		case SERVICE_RUNNING:
			return 0;

		default:
			return -1;
		}

	case SERVICE_CONTROL_INTERROGATE:
	case NSSM_SERVICE_CONTROL_ROTATE:
		return 0;
	}

	return 0;
}

static inline int await_service_control_response(uint32_t uControl,
	SC_HANDLE hService, SERVICE_STATUS* pServiceStatus, uint32_t uInitialStatus,
	uint32_t uCutoff)
{
	DWORD uTries = 0;
	DWORD uCheckpoint = 0;
	DWORD uWaitHint = 0;
	DWORD uTotalWaited = 0;
	while (QueryServiceStatus(hService, pServiceStatus)) {
		int iResponse =
			service_control_response(uControl, pServiceStatus->dwCurrentState);
		/* Alas we can't WaitForSingleObject() on an SC_HANDLE. */
		if (!iResponse) {
			return iResponse;
		}
		if (iResponse > 0 ||
			(pServiceStatus->dwCurrentState == uInitialStatus)) {
			if (pServiceStatus->dwCheckPoint != uCheckpoint ||
				pServiceStatus->dwWaitHint != uWaitHint) {
				uTries = 0;
			}
			uCheckpoint = pServiceStatus->dwCheckPoint;
			uWaitHint = pServiceStatus->dwWaitHint;
			if (++uTries > 10U) {
				uTries = 10U;
			}
			DWORD uWait = 50U * uTries;
			// Is there a timeout?
			if (uCutoff) {
				if (uTotalWaited > uCutoff) {
					return iResponse;
				}
				uTotalWaited += uWait;
			}
			Sleep(uWait);
		} else {
			return iResponse;
		}
	}
	return -1;
}

static inline int await_service_control_response(uint32_t uControl,
	SC_HANDLE hService, SERVICE_STATUS* pServiceStatus, uint32_t uInitialStatus)
{
	return await_service_control_response(
		uControl, hService, pServiceStatus, uInitialStatus, 0);
}

static inline void wait_for_hooks(nssm_service_t* pNSSMService, bool bNotify)
{
	SERVICE_STATUS_HANDLE hStatus;
	SERVICE_STATUS* pServiceStatus;

	// On a clean shutdown we need to keep the service's status up-to-date.
	if (bNotify) {
		hStatus = pNSSMService->m_hStatusHandle;
		pServiceStatus = &pNSSMService->m_ServiceStatus;
	} else {
		hStatus = NULL;
		pServiceStatus = NULL;
	}

	EnterCriticalSection(&pNSSMService->m_HookLock);
	await_hook_threads(
		&g_HookThreads, hStatus, pServiceStatus, NSSM_HOOK_THREAD_DEADLINE);
	LeaveCriticalSection(&pNSSMService->m_HookLock);
}

int affinity_mask_to_string(uint64_t uMask, wchar_t** ppString)
{
	if (!ppString) {
		return 1;
	}
	if (!uMask) {
		*ppString = NULL;
		return 0;
	}

	uint64_t i;
	uint64_t n;

	/* SetProcessAffinityMask() accepts a mask of up to 64 processors. */
	list_t set[64];
	for (n = 0; n < RTL_NUMBER_OF(set); n++) {
		set[n].m_iFirst = set[n].m_iLast = -1;
	}

	for (i = 0, n = 0; i < RTL_NUMBER_OF(set); i++) {
		if (uMask & (1LL << i)) {
			if (set[n].m_iFirst == -1) {
				set[n].m_iFirst = set[n].m_iLast = static_cast<int>(i);
			} else if (set[n].m_iLast == static_cast<int>(i) - 1) {
				set[n].m_iLast = static_cast<int>(i);
			} else {
				++n;
				set[n].m_iFirst = set[n].m_iLast = static_cast<int>(i);
			}
		}
	}

	/* Worst case is 2x2 characters for first and last CPU plus - and/or , */
	uintptr_t uLength = static_cast<uintptr_t>(n + 1U) * 6U;
	*ppString = static_cast<wchar_t*>(heap_calloc(uLength * sizeof(wchar_t)));
	if (!ppString) {
		return 2;
	}

	uintptr_t s = 0;
	for (i = 0; i <= n; i++) {
		if (i) {
			(*ppString)[s++] = ',';
		}
		HRESULT iResult =
			StringCchPrintfW(*ppString + s, 3, L"%u", set[i].m_iFirst);
		if (iResult < 0) {
			heap_free(*ppString);
			*ppString = NULL;
			return 3;
		} else {
			s += wcslen(*ppString + s);
		}
		if (set[i].m_iLast != set[i].m_iFirst) {
			iResult = StringCchPrintfW(*ppString + s, 4, L"%c%u",
				(set[i].m_iLast == set[i].m_iFirst + 1) ? L',' : L'-',
				set[i].m_iLast);
			if (iResult < 0) {
				heap_free(*ppString);
				*ppString = NULL;
				return 4;
			} else {
				s += wcslen(*ppString + s);
			}
		}
	}

	return 0;
}

int affinity_string_to_mask(const wchar_t* pString, uint64_t* pMask)
{
	if (!pMask) {
		return 1;
	}

	*pMask = 0LL;
	if (!pString) {
		return 0;
	}

	list_t set[64];

	const wchar_t* s = pString;

	int i;
	int n = 0;
	uint32_t uNumber;

	for (n = 0; n < RTL_NUMBER_OF(set); n++) {
		set[n].m_iFirst = set[n].m_iLast = -1;
	}
	n = 0;

	wchar_t* pEnd;
	while (*s) {
		int ret = str_number(s, &uNumber, &pEnd);
		s = pEnd;
		if ((ret == 0) || (ret == 2)) {
			if (uNumber >= RTL_NUMBER_OF(set)) {
				return 2;
			}
			set[n].m_iFirst = set[n].m_iLast = static_cast<int>(uNumber);

			switch (*s) {
			case 0:
				break;

			case L',':
				n++;
				s++;
				break;

			case L'-':
				if (!*(++s)) {
					return 3;
				}
				ret = str_number(s, &uNumber, &pEnd);
				if ((ret == 0) || (ret == 2)) {
					s = pEnd;
					if (!*s || *s == L',') {
						set[n].m_iLast = static_cast<int>(uNumber);
						if (!*s) {
							break;
						}
						n++;
						s++;
					} else {
						return 3;
					}
				} else {
					return 3;
				}
				break;

			default:
				return 3;
			}
		} else {
			return 4;
		}
	}

	for (i = 0; i <= n; i++) {
		for (int j = set[i].m_iFirst; j <= set[i].m_iLast; j++) {
			(uint64_t)* pMask |= (1LL << (uint64_t)j);
		}
	}

	return 0;
}

uint32_t priority_mask(void)
{
	return REALTIME_PRIORITY_CLASS | HIGH_PRIORITY_CLASS |
		ABOVE_NORMAL_PRIORITY_CLASS | NORMAL_PRIORITY_CLASS |
		BELOW_NORMAL_PRIORITY_CLASS | IDLE_PRIORITY_CLASS;
}

uint32_t priority_constant_to_index(uint32_t uConstant)
{
	switch (uConstant & priority_mask()) {
	case REALTIME_PRIORITY_CLASS:
		return NSSM_REALTIME_PRIORITY;
	case HIGH_PRIORITY_CLASS:
		return NSSM_HIGH_PRIORITY;
	case ABOVE_NORMAL_PRIORITY_CLASS:
		return NSSM_ABOVE_NORMAL_PRIORITY;
	case BELOW_NORMAL_PRIORITY_CLASS:
		return NSSM_BELOW_NORMAL_PRIORITY;
	case IDLE_PRIORITY_CLASS:
		return NSSM_IDLE_PRIORITY;
	}
	return NSSM_NORMAL_PRIORITY;
}

uint32_t priority_index_to_constant(uint32_t uIndex)
{
	switch (uIndex) {
	case NSSM_REALTIME_PRIORITY:
		return REALTIME_PRIORITY_CLASS;
	case NSSM_HIGH_PRIORITY:
		return HIGH_PRIORITY_CLASS;
	case NSSM_ABOVE_NORMAL_PRIORITY:
		return ABOVE_NORMAL_PRIORITY_CLASS;
	case NSSM_BELOW_NORMAL_PRIORITY:
		return BELOW_NORMAL_PRIORITY_CLASS;
	case NSSM_IDLE_PRIORITY:
		return IDLE_PRIORITY_CLASS;
	}
	return NORMAL_PRIORITY_CLASS;
}

static inline uint32_t throttle_milliseconds(uint32_t uThrottle)
{
	if (uThrottle > 7) {
		uThrottle = 8;
	}
	/* pow() operates on doubles. */
	uint32_t uResult = 1;
	for (uint32_t i = 1; i < uThrottle; i++) {
		uResult *= 2;
	}
	return uResult * 1000U;
}

void set_service_environment(nssm_service_t* pNSSMService)
{
	if (pNSSMService) {
		/*
		  We have to duplicate the block because this function will be called
		  multiple times between registry reads.
		*/
		if (pNSSMService->m_pEnvironmentVariables) {
			duplicate_environment_strings(
				pNSSMService->m_pEnvironmentVariables);
		}
		if (!pNSSMService->m_pExtraEnvironmentVariables) {
			return;
		}
		wchar_t* env_extra =
			copy_environment_block(pNSSMService->m_pExtraEnvironmentVariables);
		if (env_extra) {
			set_environment_block(env_extra);
			heap_free(env_extra);
		}
	}
}

/***************************************

	Restore the environment variables back to the original defaults

***************************************/

void unset_service_environment(nssm_service_t* pNSSMService)
{
	if (pNSSMService) {
		duplicate_environment_strings(
			pNSSMService->m_pInitialEnvironmentVariables);
	}
}

/*
  Wrapper to be called in a new thread so that we can acknowledge a STOP
  control immediately.
*/
static DWORD WINAPI shutdown_service(void* pInput)
{
	return stop_service(static_cast<nssm_service_t*>(pInput), 0, true, true);
}

/*
 Wrapper to be called in a new thread so that we can acknowledge start
 immediately.
*/
static DWORD WINAPI launch_service(void* pInput)
{
	return monitor_service(static_cast<nssm_service_t*>(pInput));
}

/* Connect to the service manager */
SC_HANDLE open_service_manager(uint32_t uAccess)
{
	SC_HANDLE hResult = OpenSCManagerW(0, SERVICES_ACTIVE_DATABASEW, uAccess);
	if (!hResult) {
		if (g_bIsAdmin) {
			log_event(
				EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENSCMANAGER_FAILED, NULL);
		}
		return 0;
	}

	return hResult;
}

/* Open a service by name or display name. */
SC_HANDLE open_service(SC_HANDLE hService, const wchar_t* pServiceName,
	uint32_t uAccess, wchar_t* pCanonicalName, uint32_t uCanonicalLength)
{
	SC_HANDLE hNewService = OpenServiceW(hService, pServiceName, uAccess);
	if (hNewService) {
		if (pCanonicalName && pCanonicalName != pServiceName) {
			wchar_t TempDisplayname[SERVICE_NAME_LENGTH];
			DWORD uDisplaynameLength = RTL_NUMBER_OF(TempDisplayname);
			GetServiceDisplayNameW(
				hService, pServiceName, TempDisplayname, &uDisplaynameLength);
			DWORD uKeynameLength = uCanonicalLength;
			GetServiceKeyNameW(
				hService, TempDisplayname, pCanonicalName, &uKeynameLength);
		}
		return hNewService;
	}

	DWORD uError = GetLastError();
	if (uError != ERROR_SERVICE_DOES_NOT_EXIST) {
		print_message(
			stderr, NSSM_MESSAGE_OPENSERVICE_FAILED, error_string(uError));
		return 0;
	}

	/* We can't look for a display name because there's no buffer to store it.
	 */
	if (!pCanonicalName) {
		print_message(stderr, NSSM_MESSAGE_OPENSERVICE_FAILED,
			error_string(GetLastError()));
		return 0;
	}

	DWORD required;
	DWORD count;
	DWORD resume = 0;
	EnumServicesStatusExW(hService, SC_ENUM_PROCESS_INFO,
		SERVICE_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_KERNEL_DRIVER |
			SERVICE_WIN32,
		SERVICE_STATE_ALL, 0, 0, &required, &count, &resume, 0);
	uError = GetLastError();
	if (uError != ERROR_MORE_DATA) {
		print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
			error_string(uError));
		return 0;
	}

	ENUM_SERVICE_STATUS_PROCESSW* pServiceStatus =
		static_cast<ENUM_SERVICE_STATUS_PROCESSW*>(heap_alloc(required));
	if (!pServiceStatus) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
			L"ENUM_SERVICE_STATUS_PROCESS", L"open_service()");
		return 0;
	}

	uint32_t bufsize = required;
	for (;;) {
		/*
		  EnumServicesStatusEx() returns:
		  1 when it retrieved data and there's no more data to come.
		  0 and sets last error to ERROR_MORE_DATA when it retrieved data and
			there's more data to come.
		  0 and sets last error to something else on error.
		*/
		int ret = EnumServicesStatusExW(hService, SC_ENUM_PROCESS_INFO,
			SERVICE_DRIVER | SERVICE_FILE_SYSTEM_DRIVER |
				SERVICE_KERNEL_DRIVER | SERVICE_WIN32,
			SERVICE_STATE_ALL, (LPBYTE)pServiceStatus, bufsize, &required,
			&count, &resume, 0);
		if (!ret) {
			uError = GetLastError();
			if (uError != ERROR_MORE_DATA) {
				heap_free(pServiceStatus);
				print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
					error_string(GetLastError()));
				return 0;
			}
		}

		uint32_t i;
		for (i = 0; i < count; i++) {
			if (str_equiv(pServiceStatus[i].lpDisplayName, pServiceName)) {
				if (StringCchPrintfW(pCanonicalName, uCanonicalLength, L"%s",
						pServiceStatus[i].lpServiceName) < 0) {
					heap_free(pServiceStatus);
					print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
						L"canonical_name", L"open_service()");
					return 0;
				}

				heap_free(pServiceStatus);
				return open_service(hService, pCanonicalName, uAccess, 0, 0);
			}
		}

		if (ret) {
			break;
		}
	}

	// Recurse so we can get an error message.
	return open_service(hService, pServiceName, uAccess, 0, 0);
}

QUERY_SERVICE_CONFIGW* query_service_config(
	const wchar_t* pServiceName, SC_HANDLE hService)
{
	DWORD bufsize;

	QueryServiceConfigW(hService, 0, 0, &bufsize);
	DWORD uError = GetLastError();
	QUERY_SERVICE_CONFIGW* qsc;
	if (uError == ERROR_INSUFFICIENT_BUFFER) {
		qsc = static_cast<QUERY_SERVICE_CONFIGW*>(heap_calloc(bufsize));
		if (!qsc) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
				L"QUERY_SERVICE_CONFIG", L"query_service_config()", NULL);
			return 0;
		}
	} else {
		print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG_FAILED,
			pServiceName, error_string(uError), NULL);
		return 0;
	}

	if (!QueryServiceConfigW(hService, qsc, bufsize, &bufsize)) {
		heap_free(qsc);
		print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG_FAILED,
			pServiceName, error_string(GetLastError()), NULL);
		return 0;
	}

	return qsc;
}

/* WILL NOT allocate a new string if the identifier is already present. */
static int prepend_service_group_identifier(wchar_t* pGroup, wchar_t** ppCanon)
{
	if (!pGroup || !pGroup[0] || (pGroup[0] == SC_GROUP_IDENTIFIERW)) {
		*ppCanon = pGroup;
		return 0;
	}

	uintptr_t len = wcslen(pGroup) + 1;
	*ppCanon = static_cast<wchar_t*>(heap_alloc((len + 1) * sizeof(wchar_t)));
	if (!*ppCanon) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"canon",
			L"prepend_service_group_identifier()");
		return 1;
	}

	wchar_t* s = *ppCanon;
	*s++ = SC_GROUP_IDENTIFIERW;
	memmove(s, pGroup, len * sizeof(wchar_t));
	(*ppCanon)[len] = 0;

	return 0;
}

int append_to_dependencies(wchar_t* pDependencies,
	uintptr_t uDependenciesLength, wchar_t* pString,
	wchar_t** ppNewDependencies, uintptr_t* pNewLength, uint32_t uType)
{
	*pNewLength = 0;

	wchar_t* pCanon = NULL;
	if (uType == DEPENDENCY_GROUPS) {
		if (prepend_service_group_identifier(pString, &pCanon)) {
			return 1;
		}
	} else {
		pCanon = pString;
	}
	int iResult = append_to_double_null(pDependencies, uDependenciesLength,
		ppNewDependencies, pNewLength, pCanon, 0, false);
	if (pCanon && (pCanon != pString)) {
		heap_free(pCanon);
	}

	return iResult;
}

int remove_from_dependencies(wchar_t* pDependencies,
	uintptr_t uDependenciesLength, wchar_t* pString,
	wchar_t** ppNewDependencies, uintptr_t* pNewLength, uint32_t uType)
{
	*pNewLength = 0;

	wchar_t* pCanon = 0;
	if (uType == DEPENDENCY_GROUPS) {
		if (prepend_service_group_identifier(pString, &pCanon)) {
			return 1;
		}
	} else {
		pCanon = pString;
	}
	int iResult = remove_from_double_null(pDependencies, uDependenciesLength,
		ppNewDependencies, pNewLength, pCanon, 0, false);
	if (pCanon && (pCanon != pString)) {
		heap_free(pCanon);
	}
	return iResult;
}

int set_service_dependencies(
	const wchar_t* /* pServiceName */, SC_HANDLE hService, wchar_t* pBuffer)
{
	wchar_t* pDependencies = L"";
	uint32_t uNumDependencies = 0;

	if (pBuffer && pBuffer[0]) {
		SC_HANDLE hOpenService = open_service_manager(
			SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
		if (!hOpenService) {
			print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
			return 1;
		}

		/*
		  Count the dependencies then allocate a buffer big enough for their
		  canonical names, ie n * SERVICE_NAME_LENGTH.
		*/
		wchar_t* s;
		wchar_t* pGroups = 0;
		for (s = pBuffer; *s; s++) {
			uNumDependencies++;
			if (*s == SC_GROUP_IDENTIFIERW) {
				pGroups = s;
			}
			while (*s) {
				s++;
			}
		}

		/* At least one dependency is a group so we need to verify them. */
		if (pGroups) {
			HKEY hKey;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, g_NSSMRegistryGroups, 0,
					KEY_READ, &hKey)) {
				fwprintf(stderr, L"%s: %s\n", g_NSSMRegistryGroups,
					error_string(GetLastError()));
				return 2;
			}

			DWORD uType;
			DWORD uGroupsLen;
			LONG ret = RegQueryValueExW(
				hKey, g_NSSMRegGroups, 0, &uType, NULL, &uGroupsLen);
			if (ret == ERROR_SUCCESS) {
				pGroups = static_cast<wchar_t*>(heap_alloc(uGroupsLen));
				if (!pGroups) {
					print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"groups",
						L"set_service_dependencies()");
					return 3;
				}

				ret = RegQueryValueExW(hKey, g_NSSMRegGroups, 0, &uType,
					reinterpret_cast<BYTE*>(pGroups), &uGroupsLen);
				if (ret != ERROR_SUCCESS) {
					fwprintf(stderr, L"%s\\%s: %s", g_NSSMRegistryGroups,
						g_NSSMRegGroups, error_string(GetLastError()));
					heap_free(pGroups);
					RegCloseKey(hKey);
					return 4;
				}
			} else if (ret != ERROR_FILE_NOT_FOUND) {
				fwprintf(stderr, L"%s\\%s: %s", g_NSSMRegistryGroups,
					g_NSSMRegGroups, error_string(GetLastError()));
				RegCloseKey(hKey);
				return 4;
			}

			RegCloseKey(hKey);
		}

		uint32_t uDependenciesLen =
			(uNumDependencies * SERVICE_NAME_LENGTH) + 2;
		pDependencies = static_cast<wchar_t*>(
			heap_calloc(uDependenciesLen * sizeof(wchar_t)));
		uintptr_t i = 0;

		wchar_t TempDependency[SERVICE_NAME_LENGTH];
		for (s = pBuffer; *s; s++) {
			/* Group? */
			if (*s == SC_GROUP_IDENTIFIER) {
				wchar_t* group = s + 1;

				bool ok = false;
				if (*group) {
					for (wchar_t* g = pGroups; *g; g++) {
						if (str_equiv(g, group)) {
							ok = true;
							/* Set canonical name. */
							memmove(group, g, wcslen(g) * sizeof(wchar_t));
							break;
						}

						while (*g) {
							g++;
						}
					}
				}

				if (ok) {
					StringCchPrintfW(TempDependency,
						RTL_NUMBER_OF(TempDependency), L"%s", s);
				} else {
					heap_free(pDependencies);
					if (pGroups) {
						heap_free(pGroups);
					}
					fwprintf(stderr, L"%s: %s", s,
						error_string(ERROR_SERVICE_DEPENDENCY_DELETED));
					return 5;
				}
			} else {
				SC_HANDLE hDependency =
					open_service(hOpenService, s, SERVICE_QUERY_STATUS,
						TempDependency, RTL_NUMBER_OF(TempDependency));
				if (!hDependency) {
					heap_free(pDependencies);
					if (pGroups) {
						heap_free(pGroups);
					}
					CloseServiceHandle(hOpenService);
					fwprintf(stderr, L"%s: %s", s,
						error_string(ERROR_SERVICE_DEPENDENCY_DELETED));
					return 5;
				}
			}

			uintptr_t len = wcslen(TempDependency) + 1;
			memmove(pDependencies + i, TempDependency, len * sizeof(wchar_t));
			i += len;

			while (*s) {
				s++;
			}
		}

		if (pGroups) {
			heap_free(pGroups);
		}
		CloseServiceHandle(hOpenService);
	}

	if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
			SERVICE_NO_CHANGE, 0, 0, 0, pDependencies, 0, 0, 0)) {
		if (uNumDependencies) {
			heap_free(pDependencies);
		}
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}

	if (uNumDependencies) {
		heap_free(pDependencies);
	}
	return 0;
}

int get_service_dependencies(const wchar_t* pServiceName, SC_HANDLE hService,
	wchar_t** ppBuffer, uintptr_t* pBufferSize, uint32_t uType)
{
	if (!ppBuffer) {
		return 1;
	}
	if (!pBufferSize) {
		return 2;
	}

	*ppBuffer = NULL;
	*pBufferSize = 0;

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	if (!pQueryServiceConfig) {
		return 3;
	}

	if (!pQueryServiceConfig->lpDependencies ||
		!pQueryServiceConfig->lpDependencies[0]) {
		heap_free(pQueryServiceConfig);
		return 0;
	}

	/* lpDependencies is doubly NULL terminated. */
	while (pQueryServiceConfig->lpDependencies[*pBufferSize]) {
		while (pQueryServiceConfig->lpDependencies[*pBufferSize]) {
			++*pBufferSize;
		}
		++*pBufferSize;
	}

	*pBufferSize += 2;

	*ppBuffer =
		static_cast<wchar_t*>(heap_calloc(*pBufferSize * sizeof(wchar_t)));
	if (!*ppBuffer) {
		*pBufferSize = 0;
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"lpDependencies",
			L"get_service_dependencies()");
		heap_free(pQueryServiceConfig);
		return 4;
	}

	if (uType == DEPENDENCY_ALL) {
		memmove(*ppBuffer, pQueryServiceConfig->lpDependencies,
			*pBufferSize * sizeof(wchar_t));
	} else {
		wchar_t* s;
		uintptr_t i = 0;
		*pBufferSize = 0;
		for (s = pQueryServiceConfig->lpDependencies; *s; s++) {
			/* Only copy the appropriate type of dependency. */
			if ((*s == SC_GROUP_IDENTIFIER && (uType & DEPENDENCY_GROUPS)) ||
				(*s != SC_GROUP_IDENTIFIER && (uType & DEPENDENCY_SERVICES))) {
				uintptr_t uStrLength = wcslen(s) + 1;
				*pBufferSize += uStrLength;
				memmove(*ppBuffer + i, s, uStrLength * sizeof(wchar_t));
				i += uStrLength;
			}

			while (*s) {
				s++;
			}
		}
		++*pBufferSize;
	}

	heap_free(pQueryServiceConfig);

	if (!*ppBuffer[0]) {
		heap_free(*ppBuffer);
		*ppBuffer = NULL;
		*pBufferSize = 0;
	}

	return 0;
}

int get_service_dependencies(const wchar_t* pServiceName, SC_HANDLE hService,
	wchar_t** ppBuffer, uintptr_t* pBufferSize)
{
	return get_service_dependencies(
		pServiceName, hService, ppBuffer, pBufferSize, DEPENDENCY_ALL);
}

int set_service_description(
	const wchar_t* pServiceName, SC_HANDLE hService, wchar_t* pBuffer)
{
	SERVICE_DESCRIPTIONW ServiceDescription;
	ZeroMemory(&ServiceDescription, sizeof(ServiceDescription));
	/*
	  lpDescription must be NULL if we aren't changing, the new description
	  or "".
	*/
	if (pBuffer && pBuffer[0]) {
		ServiceDescription.lpDescription = pBuffer;
	} else {
		ServiceDescription.lpDescription = L"";
	}

	if (ChangeServiceConfig2W(
			hService, SERVICE_CONFIG_DESCRIPTION, &ServiceDescription)) {
		return 0;
	}
	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SERVICE_CONFIG_DESCRIPTION_FAILED,
		pServiceName, error_string(GetLastError()), NULL);
	return 1;
}

int get_service_description(const wchar_t* pServiceName, SC_HANDLE hService,
	uint32_t uBufferLength, wchar_t* pBuffer)
{
	if (!pBuffer) {
		return 1;
	}

	DWORD uBufsize;
	QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, 0, 0, &uBufsize);
	DWORD uError = GetLastError();
	if (uError == ERROR_INSUFFICIENT_BUFFER) {
		SERVICE_DESCRIPTIONW* pDescription =
			static_cast<SERVICE_DESCRIPTIONW*>(heap_alloc(uBufsize));
		if (!pDescription) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
				L"SERVICE_CONFIG_DESCRIPTION", L"get_service_description()");
			return 2;
		}

		if (QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION,
				reinterpret_cast<BYTE*>(pDescription), uBufsize, &uBufsize)) {
			if (pDescription->lpDescription) {
				StringCchPrintfW(
					pBuffer, uBufferLength, L"%s", pDescription->lpDescription);
			} else {
				ZeroMemory(pBuffer, uBufferLength * sizeof(wchar_t));
			}
			heap_free(pDescription);
			return 0;
		} else {
			heap_free(pDescription);
			print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
				pServiceName, L"SERVICE_CONFIG_DESCRIPTION",
				error_string(uError));
			return 3;
		}
	} else {
		print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
			pServiceName, L"SERVICE_CONFIG_DESCRIPTION", error_string(uError));
		return 4;
	}
}

int get_service_startup(const wchar_t* pServiceName, SC_HANDLE hService,
	const QUERY_SERVICE_CONFIGW* pQueryServiceConfig, uint32_t* pStartup)
{
	if (!pQueryServiceConfig) {
		return 1;
	}

	switch (pQueryServiceConfig->dwStartType) {
	case SERVICE_DEMAND_START:
		*pStartup = NSSM_STARTUP_MANUAL;
		break;
	case SERVICE_DISABLED:
		*pStartup = NSSM_STARTUP_DISABLED;
		break;
	default:
		*pStartup = NSSM_STARTUP_AUTOMATIC;
		break;
	}

	if (*pStartup != NSSM_STARTUP_AUTOMATIC) {
		return 0;
	}

	/* Check for delayed start. */
	DWORD uBufferSize;
	QueryServiceConfig2W(
		hService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, 0, 0, &uBufferSize);
	DWORD uError = GetLastError();
	if (uError == ERROR_INSUFFICIENT_BUFFER) {
		SERVICE_DELAYED_AUTO_START_INFO* pInfo =
			static_cast<SERVICE_DELAYED_AUTO_START_INFO*>(
				heap_alloc(uBufferSize));
		if (!pInfo) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
				L"SERVICE_DELAYED_AUTO_START_INFO", L"get_service_startup()");
			return 2;
		}

		if (QueryServiceConfig2W(hService,
				SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
				reinterpret_cast<BYTE*>(pInfo), uBufferSize, &uBufferSize)) {
			if (pInfo->fDelayedAutostart) {
				*pStartup = NSSM_STARTUP_DELAYED;
			}
			heap_free(pInfo);
			return 0;
		} else {
			uError = GetLastError();
			if (uError != ERROR_INVALID_LEVEL) {
				print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
					pServiceName, L"SERVICE_CONFIG_DELAYED_AUTO_START_INFO",
					error_string(uError));
				return 3;
			}
		}
	} else if (uError != ERROR_INVALID_LEVEL) {
		print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
			pServiceName, L"SERVICE_DELAYED_AUTO_START_INFO",
			error_string(uError));
		return 3;
	}

	return 0;
}

int get_service_username(const wchar_t* /* pServiceName */,
	const QUERY_SERVICE_CONFIGW* pQueryServiceConfig, wchar_t** ppUsername,
	uintptr_t* pUsernameLength)
{
	if (!ppUsername) {
		return 1;
	}
	if (!pUsernameLength) {
		return 1;
	}

	*ppUsername = NULL;
	*pUsernameLength = 0;

	if (!pQueryServiceConfig) {
		return 1;
	}

	if (pQueryServiceConfig->lpServiceStartName[0]) {
		if (is_localsystem(pQueryServiceConfig->lpServiceStartName)) {
			return 0;
		}

		uintptr_t uTempLength = wcslen(pQueryServiceConfig->lpServiceStartName);
		*ppUsername = static_cast<wchar_t*>(
			heap_alloc((uTempLength + 1) * sizeof(wchar_t)));
		if (!*ppUsername) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"username",
				L"get_service_username()");
			return 2;
		}

		memmove(*ppUsername, pQueryServiceConfig->lpServiceStartName,
			(uTempLength + 1) * sizeof(wchar_t));
		*pUsernameLength = uTempLength;
	}

	return 0;
}

/* Set default values which aren't zero. */
void set_nssm_service_defaults(nssm_service_t* pNSSMService)
{
	if (pNSSMService) {
		pNSSMService->m_uServiceTypes = SERVICE_WIN32_OWN_PROCESS;
		pNSSMService->m_uPriority = NORMAL_PRIORITY_CLASS;
		pNSSMService->m_uStdinSharing = NSSM_STDIN_SHARING;
		pNSSMService->m_uStdinDisposition = NSSM_STDIN_DISPOSITION;
		pNSSMService->m_uStdinFlags = NSSM_STDIN_FLAGS;
		pNSSMService->m_uStdoutSharing = NSSM_STDOUT_SHARING;
		pNSSMService->m_uStdoutDisposition = NSSM_STDOUT_DISPOSITION;
		pNSSMService->m_uStdoutFlags = NSSM_STDOUT_FLAGS;
		pNSSMService->m_uStderrSharing = NSSM_STDERR_SHARING;
		pNSSMService->m_uStderrDisposition = NSSM_STDERR_DISPOSITION;
		pNSSMService->m_uStderrFlags = NSSM_STDERR_FLAGS;
		pNSSMService->m_uThrottleDelay = NSSM_RESET_THROTTLE_RESTART;
		pNSSMService->m_uStopMethodFlags = UINT32_MAX;
		pNSSMService->m_uKillConsoleDelay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
		pNSSMService->m_uKillWindowDelay = NSSM_KILL_WINDOW_GRACE_PERIOD;
		pNSSMService->m_uKillThreadsDelay = NSSM_KILL_THREADS_GRACE_PERIOD;
		pNSSMService->m_bKillProcessTree = true;
	}
}

/* Allocate and zero memory for a service. */
nssm_service_t* alloc_nssm_service(void)
{
	nssm_service_t* pNSSMService =
		static_cast<nssm_service_t*>(heap_calloc(sizeof(nssm_service_t)));
	if (!pNSSMService) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"service",
			L"alloc_nssm_service()", NULL);
	}
	return pNSSMService;
}

/* Free memory for a service. */
void cleanup_nssm_service(nssm_service_t* pNSSMService)
{
	if (pNSSMService) {

		if (pNSSMService->m_pUsername) {
			heap_free(pNSSMService->m_pUsername);
		}
		if (pNSSMService->m_pPassword) {
			RtlSecureZeroMemory(pNSSMService->m_pPassword,
				pNSSMService->m_uPasswordLength * sizeof(wchar_t));
			heap_free(pNSSMService->m_pPassword);
		}
		if (pNSSMService->m_pDependencies) {
			heap_free(pNSSMService->m_pDependencies);
		}
		if (pNSSMService->m_pEnvironmentVariables) {
			heap_free(pNSSMService->m_pEnvironmentVariables);
		}
		if (pNSSMService->m_pExtraEnvironmentVariables) {
			heap_free(pNSSMService->m_pExtraEnvironmentVariables);
		}
		if (pNSSMService->m_hServiceControlManager) {
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
		}
		if (pNSSMService->m_hProcess) {
			CloseHandle(pNSSMService->m_hProcess);
		}
		if (pNSSMService->m_hWait) {
			UnregisterWait(pNSSMService->m_hWait);
		}
		if (pNSSMService->m_bThrottleSectionValid) {
			DeleteCriticalSection(&pNSSMService->m_ThrottleSection);
		}
		if (pNSSMService->m_hThrottleTimer) {
			CloseHandle(pNSSMService->m_hThrottleTimer);
		}
		if (pNSSMService->m_bHookLockValid) {
			DeleteCriticalSection(&pNSSMService->m_HookLock);
		}
		if (pNSSMService->m_pInitialEnvironmentVariables) {
			heap_free(pNSSMService->m_pInitialEnvironmentVariables);
		}
		heap_free(pNSSMService);
	}
}

/* About to install the service */
int pre_install_service(int iArgc, wchar_t** ppArgv)
{
	nssm_service_t* pNSSMService = alloc_nssm_service();
	set_nssm_service_defaults(pNSSMService);
	if (iArgc) {
		StringCchPrintfW(pNSSMService->m_Name,
			RTL_NUMBER_OF(pNSSMService->m_Name), L"%s", ppArgv[0]);
	}
	/* Show the dialogue box if we didn't give the service name and path */
	if (iArgc < 2) {
		return nssm_gui(IDD_INSTALL, pNSSMService);
	}

	if (!pNSSMService) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"service",
			L"pre_install_service()");
		return 1;
	}
	StringCchPrintfW(pNSSMService->m_ExecutablePath,
		RTL_NUMBER_OF(pNSSMService->m_ExecutablePath), L"%s", ppArgv[1]);

	/* Arguments are optional */
	uintptr_t flagslen = 0;
	uintptr_t s = 0;
	int i;
	for (i = 2; i < iArgc; i++) {
		flagslen += wcslen(ppArgv[i]) + 1;
	}
	if (!flagslen) {
		flagslen = 1;
	}
	if (flagslen > RTL_NUMBER_OF(pNSSMService->m_AppParameters)) {
		print_message(stderr, NSSM_MESSAGE_FLAGS_TOO_LONG);
		return 2;
	}

	for (i = 2; i < iArgc; i++) {
		size_t len = wcslen(ppArgv[i]);
		memmove(pNSSMService->m_AppParameters + s, ppArgv[i],
			len * sizeof(wchar_t));
		s += len;
		if (i < (iArgc - 1)) {
			pNSSMService->m_AppParameters[s++] = L' ';
		}
	}

	/* Work out directory name */
	StringCchPrintfW(pNSSMService->m_WorkingDirectory,
		RTL_NUMBER_OF(pNSSMService->m_WorkingDirectory), L"%s",
		pNSSMService->m_ExecutablePath);
	strip_basename(pNSSMService->m_WorkingDirectory);

	int iResult = install_service(pNSSMService);
	cleanup_nssm_service(pNSSMService);
	return iResult;
}

/* About to edit the service. */
int pre_edit_service(int iArgc, wchar_t** ppArgv)
{
	/* Require service name. */
	if (iArgc < 2) {
		return usage(1);
	}

	/* Are we editing on the command line? */
	enum {
		MODE_EDITING,
		MODE_GETTING,
		MODE_SETTING,
		MODE_RESETTING,
		MODE_DUMPING
	} mode = MODE_EDITING;

	const wchar_t* pCommand = ppArgv[0];
	const wchar_t* pServiceName = ppArgv[1];

	/* Minimum number of arguments. */
	int iMandatory = 2;
	/* Index of first value. */
	int iRemainder = 3;
	int i;
	if (str_equiv(pCommand, L"get")) {
		iMandatory = 3;
		mode = MODE_GETTING;
	} else if (str_equiv(pCommand, L"set")) {
		iMandatory = 4;
		mode = MODE_SETTING;
	} else if (str_equiv(pCommand, L"reset") || str_equiv(pCommand, L"unset")) {
		iMandatory = 3;
		mode = MODE_RESETTING;
	} else if (str_equiv(pCommand, L"dump")) {
		iMandatory = 1;
		iRemainder = 2;
		mode = MODE_DUMPING;
	}
	if (iArgc < iMandatory) {
		return usage(1);
	}

	const wchar_t* pParameter = NULL;
	const settings_t* pSettings = NULL;
	wchar_t* pAdditional;

	/* Validate the parameter. */
	if (iMandatory > 2) {
		bool bAdditionalMandatory = false;

		pParameter = ppArgv[2];
		for (i = 0; g_Settings[i].m_pName; i++) {
			pSettings = &g_Settings[i];
			if (!str_equiv(pSettings->m_pName, pParameter))
				continue;
			if (((pSettings->m_uAdditional & ADDITIONAL_GETTING) &&
					mode == MODE_GETTING) ||
				((pSettings->m_uAdditional & ADDITIONAL_SETTING) &&
					mode == MODE_SETTING) ||
				((pSettings->m_uAdditional & ADDITIONAL_RESETTING) &&
					mode == MODE_RESETTING)) {
				bAdditionalMandatory = true;
				iMandatory++;
			}
			break;
		}
		if (!g_Settings[i].m_pName) {
			print_message(stderr, NSSM_MESSAGE_INVALID_PARAMETER, pParameter);
			for (i = 0; g_Settings[i].m_pName; i++) {
				fwprintf(stderr, L"%s\n", g_Settings[i].m_pName);
			}
			return 1;
		}

		pAdditional = NULL;
		if (bAdditionalMandatory) {
			if (iArgc < iMandatory) {
				print_message(
					stderr, NSSM_MESSAGE_MISSING_SUBPARAMETER, pParameter);
				return 1;
			}
			pAdditional = ppArgv[3];
			iRemainder = 4;
		} else if (str_equiv(pSettings->m_pName, g_NSSMNativeObjectName) &&
			mode == MODE_SETTING) {
			pAdditional = ppArgv[3];
			iRemainder = 4;
		} else {
			pAdditional = ppArgv[iRemainder];
			if (iArgc < iMandatory) {
				return usage(1);
			}
		}
	}

	nssm_service_t* pNSSMService = alloc_nssm_service();
	StringCchPrintfW(pNSSMService->m_Name, RTL_NUMBER_OF(pNSSMService->m_Name),
		L"%s", pServiceName);

	/* Open service manager */
	SC_HANDLE hOpenServices =
		open_service_manager(SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (!hOpenServices) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		return 2;
	}

	/* Try to open the service */
	uint32_t uAccess = SERVICE_QUERY_CONFIG;
	if (mode != MODE_GETTING) {
		uAccess |= SERVICE_CHANGE_CONFIG;
	}
	pNSSMService->m_hServiceControlManager =
		open_service(hOpenServices, pNSSMService->m_Name, uAccess,
			pNSSMService->m_Name, RTL_NUMBER_OF(pNSSMService->m_Name));
	if (!pNSSMService->m_hServiceControlManager) {
		CloseServiceHandle(hOpenServices);
		return 3;
	}

	/* Get system details. */
	QUERY_SERVICE_CONFIGW* pQueryServiceConfig = query_service_config(
		pNSSMService->m_Name, pNSSMService->m_hServiceControlManager);
	if (!pQueryServiceConfig) {
		CloseServiceHandle(pNSSMService->m_hServiceControlManager);
		CloseServiceHandle(hOpenServices);
		return 4;
	}

	pNSSMService->m_uServiceTypes = pQueryServiceConfig->dwServiceType;
	if (!(pNSSMService->m_uServiceTypes & SERVICE_WIN32_OWN_PROCESS)) {
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			heap_free(pQueryServiceConfig);
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			CloseServiceHandle(hOpenServices);
			print_message(stderr, NSSM_MESSAGE_CANNOT_EDIT,
				pNSSMService->m_Name, g_NSSMWin32OwnProcess, NULL);
			return 3;
		}
	}

	if (get_service_startup(pNSSMService->m_Name,
			pNSSMService->m_hServiceControlManager, pQueryServiceConfig,
			&pNSSMService->m_uStartup)) {
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			heap_free(pQueryServiceConfig);
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			CloseServiceHandle(hOpenServices);
			return 4;
		}
	}

	if (get_service_username(pNSSMService->m_Name, pQueryServiceConfig,
			&pNSSMService->m_pUsername, &pNSSMService->m_uUsernameLength)) {
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			heap_free(pQueryServiceConfig);
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			CloseServiceHandle(hOpenServices);
			return 5;
		}
	}

	StringCchPrintfW(pNSSMService->m_DisplayName,
		RTL_NUMBER_OF(pNSSMService->m_DisplayName), L"%s",
		pQueryServiceConfig->lpDisplayName);

	/* Get the canonical service name. We open it case insensitively. */
	DWORD uBufSize = RTL_NUMBER_OF(pNSSMService->m_Name);
	GetServiceKeyNameW(hOpenServices, pNSSMService->m_DisplayName,
		pNSSMService->m_Name, &uBufSize);

	/* Remember the executable in case it isn't NSSM. */
	StringCchPrintfW(pNSSMService->m_NSSMExecutablePathname,
		RTL_NUMBER_OF(pNSSMService->m_NSSMExecutablePathname), L"%s",
		pQueryServiceConfig->lpBinaryPathName);
	heap_free(pQueryServiceConfig);

	/* Get extended system details. */
	if (get_service_description(pNSSMService->m_Name,
			pNSSMService->m_hServiceControlManager,
			RTL_NUMBER_OF(pNSSMService->m_Description),
			pNSSMService->m_Description)) {
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			CloseServiceHandle(hOpenServices);
			return 6;
		}
	}

	if (get_service_dependencies(pNSSMService->m_Name,
			pNSSMService->m_hServiceControlManager,
			&pNSSMService->m_pDependencies,
			&pNSSMService->m_uDependenciesLength)) {
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			CloseServiceHandle(hOpenServices);
			return 7;
		}
	}

	/* Get NSSM details. */
	get_parameters(pNSSMService, 0);

	CloseServiceHandle(hOpenServices);

	if (!pNSSMService->m_ExecutablePath[0]) {
		pNSSMService->m_bNative = true;
		if ((mode != MODE_GETTING) && (mode != MODE_DUMPING)) {
			print_message(stderr, NSSM_MESSAGE_INVALID_SERVICE,
				pNSSMService->m_Name, g_NSSM,
				pNSSMService->m_NSSMExecutablePathname);
		}
	}

	/* Editing with the GUI. */
	if (mode == MODE_EDITING) {
		nssm_gui(IDD_EDIT, pNSSMService);
		return 0;
	}

	HKEY hKey;
	value_t TempValue;
	int iResult;

	if (mode == MODE_DUMPING) {
		wchar_t* pServiceName = pNSSMService->m_Name;
		if (iArgc > iRemainder) {
			pServiceName = ppArgv[iRemainder];
		}
		if (pNSSMService->m_bNative) {
			hKey = NULL;
		} else {
			hKey = open_registry(pNSSMService->m_Name, KEY_READ);
			if (!hKey) {
				return 4;
			}
		}

		wchar_t quoted_service_name[SERVICE_NAME_LENGTH * 2];
		wchar_t quoted_exe[EXE_LENGTH * 2];
		wchar_t quoted_nssm[EXE_LENGTH * 2];
		if (quote(pServiceName, quoted_service_name,
				RTL_NUMBER_OF(quoted_service_name))) {
			return 5;
		}
		if (quote(pNSSMService->m_ExecutablePath, quoted_exe,
				RTL_NUMBER_OF(quoted_exe))) {
			return 6;
		}
		if (quote(nssm_exe(), quoted_nssm, RTL_NUMBER_OF(quoted_nssm))) {
			return 6;
		}
		wprintf(L"%s install %s %s\n", quoted_nssm, quoted_service_name,
			quoted_exe);

		iResult = 0;
		for (i = 0; g_Settings[i].m_pName; i++) {
			pSettings = &g_Settings[i];
			if (!pSettings->m_bNative && pNSSMService->m_bNative) {
				continue;
			}
			if (dump_setting(pServiceName, hKey,
					pNSSMService->m_hServiceControlManager, pSettings)) {
				iResult++;
			}
		}

		if (!pNSSMService->m_bNative) {
			RegCloseKey(hKey);
		}
		CloseServiceHandle(pNSSMService->m_hServiceControlManager);

		if (iResult) {
			return 1;
		}
		return 0;
	}

	/* Trying to manage App* parameters for a non-NSSM service. */
	if (!pSettings->m_bNative && pNSSMService->m_bNative) {
		CloseServiceHandle(pNSSMService->m_hServiceControlManager);
		print_message(
			stderr, NSSM_MESSAGE_NATIVE_PARAMETER, pSettings->m_pName, g_NSSM);
		return 1;
	}

	if (mode == MODE_GETTING) {
		if (!pNSSMService->m_bNative) {
			hKey = open_registry(pNSSMService->m_Name, KEY_READ);
			if (!hKey) {
				return 4;
			}
		}

		if (pSettings->m_bNative) {
			iResult = get_setting(pNSSMService->m_Name,
				pNSSMService->m_hServiceControlManager, pSettings, &TempValue,
				pAdditional);
		} else {
			iResult = get_setting(
				pNSSMService->m_Name, hKey, pSettings, &TempValue, pAdditional);
		}
		if (iResult < 0) {
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			return 5;
		}

		switch (pSettings->m_uType) {
		case REG_EXPAND_SZ:
		case REG_MULTI_SZ:
		case REG_SZ:
			wprintf(L"%s\n", TempValue.m_pString ? TempValue.m_pString : L"");
			heap_free(TempValue.m_pString);
			break;

		case REG_DWORD:
			wprintf(L"%lu\n", TempValue.m_uNumber);
			break;
		}

		if (!pNSSMService->m_bNative) {
			RegCloseKey(hKey);
		}
		CloseServiceHandle(pNSSMService->m_hServiceControlManager);
		return 0;
	}

	/* Build the value. */
	if (mode == MODE_RESETTING) {
		/* Unset the parameter. */
		TempValue.m_pString = NULL;
	} else if (iRemainder == iArgc) {
		TempValue.m_pString = NULL;
	} else {
		/* Set the parameter. */
		uintptr_t len = 0;
		uintptr_t delimiterlen =
			(pSettings->m_uAdditional & ADDITIONAL_CRLF) ? 2U : 1U;
		for (i = iRemainder; i < iArgc; i++) {
			len += wcslen(ppArgv[i]) + delimiterlen;
		}
		len++;

		TempValue.m_pString = (wchar_t*)heap_alloc(len * sizeof(wchar_t));
		if (!TempValue.m_pString) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"value",
				L"edit_service()");
			CloseServiceHandle(pNSSMService->m_hServiceControlManager);
			return 2;
		}

		uintptr_t s = 0;
		for (i = iRemainder; i < iArgc; i++) {
			uintptr_t len = wcslen(ppArgv[i]);
			memmove(TempValue.m_pString + s, ppArgv[i], len * sizeof(wchar_t));
			s += len;
			if (i < (iArgc - 1)) {
				if (pSettings->m_uAdditional & ADDITIONAL_CRLF) {
					TempValue.m_pString[s++] = L'\r';
					TempValue.m_pString[s++] = L'\n';
				} else {
					TempValue.m_pString[s++] = L' ';
				}
			}
		}
		TempValue.m_pString[s] = 0;
	}

	if (!pNSSMService->m_bNative) {
		hKey = open_registry(pNSSMService->m_Name, KEY_READ | KEY_WRITE);
		if (!hKey) {
			if (TempValue.m_pString) {
				heap_free(TempValue.m_pString);
			}
			return 4;
		}
	}

	if (pSettings->m_bNative) {
		iResult = set_setting(pNSSMService->m_Name,
			pNSSMService->m_hServiceControlManager, pSettings, &TempValue,
			pAdditional);
	} else {
		iResult = set_setting(
			pNSSMService->m_Name, hKey, pSettings, &TempValue, pAdditional);
	}
	if (TempValue.m_pString) {
		heap_free(TempValue.m_pString);
	}
	if (iResult < 0) {
		if (!pNSSMService->m_bNative) {
			RegCloseKey(hKey);
		}
		CloseServiceHandle(pNSSMService->m_hServiceControlManager);
		return 6;
	}

	if (!pNSSMService->m_bNative) {
		RegCloseKey(hKey);
	}
	CloseServiceHandle(pNSSMService->m_hServiceControlManager);

	return 0;
}

/* About to remove the service */
int pre_remove_service(int iArgc, wchar_t** ppArgv)
{
	nssm_service_t* pNSSMService = alloc_nssm_service();
	set_nssm_service_defaults(pNSSMService);
	if (iArgc) {
		StringCchPrintfW(pNSSMService->m_Name,
			RTL_NUMBER_OF(pNSSMService->m_Name), L"%s", ppArgv[0]);
	}

	/* Show dialogue box if we didn't pass service name and "confirm" */
	if (iArgc < 2) {
		return nssm_gui(IDD_REMOVE, pNSSMService);
	}
	if (str_equiv(ppArgv[1], L"confirm")) {
		int iResult = remove_service(pNSSMService);
		cleanup_nssm_service(pNSSMService);
		return iResult;
	}
	print_message(stderr, NSSM_MESSAGE_PRE_REMOVE_SERVICE);
	return 100;
}

/* Install the service */
int install_service(nssm_service_t* pNSSMService)
{
	if (!pNSSMService) {
		return 1;
	}

	/* Open service manager */
	SC_HANDLE hOpenService =
		open_service_manager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!hOpenService) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 2;
	}

	/* Get path of this program */
	StringCchPrintfW(pNSSMService->m_NSSMExecutablePathname,
		RTL_NUMBER_OF(pNSSMService->m_NSSMExecutablePathname), L"%s",
		nssm_imagepath());

	/* Create the service - settings will be changed in edit_service() */
	pNSSMService->m_hServiceControlManager = CreateServiceW(hOpenService,
		pNSSMService->m_Name, pNSSMService->m_Name, SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
		pNSSMService->m_NSSMExecutablePathname, 0, 0, 0, 0, 0);
	if (!pNSSMService->m_hServiceControlManager) {
		print_message(stderr, NSSM_MESSAGE_CREATESERVICE_FAILED,
			error_string(GetLastError()));
		CloseServiceHandle(hOpenService);
		return 5;
	}

	if (edit_service(pNSSMService, false)) {
		DeleteService(pNSSMService->m_hServiceControlManager);
		CloseServiceHandle(hOpenService);
		return 6;
	}

	print_message(stdout, NSSM_MESSAGE_SERVICE_INSTALLED, pNSSMService->m_Name);

	/* Cleanup */
	CloseServiceHandle(hOpenService);

	return 0;
}

/* Edit the service. */
int edit_service(nssm_service_t* pNSSMService, bool bEditing)
{
	if (!pNSSMService) {
		return 1;
	}

	/*
	  The only two valid flags for service type are SERVICE_WIN32_OWN_PROCESS
	  and SERVICE_INTERACTIVE_PROCESS.
	*/
	pNSSMService->m_uServiceTypes &= SERVICE_INTERACTIVE_PROCESS;
	pNSSMService->m_uServiceTypes |= SERVICE_WIN32_OWN_PROCESS;

	/* Startup type. */
	uint32_t uStartup;
	switch (pNSSMService->m_uStartup) {
	case NSSM_STARTUP_MANUAL:
		uStartup = SERVICE_DEMAND_START;
		break;
	case NSSM_STARTUP_DISABLED:
		uStartup = SERVICE_DISABLED;
		break;
	default:
		uStartup = SERVICE_AUTO_START;
		break;
	}

	/* Display name. */
	if (!pNSSMService->m_DisplayName[0])
		StringCchPrintfW(pNSSMService->m_DisplayName,
			RTL_NUMBER_OF(pNSSMService->m_DisplayName), L"%s",
			pNSSMService->m_Name);

	/*
	  Username must be NULL if we aren't changing or an account name.
	  We must explicitly use LOCALSYSTEM to change it when we are editing.
	  Password must be NULL if we aren't changing, a password or "".
	  Empty passwords are valid but we won't allow them in the GUI.
	*/
	const wchar_t* pUsername = NULL;
	wchar_t* pCanon = NULL;
	wchar_t* pPassword = NULL;
	uint32_t bVirtual_account = false;
	if (pNSSMService->m_uUsernameLength) {
		pUsername = pNSSMService->m_pUsername;
		if (is_virtual_account(pNSSMService->m_Name, pUsername)) {
			bVirtual_account = true;
			pCanon = static_cast<wchar_t*>(heap_alloc(
				(pNSSMService->m_uUsernameLength + 1) * sizeof(wchar_t)));
			if (!pCanon) {
				print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"canon",
					L"edit_service()");
				return 5;
			}
			memmove(pCanon, pUsername,
				(pNSSMService->m_uUsernameLength + 1) * sizeof(wchar_t));
		} else {
			if (canonicalise_username(pUsername, &pCanon)) {
				return 5;
			}
			if (pNSSMService->m_uPasswordLength) {
				pPassword = pNSSMService->m_pPassword;
			}
		}
	} else if (bEditing) {
		pUsername = pCanon = const_cast<wchar_t*>(g_NSSMLocalSystemAccount);
	}

	if (!bVirtual_account) {
		if (well_known_username(pCanon)) {
			pPassword = L"";
		} else {
			if (grant_logon_as_service(pCanon)) {
				if (pCanon != pUsername) {
					heap_free(pCanon);
				}
				print_message(stderr,
					NSSM_MESSAGE_GRANT_LOGON_AS_SERVICE_FAILED, pUsername);
				return 5;
			}
		}
	}

	wchar_t* pDependencies = L"";
	if (pNSSMService->m_uDependenciesLength) {
		pDependencies = NULL; /* Change later. */
	}

	if (!ChangeServiceConfigW(pNSSMService->m_hServiceControlManager,
			pNSSMService->m_uServiceTypes, uStartup, SERVICE_NO_CHANGE, 0, 0, 0,
			pDependencies, pCanon, pPassword, pNSSMService->m_DisplayName)) {
		if (pCanon != pUsername) {
			heap_free(pCanon);
		}
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return 5;
	}
	if (pCanon != pUsername) {
		heap_free(pCanon);
	}

	if (pNSSMService->m_uDependenciesLength) {
		if (set_service_dependencies(pNSSMService->m_Name,
				pNSSMService->m_hServiceControlManager,
				pNSSMService->m_pDependencies)) {
			return 5;
		}
	}

	if (pNSSMService->m_Description[0] || bEditing) {
		set_service_description(pNSSMService->m_Name,
			pNSSMService->m_hServiceControlManager,
			pNSSMService->m_Description);
	}

	SERVICE_DELAYED_AUTO_START_INFO delayed;
	ZeroMemory(&delayed, sizeof(delayed));
	if (pNSSMService->m_uStartup == NSSM_STARTUP_DELAYED) {
		delayed.fDelayedAutostart = 1;
	} else {
		delayed.fDelayedAutostart = 0;
	}
	/* Delayed startup isn't supported until Vista. */
	if (!ChangeServiceConfig2W(pNSSMService->m_hServiceControlManager,
			SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) {
		DWORD uError = GetLastError();
		/* Pre-Vista we expect to fail with ERROR_INVALID_LEVEL */
		if (uError != ERROR_INVALID_LEVEL) {
			log_event(EVENTLOG_ERROR_TYPE,
				NSSM_EVENT_SERVICE_CONFIG_DELAYED_AUTO_START_INFO_FAILED,
				pNSSMService->m_Name, error_string(uError), NULL);
		}
	}

	/* Don't mess with parameters which aren't ours. */
	if (!pNSSMService->m_bNative) {
		/* Now we need to put the parameters into the registry */
		if (create_parameters(pNSSMService, bEditing)) {
			print_message(stderr, NSSM_MESSAGE_CREATE_PARAMETERS_FAILED);
			return 6;
		}

		set_service_recovery(pNSSMService);
	}

	return 0;
}

/* Control a service. */
int control_service(
	uint32_t uControl, int iArgc, wchar_t** argv, bool bReturnStatus)
{
	if (iArgc < 1) {
		return usage(1);
	}
	wchar_t* pServiceName = argv[0];
	wchar_t CanonicalName[SERVICE_NAME_LENGTH];

	SC_HANDLE hOpenService =
		open_service_manager(SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (!hOpenService) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		if (bReturnStatus) {
			return 0;
		}
		return 2;
	}

	uint32_t uAccess = SERVICE_QUERY_STATUS;
	switch (uControl) {
	case NSSM_SERVICE_CONTROL_START:
		uAccess |= SERVICE_START;
		break;

	case SERVICE_CONTROL_CONTINUE:
	case SERVICE_CONTROL_PAUSE:
		uAccess |= SERVICE_PAUSE_CONTINUE;
		break;

	case SERVICE_CONTROL_STOP:
		uAccess |= SERVICE_STOP;
		break;

	case NSSM_SERVICE_CONTROL_ROTATE:
		uAccess |= SERVICE_USER_DEFINED_CONTROL;
		break;
	}

	SC_HANDLE hNewService = open_service(hOpenService, pServiceName, uAccess,
		CanonicalName, RTL_NUMBER_OF(CanonicalName));
	if (!hNewService) {
		CloseServiceHandle(hOpenService);
		if (bReturnStatus) {
			return 0;
		}
		return 3;
	}

	int ret;
	DWORD uError;
	SERVICE_STATUS service_status;
	if (uControl == NSSM_SERVICE_CONTROL_START) {
		ret = StartServiceW(
			hNewService, static_cast<DWORD>(iArgc), (const wchar_t**)argv);
		uError = GetLastError();
		CloseServiceHandle(hOpenService);

		if (uError == ERROR_IO_PENDING) {
			/*
			  Older versions of Windows return immediately with ERROR_IO_PENDING
			  indicate that the operation is still in progress.  Newer versions
			  will return it if there really is a delay.
			*/
			ret = 1;
			uError = ERROR_SUCCESS;
		}

		if (ret) {
			uint32_t uCutoff = 0;

			/* If we manage the service, respect the throttle time. */
			HKEY hKey = open_registry(pServiceName, 0, KEY_READ, false);
			if (hKey) {
				if (get_number(hKey, g_NSSMRegThrottle, &uCutoff, false) != 1) {
					uCutoff = NSSM_RESET_THROTTLE_RESTART;
				}
				RegCloseKey(hKey);
			}
			uint32_t uInitialStatus = SERVICE_STOPPED;
			int iResponse = await_service_control_response(uControl,
				hNewService, &service_status, uInitialStatus, uCutoff);
			CloseServiceHandle(hNewService);

			if (iResponse) {
				print_message(stderr, NSSM_MESSAGE_BAD_CONTROL_RESPONSE,
					CanonicalName,
					service_status_text(service_status.dwCurrentState),
					service_control_text(uControl));
				if (bReturnStatus) {
					return 0;
				}
				return 1;
			} else {
				wprintf(L"%s: %s: %s", CanonicalName,
					service_control_text(uControl), error_string(uError));
			}
			return 0;
		} else {
			CloseServiceHandle(hNewService);
			fwprintf(stderr, L"%s: %s: %s", CanonicalName,
				service_control_text(uControl), error_string(uError));
			if (bReturnStatus) {
				return 0;
			}
			return 1;
		}
	} else if (uControl == SERVICE_CONTROL_INTERROGATE) {
		/*
		  We could actually send an INTERROGATE control but that won't return
		  any information if the service is stopped and we don't care about
		  the extra details it might give us in any case.  So we'll fake it.
		*/
		ret = QueryServiceStatus(hNewService, &service_status);
		uError = GetLastError();

		if (ret) {
			wprintf(
				L"%s\n", service_status_text(service_status.dwCurrentState));
			if (bReturnStatus) {
				return static_cast<int>(service_status.dwCurrentState);
			}
			return 0;
		} else {
			fwprintf(stderr, L"%s: %s\n", CanonicalName, error_string(uError));
			if (bReturnStatus) {
				return 0;
			}
			return 1;
		}
	} else {
		ret = ControlService(hNewService, uControl, &service_status);
		uint32_t uInitialStatus = service_status.dwCurrentState;
		uError = GetLastError();
		CloseServiceHandle(hOpenService);

		if (uError == ERROR_IO_PENDING) {
			ret = 1;
			uError = ERROR_SUCCESS;
		}

		if (ret) {
			int response = await_service_control_response(
				uControl, hNewService, &service_status, uInitialStatus);
			CloseServiceHandle(hNewService);

			if (response) {
				print_message(stderr, NSSM_MESSAGE_BAD_CONTROL_RESPONSE,
					CanonicalName,
					service_status_text(service_status.dwCurrentState),
					service_control_text(uControl));
				if (bReturnStatus) {
					return 0;
				}
				return 1;
			} else {
				wprintf(L"%s: %s: %s", CanonicalName,
					service_control_text(uControl), error_string(uError));
			}
			if (bReturnStatus) {
				return static_cast<int>(service_status.dwCurrentState);
			}
			return 0;
		} else {
			CloseServiceHandle(hNewService);
			fwprintf(stderr, L"%s: %s: %s", CanonicalName,
				service_control_text(uControl), error_string(uError));
			if (uError == ERROR_SERVICE_NOT_ACTIVE) {
				if ((uControl == SERVICE_CONTROL_SHUTDOWN) ||
					(uControl == SERVICE_CONTROL_STOP)) {
					if (bReturnStatus) {
						return SERVICE_STOPPED;
					}
					return 0;
				}
			}
			if (bReturnStatus) {
				return 0;
			}
			return 1;
		}
	}
}

int control_service(uint32_t uControl, int iArgc, wchar_t** ppArgv)
{
	return control_service(uControl, iArgc, ppArgv, false);
}

/* Remove the service */
int remove_service(nssm_service_t* pNSSMService)
{
	// Sanity check
	if (!pNSSMService) {
		return 1;
	}

	/* Open service manager */
	SC_HANDLE hOpenService =
		open_service_manager(SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (!hOpenService) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		return 2;
	}

	/* Try to open the service */
	pNSSMService->m_hServiceControlManager =
		open_service(hOpenService, pNSSMService->m_Name, DELETE,
			pNSSMService->m_Name, RTL_NUMBER_OF(pNSSMService->m_Name));
	if (!pNSSMService->m_hServiceControlManager) {
		CloseServiceHandle(hOpenService);
		return 3;
	}

	/* Get the canonical service name. We open it case insensitively. */
	DWORD uBufSize = RTL_NUMBER_OF(pNSSMService->m_DisplayName);
	GetServiceDisplayNameW(hOpenService, pNSSMService->m_Name,
		pNSSMService->m_DisplayName, &uBufSize);
	uBufSize = RTL_NUMBER_OF(pNSSMService->m_Name);
	GetServiceKeyNameW(hOpenService, pNSSMService->m_DisplayName,
		pNSSMService->m_Name, &uBufSize);

	// Try to delete the service
	if (!DeleteService(pNSSMService->m_hServiceControlManager)) {
		print_message(stderr, NSSM_MESSAGE_DELETESERVICE_FAILED);
		CloseServiceHandle(hOpenService);
		return 4;
	}

	// Cleanup
	CloseServiceHandle(hOpenService);

	print_message(stdout, NSSM_MESSAGE_SERVICE_REMOVED, pNSSMService->m_Name);
	return 0;
}

/* Service control handler */
static DWORD WINAPI service_control_handler(
	DWORD uControlInput, DWORD uEvent, void* /* pData */, void* pContext)
{
	nssm_service_t* pNSSMService = static_cast<nssm_service_t*>(pContext);
	uint32_t uControl = uControlInput;

	switch (uControl) {
	case SERVICE_CONTROL_INTERROGATE:
		/* We always keep the service status up-to-date so this is a no-op. */
		return NO_ERROR;

	case SERVICE_CONTROL_SHUTDOWN:
	case SERVICE_CONTROL_STOP:
		pNSSMService->m_uLastControl = uControl;
		log_service_control(pNSSMService->m_Name, uControl, true);

		/* Immediately block further controls. */
		pNSSMService->m_bAllowRestart = false;
		pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		pNSSMService->m_ServiceStatus.dwControlsAccepted = 0;
		SetServiceStatus(
			pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);

		/* Pre-stop hook. */
		nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventStop,
			g_NSSMHookActionPre, &uControl, NSSM_SERVICE_STATUS_DEADLINE,
			false);

		/*
		  We MUST acknowledge the stop request promptly but we're committed to
		  waiting for the application to exit.  Spawn a new thread to wait
		  while we acknowledge the request.
		*/
		if (!CreateThread(NULL, 0, shutdown_service, pContext, 0, NULL)) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED,
				error_string(GetLastError()), NULL);

			/*
			  We couldn't create a thread to tidy up so we'll have to force the
			  tidyup to complete in time in this thread.
			*/
			pNSSMService->m_uKillConsoleDelay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
			pNSSMService->m_uKillWindowDelay = NSSM_KILL_WINDOW_GRACE_PERIOD;
			pNSSMService->m_uKillThreadsDelay = NSSM_KILL_THREADS_GRACE_PERIOD;

			stop_service(pNSSMService, 0, true, true);
		}
		return NO_ERROR;

	case SERVICE_CONTROL_CONTINUE:
		pNSSMService->m_uLastControl = uControl;
		log_service_control(pNSSMService->m_Name, uControl, true);
		pNSSMService->m_uThrottle = 0;
		if (g_bUseCriticalSection) {
			g_Imports.WakeConditionVariable(&pNSSMService->m_ThrottleCondition);
		} else {
			if (!pNSSMService->m_hThrottleTimer) {
				return ERROR_CALL_NOT_IMPLEMENTED;
			}
			ZeroMemory(&pNSSMService->m_iThrottleDuetime,
				sizeof(pNSSMService->m_iThrottleDuetime));
			SetWaitableTimer(pNSSMService->m_hThrottleTimer,
				&pNSSMService->m_iThrottleDuetime, 0, 0, 0, 0);
		}
		/* We can't continue if the application is running! */
		if (!pNSSMService->m_hProcess) {
			pNSSMService->m_ServiceStatus.dwCurrentState =
				SERVICE_CONTINUE_PENDING;
		}
		pNSSMService->m_ServiceStatus.dwWaitHint =
			throttle_milliseconds(pNSSMService->m_uThrottle) +
			NSSM_WAITHINT_MARGIN;
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_RESET_THROTTLE,
			pNSSMService->m_Name, NULL);
		SetServiceStatus(
			pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);
		return NO_ERROR;

	case SERVICE_CONTROL_PAUSE:
		/*
		  We don't accept pause messages but it isn't possible to register
		  only for continue messages so we have to handle this case.
		*/
		log_service_control(pNSSMService->m_Name, uControl, false);
		return ERROR_CALL_NOT_IMPLEMENTED;

	case NSSM_SERVICE_CONTROL_ROTATE:
		pNSSMService->m_uLastControl = uControl;
		log_service_control(pNSSMService->m_Name, uControl, true);
		nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventRotate,
			g_NSSMHookActionPre, &uControl, NSSM_HOOK_DEADLINE, false);
		if (pNSSMService->m_uRotateStdoutOnline == NSSM_ROTATE_ONLINE) {
			pNSSMService->m_uRotateStdoutOnline = NSSM_ROTATE_ONLINE_ASAP;
		}
		if (pNSSMService->m_uRotateStderrOnline == NSSM_ROTATE_ONLINE) {
			pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_ONLINE_ASAP;
		}
		nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventRotate,
			g_NSSMHookActionPost, &uControl);
		return NO_ERROR;

	case SERVICE_CONTROL_POWEREVENT:
		/* Resume from suspend. */
		if (uEvent == PBT_APMRESUMEAUTOMATIC) {
			pNSSMService->m_uLastControl = uControl;
			log_service_control(pNSSMService->m_Name, uControl, true);
			nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventPower,
				g_NSSMHookActionResume, &uControl);
			return NO_ERROR;
		}

		/* Battery low or changed to A/C power or something. */
		if (uEvent == PBT_APMPOWERSTATUSCHANGE) {
			pNSSMService->m_uLastControl = uControl;
			log_service_control(pNSSMService->m_Name, uControl, true);
			nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventPower,
				g_NSSMHookActionChange, &uControl);
			return NO_ERROR;
		}
		log_service_control(pNSSMService->m_Name, uControl, false);
		return NO_ERROR;
	}

	/* Unknown control */
	log_service_control(pNSSMService->m_Name, uControl, false);
	return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Service initialization */
void WINAPI service_main(unsigned long uArgc, wchar_t** ppArgv)
{
	nssm_service_t* pNSSMService = alloc_nssm_service();
	if (!pNSSMService) {
		return;
	}

	static volatile bool g_bAwaitDebugger =
		(uArgc > 1 && str_equiv(ppArgv[1], L"debug"));

	while (g_bAwaitDebugger) {
		Sleep(1000);
	}

	if (StringCchPrintfW(pNSSMService->m_Name,
			RTL_NUMBER_OF(pNSSMService->m_Name), L"%s", ppArgv[0]) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"service->name", L"service_main()", NULL);
		return;
	}

	/* We can use a condition variable in a critical section on Vista or later.
	 */
	if (g_Imports.SleepConditionVariableCS && g_Imports.WakeConditionVariable) {
		g_bUseCriticalSection = true;
	} else {
		g_bUseCriticalSection = false;
	}

	/* Initialise status */
	ZeroMemory(
		&pNSSMService->m_ServiceStatus, sizeof(pNSSMService->m_ServiceStatus));
	pNSSMService->m_ServiceStatus.dwServiceType =
		SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
	pNSSMService->m_ServiceStatus.dwControlsAccepted = 0;
	pNSSMService->m_ServiceStatus.dwWin32ExitCode = NO_ERROR;
	pNSSMService->m_ServiceStatus.dwServiceSpecificExitCode = 0;
	pNSSMService->m_ServiceStatus.dwCheckPoint = 0;
	pNSSMService->m_ServiceStatus.dwWaitHint = NSSM_WAITHINT_MARGIN;

	/* Signal we AREN'T running the server */
	pNSSMService->m_hProcess = NULL;
	pNSSMService->m_uPID = 0;

	/* Register control handler */
	pNSSMService->m_hStatusHandle = RegisterServiceCtrlHandlerExW(
		g_NSSM, service_control_handler, pNSSMService);
	if (!pNSSMService->m_hStatusHandle) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_REGISTERSERVICECTRLHANDER_FAILED,
			error_string(GetLastError()), NULL);
		return;
	}

	log_service_control(pNSSMService->m_Name, 0, true);

	pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	pNSSMService->m_ServiceStatus.dwWaitHint =
		pNSSMService->m_uThrottleDelay + NSSM_WAITHINT_MARGIN;
	SetServiceStatus(
		pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);

	if (g_bIsAdmin) {
		/* Try to create the exit action parameters; we don't care if it fails
		 */
		create_exit_action(pNSSMService->m_Name, g_ExitActionStrings[0], false);

		SC_HANDLE hOpenServices = open_service_manager(SC_MANAGER_CONNECT);
		if (hOpenServices) {
			pNSSMService->m_hServiceControlManager = open_service(hOpenServices,
				pNSSMService->m_Name, SERVICE_CHANGE_CONFIG, 0, 0);
			set_service_recovery(pNSSMService);

			/* Remember our display name. */
			DWORD uDisplaynameLen = RTL_NUMBER_OF(pNSSMService->m_DisplayName);
			GetServiceDisplayNameW(hOpenServices, pNSSMService->m_Name,
				pNSSMService->m_DisplayName, &uDisplaynameLen);

			CloseServiceHandle(hOpenServices);
		}
	}

	/* Used for signalling a resume if the service pauses when throttled. */
	if (g_bUseCriticalSection) {
		InitializeCriticalSection(&pNSSMService->m_ThrottleSection);
		pNSSMService->m_bThrottleSectionValid = true;
	} else {
		pNSSMService->m_hThrottleTimer = CreateWaitableTimerW(0, 1, NULL);
		if (!pNSSMService->m_hThrottleTimer) {
			log_event(EVENTLOG_WARNING_TYPE,
				NSSM_EVENT_CREATEWAITABLETIMER_FAILED, pNSSMService->m_Name,
				error_string(GetLastError()), NULL);
		}
	}

	/* Critical section for hooks. */
	InitializeCriticalSection(&pNSSMService->m_HookLock);
	pNSSMService->m_bHookLockValid = true;

	/* Remember our initial environment. */
	pNSSMService->m_pInitialEnvironmentVariables = copy_environment();

	/* Remember our creation time. */
	if (get_process_creation_time(
			GetCurrentProcess(), &pNSSMService->m_NSSMCreationTime)) {
		ZeroMemory(&pNSSMService->m_NSSMCreationTime,
			sizeof(pNSSMService->m_NSSMCreationTime));
	}

	pNSSMService->m_bAllowRestart = true;
	if (!CreateThread(NULL, 0, launch_service, pNSSMService, 0, NULL)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED,
			error_string(GetLastError()), NULL);
		stop_service(pNSSMService, 0, true, true);
	}
}

/* Make sure service recovery actions are taken where necessary */
void set_service_recovery(nssm_service_t* pNSSMService)
{
	SERVICE_FAILURE_ACTIONS_FLAG flag;
	ZeroMemory(&flag, sizeof(flag));
	flag.fFailureActionsOnNonCrashFailures = true;

	/* This functionality was added in Vista so the call may fail */
	if (!ChangeServiceConfig2W(pNSSMService->m_hServiceControlManager,
			SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
		DWORD uError = GetLastError();
		/* Pre-Vista we expect to fail with ERROR_INVALID_LEVEL */
		if (uError != ERROR_INVALID_LEVEL) {
			log_event(EVENTLOG_ERROR_TYPE,
				NSSM_EVENT_SERVICE_CONFIG_FAILURE_ACTIONS_FAILED,
				pNSSMService->m_Name, error_string(uError), NULL);
		}
	}
}

uint32_t monitor_service(nssm_service_t* pNSSMService)
{
	/* Set service status to started */
	int ret = start_service(pNSSMService);
	if (ret) {
		wchar_t code[16];
		StringCchPrintfW(code, RTL_NUMBER_OF(code), L"%d", ret);
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_START_SERVICE_FAILED,
			pNSSMService->m_ExecutablePath, pNSSMService->m_Name, ret, NULL);
		return static_cast<uint32_t>(ret);
	}
	log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_STARTED_SERVICE,
		pNSSMService->m_ExecutablePath, pNSSMService->m_AppParameters,
		pNSSMService->m_Name, pNSSMService->m_WorkingDirectory, NULL);

	/* Monitor service */
	if (!RegisterWaitForSingleObject(&pNSSMService->m_hWait,
			pNSSMService->m_hProcess, end_service, pNSSMService, INFINITE,
			WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
		log_event(EVENTLOG_WARNING_TYPE,
			NSSM_EVENT_REGISTERWAITFORSINGLEOBJECT_FAILED, pNSSMService->m_Name,
			pNSSMService->m_ExecutablePath, error_string(GetLastError()), NULL);
	}

	return 0;
}

const wchar_t* service_control_text(uint32_t uControl)
{
	switch (uControl) {
	/* HACK: there is no SERVICE_CONTROL_START constant */
	case NSSM_SERVICE_CONTROL_START:
		return L"START";
	case SERVICE_CONTROL_STOP:
		return L"STOP";
	case SERVICE_CONTROL_SHUTDOWN:
		return L"SHUTDOWN";
	case SERVICE_CONTROL_PAUSE:
		return L"PAUSE";
	case SERVICE_CONTROL_CONTINUE:
		return L"CONTINUE";
	case SERVICE_CONTROL_INTERROGATE:
		return L"INTERROGATE";
	case NSSM_SERVICE_CONTROL_ROTATE:
		return L"ROTATE";
	case SERVICE_CONTROL_POWEREVENT:
		return L"POWEREVENT";
	default:
		return NULL;
	}
}

const wchar_t* service_status_text(uint32_t uStatus)
{
	switch (uStatus) {
	case SERVICE_STOPPED:
		return L"SERVICE_STOPPED";
	case SERVICE_START_PENDING:
		return L"SERVICE_START_PENDING";
	case SERVICE_STOP_PENDING:
		return L"SERVICE_STOP_PENDING";
	case SERVICE_RUNNING:
		return L"SERVICE_RUNNING";
	case SERVICE_CONTINUE_PENDING:
		return L"SERVICE_CONTINUE_PENDING";
	case SERVICE_PAUSE_PENDING:
		return L"SERVICE_PAUSE_PENDING";
	case SERVICE_PAUSED:
		return L"SERVICE_PAUSED";
	default:
		return NULL;
	}
}

void log_service_control(
	const wchar_t* pServiceName, uint32_t uControl, bool bHandled)
{
	wchar_t* pText = const_cast<wchar_t*>(service_control_text(uControl));
	uint32_t uEvent;

	if (!pText) {
		/* "0x" + 8 x hex + NULL */
		pText = static_cast<wchar_t*>(heap_alloc(11 * sizeof(wchar_t)));
		if (!pText) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
				L"control code", L"log_service_control()", NULL);
			return;
		}
		if (StringCchPrintfW(pText, 11, L"0x%08x", uControl) < 0) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
				L"control code", L"log_service_control()", NULL);
			heap_free(pText);
			return;
		}

		uEvent = NSSM_EVENT_SERVICE_CONTROL_UNKNOWN;
	} else if (bHandled) {
		uEvent = NSSM_EVENT_SERVICE_CONTROL_HANDLED;
	} else {
		uEvent = NSSM_EVENT_SERVICE_CONTROL_NOT_HANDLED;
	}
	log_event(EVENTLOG_INFORMATION_TYPE, uEvent, pServiceName, pText, NULL);

	if (uEvent == NSSM_EVENT_SERVICE_CONTROL_UNKNOWN) {
		heap_free(pText);
	}
}

/* Start the service */
int start_service(nssm_service_t* pNSSMService)
{
	pNSSMService->m_bStopping = false;

	if (pNSSMService->m_hProcess) {
		return 0;
	}
	pNSSMService->m_uStartRequestedCount++;

	/* Allocate a STARTUPINFO structure for a new process */
	STARTUPINFOW si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	/* Allocate a PROCESSINFO structure for the process */
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));

	/* Get startup parameters */
	int ret = get_parameters(pNSSMService, &si);
	if (ret) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_PARAMETERS_FAILED,
			pNSSMService->m_Name, NULL);
		unset_service_environment(pNSSMService);
		return static_cast<int>(stop_service(pNSSMService, 2, true, true));
	}

	/* Launch executable with arguments */
	wchar_t cmd[CMD_LENGTH];
	if (StringCchPrintfW(cmd, RTL_NUMBER_OF(cmd), L"\"%s\" %s",
			pNSSMService->m_ExecutablePath,
			pNSSMService->m_AppParameters) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"command line", L"start_service", NULL);
		unset_service_environment(pNSSMService);
		return static_cast<int>(stop_service(pNSSMService, 2, true, true));
	}

	throttle_restart(pNSSMService);

	pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	pNSSMService->m_ServiceStatus.dwControlsAccepted =
		SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_SHUTDOWN |
		SERVICE_ACCEPT_STOP;
	SetServiceStatus(
		pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);

	uint32_t control = NSSM_SERVICE_CONTROL_START;

	/* Did another thread receive a stop control? */
	if (pNSSMService->m_bAllowRestart) {
		/* Set up I/O redirection. */
		if (get_output_handles(pNSSMService, &si)) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_OUTPUT_HANDLES_FAILED,
				pNSSMService->m_Name, NULL);
			FreeConsole();
			close_output_handles(&si);
			unset_service_environment(pNSSMService);
			return static_cast<int>(stop_service(pNSSMService, 4, true, true));
		}
		FreeConsole();

		/* Pre-start hook. May need I/O to have been redirected already. */
		if (nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventStart,
				g_NSSMHookActionPre, &control, NSSM_SERVICE_STATUS_DEADLINE,
				false) == NSSM_HOOK_STATUS_ABORT) {
			wchar_t code[16];
			StringCchPrintfW(
				code, RTL_NUMBER_OF(code), L"%lu", NSSM_HOOK_STATUS_ABORT);
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_PRESTART_HOOK_ABORT,
				g_NSSMHookEventStart, g_NSSMHookActionPre, pNSSMService->m_Name,
				code, NULL);
			unset_service_environment(pNSSMService);
			return static_cast<int>(stop_service(pNSSMService, 5, true, true));
		}

		/* The pre-start hook will have cleaned the environment. */
		set_service_environment(pNSSMService);

		bool inherit_handles = false;
		if (si.dwFlags & STARTF_USESTDHANDLES) {
			inherit_handles = true;
		}
		uint32_t flags = pNSSMService->m_uPriority & priority_mask();
		if (pNSSMService->m_uAffinity) {
			flags |= CREATE_SUSPENDED;
		}
		if (!pNSSMService->m_bDontSpawnConsole) {
			flags |= CREATE_NEW_CONSOLE;
		}

		if (!CreateProcessW(0, cmd, 0, 0, inherit_handles, flags, 0,
				pNSSMService->m_WorkingDirectory, &si, &pi)) {
			DWORD exitcode = 3;
			DWORD error = GetLastError();
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED,
				pNSSMService->m_Name, pNSSMService->m_ExecutablePath,
				error_string(error), NULL);
			close_output_handles(&si);
			unset_service_environment(pNSSMService);
			return static_cast<int>(
				stop_service(pNSSMService, exitcode, true, true));
		}
		pNSSMService->m_uStartCount++;
		pNSSMService->m_hProcess = pi.hProcess;
		pNSSMService->m_uPID = pi.dwProcessId;

		if (get_process_creation_time(pNSSMService->m_hProcess,
				&pNSSMService->m_ProcessCreationTime)) {
			ZeroMemory(&pNSSMService->m_ProcessCreationTime,
				sizeof(pNSSMService->m_ProcessCreationTime));
		}

		close_output_handles(&si);

		if (pNSSMService->m_uAffinity) {
			/*
			  We are explicitly storing service->affinity as a 64-bit unsigned
			  integer so that we can parse it regardless of whether we're
			  running in 32-bit or 64-bit mode.  The arguments to
			  SetProcessAffinityMask(), however, are defined as type DWORD_PTR
			  and hence limited to 32 bits on a 32-bit system (or when running
			  the 32-bit NSSM).

			  The result is a lot of seemingly-unnecessary casting throughout
			  the code and potentially confusion when we actually try to start
			  the service. Having said that, however, it's unlikely that we're
			  actually going to run in 32-bit mode on a system which has more
			  than 32 CPUs so the likelihood of seeing a confusing situation is
			  somewhat diminished.
			*/
			DWORD_PTR affinity;
			DWORD_PTR system_affinity;

			if (GetProcessAffinityMask(
					pNSSMService->m_hProcess, &affinity, &system_affinity)) {
				affinity = static_cast<DWORD_PTR>(pNSSMService->m_uAffinity) &
					system_affinity;
			} else {
				affinity = static_cast<DWORD_PTR>(pNSSMService->m_uAffinity);
				log_event(EVENTLOG_ERROR_TYPE,
					NSSM_EVENT_GETPROCESSAFFINITYMASK_FAILED,
					pNSSMService->m_Name, error_string(GetLastError()), NULL);
			}

			if (!SetProcessAffinityMask(pNSSMService->m_hProcess, affinity)) {
				log_event(EVENTLOG_WARNING_TYPE,
					NSSM_EVENT_SETPROCESSAFFINITYMASK_FAILED,
					pNSSMService->m_Name, error_string(GetLastError()), NULL);
			}

			ResumeThread(pi.hThread);
		}
	}

	/* Restore our environment. */
	unset_service_environment(pNSSMService);

	/*
	  Wait for a clean startup before changing the service status to RUNNING
	  but be mindful of the fact that we are blocking the service control
	  manager so abandon the wait before too much time has elapsed.
	*/
	if (await_single_handle(pNSSMService->m_hStatusHandle,
			&pNSSMService->m_ServiceStatus, pNSSMService->m_hProcess,
			pNSSMService->m_Name, L"start_service",
			pNSSMService->m_uThrottleDelay) == 1)
		pNSSMService->m_uThrottle = 0;

	/* Did another thread receive a stop control? */
	if (!pNSSMService->m_bAllowRestart) {
		return 0;
	}

	/* Signal successful start */
	pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
	pNSSMService->m_ServiceStatus.dwControlsAccepted &=
		(~SERVICE_ACCEPT_PAUSE_CONTINUE);
	SetServiceStatus(
		pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);

	/* Post-start hook. */
	if (!pNSSMService->m_uThrottle) {
		nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventStart,
			g_NSSMHookActionPost, &control);
	}

	/* Ensure the restart delay is always applied. */
	if (pNSSMService->m_uRestartDelay && !pNSSMService->m_uThrottle) {
		pNSSMService->m_uThrottle++;
	}

	return 0;
}

/* Stop the service */
uint32_t stop_service(nssm_service_t* pNSSMService, uint32_t uExitcode,
	bool bGraceful, bool bDefaultAction)
{
	pNSSMService->m_bAllowRestart = false;
	if (pNSSMService->m_hWait) {
		UnregisterWait(pNSSMService->m_hWait);
		pNSSMService->m_hWait = NULL;
	}

	pNSSMService->m_uRotateStdoutOnline = pNSSMService->m_uRotateStderrOnline =
		NSSM_ROTATE_OFFLINE;

	if (bDefaultAction && !uExitcode && !bGraceful) {
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_GRACEFUL_SUICIDE,
			pNSSMService->m_Name, pNSSMService->m_ExecutablePath,
			g_ExitActionStrings[NSSM_EXIT_UNCLEAN],
			g_ExitActionStrings[NSSM_EXIT_UNCLEAN],
			g_ExitActionStrings[NSSM_EXIT_UNCLEAN],
			g_ExitActionStrings[NSSM_EXIT_REALLY], NULL);
		bGraceful = true;
	}

	/* Signal we are stopping */
	if (bGraceful) {
		pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		pNSSMService->m_ServiceStatus.dwWaitHint = NSSM_WAITHINT_MARGIN;
		SetServiceStatus(
			pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);
	}

	/* Nothing to do if service isn't running */
	if (pNSSMService->m_uPID) {
		/* Shut down service */
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_TERMINATEPROCESS,
			pNSSMService->m_Name, pNSSMService->m_ExecutablePath, NULL);
		kill_t k;
		service_kill_t(pNSSMService, &k);
		k.m_uExitcode = 0;
		kill_process(&k);
	} else {
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_PROCESS_ALREADY_STOPPED,
			pNSSMService->m_Name, pNSSMService->m_ExecutablePath, NULL);
	}

	end_service(pNSSMService, true);

	/* Signal we stopped */
	if (bGraceful) {
		pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		wait_for_hooks(pNSSMService, true);
		pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		if (uExitcode) {
			pNSSMService->m_ServiceStatus.dwWin32ExitCode =
				ERROR_SERVICE_SPECIFIC_ERROR;
			pNSSMService->m_ServiceStatus.dwServiceSpecificExitCode = uExitcode;
		} else {
			pNSSMService->m_ServiceStatus.dwWin32ExitCode = NO_ERROR;
			pNSSMService->m_ServiceStatus.dwServiceSpecificExitCode = 0;
		}
		SetServiceStatus(
			pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);
	}

	return uExitcode;
}

/* Callback function triggered when the server exits */
void CALLBACK end_service(void* pArg, unsigned char bWhy)
{
	nssm_service_t* pNSSMService = static_cast<nssm_service_t*>(pArg);

	if (pNSSMService->m_bStopping) {
		return;
	}

	pNSSMService->m_bStopping = true;

	pNSSMService->m_uRotateStdoutOnline = pNSSMService->m_uRotateStderrOnline =
		NSSM_ROTATE_OFFLINE;

	/* Use now as a dummy exit time. */
	GetSystemTimeAsFileTime(&pNSSMService->m_ProcessExitTime);

	/* Check exit code */
	DWORD uExitcode = 0;
	wchar_t code[16];
	if (pNSSMService->m_hProcess) {
		GetExitCodeProcess(pNSSMService->m_hProcess, &uExitcode);
		pNSSMService->m_uExitcode = uExitcode;
		/* Check real exit time. */
		if (uExitcode != STILL_ACTIVE) {
			get_process_exit_time(
				pNSSMService->m_hProcess, &pNSSMService->m_ProcessExitTime);
		}
		CloseHandle(pNSSMService->m_hProcess);
	}

	pNSSMService->m_hProcess = NULL;

	/*
	  Log that the service ended BEFORE logging about killing the process
	  tree.  See below for the possible values of the why argument.
	*/
	if (!bWhy) {
		StringCchPrintfW(code, RTL_NUMBER_OF(code), L"%lu", uExitcode);
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ENDED_SERVICE,
			pNSSMService->m_ExecutablePath, pNSSMService->m_Name, code, NULL);
	}

	/* Clean up. */
	if (uExitcode == STILL_ACTIVE) {
		uExitcode = 0;
	}
	if (pNSSMService->m_uPID && pNSSMService->m_bKillProcessTree) {
		kill_t k;
		service_kill_t(pNSSMService, &k);
		kill_process_tree(&k, pNSSMService->m_uPID);
	}
	pNSSMService->m_uPID = 0;

	/* Exit hook. */
	pNSSMService->m_uExitCount++;
	nssm_hook(&g_HookThreads, pNSSMService, g_NSSMHookEventExit,
		g_NSSMHookActionPost, NULL, NSSM_HOOK_DEADLINE, true);

	/* Exit logging threads. */
	cleanup_loggers(pNSSMService);

	/*
	  The why argument is true if our wait timed out or false otherwise.
	  Our wait is infinite so why will never be true when called by the system.
	  If it is indeed true, assume we were called from stop_service() because
	  this is a controlled shutdown, and don't take any restart action.
	*/
	if (bWhy) {
		return;
	}
	if (!pNSSMService->m_bAllowRestart) {
		return;
	}

	/* What action should we take? */
	int action = NSSM_EXIT_RESTART;
	wchar_t action_string[ACTION_LEN];
	bool bDefaultAction;
	if (!get_exit_action(pNSSMService->m_Name, (uint32_t*)&uExitcode,
			action_string, &bDefaultAction)) {
		for (int i = 0; g_ExitActionStrings[i]; i++) {
			if (!_wcsnicmp(action_string, g_ExitActionStrings[i], ACTION_LEN)) {
				action = i;
				break;
			}
		}
	}

	switch (action) {
	/* Try to restart the service or return failure code to service manager */
	case NSSM_EXIT_RESTART:
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_RESTART,
			pNSSMService->m_Name, code, g_ExitActionStrings[action],
			pNSSMService->m_ExecutablePath, NULL);
		while (monitor_service(pNSSMService)) {
			log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_RESTART_SERVICE_FAILED,
				pNSSMService->m_ExecutablePath, pNSSMService->m_Name, NULL);
			Sleep(30000);
		}
		break;

	/* Do nothing, just like srvany would */
	case NSSM_EXIT_IGNORE:
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_IGNORE,
			pNSSMService->m_Name, code, g_ExitActionStrings[action],
			pNSSMService->m_ExecutablePath, NULL);
		wait_for_hooks(pNSSMService, false);
		Sleep(INFINITE);
		break;

	/* Tell the service manager we are finished */
	case NSSM_EXIT_REALLY:
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_REALLY,
			pNSSMService->m_Name, code, g_ExitActionStrings[action], NULL);
		stop_service(pNSSMService, uExitcode, true, bDefaultAction);
		break;

	/* Fake a crash so pre-Vista service managers will run recovery actions. */
	case NSSM_EXIT_UNCLEAN:
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_UNCLEAN,
			pNSSMService->m_Name, code, g_ExitActionStrings[action], NULL);
		stop_service(pNSSMService, uExitcode, false, bDefaultAction);
		wait_for_hooks(pNSSMService, false);
		nssm_exit(static_cast<int>(uExitcode));
	}
}

void throttle_restart(nssm_service_t* pNSSMService)
{
	/* This can't be a restart if the service is already running. */
	if (!pNSSMService->m_uThrottle++) {
		return;
	}

	uint32_t ms;
	uint32_t throttle_ms = throttle_milliseconds(pNSSMService->m_uThrottle);
	wchar_t threshold[8];
	wchar_t milliseconds[8];

	if (pNSSMService->m_uRestartDelay > throttle_ms) {
		ms = pNSSMService->m_uRestartDelay;
	} else {
		ms = throttle_ms;
	}

	StringCchPrintfW(milliseconds, RTL_NUMBER_OF(milliseconds), L"%lu", ms);

	if ((pNSSMService->m_uThrottle == 1) &&
		(pNSSMService->m_uRestartDelay > throttle_ms)) {
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_RESTART_DELAY,
			pNSSMService->m_Name, milliseconds, NULL);
	} else {
		StringCchPrintfW(threshold, RTL_NUMBER_OF(threshold), L"%lu",
			pNSSMService->m_uThrottleDelay);
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_THROTTLED,
			pNSSMService->m_Name, threshold, milliseconds, NULL);
	}

	if (g_bUseCriticalSection) {
		EnterCriticalSection(&pNSSMService->m_ThrottleSection);
	} else if (pNSSMService->m_hThrottleTimer) {
		ZeroMemory(&pNSSMService->m_iThrottleDuetime,
			sizeof(pNSSMService->m_iThrottleDuetime));
		pNSSMService->m_iThrottleDuetime.QuadPart = 0 - (ms * 10000LL);
		SetWaitableTimer(pNSSMService->m_hThrottleTimer,
			&pNSSMService->m_iThrottleDuetime, 0, 0, 0, 0);
	}

	pNSSMService->m_ServiceStatus.dwCurrentState = SERVICE_PAUSED;
	pNSSMService->m_ServiceStatus.dwControlsAccepted |=
		SERVICE_ACCEPT_PAUSE_CONTINUE;
	SetServiceStatus(
		pNSSMService->m_hStatusHandle, &pNSSMService->m_ServiceStatus);

	if (g_bUseCriticalSection) {
		g_Imports.SleepConditionVariableCS(&pNSSMService->m_ThrottleCondition,
			&pNSSMService->m_ThrottleSection, ms);
		LeaveCriticalSection(&pNSSMService->m_ThrottleSection);
	} else {
		if (pNSSMService->m_hThrottleTimer) {
			WaitForSingleObject(pNSSMService->m_hThrottleTimer, INFINITE);
		} else {
			Sleep(ms);
		}
	}
}

/*
  When responding to a stop (or any other) request we need to set dwWaitHint to
  the number of milliseconds we expect the operation to take, and optionally
  increase dwCheckPoint.  If dwWaitHint milliseconds elapses without the
  operation completing or dwCheckPoint increasing, the system will consider the
  service to be hung.

  However the system will consider the service to be hung after 30000
  milliseconds regardless of the value of dwWaitHint if dwCheckPoint has not
  changed.  Therefore if we want to wait longer than that we must periodically
  increase dwCheckPoint.

  Furthermore, it will consider the service to be hung after 60000 milliseconds
  regardless of the value of dwCheckPoint unless dwWaitHint is increased every
  time dwCheckPoint is also increased.

  Our strategy then is to retrieve the initial dwWaitHint and wait for
  NSSM_SERVICE_STATUS_DEADLINE milliseconds.  If the process is still running
  and we haven't finished waiting we increment dwCheckPoint and add whichever is
  smaller of NSSM_SERVICE_STATUS_DEADLINE or the remaining timeout to
  dwWaitHint.

  Only doing both these things will prevent the system from killing the service.

  If the status_handle and service_status arguments are omitted, this function
  will not try to update the service manager but it will still log to the
  event log that it is waiting for a handle.

  Returns: 1 if the wait timed out.
		   0 if the wait completed.
		  -1 on error.
*/
int await_single_handle(SERVICE_STATUS_HANDLE hStatusHandle,
	SERVICE_STATUS* pServiceStatus, HANDLE hHandle, const wchar_t* pName,
	const wchar_t* pFunctionName, uint32_t uTimeout)
{

	wchar_t interval_milliseconds[16];
	wchar_t timeout_milliseconds[16];
	wchar_t waited_milliseconds[16];
	const wchar_t* pFunction = pFunctionName;

	/* Add brackets to function name. */
	uintptr_t funclen = wcslen(pFunctionName) + 3;
	wchar_t* func =
		static_cast<wchar_t*>(heap_alloc(funclen * sizeof(wchar_t)));
	if (func) {
		if (StringCchPrintfW(func, funclen, L"%s()", pFunctionName) > -1) {
			pFunction = func;
		}
	}

	StringCchPrintfW(timeout_milliseconds, RTL_NUMBER_OF(timeout_milliseconds),
		L"%lu", uTimeout);

	int ret;
	uint32_t waited = 0;
	while (waited < uTimeout) {
		uint32_t interval = uTimeout - waited;
		if (interval > NSSM_SERVICE_STATUS_DEADLINE) {
			interval = NSSM_SERVICE_STATUS_DEADLINE;
		}

		if (pServiceStatus) {
			pServiceStatus->dwWaitHint += interval;
			pServiceStatus->dwCheckPoint++;
			SetServiceStatus(hStatusHandle, pServiceStatus);
		}

		if (waited) {
			StringCchPrintfW(waited_milliseconds,
				RTL_NUMBER_OF(waited_milliseconds), L"%lu", waited);
			StringCchPrintfW(interval_milliseconds,
				RTL_NUMBER_OF(interval_milliseconds), L"%lu", interval);
			log_event(EVENTLOG_INFORMATION_TYPE,
				NSSM_EVENT_AWAITING_SINGLE_HANDLE, pFunction, pName,
				waited_milliseconds, interval_milliseconds,
				timeout_milliseconds, NULL);
		}

		switch (WaitForSingleObject(hHandle, interval)) {
		case WAIT_OBJECT_0:
			ret = 0;
			goto awaited;

		case WAIT_TIMEOUT:
			ret = 1;
			break;

		default:
			ret = -1;
			goto awaited;
		}

		waited += interval;
	}

awaited:
	if (func) {
		heap_free(func);
	}
	return ret;
}

int list_nssm_services(int iArgc, wchar_t** ppArgv)
{
	bool including_native = (iArgc > 0 && str_equiv(ppArgv[0], L"all"));

	/* Open service manager. */
	SC_HANDLE hServices =
		open_service_manager(SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (!hServices) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		return 1;
	}

	DWORD required;
	DWORD count;
	DWORD resume = 0;
	EnumServicesStatusExW(hServices, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
		SERVICE_STATE_ALL, 0, 0, &required, &count, &resume, 0);
	DWORD uError = GetLastError();
	if (uError != ERROR_MORE_DATA) {
		print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
			error_string(uError));
		return 2;
	}

	ENUM_SERVICE_STATUS_PROCESSW* pEnumService =
		static_cast<ENUM_SERVICE_STATUS_PROCESSW*>(heap_alloc(required));
	if (!pEnumService) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
			L"ENUM_SERVICE_STATUS_PROCESS", L"list_nssm_services()");
		return 3;
	}

	DWORD bufsize = required;
	while (true) {
		int ret = EnumServicesStatusExW(hServices, SC_ENUM_PROCESS_INFO,
			SERVICE_WIN32, SERVICE_STATE_ALL,
			reinterpret_cast<LPBYTE>(pEnumService), bufsize, &required, &count,
			&resume, 0);
		if (!ret) {
			uError = GetLastError();
			if (uError != ERROR_MORE_DATA) {
				heap_free(pEnumService);
				print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
					error_string(uError));
				return 4;
			}
		}

		DWORD i;
		for (i = 0; i < count; i++) {
			/* Try to get the service parameters. */
			nssm_service_t* pNSSMService = alloc_nssm_service();
			if (!pNSSMService) {
				heap_free(pEnumService);
				print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
					L"nssm_service_t", L"list_nssm_services()");
				return 5;
			}
			StringCchPrintfW(pNSSMService->m_Name,
				RTL_NUMBER_OF(pNSSMService->m_Name), L"%s",
				pEnumService[i].lpServiceName);

			get_parameters(pNSSMService, 0);
			/* We manage the service if we have an Application. */
			if (including_native || pNSSMService->m_ExecutablePath[0]) {
				wprintf(L"%s\n", pNSSMService->m_Name);
			}

			cleanup_nssm_service(pNSSMService);
		}

		if (ret) {
			break;
		}
	}

	heap_free(pEnumService);
	return 0;
}

int service_process_tree(int iArgc, wchar_t** ppArgv)
{
	if (iArgc < 1) {
		return usage(1);
	}

	SC_HANDLE hOpenServices = open_service_manager(SC_MANAGER_CONNECT);
	if (!hOpenServices) {
		print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		return 1;
	}

	/*
	  We need SeDebugPrivilege to read the process tree.
	  We ignore failure here so that an error will be printed later when we
	  try to open a process handle.
	*/
	HANDLE hDebugToken = get_debug_token();

	wchar_t canonical_name[SERVICE_NAME_LENGTH];
	SERVICE_STATUS_PROCESS service_status;
	kill_t k;
	int errors = 0;
	int i;
	for (i = 0; i < iArgc; i++) {
		const wchar_t* pServiceName = ppArgv[i];
		SC_HANDLE hService =
			open_service(hOpenServices, pServiceName, SERVICE_QUERY_STATUS,
				canonical_name, RTL_NUMBER_OF(canonical_name));
		if (!hService) {
			errors++;
			continue;
		}

		unsigned long size;
		int ret = QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO,
			reinterpret_cast<LPBYTE>(&service_status), sizeof(service_status),
			&size);
		DWORD error = GetLastError();
		CloseServiceHandle(hService);

		if (!ret) {
			fwprintf(stderr, L"%s: %s\n", canonical_name, error_string(error));
			errors++;
			continue;
		}

		ZeroMemory(&k, sizeof(k));
		k.m_uPID = service_status.dwProcessId;
		if (!k.m_uPID) {
			continue;
		}

		k.m_hProcess = OpenProcess(
			PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, k.m_uPID);
		if (!k.m_hProcess) {
			fwprintf(stderr, L"%s: %lu: %s\n", canonical_name, k.m_uPID,
				error_string(GetLastError()));
			continue;
		}

		if (get_process_creation_time(k.m_hProcess, &k.m_uCreationTime)) {
			continue;
		}
		/* Dummy exit time so we can check processes' parents. */
		GetSystemTimeAsFileTime(&k.m_uExitTime);

		nssm_service_t* pNSSMService = alloc_nssm_service();
		if (!pNSSMService) {
			errors++;
			continue;
		}

		StringCchPrintfW(pNSSMService->m_Name,
			RTL_NUMBER_OF(pNSSMService->m_Name), L"%s", canonical_name);
		k.m_pName = pNSSMService->m_Name;
		walk_process_tree(pNSSMService, print_process, &k, k.m_uPID);

		cleanup_nssm_service(pNSSMService);
	}

	CloseServiceHandle(hOpenServices);
	if (hDebugToken != INVALID_HANDLE_VALUE) {
		CloseHandle(hDebugToken);
	}
	return errors;
}

void alloc_console(nssm_service_t* pNSSMService)
{
	if (!pNSSMService->m_bDontSpawnConsole) {
		AllocConsole();
	}
}
