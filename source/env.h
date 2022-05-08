/***************************************

	Environment variable handlers

***************************************/

#ifndef __ENV_H__
#define __ENV_H__

#include <stdint.h>

extern uintptr_t environment_length(const wchar_t* pEnvironmentVars);
extern wchar_t* copy_environment_block(wchar_t* pEnvironmentVars);
extern wchar_t* useful_environment(wchar_t* pEnvironmentVars);
extern wchar_t* expand_environment_string(wchar_t* pString);
extern uint32_t set_environment_block(wchar_t* pEnvironmentVars);
extern uint32_t clear_environment(void);
extern uint32_t duplicate_environment(wchar_t* pEnvironmentVars);
extern int test_environment(wchar_t* pEnvironmentVars);
extern void duplicate_environment_strings(wchar_t* pEnvironmentVars);
extern wchar_t* copy_environment(void);
extern int append_to_environment_block(wchar_t* pEnvironmentVars,
	uintptr_t uEnvironmentLength, wchar_t* pString, wchar_t** pNewEnvironment,
	uintptr_t* pNewLength);
extern int remove_from_environment_block(wchar_t* pEnvironmentVars,
	uintptr_t uEnvironmentLength, const wchar_t* pString,
	wchar_t** pNewEnvironment, uintptr_t* pNewLength);

#endif
