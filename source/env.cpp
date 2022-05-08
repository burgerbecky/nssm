/***************************************

	Environment variable handlers

***************************************/

#include "env.h"
#include "event.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "registry.h"

#include <stdlib.h>
#include <wchar.h>

/***************************************

	Environment block is of the form:

	KEY1=VALUE1 NULL
	KEY2=VALUE2 NULL
	NULL

	A single variable KEY=VALUE has length 15:

	KEY=VALUE (13) NULL (1)
	NULL (1)

	Environment variable names are case-insensitive!

***************************************/

/***************************************

	Find the length in characters of an environment block.

	Scan a list of strings until it hits a double zero

***************************************/

uintptr_t environment_length(const wchar_t* pEnvironmentVars)
{
	uintptr_t uLength = 0;

	const wchar_t* pWork;
	for (pWork = pEnvironmentVars;; pWork++) {
		++uLength;
		if (*pWork == 0) {
			if (*(pWork + 1) == 0) {
				++uLength;
				break;
			}
		}
	}

	return uLength;
}

/***************************************

	Copy an environment block.

***************************************/

wchar_t* copy_environment_block(wchar_t* pEnvironmentVars)
{
	wchar_t* pResult;
	if (copy_double_null(
			pEnvironmentVars, environment_length(pEnvironmentVars), &pResult)) {
		return NULL;
	}
	return pResult;
}

/***************************************

	The environment block starts with variables of the form
	=C:=C:\Windows\System32 which we ignore.

***************************************/

wchar_t* useful_environment(wchar_t* pEnvironmentVars)
{
	wchar_t* pEnvironment = pEnvironmentVars;

	if (pEnvironment) {
		while (*pEnvironment == L'=') {
			for (; *pEnvironment; pEnvironment++)
				;
			pEnvironment++;
		}
	}

	return pEnvironment;
}

/***************************************

	Expand an environment variable.  Must call heap_free() on the result.

***************************************/

wchar_t* expand_environment_string(wchar_t* pString)
{
	DWORD uLength = ExpandEnvironmentStringsW(pString, NULL, 0);
	if (!uLength) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_EXPANDENVIRONMENTSTRINGS_FAILED, pString,
			error_string(GetLastError()), NULL);
		return NULL;
	}

	wchar_t* pResult =
		static_cast<wchar_t*>(heap_alloc(uLength * sizeof(wchar_t)));
	if (!pResult) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			L"ExpandEnvironmentStrings()", L"expand_environment_string", NULL);
		return NULL;
	}

	if (!ExpandEnvironmentStringsW(pString, pResult, uLength)) {
		log_event(EVENTLOG_ERROR_TYPE,
			NSSM_EVENT_EXPANDENVIRONMENTSTRINGS_FAILED, pString,
			error_string(GetLastError()), NULL);
		heap_free(pResult);
		return NULL;
	}

	return pResult;
}

/***************************************

	Set all the environment variables from an environment block in the current
	environment or remove all the variables in the block from the current
	environment.

	Return the number of environment variables parsed

***************************************/

static uint32_t set_environment_block(
	wchar_t* pEnvironmentVars, bool bSetVariables)
{
	uint32_t uResult = 0;

	wchar_t* s;
	wchar_t* t;
	for (s = pEnvironmentVars; *s; s++) {
		for (t = s; *t && *t != L'='; t++) {
		}

		if (*t == L'=') {
			*t = 0;

			// Set the variables?
			if (bSetVariables) {
				wchar_t* pExpanded = expand_environment_string(++t);
				if (pExpanded) {
					if (!SetEnvironmentVariableW(s, pExpanded)) {
						++uResult;
					}
					heap_free(pExpanded);
				} else {
					if (!SetEnvironmentVariableW(s, t)) {
						++uResult;
					}
				}
			} else {
				// Clear the variables
				if (!SetEnvironmentVariableW(s, NULL)) {
					++uResult;
				}
			}
			for (t++; *t; t++) {
			}
		}
		s = t;
	}

	return uResult;
}

/***************************************

	Set all the environment variables from an environment block in the current
	environment

	Return the number of environment variables parsed

***************************************/

uint32_t set_environment_block(wchar_t* pEnvironmentVars)
{
	return set_environment_block(pEnvironmentVars, true);
}

/***************************************

	Clear all the environment variables from an environment block in the current
	environment

	Return the number of environment variables parsed

***************************************/

static uint32_t unset_environment_block(wchar_t* pEnvironmentVars)
{
	return set_environment_block(pEnvironmentVars, false);
}

/***************************************

	Remove all variables from the process environment.

***************************************/

uint32_t clear_environment(void)
{
	uint32_t uResult = 0;
	wchar_t* pRawStrings = GetEnvironmentStringsW();
	if (pRawStrings) {
		wchar_t* pUseful = useful_environment(pRawStrings);

		uResult = unset_environment_block(pUseful);

		// Release the allocated strings
		FreeEnvironmentStringsW(pRawStrings);
	}
	return uResult;
}

/***************************************

	Set the current environment to exactly duplicate an environment block.

***************************************/

uint32_t duplicate_environment(wchar_t* pEnvironmentVars)
{
	uint32_t uResult = clear_environment();
	wchar_t* pUseful = useful_environment(pEnvironmentVars);
	uResult += set_environment_block(pUseful);
	return uResult;
}

/***************************************

	Verify an environment block.
	Returns:	1 if environment is invalid.
				0 if environment is OK.
				-1 on error.

***************************************/

int test_environment(wchar_t* pEnvironmentVars)
{
	const wchar_t* path = nssm_imagepath();
	STARTUPINFOW si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	DWORD uFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

	/*
	  Try to relaunch ourselves but with the candidate environment set.
	  Assuming no solar flare activity, the only reason this would fail is if
	  the environment were invalid.
	*/
	if (CreateProcessW(NULL, const_cast<wchar_t*>(path), NULL, NULL, 0, uFlags,
			pEnvironmentVars, NULL, &si, &pi)) {
		TerminateProcess(pi.hProcess, 0);
	} else {
		DWORD uError = GetLastError();
		if (uError == ERROR_INVALID_PARAMETER) {
			return 1;
		} else {
			return -1;
		}
	}
	return 0;
}

/***************************************

	Duplicate an environment block returned by GetEnvironmentStrings().
	Since such a block is by definition readonly, and duplicate_environment()
	modifies its inputs, this function takes a copy of the input and operates
	on that.

***************************************/

void duplicate_environment_strings(wchar_t* pEnvironmentVars)
{
	wchar_t* pCopy = copy_environment_block(pEnvironmentVars);
	if (pCopy) {
		duplicate_environment(pCopy);
		heap_free(pCopy);
	}
}

/***************************************

	Safely get a copy of the current environment.

***************************************/

wchar_t* copy_environment(void)
{
	wchar_t* pResult = NULL;
	wchar_t* pRawStrings = GetEnvironmentStringsW();
	if (pRawStrings) {
		pResult = copy_environment_block(pRawStrings);
		FreeEnvironmentStringsW(pRawStrings);
	}
	return pResult;
}

/***************************************

	Create a new block with all the strings of the first block plus a new
	string. If the key is already present its value will be overwritten in
	place. If the key is blank or empty the new block will still be allocated
	and have non-zero length.

***************************************/

int append_to_environment_block(wchar_t* pEnvironmentVars,
	uintptr_t uEnvironmentLength, wchar_t* pString, wchar_t** pNewEnvironment,
	uintptr_t* pNewLength)
{
	uintptr_t uKeylength = 0;
	if (pString && pString[0]) {
		for (; pString[uKeylength]; ++uKeylength) {
			if (pString[uKeylength] == L'=') {
				++uKeylength;
				break;
			}
		}
	}
	return append_to_double_null(pEnvironmentVars, uEnvironmentLength,
		pNewEnvironment, pNewLength, pString, uKeylength, false);
}

/***************************************

	Create a new block with all the strings of the first block minus the given
	string.

	If the key is not present the new block will be a copy of the original.

	If the string is KEY=VALUE the key will only be removed if its value is
	VALUE.

	If the string is just KEY the key will unconditionally be removed.

	If removing the string results in an empty list the new block will still be
	allocated and have non-zero length.

***************************************/

int remove_from_environment_block(wchar_t* pEnvironmentVars,
	uintptr_t uEnvironmentLength, const wchar_t* pString,
	wchar_t** pNewEnvironment, uintptr_t* pNewLength)
{
	if (!pString || !pString[0] || pString[0] == L'=') {
		return 1;
	}

	uintptr_t uStringLength = wcslen(pString);
	uintptr_t i;
	for (i = 0; i < uStringLength; i++) {
		if (pString[i] == L'=') {
			break;
		}
	}

	/* Rewrite KEY to KEY= but leave KEY=VALUE alone. */
	uintptr_t uKeyLength = uStringLength;
	if (i == uStringLength) {
		++uKeyLength;
	}

	wchar_t* pNewKey = (wchar_t*)heap_alloc((uKeyLength + 1) * sizeof(wchar_t));
	if (!pNewKey) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"key",
			L"remove_from_environment_block()", NULL);
		return 2;
	}
	memmove(pNewKey, pString, uStringLength * sizeof(wchar_t));
	if (uKeyLength > uStringLength) {
		pNewKey[uKeyLength - 1] = L'=';
	}
	pNewKey[uKeyLength] = 0;

	int iResult = remove_from_double_null(pEnvironmentVars, uEnvironmentLength,
		pNewEnvironment, pNewLength, pNewKey, uKeyLength, false);
	heap_free(pNewKey);

	return iResult;
}
