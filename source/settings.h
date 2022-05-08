/***************************************

	settings manager

***************************************/

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdint.h>

// Are additional arguments needed?
#define ADDITIONAL_GETTING (1U << 0U)
#define ADDITIONAL_SETTING (1U << 1U)
#define ADDITIONAL_RESETTING (1U << 2U)
#define ADDITIONAL_CRLF (1U << 3U)
#define ADDITIONAL_MANDATORY \
	ADDITIONAL_GETTING | ADDITIONAL_SETTING | ADDITIONAL_RESETTING

#define DEPENDENCY_SERVICES (1 << 0)
#define DEPENDENCY_GROUPS (1 << 1)
#define DEPENDENCY_ALL (DEPENDENCY_SERVICES | DEPENDENCY_GROUPS)

struct HKEY__;
struct SC_HANDLE__;

union value_t {
	// Numeric value
	uint32_t m_uNumber;
	// Pointer to string value
	wchar_t* m_pString;
};

typedef int (*SettingFunctionProc)(const wchar_t* pServiceName, void* pParam,
	const wchar_t* pName, void* pDefaultValue, value_t* pValue,
	const wchar_t* pAdditional);

struct settings_t {
	// Name of the setting
	const wchar_t* m_pName;
	// Windows registry data type
	uint32_t m_uType;
	// Default data
	void* m_pDefaultValue;
	// Native class of setting entries
	bool m_bNative;
	// Additional flags ADDITIONAL_*
	uint32_t m_uAdditional;
	// Function to set value
	SettingFunctionProc m_pSet;
	// Function to get value
	SettingFunctionProc m_pGet;
	// Function to dump value
	SettingFunctionProc m_pDump;
};

extern const settings_t g_Settings[];

extern int set_setting(const wchar_t* pServiceName, HKEY__* hKey,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional);
extern int set_setting(const wchar_t* pServiceName, SC_HANDLE__* hService,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional);
extern int get_setting(const wchar_t* pServiceName, HKEY__* hKey,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional);
extern int get_setting(const wchar_t* pServiceName, SC_HANDLE__* hService,
	const settings_t* pSettings, value_t* pValue, const wchar_t* pAdditional);
extern int dump_setting(const wchar_t* pServiceName, HKEY__* hKey,
	SC_HANDLE__* hService, const settings_t* pSettings);

#endif
