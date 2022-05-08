/***************************************

	Process manager functions

***************************************/

#ifndef __PROCESS_H__
#define __PROCESS_H__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <stdint.h>

#include <Windows.h>

struct nssm_service_t;
struct tagPROCESSENTRY32W;

struct kill_t {
	// Pointer to the name of the thread (Not allocated)
	wchar_t* m_pName;
	// Handle to the parent process
	HANDLE m_hProcess;
	// Depth of recursion
	uint32_t m_uDepth;
	// Process ID of the thread
	uint32_t m_uPID;
	// Exit code received by the thread
	uint32_t m_uExitcode;
	// NSSM_STOP_METHOD* flags
	uint32_t m_uStopMethodFlags;

	// Timeout for await_single_handle()
	uint32_t m_uKillConsoleDelay;
	// Timeout for EnumWindow
	uint32_t m_uKillWindowDelay;
	// Timeout for a thread
	uint32_t m_uKillThreadsDelay;
	// Handle for the service status
	SERVICE_STATUS_HANDLE m_hService;
	// Actual service status
	SERVICE_STATUS* m_pStatus;
	// Timestamp when the thread started
	FILETIME m_uCreationTime;
	// Timestamp when the thread ended
	FILETIME m_uExitTime;
	// Result from PostMessage, TRUE if success
	int m_iSignalled;
};

typedef int (*walk_function_t)(nssm_service_t* pNSSMService, kill_t* pKill);

extern HANDLE get_debug_token(void);
extern void service_kill_t(nssm_service_t* pNSSMService, kill_t* pKill);
extern int get_process_creation_time(HANDLE hProcessHandle, FILETIME* pOutput);
extern int get_process_exit_time(HANDLE hProcessHandle, FILETIME* pOutput);
extern int check_parent(kill_t* pKill, tagPROCESSENTRY32W* pProcessEntry,
	uint32_t uParentProcessID);
extern int CALLBACK kill_window(HWND hWindow, LPARAM pArg);
extern int kill_threads(nssm_service_t* pNSSMService, kill_t* pKill);
extern int kill_threads(kill_t* pKill);
extern int kill_process(nssm_service_t* pNSSMService, kill_t* pKill);
extern int kill_process(kill_t* pKill);
extern int kill_console(nssm_service_t* pNSSMService, kill_t* pKill);
extern int kill_console(kill_t* pKill);
extern void walk_process_tree(nssm_service_t* pNSSMService, walk_function_t,
	kill_t* pKill, uint32_t uParentProcessID);
extern void kill_process_tree(kill_t* pKill, uint32_t uParentProcessID);
extern int print_process(nssm_service_t* pNSSMService, kill_t* pKill);
extern int print_process(kill_t* pKill);

#endif
