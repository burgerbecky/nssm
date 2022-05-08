/***************************************

	NSSM file/IO manager

***************************************/

#ifndef __NSSM_IO_H__
#define __NSSM_IO_H__

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

// Defaults for stdin/stdout/stderr file creation
#define NSSM_STDIN_SHARING FILE_SHARE_WRITE
#define NSSM_STDIN_DISPOSITION OPEN_EXISTING
#define NSSM_STDIN_FLAGS FILE_ATTRIBUTE_NORMAL

#define NSSM_STDOUT_SHARING (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define NSSM_STDOUT_DISPOSITION OPEN_ALWAYS
#define NSSM_STDOUT_FLAGS FILE_ATTRIBUTE_NORMAL

#define NSSM_STDERR_SHARING (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define NSSM_STDERR_DISPOSITION OPEN_ALWAYS
#define NSSM_STDERR_FLAGS FILE_ATTRIBUTE_NORMAL

struct nssm_service_t;

struct logger_t {
	// Max size of the log file before starting a new one
	uint64_t m_uSize;
	// Length of a timestamp line
	uint64_t m_uLineLength;

	// Name of the service being logged
	const wchar_t* m_pServiceName;
	// Pathname to the log file
	const wchar_t* m_pPath;

	// Handle for reading from a file
	HANDLE m_hRead;
	// Handle for writing to the log file
	HANDLE m_hWrite;

	// Pointer to the monitored thread ID
	uint32_t* m_pThreadID;
	// Pointer to the log file rotation state
	uint32_t* m_pRotateOnline;

	// Delay in milliseconds for file rotation
	uint32_t m_uRotateDelay;
	// File sharing flags for CreateFileW()
	uint32_t m_uSharing;
	// File disposition flags for CreateFileW()
	uint32_t m_uDisposition;
	// File flags for CreateFileW()
	uint32_t m_uFlags;

	// True if timestamps should be created
	bool m_bTimestampLog;
	// True if files should be copied and trucated
	bool m_bCopyAndTruncate;
};

extern void close_handle(HANDLE* pHandle, HANDLE* pSaved);
extern void close_handle(HANDLE* hHandle);
extern int get_createfile_parameters(HKEY hKey, const wchar_t* pPrefix,
	wchar_t* pPath, uint32_t* pSharing, uint32_t uDefaultSharing,
	uint32_t* pDisposition, uint32_t uDefaultDisposition, uint32_t* pFlags,
	uint32_t uDefaultFlags, bool* pCopyAndTruncate);
extern int set_createfile_parameter(HKEY hKey, const wchar_t* pPrefix,
	const wchar_t* pSuffix, uint32_t uNumber);
extern int delete_createfile_parameter(
	HKEY hKey, const wchar_t* pPrefix, const wchar_t* pSuffix);
extern HANDLE write_to_file(const wchar_t* pPath, uint32_t uSharing,
	SECURITY_ATTRIBUTES* pAttributes, uint32_t uDisposition, uint32_t uFlags);
extern void rotate_file(const wchar_t* pServiceName, const wchar_t* pPath,
	uint32_t uSeconds, uint32_t uDelay, uint32_t uLow, uint32_t uHigh,
	bool bCopyAndTruncate);
extern int get_output_handles(
	nssm_service_t* pNSSMService, STARTUPINFOW* pStartupInfo);
extern int use_output_handles(
	nssm_service_t* pNSSMService, STARTUPINFOW* pStartupInfo);
extern void close_output_handles(STARTUPINFOW* pStartupInfo);
extern void cleanup_loggers(nssm_service_t* pNSSMService);
extern unsigned long WINAPI log_and_rotate(void* pParam);

#endif
