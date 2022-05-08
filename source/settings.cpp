/***************************************

	settings manager

***************************************/

#include "settings.h"
#include "account.h"
#include "constants.h"
#include "env.h"
#include "event.h"
#include "hook.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "nssm_io.h"
#include "registry.h"
#include "service.h"

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
#endif

/* XXX: (value && value->string) is probably bogus because value is probably
 * never null */

/***************************************

	Does the parameter refer to the default value of the setting?

***************************************/

static inline int is_default(const wchar_t* pValue)
{
	return (str_equiv(pValue, g_NSSMDefaultString) || str_equiv(pValue, L"*") ||
		!pValue[0]);
}

/***************************************

	What type of parameter is it parameter?

***************************************/

static inline bool is_string_type(const uint32_t uType)
{
	return (uType == REG_MULTI_SZ) || (uType == REG_EXPAND_SZ) ||
		(uType == REG_SZ);
}

/***************************************

	What type of parameter is it parameter?

***************************************/

static inline bool is_numeric_type(const uint32_t uType)
{
	return (uType == REG_DWORD);
}

/***************************************

	Get the string value

***************************************/

static int value_from_string(
	const wchar_t* pName, value_t* pValue, const wchar_t* pString)
{
	uintptr_t uStringLength = wcslen(pString);
	if (!uStringLength) {
		pValue->m_pString = NULL;
		return 0;
	}
	++uStringLength;

	pValue->m_pString =
		static_cast<wchar_t*>(heap_alloc(uStringLength * sizeof(wchar_t)));
	if (!pValue->m_pString) {
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, pName, L"value_from_string()");
		return -1;
	}

	if (StringCchPrintfW(pValue->m_pString, uStringLength, L"%s", pString) <
		0) {
		heap_free(pValue->m_pString);
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, pName, L"value_from_string()");
		return -1;
	}

	return 1;
}

/***************************************

	Functions to manage NSSM-specific settings in the registry.

***************************************/

static int setting_set_number(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey)
		return -1;

	uint32_t uNumber;
	LONG iError;

	/* Resetting to default? */
	if (!pValue || !pValue->m_pString) {
		iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}
	if (str_number(pValue->m_pString, &uNumber)) {
		return -1;
	}

	if (pDefaultValue &&
		uNumber ==
			static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pDefaultValue))) {
		iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}

	if (set_number(hKey, pName, uNumber)) {
		return -1;
	}
	return 1;
}

static int setting_get_number(const wchar_t* /* pServiceName */, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	return get_number(hKey, pName, &pValue->m_uNumber, false);
}

static int setting_set_string(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	LONG iError;

	/* Resetting to default? */
	if (!pValue || !pValue->m_pString) {
		if (pDefaultValue) {
			pValue->m_pString = static_cast<wchar_t*>(pDefaultValue);
		} else {
			iError = RegDeleteValueW(hKey, pName);
			if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
				return 0;
			}
			print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
				pServiceName, error_string(static_cast<uint32_t>(iError)));
			return -1;
		}
	}
	if (pDefaultValue && wcslen(static_cast<wchar_t*>(pDefaultValue)) &&
		str_equiv(pValue->m_pString, static_cast<wchar_t*>(pDefaultValue))) {
		iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}

	if (set_expand_string(hKey, pName, pValue->m_pString)) {
		return -1;
	}

	return 1;
}

static int setting_get_string(const wchar_t* /* pServiceName */, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	wchar_t Buffer[VALUE_LENGTH];

	if (get_string(hKey, pName, Buffer, static_cast<uint32_t>(sizeof(Buffer)),
			false, false, false)) {
		return -1;
	}

	return value_from_string(pName, pValue, Buffer);
}

static int setting_not_dumpable(const wchar_t* /* pServiceName */,
	void* /* pParam */, const wchar_t* /* pName */, void* /* pDefaultValue */,
	value_t* /* pValue */, const wchar_t* /* pAdditional */)
{
	return 0;
}

static int setting_dump_string(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, const value_t* pValue, const wchar_t* pAdditional)
{
	wchar_t quoted_service_name[SERVICE_NAME_LENGTH * 2];
	wchar_t quoted_value[VALUE_LENGTH * 2];
	wchar_t quoted_additional[VALUE_LENGTH * 2];
	wchar_t quoted_nssm[EXE_LENGTH * 2];

	if (quote(pServiceName, quoted_service_name,
			RTL_NUMBER_OF(quoted_service_name))) {
		return 1;
	}

	if (pAdditional) {
		if (wcslen(pAdditional)) {
			if (quote(pAdditional, quoted_additional,
					RTL_NUMBER_OF(quoted_additional))) {
				return 3;
			}
		} else {
			StringCchPrintfW(
				quoted_additional, RTL_NUMBER_OF(quoted_additional), L"\"\"");
		}
	} else {
		quoted_additional[0] = 0;
	}

	uint32_t uType = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pParam));
	if (is_string_type(uType)) {
		if (wcslen(pValue->m_pString)) {
			if (quote(pValue->m_pString, quoted_value,
					RTL_NUMBER_OF(quoted_value))) {
				return 2;
			}
		} else {
			StringCchPrintfW(
				quoted_value, RTL_NUMBER_OF(quoted_value), L"\"\"");
		}
	} else if (is_numeric_type(uType)) {
		StringCchPrintfW(quoted_value, RTL_NUMBER_OF(quoted_value), L"%lu",
			pValue->m_uNumber);
	} else {
		return 2;
	}

	if (quote(nssm_exe(), quoted_nssm, RTL_NUMBER_OF(quoted_nssm))) {
		return 3;
	}
	if (wcslen(quoted_additional)) {
		wprintf(L"%s set %s %s %s %s\n", quoted_nssm, quoted_service_name,
			pName, quoted_additional, quoted_value);
	} else {
		wprintf(L"%s set %s %s %s\n", quoted_nssm, quoted_service_name, pName,
			quoted_value);
	}
	return 0;
}

static int setting_set_exit_action(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	uint32_t uExitcode;
	const wchar_t* pCode;
	wchar_t ActionString[ACTION_LEN];

	if (pAdditional) {
		// Default action?
		if (is_default(pAdditional)) {
			pCode = NULL;
		} else {
			if (str_number(pAdditional, &uExitcode)) {
				return -1;
			}
			pCode = static_cast<const wchar_t*>(pAdditional);
		}
	}

	HKEY hKey = open_registry(pServiceName, pName, KEY_WRITE);
	if (!hKey) {
		return -1;
	}
	LONG iError;
	int iResult = 1;

	// Resetting to default?
	if (pValue && pValue->m_pString) {
		StringCchPrintfW(ActionString, RTL_NUMBER_OF(ActionString), L"%s",
			pValue->m_pString);
	} else {
		if (pCode) {
			// Delete explicit action.
			iError = RegDeleteValueW(hKey, pCode);
			RegCloseKey(hKey);
			if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
				return 0;
			}
			print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pCode,
				pServiceName, error_string(static_cast<uint32_t>(iError)));
			return -1;
		} else {
			// Explicitly keep the default action.
			if (pDefaultValue) {
				StringCchPrintfW(ActionString, RTL_NUMBER_OF(ActionString),
					L"%s", pDefaultValue);
			}
			iResult = 0;
		}
	}
	uint32_t i;
	// Validate the string.
	for (i = 0; g_ExitActionStrings[i]; i++) {
		if (!_wcsnicmp(ActionString, g_ExitActionStrings[i], ACTION_LEN)) {
			if (pDefaultValue &&
				str_equiv(ActionString, static_cast<wchar_t*>(pDefaultValue))) {
				iResult = 0;
			}
			if (RegSetValueExW(hKey, pCode, 0, REG_SZ,
					reinterpret_cast<const BYTE*>(g_ExitActionStrings[i]),
					static_cast<DWORD>((wcslen(ActionString) + 1) *
						sizeof(wchar_t))) != ERROR_SUCCESS) {
				print_message(stderr, NSSM_MESSAGE_SETVALUE_FAILED, pCode,
					pServiceName, error_string(GetLastError()));
				RegCloseKey(hKey);
				return -1;
			}

			RegCloseKey(hKey);
			return iResult;
		}
	}

	print_message(stderr, NSSM_MESSAGE_INVALID_EXIT_ACTION, ActionString);
	for (i = 0; g_ExitActionStrings[i]; i++) {
		fwprintf(stderr, L"%s\n", g_ExitActionStrings[i]);
	}
	return -1;
}

static int setting_get_exit_action(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	uint32_t uExitcode = 0;
	uint32_t* pCode = NULL;

	if (pAdditional) {
		if (!is_default(pAdditional)) {
			if (str_number(pAdditional, &uExitcode)) {
				return -1;
			}
			pCode = &uExitcode;
		}
	}

	wchar_t ActionString[ACTION_LEN];
	bool bDefaultAction;
	if (get_exit_action(pServiceName, pCode, ActionString, &bDefaultAction)) {
		return -1;
	}

	value_from_string(pName, pValue, ActionString);

	if (bDefaultAction &&
		!_wcsnicmp(ActionString, static_cast<const wchar_t*>(pDefaultValue),
			ACTION_LEN)) {
		return 0;
	}
	return 1;
}

static int setting_dump_exit_action(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	HKEY hKey = open_registry(pServiceName, g_NSSMRegExit, KEY_READ);
	if (!hKey) {
		return -1;
	}

	wchar_t CodeBuffer[16];
	uint32_t uIndex = 0;
	uint32_t uErrors = 0;
	for (;;) {
		int iReturn = enumerate_registry_values(
			hKey, &uIndex, CodeBuffer, RTL_NUMBER_OF(CodeBuffer));
		if (iReturn == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (iReturn != ERROR_SUCCESS) {
			continue;
		}
		bool bValid = true;
		uint32_t i;
		for (i = 0; i < RTL_NUMBER_OF(CodeBuffer); i++) {
			if (!CodeBuffer[i]) {
				break;
			}
			if (CodeBuffer[i] >= L'0' || CodeBuffer[i] <= L'9') {
				continue;
			}
			bValid = false;
			break;
		}
		if (!bValid) {
			continue;
		}

		const wchar_t* additional =
			CodeBuffer[i] ? CodeBuffer : g_NSSMDefaultString;

		iReturn = setting_get_exit_action(
			pServiceName, NULL, pName, pDefaultValue, pValue, pAdditional);
		if (iReturn == 1) {
			if (setting_dump_string(pServiceName,
					reinterpret_cast<void*>(REG_SZ), pName, pValue,
					pAdditional)) {
				++uErrors;
			}
		} else if (iReturn < 0) {
			++uErrors;
		}
	}

	if (uErrors) {
		return -1;
	}
	return 0;
}

static inline bool split_hook_name(
	const wchar_t* pHookName, wchar_t* pHookEvent, wchar_t* pHookAction)
{
	wchar_t* s;

	for (s = const_cast<wchar_t*>(pHookName); *s; s++) {
		if (*s == L'/') {
			*s = 0;
			StringCchPrintfW(pHookEvent, HOOK_NAME_LENGTH, L"%s", pHookName);
			*s++ = L'/';
			StringCchPrintfW(pHookAction, HOOK_NAME_LENGTH, L"%s", s);
			return valid_hook_name(pHookEvent, pHookAction, false);
		}
	}

	print_message(stderr, NSSM_MESSAGE_INVALID_HOOK_NAME, pHookName);
	return false;
}

static int setting_set_hook(const wchar_t* pServiceName, void* /* pParam */,
	const wchar_t* /* pName */, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* pAdditional)
{
	wchar_t HookEvent[HOOK_NAME_LENGTH];
	wchar_t HookAction[HOOK_NAME_LENGTH];
	if (!split_hook_name(pAdditional, HookEvent, HookAction)) {
		return -1;
	}

	wchar_t* cmd;
	if (pValue && pValue->m_pString) {
		cmd = pValue->m_pString;
	} else {
		cmd = L"";
	}
	if (set_hook(pServiceName, HookEvent, HookAction, cmd)) {
		return -1;
	}
	if (!wcslen(cmd)) {
		return 0;
	}
	return 1;
}

static int setting_get_hook(const wchar_t* pServiceName, void* /* pParam */,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* pAdditional)
{
	wchar_t HookEvent[HOOK_NAME_LENGTH];
	wchar_t HookAction[HOOK_NAME_LENGTH];
	if (!split_hook_name(pAdditional, HookEvent, HookAction)) {
		return -1;
	}

	wchar_t CommandLine[CMD_LENGTH];
	if (get_hook(pServiceName, HookEvent, HookAction, CommandLine,
			sizeof(CommandLine))) {
		return -1;
	}

	value_from_string(pName, pValue, CommandLine);

	if (!wcslen(CommandLine)) {
		return 0;
	}
	return 1;
}

static int setting_dump_hooks(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	uint32_t i, j;

	uint32_t uErrors = 0;
	for (i = 0; g_HookEventStrings[i]; i++) {
		const wchar_t* pHookEvent = g_HookEventStrings[i];
		for (j = 0; g_HookActionStrings[j]; j++) {
			const wchar_t* hook_action = g_HookActionStrings[j];
			if (!valid_hook_name(pHookEvent, hook_action, true)) {
				continue;
			}

			wchar_t HookName[HOOK_NAME_LENGTH];
			StringCchPrintfW(HookName, RTL_NUMBER_OF(HookName), L"%s/%s",
				pHookEvent, hook_action);

			int iReturn = setting_get_hook(
				pServiceName, pParam, pName, pDefaultValue, pValue, HookName);
			if (iReturn != 1) {
				if (iReturn < 0) {
					++uErrors;
				}
				continue;
			}

			if (setting_dump_string(pServiceName,
					reinterpret_cast<void*>(REG_SZ), pName, pValue, HookName)) {
				++uErrors;
			}
		}
	}

	if (uErrors) {
		return -1;
	}
	return 0;
}

static int setting_set_affinity(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	uint64_t uMask;
	DWORD_PTR uSystemAffinity = 0ULL;

	if (pValue && pValue->m_pString) {
		DWORD_PTR uAffinity;
		if (!GetProcessAffinityMask(
				GetCurrentProcess(), &uAffinity, &uSystemAffinity)) {
			uSystemAffinity = UINTPTR_MAX;
		}

		if (is_default(pValue->m_pString) ||
			str_equiv(pValue->m_pString, g_AffinityAll)) {
			uMask = 0LL;
		} else if (affinity_string_to_mask(pValue->m_pString, &uMask)) {
			print_message(stderr, NSSM_MESSAGE_BOGUS_AFFINITY_MASK,
				pValue->m_pString, num_cpus() - 1);
			return -1;
		}
	} else {
		uMask = 0ULL;
	}

	if (!uMask) {
		long iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}

	// Canonicalise.
	wchar_t* pCanon = NULL;
	if (affinity_mask_to_string(uMask, &pCanon)) {
		pCanon = pValue->m_pString;
	}

	uint64_t uEffectiveAffinity = uMask & uSystemAffinity;
	if (uEffectiveAffinity != uMask) {
		// Requested CPUs did not intersect with available CPUs?
		if (!uEffectiveAffinity) {
			uMask = uEffectiveAffinity = uSystemAffinity;
		}

		wchar_t* pSystem = NULL;
		if (!affinity_mask_to_string(uSystemAffinity, &pSystem)) {
			wchar_t* pEffective = NULL;
			if (!affinity_mask_to_string(uEffectiveAffinity, &pEffective)) {
				print_message(stderr, NSSM_MESSAGE_EFFECTIVE_AFFINITY_MASK,
					pValue->m_pString, pSystem, pEffective);
				heap_free(pEffective);
			}
			heap_free(pSystem);
		}
	}

	if (RegSetValueExW(hKey, pName, 0, REG_SZ, (const BYTE*)pCanon,
			static_cast<DWORD>((wcslen(pCanon) + 1) * sizeof(wchar_t))) !=
		ERROR_SUCCESS) {
		if (pCanon != pValue->m_pString) {
			heap_free(pCanon);
		}
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED, pName,
			error_string(GetLastError()), NULL);
		return -1;
	}

	if (pCanon != pValue->m_pString) {
		heap_free(pCanon);
	}
	return 1;
}

static int setting_get_affinity(const wchar_t* /* pServiceName */, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	DWORD uType;
	DWORD uBufferLength = 0;

	int iReturn = RegQueryValueExW(hKey, pName, 0, &uType, 0, &uBufferLength);
	if (iReturn == ERROR_FILE_NOT_FOUND) {
		if (value_from_string(pName, pValue, g_AffinityAll) == 1) {
			return 0;
		}
		return -1;
	}
	if (iReturn != ERROR_SUCCESS) {
		return -1;
	}

	if (uType != REG_SZ) {
		return -1;
	}

	wchar_t* pBuffer = static_cast<wchar_t*>(heap_alloc(uBufferLength));
	if (!pBuffer) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"affinity",
			L"setting_get_affinity");
		return -1;
	}

	if (get_string(hKey, pName, pBuffer, uBufferLength, false, false, true)) {
		heap_free(pBuffer);
		return -1;
	}

	uint64_t uAffinity;
	if (affinity_string_to_mask(pBuffer, &uAffinity)) {
		print_message(
			stderr, NSSM_MESSAGE_BOGUS_AFFINITY_MASK, pBuffer, num_cpus() - 1);
		heap_free(pBuffer);
		return -1;
	}

	heap_free(pBuffer);

	/* Canonicalise. */
	if (affinity_mask_to_string(uAffinity, &pBuffer)) {
		if (pBuffer) {
			heap_free(pBuffer);
		}
		return -1;
	}

	iReturn = value_from_string(pName, pValue, pBuffer);
	heap_free(pBuffer);
	return iReturn;
}

static int setting_set_environment(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	wchar_t* pString = NULL;
	int iOperation = 0;
	if (pValue && pValue->m_pString && pValue->m_pString[0]) {
		pString = pValue->m_pString;
		switch (pString[0]) {
		case L'+':
			iOperation = 1;
			break;
		case L'-':
			iOperation = -1;
			break;
		case L':':
			++pString;
			break;
		}
	}

	wchar_t* pUnformatted = NULL;
	uintptr_t uEnvironmentLength;
	uintptr_t uNewLength = 0;
	if (iOperation) {
		++pString;
		wchar_t* pEnvironment = NULL;
		if (get_environment(pServiceName, hKey, pName, &pEnvironment,
				&uEnvironmentLength)) {
			return -1;
		}
		if (pEnvironment) {
			int iResult;
			if (iOperation > 0) {
				iResult = append_to_environment_block(pEnvironment,
					uEnvironmentLength, pString, &pUnformatted, &uNewLength);
			} else {
				iResult = remove_from_environment_block(pEnvironment,
					uEnvironmentLength, pString, &pUnformatted, &uNewLength);
			}
			if (uEnvironmentLength) {
				heap_free(pEnvironment);
			}
			if (iResult) {
				return -1;
			}
			pString = pUnformatted;
		} else {
			/*
			  No existing environment.
			  We can't remove from an empty environment so just treat an add
			  operation as setting a new string.
			*/
			if (iOperation < 0) {
				return 0;
			}
			iOperation = 0;
		}
	}

	if (!pString || !pString[0]) {
		long iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}

	if (!iOperation) {
		if (unformat_double_null(
				pString, wcslen(pString), &pUnformatted, &uNewLength))
			return -1;
	}

	if (test_environment(pUnformatted)) {
		heap_free(pUnformatted);
		print_message(stderr, NSSM_GUI_INVALID_ENVIRONMENT);
		return -1;
	}

	if (RegSetValueExW(hKey, pName, 0, REG_MULTI_SZ,
			reinterpret_cast<const BYTE*>(pUnformatted),
			static_cast<DWORD>(uNewLength * sizeof(wchar_t))) !=
		ERROR_SUCCESS) {
		if (uNewLength) {
			heap_free(pUnformatted);
		}
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED, g_NSSMRegEnv,
			error_string(GetLastError()), NULL);
		return -1;
	}

	if (uNewLength) {
		heap_free(pUnformatted);
	}
	return 1;
}

static int setting_get_environment(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* pAdditional)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	wchar_t* pEnvironment = NULL;
	uintptr_t uEnvironmentLength;
	if (get_environment(
			pServiceName, hKey, pName, &pEnvironment, &uEnvironmentLength)) {
		return -1;
	}
	if (!uEnvironmentLength) {
		return 0;
	}

	wchar_t* pFormatted = NULL;
	uintptr_t uNewLength;
	if (format_double_null(
			pEnvironment, uEnvironmentLength, &pFormatted, &uNewLength)) {
		return -1;
	}

	if (pAdditional) {

		// Find named environment variable.
		wchar_t* s;
		uintptr_t uLength = wcslen(pAdditional);
		for (s = pEnvironment; *s; s++) {

			// Look for <additional>=<string> NULL NULL
			if (!_wcsnicmp(s, pAdditional, uLength) && s[uLength] == L'=') {
				// Strip <key>=
				s += uLength + 1U;
				int iRet = value_from_string(pName, pValue, s);
				heap_free(pEnvironment);
				return iRet;
			}

			// Skip this string.
			for (; *s; s++) {
			}
		}
		heap_free(pEnvironment);
		return 0;
	}

	heap_free(pEnvironment);

	int iReturn = value_from_string(pName, pValue, pFormatted);
	if (uNewLength) {
		heap_free(pFormatted);
	}
	return iReturn;
}

static int setting_dump_environment(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	wchar_t* pEnvironment = NULL;
	uintptr_t uEnvironmentLength;
	if (get_environment(
			pServiceName, hKey, pName, &pEnvironment, &uEnvironmentLength)) {
		return -1;
	}
	if (!uEnvironmentLength) {
		return 0;
	}

	uint32_t uErrors = 0;
	wchar_t* s;
	for (s = pEnvironment; *s; s++) {
		uintptr_t uLength = wcslen(s) + 2U;
		pValue->m_pString =
			static_cast<wchar_t*>(heap_alloc(uLength * sizeof(wchar_t)));
		if (!pValue->m_pString) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"dump",
				L"setting_dump_environment");
			break;
		}

		StringCchPrintfW(pValue->m_pString, uLength, L"%c%s",
			(s > pEnvironment) ? L'+' : L':', s);
		if (setting_dump_string(pServiceName, reinterpret_cast<void*>(REG_SZ),
				pName, pValue, NULL)) {
			++uErrors;
		}
		heap_free(pValue->m_pString);
		pValue->m_pString = NULL;

		for (; *s; s++) {
		}
	}

	heap_free(pEnvironment);

	if (uErrors) {
		return 1;
	}
	return 0;
}

static int setting_set_priority(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	wchar_t* pPriorityString;

	long iError;
	if (pValue && pValue->m_pString) {
		pPriorityString = pValue->m_pString;
	} else if (pDefaultValue) {
		pPriorityString = static_cast<wchar_t*>(pDefaultValue);
	} else {
		iError = RegDeleteValueW(hKey, pName);
		if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
			return 0;
		}
		print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
			pServiceName, error_string(static_cast<uint32_t>(iError)));
		return -1;
	}

	uint32_t i;
	for (i = 0; g_PriorityStrings[i]; i++) {
		if (!str_equiv(g_PriorityStrings[i], pPriorityString)) {
			continue;
		}

		if (pDefaultValue &&
			str_equiv(pPriorityString, static_cast<wchar_t*>(pDefaultValue))) {
			iError = RegDeleteValueW(hKey, pName);
			if ((iError == ERROR_SUCCESS) || (iError == ERROR_FILE_NOT_FOUND)) {
				return 0;
			}
			print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, pName,
				pServiceName, error_string(static_cast<uint32_t>(iError)));
			return -1;
		}

		if (set_number(hKey, pName, priority_index_to_constant(i))) {
			return -1;
		}
		return 1;
	}

	print_message(stderr, NSSM_MESSAGE_INVALID_PRIORITY, pPriorityString);
	for (i = 0; g_PriorityStrings[i]; i++) {
		fwprintf(stderr, L"%s\n", g_PriorityStrings[i]);
	}
	return -1;
}

static int setting_get_priority(const wchar_t* /* pServiceName */, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	HKEY hKey = static_cast<HKEY>(pParam);
	if (!hKey) {
		return -1;
	}

	uint32_t uConstant;
	switch (get_number(hKey, pName, &uConstant, false)) {
	case 0:
		if (value_from_string(pName, pValue,
				static_cast<const wchar_t*>(pDefaultValue)) == -1) {
			return -1;
		}
		return 0;
	case -1:
		return -1;
	}

	return value_from_string(pName, pValue,
		g_PriorityStrings[priority_constant_to_index(uConstant)]);
}

static int setting_dump_priority(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	settings_t* pSettings = static_cast<settings_t*>(pDefaultValue);
	int iReturn = setting_get_priority(
		pServiceName, pParam, pName, pSettings->m_pDefaultValue, pValue, NULL);
	if (iReturn != 1) {
		return iReturn;
	}
	return setting_dump_string(
		pServiceName, reinterpret_cast<void*>(REG_SZ), pName, pValue, NULL);
}

/***************************************

	Functions to manage native service settings.

***************************************/

static int native_set_dependon(const wchar_t* pServiceName,
	SC_HANDLE hServiceHandle, wchar_t** ppDependencies,
	uintptr_t* pDependenciesLength, value_t* pValue, uint32_t uType)
{
	*pDependenciesLength = 0;
	if (!pValue || !pValue->m_pString || !pValue->m_pString[0]) {
		return 0;
	}

	wchar_t* pString = pValue->m_pString;
	int iOperation = 0;
	switch (pString[0]) {
	case L'+':
		iOperation = 1;
		break;
	case L'-':
		iOperation = -1;
		break;
	case L':':
		++pString;
		break;
	}

	uintptr_t uBufferLength;
	if (iOperation) {
		++pString;
		wchar_t* pBuffer = NULL;
		if (get_service_dependencies(pServiceName, hServiceHandle, &pBuffer,
				&uBufferLength, uType)) {
			return -1;
		}
		if (pBuffer) {
			int iResult;
			if (iOperation > 0) {
				iResult = append_to_dependencies(pBuffer, uBufferLength,
					pString, ppDependencies, pDependenciesLength, uType);
			} else {
				iResult = remove_from_dependencies(pBuffer, uBufferLength,
					pString, ppDependencies, pDependenciesLength, uType);
			}
			if (uBufferLength) {
				heap_free(pBuffer);
			}
			return iResult;
		} else {
			/*
			  No existing list.
			  We can't remove from an empty list so just treat an add
			  operation as setting a new string.
			*/
			if (iOperation < 0) {
				return 0;
			}
			iOperation = 0;
		}
	}

	if (!iOperation) {
		wchar_t* pUnformatted = NULL;
		uintptr_t uNewLength;
		if (unformat_double_null(
				pString, wcslen(pString), &pUnformatted, &uNewLength)) {
			return -1;
		}

		if (uType == DEPENDENCY_GROUPS) {
			// Prepend group identifier.
			uintptr_t uMissing = 0;
			wchar_t* pCanon = pUnformatted;
			uintptr_t uCanonLength = 0;
			wchar_t* s;
			for (s = pUnformatted; *s; s++) {
				if (*s != SC_GROUP_IDENTIFIERW) {
					++uMissing;
				}
				uintptr_t uTempLength = wcslen(s);
				uCanonLength += uTempLength + 1;
				s += uTempLength;
			}

			if (uMissing) {
				// Missing identifiers plus double NULL terminator.
				uCanonLength += uMissing + 1;

				pCanon = static_cast<wchar_t*>(
					heap_calloc(uCanonLength * sizeof(wchar_t)));
				if (!pCanon) {
					print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"canon",
						L"native_set_dependon");
					if (pUnformatted) {
						heap_free(pUnformatted);
					}
					return -1;
				}

				uintptr_t i = 0;
				for (s = pUnformatted; *s; s++) {
					if (*s != SC_GROUP_IDENTIFIERW) {
						pCanon[i++] = SC_GROUP_IDENTIFIERW;
					}
					uintptr_t uLen = wcslen(s);
					memmove(pCanon + i, s, (uLen + 1) * sizeof(wchar_t));
					i += uLen + 1;
					s += uLen;
				}

				heap_free(pUnformatted);
				pUnformatted = pCanon;
				uNewLength = uCanonLength;
			}
		}

		*ppDependencies = pUnformatted;
		*pDependenciesLength = uNewLength;
	}

	return 0;
}

static int native_set_dependongroup(const wchar_t* pServiceName, void* pParam,
	const wchar_t* /* pName */, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/*
	  Get existing service dependencies because we must set both types together.
	*/
	wchar_t* pServicesBuffer;
	uintptr_t uServicesLength;
	if (get_service_dependencies(pServiceName, hService, &pServicesBuffer,
			&uServicesLength, DEPENDENCY_SERVICES)) {
		return -1;
	}

	if (!pValue || !pValue->m_pString || !pValue->m_pString[0]) {
		int iExit = 0;
		if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE,
				SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 0, 0, 0, pServicesBuffer,
				0, 0, 0)) {
			print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
				error_string(GetLastError()));
			iExit = -1;
		}

		if (pServicesBuffer) {
			heap_free(pServicesBuffer);
		}
		return iExit;
	}

	// Update the group list.
	wchar_t* pGroups;
	uintptr_t uGroupsLength;
	if (native_set_dependon(pServiceName, hService, &pGroups, &uGroupsLength,
			pValue, DEPENDENCY_GROUPS)) {
		if (pServicesBuffer) {
			heap_free(pServicesBuffer);
		}
		return -1;
	}

	wchar_t* pDependencies;
	if (uServicesLength > 2) {
		pDependencies = static_cast<wchar_t*>(
			heap_alloc((uGroupsLength + uServicesLength) * sizeof(wchar_t)));
		if (!pDependencies) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"dependencies",
				L"native_set_dependongroup");
			if (pGroups) {
				heap_free(pGroups);
			}
			if (pServicesBuffer) {
				heap_free(pServicesBuffer);
			}
			return -1;
		}

		memmove(
			pDependencies, pServicesBuffer, uServicesLength * sizeof(wchar_t));
		memmove(pDependencies + uServicesLength - 1, pGroups,
			uGroupsLength * sizeof(wchar_t));
	} else {
		pDependencies = pGroups;
	}
	int iResult = 1;
	if (set_service_dependencies(pServiceName, hService, pDependencies)) {
		iResult = -1;
	}
	if (pDependencies != pGroups) {
		heap_free(pDependencies);
	}
	if (pGroups) {
		heap_free(pGroups);
	}
	if (pServicesBuffer) {
		heap_free(pServicesBuffer);
	}
	return iResult;
}

static int native_get_dependongroup(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	wchar_t* pBuffer;
	uintptr_t uBufferLength;
	if (get_service_dependencies(pServiceName, hService, &pBuffer,
			&uBufferLength, DEPENDENCY_GROUPS)) {
		return -1;
	}

	int iResult;
	if (uBufferLength) {
		wchar_t* pFormatted;
		uintptr_t uFormattedLength;
		if (format_double_null(
				pBuffer, uBufferLength, &pFormatted, &uFormattedLength)) {
			heap_free(pBuffer);
			return -1;
		}

		iResult = value_from_string(pName, pValue, pFormatted);
		heap_free(pFormatted);
		heap_free(pBuffer);
	} else {
		pValue->m_pString = NULL;
		iResult = 0;
	}

	return iResult;
}

static int setting_dump_dependon(const wchar_t* pServiceName,
	SC_HANDLE hService, const wchar_t* pName, uint32_t uType, value_t* pValue)
{
	wchar_t* pDependencies = NULL;
	uintptr_t uDependenciesLength;
	if (get_service_dependencies(pServiceName, hService, &pDependencies,
			&uDependenciesLength, uType)) {
		return -1;
	}
	if (!uDependenciesLength) {
		return 0;
	}

	uint32_t uErrors = 0;
	wchar_t* s;
	for (s = pDependencies; *s; s++) {
		uintptr_t uStringLength = wcslen(s) + 2;
		pValue->m_pString =
			static_cast<wchar_t*>(heap_alloc(uStringLength * sizeof(wchar_t)));
		if (!pValue->m_pString) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"dump",
				L"setting_dump_dependon");
			break;
		}

		StringCchPrintfW(pValue->m_pString, uStringLength, L"%c%s",
			(s > pDependencies) ? L'+' : L':', s);
		if (setting_dump_string(pServiceName, reinterpret_cast<void*>(REG_SZ),
				pName, pValue, 0)) {
			++uErrors;
		}
		heap_free(pValue->m_pString);
		pValue->m_pString = NULL;

		for (; *s; s++) {
		}
	}

	heap_free(pDependencies);

	if (uErrors) {
		return 1;
	}
	return 0;
}

static int native_dump_dependongroup(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	return setting_dump_dependon(pServiceName, static_cast<SC_HANDLE>(pParam),
		pName, DEPENDENCY_GROUPS, pValue);
}

static int native_set_dependonservice(const wchar_t* pServiceName, void* pParam,
	const wchar_t* /* pName */, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/*
	  Get existing group dependencies because we must set both types together.
	*/
	wchar_t* pGroupsBuffer;
	uintptr_t uGroupsLength;
	if (get_service_dependencies(pServiceName, hService, &pGroupsBuffer,
			&uGroupsLength, DEPENDENCY_GROUPS)) {
		return -1;
	}

	int iResult;
	if (!pValue || !pValue->m_pString || !pValue->m_pString[0]) {
		iResult = 0;
		if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE,
				SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 0, 0, 0, pGroupsBuffer, 0,
				NULL, NULL)) {
			print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
				error_string(GetLastError()));
			iResult = -1;
		}

		if (pGroupsBuffer) {
			heap_free(pGroupsBuffer);
		}
		return iResult;
	}

	// Update the service list.
	wchar_t* pServices;
	uintptr_t uServicesLength;
	if (native_set_dependon(pServiceName, hService, &pServices,
			&uServicesLength, pValue, DEPENDENCY_SERVICES)) {
		// Don't leak
		if (pGroupsBuffer) {
			heap_free(pGroupsBuffer);
		}
		return -1;
	}

	wchar_t* pDependencies;
	if (uGroupsLength > 2) {
		pDependencies = static_cast<wchar_t*>(
			heap_alloc((uServicesLength + uGroupsLength) * sizeof(wchar_t)));
		if (!pDependencies) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"dependencies",
				L"native_set_dependonservice");
			if (pGroupsBuffer) {
				heap_free(pGroupsBuffer);
			}
			if (pServices) {
				heap_free(pServices);
			}
			return -1;
		}

		memmove(pDependencies, pServices, uServicesLength * sizeof(wchar_t));
		memmove(pDependencies + uServicesLength - 1U, pGroupsBuffer,
			uGroupsLength * sizeof(wchar_t));
	} else {
		pDependencies = pServices;
	}

	iResult = 1;
	if (set_service_dependencies(pServiceName, hService, pDependencies)) {
		iResult = -1;
	}
	if (pDependencies != pServices) {
		heap_free(pDependencies);
	}
	if (pGroupsBuffer) {
		heap_free(pGroupsBuffer);
	}
	if (pServices) {
		heap_free(pServices);
	}
	return iResult;
}

static int native_get_dependonservice(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	wchar_t* pBuffer;
	uintptr_t uBufferLength;
	if (get_service_dependencies(pServiceName, hService, &pBuffer,
			&uBufferLength, DEPENDENCY_SERVICES)) {
		return -1;
	}

	int iResult;
	if (uBufferLength) {
		wchar_t* pFormatted;
		uintptr_t uFormattedLength;
		if (format_double_null(
				pBuffer, uBufferLength, &pFormatted, &uFormattedLength)) {
			// Error!
			iResult = -1;
		} else {
			iResult = value_from_string(pName, pValue, pFormatted);
			heap_free(pFormatted);
		}
		// Release the buffer
		heap_free(pBuffer);
	} else {
		pValue->m_pString = NULL;
		iResult = 0;
	}

	return iResult;
}

static int native_dump_dependonservice(const wchar_t* pServiceName,
	void* pParam, const wchar_t* pName, void* /* pDefaultValue */,
	value_t* pValue, const wchar_t* /* pAdditional */)
{
	return setting_dump_dependon(pServiceName, static_cast<SC_HANDLE>(pParam),
		pName, DEPENDENCY_SERVICES, pValue);
}

static int native_set_description(const wchar_t* pServiceName, void* pParam,
	const wchar_t* /* pName */, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	wchar_t* pDescription = NULL;
	if (pValue) {
		pDescription = pValue->m_pString;
	}
	if (set_service_description(pServiceName, hService, pDescription)) {
		return -1;
	}

	if (pDescription && pDescription[0]) {
		return 1;
	}
	return 0;
}

static int native_get_description(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	wchar_t Buffer[VALUE_LENGTH];
	if (get_service_description(
			pServiceName, hService, RTL_NUMBER_OF(Buffer), Buffer)) {
		return -1;
	}

	if (Buffer[0]) {
		return value_from_string(pName, pValue, Buffer);
	}
	pValue->m_pString = NULL;

	return 0;
}

static int native_set_displayname(const wchar_t* pServiceName, void* pParam,
	const wchar_t* /* pName */, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	const wchar_t* pDisplayName = NULL;
	if (pValue && pValue->m_pString) {
		pDisplayName = pValue->m_pString;
	} else {
		pDisplayName = pServiceName;
	}

	if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
			SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, pDisplayName)) {
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}

	/*
	  If the display name and service name differ only in case,
	  ChangeServiceConfig() will return success but the display name will be
	  set to the service name, NOT the value passed to the function.
	  This appears to be a quirk of Windows rather than a bug here.
	*/
	if (pDisplayName != pServiceName &&
		!str_equiv(pDisplayName, pServiceName)) {
		return 1;
	}

	return 0;
}

static int native_get_displayname(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	int iResult;
	if (pQueryServiceConfig) {
		iResult = value_from_string(
			pName, pValue, pQueryServiceConfig->lpDisplayName);
		heap_free(pQueryServiceConfig);
	} else {
		iResult = -1;
	}
	return iResult;
}

static int native_set_environment(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	HKEY hKey = open_service_registry(pServiceName, KEY_SET_VALUE, true);
	int iResult = -1;
	if (hKey) {
		iResult = setting_set_environment(
			pServiceName, hKey, pName, pDefaultValue, pValue, pAdditional);
		RegCloseKey(hKey);
	}
	return iResult;
}

static int native_get_environment(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	HKEY hKey = open_service_registry(pServiceName, KEY_READ, true);
	int iResult = -1;
	if (hKey) {
		ZeroMemory(pValue, sizeof(value_t));
		iResult = setting_get_environment(
			pServiceName, hKey, pName, pDefaultValue, pValue, pAdditional);
		RegCloseKey(hKey);
	}
	return iResult;
}

static int native_dump_environment(const wchar_t* pServiceName,
	void* /* pParam */, const wchar_t* pName, void* pDefaultValue,
	value_t* pValue, const wchar_t* pAdditional)
{
	HKEY hKey = open_service_registry(pServiceName, KEY_READ, true);
	int iResult = -1;
	if (hKey) {
		iResult = setting_dump_environment(
			pServiceName, hKey, pName, pDefaultValue, pValue, pAdditional);
		RegCloseKey(hKey);
	}
	return iResult;
}

static int native_set_imagepath(const wchar_t* /* pServiceName */, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/* It makes no sense to try to reset the image path. */
	if (!pValue || !pValue->m_pString) {
		print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, pName);
		return -1;
	}

	if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
			SERVICE_NO_CHANGE, pValue->m_pString, 0, 0, 0, 0, 0, 0)) {
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}
	return 1;
}

static int native_get_imagepath(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	if (!pQueryServiceConfig) {
		return -1;
	}

	int iResult =
		value_from_string(pName, pValue, pQueryServiceConfig->lpBinaryPathName);
	heap_free(pQueryServiceConfig);

	return iResult;
}

static int native_set_name(const wchar_t* /* pServiceName */,
	void* /* pParam */, const wchar_t* /* pName */, void* /* pDefaultValue */,
	value_t* /* pValue */, const wchar_t* /* pAdditional */)
{
	print_message(stderr, NSSM_MESSAGE_CANNOT_RENAME_SERVICE);
	return -1;
}

static int native_get_name(const wchar_t* pServiceName, void* /* pParam */,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	return value_from_string(pName, pValue, pServiceName);
}

static int native_set_objectname(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* pAdditional)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/*
	  Logical syntax is: nssm set <service> ObjectName <username> <password>
	  That means the username is actually passed in the additional parameter.
	*/
	bool bLocalSystem = false;
	bool bVirtualAccount = false;
	const wchar_t* pUserName = g_NSSMLocalSystemAccount;
	wchar_t* pPassword = NULL;
	if (pAdditional) {
		pUserName = pAdditional;
		if (pValue && pValue->m_pString) {
			pPassword = pValue->m_pString;
		}
	} else if (pValue && pValue->m_pString) {
		pUserName = pValue->m_pString;
	}

	const wchar_t* pWellKnown = well_known_username(pUserName);
	uintptr_t uPasswordLength = 0;
	if (pWellKnown) {
		if (str_equiv(pWellKnown, g_NSSMLocalSystemAccount)) {
			bLocalSystem = true;
		}
		pUserName = pWellKnown;
		pPassword = L"";
	} else if (is_virtual_account(pServiceName, pUserName)) {
		bVirtualAccount = true;
	} else if (!pPassword) {
		// We need a password if the account requires it.
		print_message(stderr, NSSM_MESSAGE_MISSING_PASSWORD, pName);
		return -1;
	} else {
		uPasswordLength = wcslen(pPassword) * sizeof(wchar_t);
	}

	/*
	  ChangeServiceConfig() will fail to set the username if the service is set
	  to interact with the desktop.
	*/
	DWORD uType = SERVICE_NO_CHANGE;
	if (!bLocalSystem) {
		QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
			query_service_config(pServiceName, hService);
		if (!pQueryServiceConfig) {
			if (uPasswordLength) {
				RtlSecureZeroMemory(pPassword, uPasswordLength);
			}
			return -1;
		}

		uType =
			pQueryServiceConfig->dwServiceType & (~SERVICE_INTERACTIVE_PROCESS);
		heap_free(pQueryServiceConfig);
	}

	if (!pWellKnown && !bVirtualAccount) {
		if (grant_logon_as_service(pUserName)) {
			if (uPasswordLength) {
				RtlSecureZeroMemory(pPassword, uPasswordLength);
			}
			print_message(
				stderr, NSSM_MESSAGE_GRANT_LOGON_AS_SERVICE_FAILED, pUserName);
			return -1;
		}
	}

	if (!ChangeServiceConfigW(hService, uType, SERVICE_NO_CHANGE,
			SERVICE_NO_CHANGE, 0, 0, 0, 0, pUserName, pPassword, 0)) {
		if (uPasswordLength) {
			RtlSecureZeroMemory(pPassword, uPasswordLength);
		}
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}

	if (uPasswordLength) {
		RtlSecureZeroMemory(pPassword, uPasswordLength);
	}
	if (bLocalSystem) {
		return 0;
	}
	return 1;
}

static int native_get_objectname(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	if (!pQueryServiceConfig) {
		return -1;
	}

	int iResult = value_from_string(
		pName, pValue, pQueryServiceConfig->lpServiceStartName);
	heap_free(pQueryServiceConfig);

	return iResult;
}

static int native_dump_objectname(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* pAdditional)
{
	int iResult = native_get_objectname(
		pServiceName, pParam, pName, pDefaultValue, pValue, pAdditional);
	if (iResult != 1) {
		return iResult;
	}

	/* Properly checking for a virtual account requires the actual service name.
	 */
	if (!_wcsnicmp(g_NSSMVirtualServiceAccountDomain, pValue->m_pString,
			wcslen(g_NSSMVirtualServiceAccountDomain))) {
		wchar_t* pAccountName = virtual_account(pServiceName);
		if (!pAccountName) {
			return -1;
		}
		heap_free(pValue->m_pString);
		pValue->m_pString = pAccountName;
	} else {
		// Do we need to dump a dummy password?
		if (!well_known_username(pValue->m_pString)) {
			// Parameters are the other way round.
			value_t TempValue;
			TempValue.m_pString = L"****";
			return setting_dump_string(pServiceName,
				reinterpret_cast<void*>(REG_SZ), pName, &TempValue,
				pValue->m_pString);
		}
	}
	return setting_dump_string(
		pServiceName, reinterpret_cast<void*>(REG_SZ), pName, pValue, NULL);
}

static int native_set_startup(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/* It makes no sense to try to reset the startup type. */
	if (!pValue || !pValue->m_pString) {
		print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, pName);
		return -1;
	}

	/* Map NSSM_STARTUP_* constant to Windows SERVICE_*_START constant. */
	int iServiceStartup = -1;
	int i;
	for (i = 0; g_StartupStrings[i]; i++) {
		if (str_equiv(pValue->m_pString, g_StartupStrings[i])) {
			iServiceStartup = i;
			break;
		}
	}

	if (iServiceStartup < 0) {
		print_message(
			stderr, NSSM_MESSAGE_INVALID_SERVICE_STARTUP, pValue->m_pString);
		for (i = 0; g_StartupStrings[i]; i++) {
			fwprintf(stderr, L"%s\n", g_StartupStrings[i]);
		}
		return -1;
	}

	DWORD uStartType;
	switch (iServiceStartup) {
	case NSSM_STARTUP_MANUAL:
		uStartType = SERVICE_DEMAND_START;
		break;
	case NSSM_STARTUP_DISABLED:
		uStartType = SERVICE_DISABLED;
		break;
	default:
		uStartType = SERVICE_AUTO_START;
		break;
	}

	if (!ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, uStartType,
			SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, 0)) {
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}

	SERVICE_DELAYED_AUTO_START_INFO DelayedAutoStart;
	ZeroMemory(&DelayedAutoStart, sizeof(DelayedAutoStart));
	if (iServiceStartup == NSSM_STARTUP_DELAYED) {
		DelayedAutoStart.fDelayedAutostart = 1;
	} else {
		DelayedAutoStart.fDelayedAutostart = 0;
	}
	if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
			&DelayedAutoStart)) {
		DWORD uError = GetLastError();
		/* Pre-Vista we expect to fail with ERROR_INVALID_LEVEL */
		if (uError != ERROR_INVALID_LEVEL) {
			log_event(EVENTLOG_ERROR_TYPE,
				NSSM_MESSAGE_SERVICE_CONFIG_DELAYED_AUTO_START_INFO_FAILED,
				pServiceName, error_string(uError), NULL);
		}
	}

	return 1;
}

static int native_get_startup(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	if (!pQueryServiceConfig) {
		return -1;
	}
	uint32_t uStartIndex;
	int iResult = get_service_startup(
		pServiceName, hService, pQueryServiceConfig, &uStartIndex);
	heap_free(pQueryServiceConfig);

	if (iResult) {
		return -1;
	}

	uint32_t i;
	for (i = 0; g_StartupStrings[i]; i++) {
	}

	if (uStartIndex >= i) {
		return -1;
	}

	return value_from_string(pName, pValue, g_StartupStrings[uStartIndex]);
}

static int native_set_type(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = static_cast<SC_HANDLE>(pParam);
	if (!hService) {
		return -1;
	}

	/* It makes no sense to try to reset the service type. */
	if (!pValue || !pValue->m_pString) {
		print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, pName);
		return -1;
	}

	/*
	  We can only manage services of type SERVICE_WIN32_OWN_PROCESS
	  and SERVICE_INTERACTIVE_PROCESS.
	*/
	DWORD uType = SERVICE_WIN32_OWN_PROCESS;
	if (str_equiv(pValue->m_pString, g_NSSMInteractiveProcess))
		uType |= SERVICE_INTERACTIVE_PROCESS;
	else if (!str_equiv(pValue->m_pString, g_NSSMWin32OwnProcess)) {
		print_message(
			stderr, NSSM_MESSAGE_INVALID_SERVICE_TYPE, pValue->m_pString);
		fwprintf(stderr, L"%s\n", g_NSSMWin32OwnProcess);
		fwprintf(stderr, L"%s\n", g_NSSMInteractiveProcess);
		return -1;
	}

	/*
	  ChangeServiceConfig() will fail if the service runs under an account
	  other than LOCALSYSTEM and we try to make it interactive.
	*/
	if (uType & SERVICE_INTERACTIVE_PROCESS) {
		QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
			query_service_config(pServiceName, hService);
		if (!pQueryServiceConfig) {
			return -1;
		}

		if (!str_equiv(pQueryServiceConfig->lpServiceStartName,
				g_NSSMLocalSystemAccount)) {
			heap_free(pQueryServiceConfig);
			print_message(stderr, NSSM_MESSAGE_INTERACTIVE_NOT_LOCALSYSTEM,
				pValue->m_pString, pServiceName, g_NSSMLocalSystemAccount);
			return -1;
		}

		heap_free(pQueryServiceConfig);
	}

	if (!ChangeServiceConfigW(hService, uType, SERVICE_NO_CHANGE,
			SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, 0)) {
		print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
			error_string(GetLastError()));
		return -1;
	}

	return 1;
}

static int native_get_type(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* /* pDefaultValue */, value_t* pValue,
	const wchar_t* /* pAdditional */)
{
	SC_HANDLE hService = (SC_HANDLE)pParam;
	if (!hService) {
		return -1;
	}

	QUERY_SERVICE_CONFIGW* pQueryServiceConfig =
		query_service_config(pServiceName, hService);
	if (!pQueryServiceConfig) {
		return -1;
	}

	pValue->m_uNumber = pQueryServiceConfig->dwServiceType;
	heap_free(pQueryServiceConfig);

	const wchar_t* pString;
	switch (pValue->m_uNumber) {
	case SERVICE_KERNEL_DRIVER:
		pString = g_NSSMKernelDriver;
		break;
	case SERVICE_FILE_SYSTEM_DRIVER:
		pString = g_NSSMFileSystemDriver;
		break;
	case SERVICE_WIN32_OWN_PROCESS:
		pString = g_NSSMWin32OwnProcess;
		break;
	case SERVICE_WIN32_SHARE_PROCESS:
		pString = g_NSSMWin32ShareProcess;
		break;
	case SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS:
		pString = g_NSSMInteractiveProcess;
		break;
	case SERVICE_WIN32_SHARE_PROCESS | SERVICE_INTERACTIVE_PROCESS:
		pString = g_NSSMShareInteractiveProcess;
		break;
	default:
		pString = g_NSSMUnknown;
		break;
	}

	return value_from_string(pName, pValue, pString);
}

int set_setting(const wchar_t* pServiceName, HKEY__* hKey,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional)
{
	if (!hKey) {
		return -1;
	}
	int iResult;

	if (pSettings->m_pSet) {
		iResult = pSettings->m_pSet(pServiceName, hKey, pSettings->m_pName,
			pSettings->m_pDefaultValue, pValue, pAdditional);
	} else {
		iResult = -1;
	}
	if (!iResult) {
		print_message(stdout, NSSM_MESSAGE_RESET_SETTING, pSettings->m_pName,
			pServiceName);
	} else if (iResult > 0) {
		print_message(
			stdout, NSSM_MESSAGE_SET_SETTING, pSettings->m_pName, pServiceName);
	} else {
		print_message(stderr, NSSM_MESSAGE_SET_SETTING_FAILED,
			pSettings->m_pName, pServiceName);
	}
	return iResult;
}

int set_setting(const wchar_t* pServiceName, SC_HANDLE__* hService,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional)
{
	if (!hService) {
		return -1;
	}

	int iResult;
	if (pSettings->m_pSet) {
		iResult = pSettings->m_pSet(pServiceName, hService, pSettings->m_pName,
			pSettings->m_pDefaultValue, pValue, pAdditional);
	} else {
		iResult = -1;
	}
	if (!iResult) {
		print_message(stdout, NSSM_MESSAGE_RESET_SETTING, pSettings->m_pName,
			pServiceName);
	} else if (iResult > 0) {
		print_message(
			stdout, NSSM_MESSAGE_SET_SETTING, pSettings->m_pName, pServiceName);
	} else {
		print_message(stderr, NSSM_MESSAGE_SET_SETTING_FAILED,
			pSettings->m_pName, pServiceName);
	}
	return iResult;
}

/*
  Returns:  1 if the value was retrieved.
			0 if the default value was retrieved.
		   -1 on error.
*/
int get_setting(const wchar_t* pServiceName, HKEY__* hKey,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional)
{
	if (!hKey) {
		return -1;
	}

	int iResult;
	if (is_string_type(pSettings->m_uType)) {
		pValue->m_pString = static_cast<wchar_t*>(pSettings->m_pDefaultValue);
		if (pSettings->m_pGet) {
			iResult = pSettings->m_pGet(pServiceName, hKey, pSettings->m_pName,
				pSettings->m_pDefaultValue, pValue, pAdditional);
		} else {
			iResult = -1;
		}
	} else if (is_numeric_type(pSettings->m_uType)) {
		pValue->m_uNumber = static_cast<uint32_t>(
			reinterpret_cast<uintptr_t>(pSettings->m_pDefaultValue));
		if (pSettings->m_pGet) {
			iResult = pSettings->m_pGet(pServiceName, hKey, pSettings->m_pName,
				pSettings->m_pDefaultValue, pValue, pAdditional);
		} else {
			iResult = -1;
		}
	} else {
		iResult = -1;
	}
	if (iResult < 0) {
		print_message(stderr, NSSM_MESSAGE_GET_SETTING_FAILED,
			pSettings->m_pName, pServiceName);
	}
	return iResult;
}

int get_setting(const wchar_t* pServiceName, SC_HANDLE__* hService,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional)
{
	if (!hService) {
		return -1;
	}
	return pSettings->m_pGet(
		pServiceName, hService, pSettings->m_pName, 0, pValue, pAdditional);
}

int dump_setting(const wchar_t* pServiceName, HKEY__* hKey,
	SC_HANDLE__* hService, const settings_t* pSettings)
{
	void* pParam;
	if (pSettings->m_bNative) {
		if (!hService) {
			return -1;
		}
		pParam = hService;
	} else {
		/* Will be null for native services. */
		pParam = hKey;
	}

	value_t TempValue = {0};

	if (pSettings->m_pDump) {
		return pSettings->m_pDump(pServiceName, pParam, pSettings->m_pName,
			const_cast<settings_t*>(pSettings), &TempValue, 0);
	}
	int iResult;
	if (pSettings->m_bNative) {
		iResult = get_setting(pServiceName, hService, pSettings, &TempValue, 0);
	} else {
		iResult = get_setting(pServiceName, hKey, pSettings, &TempValue, 0);
	}
	if (iResult != 1) {
		return iResult;
	}
	return setting_dump_string(pServiceName,
		reinterpret_cast<void*>(static_cast<uintptr_t>(pSettings->m_uType)),
		pSettings->m_pName, &TempValue, 0);
}

/***************************************

	Setting handlers

***************************************/

const settings_t g_Settings[] = {
	{g_NSSMRegExe, REG_EXPAND_SZ, const_cast<wchar_t*>(L""), false, 0,
		setting_set_string, setting_get_string, setting_not_dumpable},
	{g_NSSMRegFlags, REG_EXPAND_SZ, const_cast<wchar_t*>(L""), false, 0,
		setting_set_string, setting_get_string, NULL},
	{g_NSSMRegDir, REG_EXPAND_SZ, const_cast<wchar_t*>(L""), false, 0,
		setting_set_string, setting_get_string, NULL},
	{g_NSSMRegExit, REG_SZ, (void*)g_ExitActionStrings[NSSM_EXIT_RESTART],
		false, ADDITIONAL_MANDATORY, setting_set_exit_action,
		setting_get_exit_action, setting_dump_exit_action},
	{g_NSSMRegHook, REG_SZ, const_cast<wchar_t*>(L""), false,
		ADDITIONAL_MANDATORY, setting_set_hook, setting_get_hook,
		setting_dump_hooks},
	{g_NSSMRegAffinity, REG_SZ, NULL, false, 0, setting_set_affinity,
		setting_get_affinity, NULL},
	{g_NSSMRegEnv, REG_MULTI_SZ, NULL, false, ADDITIONAL_CRLF,
		setting_set_environment, setting_get_environment,
		setting_dump_environment},
	{g_NSSMRegEnvExtra, REG_MULTI_SZ, NULL, false, ADDITIONAL_CRLF,
		setting_set_environment, setting_get_environment,
		setting_dump_environment},
	{g_NSSMRegNoConsole, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegPriority, REG_SZ, (void*)g_PriorityStrings[NSSM_NORMAL_PRIORITY],
		false, 0, setting_set_priority, setting_get_priority,
		setting_dump_priority},
	{g_NSSMRegRestartDelay, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegStdIn, REG_EXPAND_SZ, NULL, false, 0, setting_set_string,
		setting_get_string, NULL},
	{g_NSSMRegStdInSharing, REG_DWORD, (void*)NSSM_STDIN_SHARING, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdInDisposition, REG_DWORD, (void*)NSSM_STDIN_DISPOSITION, false,
		0, setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdInFlags, REG_DWORD, (void*)NSSM_STDIN_FLAGS, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdOut, REG_EXPAND_SZ, NULL, false, 0, setting_set_string,
		setting_get_string, NULL},
	{g_NSSMRegStdOutSharing, REG_DWORD, (void*)NSSM_STDOUT_SHARING, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdOutDisposition, REG_DWORD, (void*)NSSM_STDOUT_DISPOSITION,
		false, 0, setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdOutFlags, REG_DWORD, (void*)NSSM_STDOUT_FLAGS, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdOutCopyAndTruncate, REG_DWORD, NULL, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStdErr, REG_EXPAND_SZ, NULL, false, 0, setting_set_string,
		setting_get_string, NULL},
	{g_NSSMRegStdErrSharing, REG_DWORD, (void*)NSSM_STDERR_SHARING, false, 0,
		setting_set_number, setting_get_number, 0},
	{g_NSSMRegStdErrDisposition, REG_DWORD, (void*)NSSM_STDERR_DISPOSITION,
		false, 0, setting_set_number, setting_get_number, 0},
	{g_NSSMRegStdErrFlags, REG_DWORD, (void*)NSSM_STDERR_FLAGS, false, 0,
		setting_set_number, setting_get_number, 0},
	{g_NSSMRegStdErrCopyAndTruncate, REG_DWORD, NULL, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegStopMethodSkip, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegKillConsoleGracePeriod, REG_DWORD,
		(void*)NSSM_KILL_CONSOLE_GRACE_PERIOD, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegKillWindowGracePeriod, REG_DWORD,
		(void*)NSSM_KILL_WINDOW_GRACE_PERIOD, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegKillThreadsGracePeriod, REG_DWORD,
		(void*)NSSM_KILL_THREADS_GRACE_PERIOD, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegKillProcessTree, REG_DWORD, (void*)1, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegThrottle, REG_DWORD, (void*)NSSM_RESET_THROTTLE_RESTART, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegHookShareOutputHandles, REG_DWORD, NULL, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegRotate, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegRotateOnline, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegRotateSeconds, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegRotateBytesLow, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegRotateBytesHigh, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMRegRotateDelay, REG_DWORD, (void*)NSSM_ROTATE_DELAY, false, 0,
		setting_set_number, setting_get_number, NULL},
	{g_NSSMRegTimeStampLog, REG_DWORD, NULL, false, 0, setting_set_number,
		setting_get_number, NULL},
	{g_NSSMNativeDependOnGroup, REG_MULTI_SZ, NULL, true, ADDITIONAL_CRLF,
		native_set_dependongroup, native_get_dependongroup,
		native_dump_dependongroup},
	{g_NSSMNativeDependOnService, REG_MULTI_SZ, NULL, true, ADDITIONAL_CRLF,
		native_set_dependonservice, native_get_dependonservice,
		native_dump_dependonservice},
	{g_NSSMNativeDescription, REG_SZ, const_cast<wchar_t*>(L""), true, 0,
		native_set_description, native_get_description, NULL},
	{g_NSSMNativeDisplayName, REG_SZ, NULL, true, 0, native_set_displayname,
		native_get_displayname, NULL},
	{g_NSSMNativeEnvironment, REG_MULTI_SZ, NULL, true, ADDITIONAL_CRLF,
		native_set_environment, native_get_environment,
		native_dump_environment},
	{g_NSSMNativeImagePath, REG_EXPAND_SZ, NULL, true, 0, native_set_imagepath,
		native_get_imagepath, setting_not_dumpable},
	{g_NSSMNativeObjectName, REG_SZ,
		const_cast<wchar_t*>(g_NSSMLocalSystemAccount), true, 0,
		native_set_objectname, native_get_objectname, native_dump_objectname},
	{g_NSSMNativeName, REG_SZ, NULL, true, 0, native_set_name, native_get_name,
		setting_not_dumpable},
	{g_NSSMNativeStartup, REG_SZ, NULL, true, 0, native_set_startup,
		native_get_startup, NULL},
	{g_NSSMNativeType, REG_SZ, NULL, true, 0, native_set_type, native_get_type,
		NULL},
	{NULL, 0, NULL, false, 0, NULL, NULL, NULL}};
