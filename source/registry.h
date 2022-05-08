/***************************************

	Registry handler

***************************************/

#ifndef __REGISTRY_H__
#define __REGISTRY_H__

#define NSSM_STDIO_LENGTH 29

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

struct nssm_service_t;

extern int create_messages(void);
extern int enumerate_registry_values(
	HKEY hKey, uint32_t* pIndex, wchar_t* pName, uint32_t uNameLength);
extern int create_parameters(nssm_service_t* pNSSMService, bool bEditing);
extern int create_exit_action(
	const wchar_t* pServiceName, const wchar_t* pActionString, bool bEditing);
extern int get_environment(const wchar_t* pServiceName, HKEY hKey,
	const wchar_t* pValueName, wchar_t** ppEnvironmentVariables,
	uintptr_t* pEnvironmentVariablesLength);
extern int get_string(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bExpand, bool bSanitize, bool bMustExist);
extern int get_string(HKEY hKey, const wchar_t* pValueName, wchar_t* pBuffer,
	uint32_t uBufferLength, bool bSanitize);
extern int expand_parameter(HKEY hKey, const wchar_t* pValueName,
	wchar_t* pBuffer, uint32_t uBufferLength, bool bSanitize, bool bMustExist);
extern int expand_parameter(HKEY hKey, const wchar_t* pValueName,
	wchar_t* pBuffer, uint32_t uBufferLength, bool bSanitize);
extern int set_string(
	HKEY hKey, const wchar_t* pValueName, const wchar_t* pString, bool bExpand);
extern int set_string(
	HKEY hKey, const wchar_t* pValueName, const wchar_t* pString);
extern int set_expand_string(
	HKEY hKey, const wchar_t* pValueName, const wchar_t* pString);
extern int set_number(HKEY hKey, const wchar_t* pValueName, uint32_t uNumber);
extern int get_number(
	HKEY hKey, const wchar_t* pValueName, uint32_t* pNumber, bool bMustExist);
extern int get_number(HKEY hKey, const wchar_t* pValueName, uint32_t* pNumber);
extern int format_double_null(const wchar_t* pInput, uintptr_t uInputLength,
	wchar_t** ppFormatted, uintptr_t* pFormattedLength);
extern int unformat_double_null(wchar_t* pFormatted, uintptr_t uFormattedLength,
	wchar_t** ppParsed, uintptr_t* pParsedLength);
extern int copy_double_null(
	const wchar_t* pInput, uintptr_t uInputLength, wchar_t** ppOutput);
extern int append_to_double_null(const wchar_t* pInput, uintptr_t uInputLength,
	wchar_t** ppOutput, uintptr_t* pOutputLength, const wchar_t* pAppend,
	uintptr_t uKeyLength, bool bCaseSensitive);
extern int remove_from_double_null(const wchar_t* pInput,
	uintptr_t uInputLength, wchar_t** ppOutput, uintptr_t* pOutputLength,
	const wchar_t* pRemove, uintptr_t uKeyLength, bool bCaseSensitive);
extern void override_milliseconds(const wchar_t* pServiceName, HKEY hKey,
	const wchar_t* pValueName, uint32_t* pNumber, uint32_t uDefaultValue,
	uint32_t uLogEvent);
extern HKEY open_service_registry(
	const wchar_t* pServiceName, REGSAM uAccessMask, bool bMustExist);
extern long open_registry(const wchar_t* pServiceName, const wchar_t* pSub,
	REGSAM uAccessMask, HKEY* pKey, bool bMustExist);
extern HKEY open_registry(const wchar_t* pServiceName, const wchar_t* pSub,
	REGSAM uAccessMask, bool bMustExist);
extern HKEY open_registry(
	const wchar_t* pServiceName, const wchar_t* pSub, REGSAM uAccessMask);
extern HKEY open_registry(const wchar_t* pServiceName, REGSAM uAccessMask);
extern int get_io_parameters(nssm_service_t* pNSSMService, HKEY hKey);
extern int get_parameters(
	nssm_service_t* pNSSMService, const STARTUPINFOW* pStartupInfo);
extern int get_exit_action(const wchar_t* pServiceName, uint32_t* pExitcode,
	wchar_t* pAction, bool* pDefaultAction);
extern int set_hook(const wchar_t* pServiceName, const wchar_t* pHookEvent,
	const wchar_t* pHookAction, const wchar_t* pCommandLine);
extern int get_hook(const wchar_t* pServiceName, const wchar_t* pHookEvent,
	const wchar_t* pHookAction, wchar_t* pBuffer, uint32_t uBufferLength);

#endif
