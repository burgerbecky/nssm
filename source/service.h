/***************************************

	Windows services manager

***************************************/

#ifndef __SERVICE_H__
#define __SERVICE_H__

#ifndef __CONSTANTS_H__
#include "constants.h"
#endif

#ifndef __IMPORTS_H__
#include "imports.h"
#endif

#include <stdint.h>

// Note: NSSM_ROTATE_OFFLINE must be zero so that some tests will success for
// testing online/offline status
#define NSSM_ROTATE_OFFLINE 0
#define NSSM_ROTATE_ONLINE 1
#define NSSM_ROTATE_ONLINE_ASAP 2

struct nssm_service_t {

	// CPU affinity flags
	uint64_t m_uAffinity;

	// Time in 100ns granularity for the throttle timer to timeout
	LARGE_INTEGER m_iThrottleDuetime;

	// User name for the account to log in with
	wchar_t* m_pUsername;
	// Length of the user name in elements including the terminating zero
	uintptr_t m_uUsernameLength;

	// Password token for the account to log in with
	wchar_t* m_pPassword;
	// Length of the password in elements including the terminating zero
	uintptr_t m_uPasswordLength;

	// List of dependencies before the service can start
	wchar_t* m_pDependencies;
	// Length of the dependencies string in elements
	uintptr_t m_uDependenciesLength;

	// Environment variables to set for the spawned service
	wchar_t* m_pEnvironmentVariables;
	// Length of m_pEnvironmentVariables
	uintptr_t m_uEnvironmentVariablesLength;

	// Environment variables to APPEND (Not replace) for the spawned service
	wchar_t* m_pExtraEnvironmentVariables;
	// Length of m_pExtraEnvironmentVariables
	uintptr_t m_uExtraEnvironmentVariablesLength;

	// String with the initial environment variables
	wchar_t* m_pInitialEnvironmentVariables;

	// Service control manager handle
	SC_HANDLE m_hServiceControlManager;

	// Stdout input pipe
	HANDLE m_hStdoutInputPipe;
	// Stdout output pipe
	HANDLE m_hStdoutOutputPipe;
	// Stdout thread handle
	HANDLE m_hStdoutThread;

	// Stderr input pipe
	HANDLE m_hStderrInputPipe;
	// Stderr output pipe
	HANDLE m_hStderrOutputPipe;
	// Stderr thread handle
	HANDLE m_hStderrThread;

	// Handle for the throttling timer
	HANDLE m_hThrottleTimer;
	// Handle for the process under our control
	HANDLE m_hProcess;
	// Handle for event waiting
	HANDLE m_hWait;
	// Handle to the status of the service
	SERVICE_STATUS_HANDLE m_hStatusHandle;

	// Current status of the service
	SERVICE_STATUS m_ServiceStatus;

	// Thread lock for throttle
	CRITICAL_SECTION m_ThrottleSection;
	// Thread lock for hook
	CRITICAL_SECTION m_HookLock;
	// Sleep/Wake condition for throttling
	CONDITION_VARIABLE m_ThrottleCondition;

	// Time mark when NSSM started
	FILETIME m_NSSMCreationTime;
	// Time mark when the controlled process started
	FILETIME m_ProcessCreationTime;
	// Time mark when the controlled process ended
	FILETIME m_ProcessExitTime;

	// Enumeration for startup type (Auto, manual, delayed)
	uint32_t m_uStartup;
	// Windows Service Types (Bit Mask)
	uint32_t m_uServiceTypes;
	// NSSM_EXIT_* enumeration
	uint32_t m_uDefaultExitAction;

	// Service thread priority mask for Windows
	uint32_t m_uPriority;
	// If true, don't spawn a text console when launching
	uint32_t m_bDontSpawnConsole;
	// NSSM_STOP_METHOD_* flags
	uint32_t m_uStopMethodFlags;
	// Delay in milliseconds before killing console
	uint32_t m_uKillConsoleDelay;
	// Delay in milliseconds before killing windows
	uint32_t m_uKillWindowDelay;
	// Delay in milliseconds before killing threads
	uint32_t m_uKillThreadsDelay;
	// Delay in milliseconds before restarting service
	uint32_t m_uRestartDelay;
	// Delay in milliseconds before throttling service
	uint32_t m_uThrottleDelay;
	// Number of times a service has been throttled
	uint32_t m_uThrottle;
	// Delay in milliseconds before rotating log files
	uint32_t m_uRotateDelay;
	// Restrict rotation to files older than this length in seconds
	uint32_t m_uRotateSeconds;
	// Lower 32 bits of the file size needed to rotate logs
	uint32_t m_uRotateBytesLow;
	// Upper 32 bits of the file size needed to rotate logs
	uint32_t m_uRotateBytesHigh;

	// Stdin file sharing flags for CreateFileW()
	uint32_t m_uStdinSharing;
	// Stdin file disposition flags for CreateFileW()
	uint32_t m_uStdinDisposition;
	// Stdin file flags for CreateFileW()
	uint32_t m_uStdinFlags;

	// Stdout file sharing flags for CreateFileW()
	uint32_t m_uStdoutSharing;
	// Stdout file disposition flags for CreateFileW()
	uint32_t m_uStdoutDisposition;
	// Stdout file flags for CreateFileW()
	uint32_t m_uStdoutFlags;
	// Stdout file thread ID
	uint32_t m_uStdoutTID;
	// NSSM_ROTATE_* enumeration for stdout
	uint32_t m_uRotateStdoutOnline;

	// Stderr file sharing flags for CreateFileW()
	uint32_t m_uStderrSharing;
	// Stderr file disposition flags for CreateFileW()
	uint32_t m_uStderrDisposition;
	// Stderr file flags for CreateFileW()
	uint32_t m_uStderrFlags;
	// Stderr file thread ID
	uint32_t m_uStderrTID;
	// NSSM_ROTATE_* enumeration for stderr
	uint32_t m_uRotateStderrOnline;
	// SERVICE_CONTROL_* enumeration
	uint32_t m_uLastControl;

	// Number of times a start was requested
	uint32_t m_uStartRequestedCount;
	// Number of times a thread started
	uint32_t m_uStartCount;
	// Number of times a thread exited
	uint32_t m_uExitCount;
	// PID of the managed process
	uint32_t m_uPID;
	// Exit code returned by the thread upon termination
	uint32_t m_uExitcode;

	// Name of the service
	wchar_t m_Name[SERVICE_NAME_LENGTH];
	// Name to display of the service
	wchar_t m_DisplayName[SERVICE_NAME_LENGTH];
	// Description string for the service
	wchar_t m_Description[VALUE_LENGTH];
	// Pathname to NSSM
	wchar_t m_NSSMExecutablePathname[PATH_LENGTH];
	// Executable path
	wchar_t m_ExecutablePath[EXE_LENGTH];
	// Command line parameters for the exe
	wchar_t m_AppParameters[VALUE_LENGTH];
	// Working directory for the service
	wchar_t m_WorkingDirectory[DIR_LENGTH];

	// Pathname of file to point to for stdin
	wchar_t m_StdinPathname[PATH_LENGTH];
	// Pathname of file to point to for stdout
	wchar_t m_StdoutPathname[PATH_LENGTH];
	// Pathname of file to point to for stderr
	wchar_t m_StderrPathname[PATH_LENGTH];

	// Redirect stdout
	bool m_bUseStdoutPipe;
	// Redirect stderr
	bool m_bUseStderrPipe;
	// Allow copying and truncating stdout log file
	bool m_bStdoutCopyAndTruncate;
	// Allow copying and truncating stderr log file
	bool m_bStderrCopyAndTruncate;

	// True if managing NSSM itself
	bool m_bNative;
	// True if killing the process also kills the entire tree
	bool m_bKillProcessTree;
	// True if log files are rotated
	bool m_bRotateFiles;
	// Add a timestamp when logging
	bool m_bTimestampLog;

	// m_ThrottleSection is valid
	bool m_bThrottleSectionValid;
	// m_HookLock is valid
	bool m_bHookLockValid;
	// Shutdown in progress if true
	bool m_bStopping;
	// Restart the service if it crashed
	bool m_bAllowRestart;

	// Redirect output from hooks
	bool m_bHookShareOutputHandles;
};

extern int affinity_mask_to_string(uint64_t uMask, wchar_t** ppString);
extern int affinity_string_to_mask(const wchar_t* pString, uint64_t* pMask);
extern uint32_t priority_mask(void);
extern uint32_t priority_constant_to_index(uint32_t uConstant);
extern uint32_t priority_index_to_constant(uint32_t uIndex);
extern void set_service_environment(nssm_service_t* pNSSMService);
extern void unset_service_environment(nssm_service_t* pNSSMService);
extern SC_HANDLE open_service_manager(uint32_t uAccess);
extern SC_HANDLE open_service(SC_HANDLE hService, const wchar_t* pServiceName,
	uint32_t uAccess, wchar_t* pCanonicalName, uint32_t uCanonicalLength);
extern QUERY_SERVICE_CONFIGW* query_service_config(
	const wchar_t* pServiceName, SC_HANDLE hService);
extern int append_to_dependencies(wchar_t* pDependencies,
	uintptr_t uDependenciesLength, wchar_t* pString,
	wchar_t** ppNewDependencies, uintptr_t* pNewLength, uint32_t uType);
extern int remove_from_dependencies(wchar_t* pDependencies,
	uintptr_t uDependenciesLength, wchar_t* pString,
	wchar_t** ppNewDependencies, uintptr_t* pNewLength, uint32_t uType);
extern int set_service_dependencies(
	const wchar_t* pServiceName, SC_HANDLE hService, wchar_t* pBuffer);
extern int get_service_dependencies(const wchar_t* pServiceName,
	SC_HANDLE hService, wchar_t** ppBuffer, uintptr_t* pBufferSize,
	uint32_t uType);
extern int get_service_dependencies(const wchar_t* pServiceName,
	SC_HANDLE hService, wchar_t** ppBuffer, uintptr_t* pBufferSize);
extern int set_service_description(
	const wchar_t* pServiceName, SC_HANDLE hService, wchar_t* pBuffer);
extern int get_service_description(const wchar_t* pServiceName,
	SC_HANDLE hService, uint32_t uBufferLength, wchar_t* pBuffer);
extern int get_service_startup(const wchar_t* pServiceName, SC_HANDLE hService,
	const QUERY_SERVICE_CONFIGW* pQueryServiceConfig, uint32_t* pStartup);
extern int get_service_username(const wchar_t* pServiceName,
	const QUERY_SERVICE_CONFIGW* pQueryServiceConfig, wchar_t** ppUsername,
	uintptr_t* pUsernameLength);
extern void set_nssm_service_defaults(nssm_service_t* pNSSMService);
extern nssm_service_t* alloc_nssm_service(void);
extern void cleanup_nssm_service(nssm_service_t* pNSSMService);
extern int pre_install_service(int iArgc, wchar_t** ppArgv);
extern int pre_edit_service(int iArgc, wchar_t** ppArgv);
extern int pre_remove_service(int iArgc, wchar_t** ppArgv);
extern int install_service(nssm_service_t* pNSSMService);
extern int edit_service(nssm_service_t* pNSSMService, bool bEditing);
extern int control_service(
	uint32_t uControl, int iArgc, wchar_t** ppArgv, bool bReturnStatus);
extern int control_service(uint32_t uControl, int iArgc, wchar_t** ppArgv);
extern int remove_service(nssm_service_t* pNSSMService);
extern void WINAPI service_main(unsigned long uArgc, wchar_t** ppArgv);
extern void set_service_recovery(nssm_service_t* pNSSMService);
extern uint32_t monitor_service(nssm_service_t* pNSSMService);
extern const wchar_t* service_control_text(uint32_t uControl);
extern const wchar_t* service_status_text(uint32_t uStatus);
extern void log_service_control(
	const wchar_t* pServiceName, uint32_t uControl, bool bHandled);
extern int start_service(nssm_service_t* pNSSMService);
extern uint32_t stop_service(nssm_service_t* pNSSMService, uint32_t uExitcode,
	bool bGraceful, bool bDefaultAction);
extern void CALLBACK end_service(void* pArg, unsigned char bWhy);
extern void throttle_restart(nssm_service_t* pNSSMService);
extern int await_single_handle(SERVICE_STATUS_HANDLE hStatusHandle,
	SERVICE_STATUS* pServiceStatus, HANDLE hHandle, const wchar_t* pName,
	const wchar_t* pFunctionName, uint32_t uTimeout);
extern int list_nssm_services(int iArgc, wchar_t** ppArgv);
extern int service_process_tree(int iArgc, wchar_t** ppArgv);
extern void alloc_console(nssm_service_t* pNSSMService);

#endif
