/***************************************

	NSSM file/IO manager

***************************************/

#include "nssm_io.h"
#include "constants.h"
#include "event.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "registry.h"
#include "service.h"
#include "utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <Shlwapi.h>
#include <wchar.h>

#include <strsafe.h>

#define COMPLAINED_READ (1 << 0)
#define COMPLAINED_WRITE (1 << 1)
#define COMPLAINED_ROTATE (1 << 2)
#define TIMESTAMP_FORMAT "%04u-%02u-%02u %02u:%02u:%02u.%03u: "
#define TIMESTAMP_LEN 25

static int dup_handle(HANDLE hSource, HANDLE* pDestHandle,
	const wchar_t* pSourceDescription, const wchar_t* pDestDescription,
	uint32_t uFlags)
{
	if (!pDestHandle) {
		return 1;
	}

	if (!DuplicateHandle(GetCurrentProcess(), hSource, GetCurrentProcess(),
			pDestHandle, 0, true, uFlags)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_DUPLICATEHANDLE_FAILED,
			pSourceDescription, pDestDescription, error_string(GetLastError()),
			NULL);
		return 2;
	}
	return 0;
}

static int dup_handle(HANDLE hSource, HANDLE* pDestHandle,
	const wchar_t* pSourceDescription, wchar_t* pDestDescription)
{
	return dup_handle(hSource, pDestHandle, pSourceDescription,
		pDestDescription, DUPLICATE_SAME_ACCESS);
}

/*
  read_handle:  read from application
  pipe_handle:  stdout of application
  write_handle: to file
*/
static HANDLE create_logging_thread(wchar_t* pServiceName, wchar_t* path,
	uint32_t sharing, uint32_t disposition, uint32_t flags,
	HANDLE* read_handle_ptr, HANDLE* pipe_handle_ptr, HANDLE* write_handle_ptr,
	uint32_t rotate_bytes_low, uint32_t rotate_bytes_high,
	uint32_t rotate_delay, uint32_t* tid_ptr, uint32_t* rotate_online,
	bool timestamp_log, bool copy_and_truncate)
{
	*tid_ptr = 0;

	/* Pipe between application's stdout/stderr and our logging handle. */
	if (read_handle_ptr && !*read_handle_ptr) {
		if (pipe_handle_ptr && !*pipe_handle_ptr) {
			if (CreatePipe(read_handle_ptr, pipe_handle_ptr, 0, 0)) {
				SetHandleInformation(
					*pipe_handle_ptr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
			} else {
				log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPIPE_FAILED,
					pServiceName, path, error_string(GetLastError()), NULL);
				return NULL;
			}
		}
	}

	logger_t* pLogger = static_cast<logger_t*>(heap_calloc(sizeof(logger_t)));
	if (!pLogger) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"logger",
			L"create_logging_thread()", NULL);
		return NULL;
	}

	ULARGE_INTEGER size;
	size.LowPart = rotate_bytes_low;
	size.HighPart = rotate_bytes_high;

	pLogger->m_pServiceName = pServiceName;
	pLogger->m_pPath = path;
	pLogger->m_uSharing = sharing;
	pLogger->m_uDisposition = disposition;
	pLogger->m_uFlags = flags;
	pLogger->m_hRead = *read_handle_ptr;
	pLogger->m_hWrite = *write_handle_ptr;
	pLogger->m_uSize = size.QuadPart;
	pLogger->m_pThreadID = tid_ptr;
	pLogger->m_bTimestampLog = timestamp_log;
	pLogger->m_uLineLength = 0;
	pLogger->m_pRotateOnline = rotate_online;
	pLogger->m_uRotateDelay = rotate_delay;
	pLogger->m_bCopyAndTruncate = copy_and_truncate;

	HANDLE hThread = CreateThread(
		NULL, 0, log_and_rotate, pLogger, 0, (DWORD*)pLogger->m_pThreadID);
	if (!hThread) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED,
			error_string(GetLastError()), NULL);
		heap_free(pLogger);
	}

	return hThread;
}

static inline uint32_t guess_charsize(void* pBuffer, uint32_t uBufferSize)
{
	if (IsTextUnicode(pBuffer, static_cast<int>(uBufferSize), NULL)) {
		return static_cast<uint32_t>(sizeof(wchar_t));
	}
	return static_cast<uint32_t>(sizeof(char));
}

/***************************************

	Write out the UTF16 Byte Order Mark

***************************************/

static inline void write_bom(logger_t* pLogger, uint32_t* pOutput)
{
	wchar_t bom = L'\ufeff';
	if (!WriteFile(pLogger->m_hWrite, &bom, sizeof(bom),
			reinterpret_cast<DWORD*>(pOutput), NULL)) {
		log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_SOMEBODY_SET_UP_US_THE_BOM,
			pLogger->m_pServiceName, pLogger->m_pPath,
			error_string(GetLastError()), NULL);
	}
}

void close_handle(HANDLE* pHandle, HANDLE* pSaved)
{
	if (pSaved) {
		*pSaved = INVALID_HANDLE_VALUE;
	}
	if (!pHandle) {
		return;
	}
	if (!*pHandle) {
		return;
	}
	CloseHandle(*pHandle);
	// Save the closed handle
	if (pSaved) {
		*pSaved = *pHandle;
	}
	*pHandle = NULL;
}

void close_handle(HANDLE* hHandle)
{
	close_handle(hHandle, NULL);
}

/* Get path, share mode, creation disposition and flags for a stream. */
int get_createfile_parameters(HKEY hKey, const wchar_t* pPrefix, wchar_t* pPath,
	uint32_t* pSharing, uint32_t uDefaultSharing, uint32_t* pDisposition,
	uint32_t uDefaultDisposition, uint32_t* pFlags, uint32_t uDefaultFlags,
	bool* pCopyAndTruncate)
{
	wchar_t value[NSSM_STDIO_LENGTH];

	/* Path. */
	if (StringCchPrintfW(value, RTL_NUMBER_OF(value), L"%s", pPrefix) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, pPrefix,
			L"get_createfile_parameters()", NULL);
		return 1;
	}
	switch (expand_parameter(hKey, value, pPath, PATH_LENGTH, true, false)) {
	case 0:
		if (!pPath[0]) {
			return 0;
		}
		break; /* OK. */
	default:
		return 2; /* Error. */
	}

	/* ShareMode. */
	if (StringCchPrintfW(value, RTL_NUMBER_OF(value), L"%s%s", pPrefix,
			g_NSSMRegStdIOSharing) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			g_NSSMRegStdIOSharing, L"get_createfile_parameters()", NULL);
		return 3;
	}
	switch (get_number(hKey, value, pSharing, false)) {
	case 0:
		*pSharing = uDefaultSharing;
		break; /* Missing. */
	case 1:
		break; /* Found. */
	case -2:
		return 4; /* Error. */
	}

	/* CreationDisposition. */
	if (StringCchPrintfW(value, RTL_NUMBER_OF(value), L"%s%s", pPrefix,
			g_NSSMRegStdIODisposition) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			g_NSSMRegStdIODisposition, L"get_createfile_parameters()", NULL);
		return 5;
	}
	switch (get_number(hKey, value, pDisposition, false)) {
	case 0:
		*pDisposition = uDefaultDisposition;
		break; /* Missing. */
	case 1:
		break; /* Found. */
	case -2:
		return 6; /* Error. */
	}

	/* Flags. */
	if (StringCchPrintfW(value, RTL_NUMBER_OF(value), L"%s%s", pPrefix,
			g_NSSMRegStdIOFlags) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
			g_NSSMRegStdIOFlags, L"get_createfile_parameters()", NULL);
		return 7;
	}
	switch (get_number(hKey, value, pFlags, false)) {
	case 0:
		*pFlags = uDefaultFlags;
		break; /* Missing. */
	case 1:
		break; /* Found. */
	case -2:
		return 8; /* Error. */
	}

	/* Rotate with CopyFile() and SetEndOfFile(). */
	if (pCopyAndTruncate) {
		uint32_t data;
		if (StringCchPrintfW(value, RTL_NUMBER_OF(value), L"%s%s", pPrefix,
				g_NSSMRegStdIOCopyAndTruncate) < 0) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
				g_NSSMRegStdIOCopyAndTruncate, L"get_createfile_parameters()",
				NULL);
			return 9;
		}
		switch (get_number(hKey, value, &data, false)) {
		case 0:
			*pCopyAndTruncate = false;
			break; /* Missing. */
		case 1:    /* Found. */
			if (data) {
				*pCopyAndTruncate = true;
			} else {
				*pCopyAndTruncate = false;
			}
			break;
		case -2:
			return 9; /* Error. */
		}
	}

	return 0;
}

int set_createfile_parameter(
	HKEY hKey, const wchar_t* pPrefix, const wchar_t* pSuffix, uint32_t uNumber)
{
	wchar_t value[NSSM_STDIO_LENGTH];

	if (StringCchPrintfW(
			value, RTL_NUMBER_OF(value), L"%s%s", pPrefix, pSuffix) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, pSuffix,
			L"set_createfile_parameter()", NULL);
		return 1;
	}

	return set_number(hKey, value, uNumber);
}

int delete_createfile_parameter(
	HKEY hKey, const wchar_t* pPrefix, const wchar_t* pSuffix)
{
	wchar_t value[NSSM_STDIO_LENGTH];

	if (StringCchPrintfW(
			value, RTL_NUMBER_OF(value), L"%s%s", pPrefix, pSuffix) < 0) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, pSuffix,
			L"delete_createfile_parameter()", NULL);
		return 1;
	}

	if (RegDeleteValueW(hKey, value)) {
		return 0;
	}
	return 1;
}

HANDLE write_to_file(const wchar_t* pPath, uint32_t uSharing,
	SECURITY_ATTRIBUTES* pAttributes, uint32_t uDisposition, uint32_t uFlags)
{
	static LARGE_INTEGER offset = {0};
	HANDLE hResult = CreateFileW(
		pPath, FILE_WRITE_DATA, uSharing, pAttributes, uDisposition, uFlags, 0);
	if (hResult != INVALID_HANDLE_VALUE) {
		if (SetFilePointerEx(hResult, offset, 0, FILE_END)) {
			SetEndOfFile(hResult);
		}
		return hResult;
	}

	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEFILE_FAILED, pPath,
		error_string(GetLastError()), NULL);
	return hResult;
}

static void rotated_filename(const wchar_t* pPath, wchar_t* pRotated,
	uint32_t uRotatedLength, SYSTEMTIME* pSystemTime)
{
	// Don't let it go out of scope below.
	SYSTEMTIME now;
	if (!pSystemTime) {
		pSystemTime = &now;
		GetSystemTime(pSystemTime);
	}

	wchar_t buffer[PATH_LENGTH];
	memmove(buffer, pPath, sizeof(buffer));
	wchar_t* ext = PathFindExtensionW(buffer);
	wchar_t extension[PATH_LENGTH];
	StringCchPrintfW(extension, RTL_NUMBER_OF(extension),
		L"-%04u%02u%02uT%02u%02u%02u.%03u%s", pSystemTime->wYear,
		pSystemTime->wMonth, pSystemTime->wDay, pSystemTime->wHour,
		pSystemTime->wMinute, pSystemTime->wSecond, pSystemTime->wMilliseconds,
		ext);
	*ext = 0;
	StringCchPrintfW(pRotated, uRotatedLength, L"%s%s", buffer, extension);
}

void rotate_file(const wchar_t* pServiceName, const wchar_t* pPath,
	uint32_t uSeconds, uint32_t uDelay, uint32_t uLow, uint32_t uHigh,
	bool bCopyAndTruncate)
{
	uint32_t error;

	/* Now. */
	SYSTEMTIME st;
	GetSystemTime(&st);

	BY_HANDLE_FILE_INFORMATION info;

	/* Try to open the file to check if it exists and to get attributes. */
	HANDLE file = CreateFileW(pPath, 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (file != INVALID_HANDLE_VALUE) {
		/* Get file attributes. */
		if (!GetFileInformationByHandle(file, &info)) {
			/* Reuse current time for rotation timestamp. */
			uSeconds = uLow = uHigh = 0;
			SystemTimeToFileTime(&st, &info.ftLastWriteTime);
		}

		CloseHandle(file);
	} else {
		error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND) {
			return;
		}
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ROTATE_FILE_FAILED,
			pServiceName, pPath, L"CreateFile()", pPath, error_string(error),
			NULL);
		/* Reuse current time for rotation timestamp. */
		uSeconds = uLow = uHigh = 0;
		SystemTimeToFileTime(&st, &info.ftLastWriteTime);
	}

	/* Check file age. */
	if (uSeconds) {
		FILETIME ft;
		SystemTimeToFileTime(&st, &ft);

		ULARGE_INTEGER s;
		s.LowPart = ft.dwLowDateTime;
		s.HighPart = ft.dwHighDateTime;
		s.QuadPart -= uSeconds * 10000000LL;
		ft.dwLowDateTime = s.LowPart;
		ft.dwHighDateTime = s.HighPart;
		if (CompareFileTime(&info.ftLastWriteTime, &ft) > 0) {
			return;
		}
	}

	/* Check file size. */
	if (uLow || uHigh) {
		if (info.nFileSizeHigh < uHigh) {
			return;
		}
		if ((info.nFileSizeHigh == uHigh) && (info.nFileSizeLow < uLow)) {
			return;
		}
	}

	/* Get new filename. */
	FileTimeToSystemTime(&info.ftLastWriteTime, &st);

	wchar_t rotated[PATH_LENGTH];
	rotated_filename(pPath, rotated, RTL_NUMBER_OF(rotated), &st);

	/* Rotate. */
	bool ok = true;
	const wchar_t* pFunction;
	if (bCopyAndTruncate) {
		pFunction = L"CopyFile()";
		if (CopyFileW(pPath, rotated, TRUE)) {
			file = write_to_file(pPath, NSSM_STDOUT_SHARING, 0,
				NSSM_STDOUT_DISPOSITION, NSSM_STDOUT_FLAGS);
			Sleep(uDelay);
			SetFilePointer(file, 0, 0, FILE_BEGIN);
			SetEndOfFile(file);
			CloseHandle(file);
		} else {
			ok = false;
		}
	} else {
		pFunction = L"MoveFile()";
		if (!MoveFileW(pPath, rotated)) {
			ok = false;
		}
	}
	if (ok) {
		log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ROTATED, pServiceName,
			pPath, rotated, NULL);
		return;
	}
	error = GetLastError();

	if (error == ERROR_FILE_NOT_FOUND) {
		return;
	}
	log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ROTATE_FILE_FAILED, pServiceName,
		pPath, pFunction, rotated, error_string(error), NULL);
	return;
}

int get_output_handles(nssm_service_t* pNSSMService, STARTUPINFOW* pStartupInfo)
{
	if (!pStartupInfo) {
		return 1;
	}
	bool inherit_handles = false;

	/* Allocate a new console so we get a fresh stdin, stdout and stderr. */
	alloc_console(pNSSMService);

	/* stdin */
	if (pNSSMService->m_StdinPathname[0]) {
		pStartupInfo->hStdInput = CreateFileW(pNSSMService->m_StdinPathname,
			FILE_READ_DATA, pNSSMService->m_uStdinSharing, 0,
			pNSSMService->m_uStdinDisposition, pNSSMService->m_uStdinFlags, 0);
		if (pStartupInfo->hStdInput == INVALID_HANDLE_VALUE) {
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEFILE_FAILED,
				pNSSMService->m_StdinPathname, error_string(GetLastError()),
				NULL);
			return 2;
		}

		inherit_handles = true;
	}

	/* stdout */
	if (pNSSMService->m_StdoutPathname[0]) {
		if (pNSSMService->m_bRotateFiles)
			rotate_file(pNSSMService->m_Name, pNSSMService->m_StdoutPathname,
				pNSSMService->m_uRotateSeconds, pNSSMService->m_uRotateBytesLow,
				pNSSMService->m_uRotateBytesHigh, pNSSMService->m_uRotateDelay,
				pNSSMService->m_bStdoutCopyAndTruncate);
		HANDLE stdout_handle = write_to_file(pNSSMService->m_StdoutPathname,
			pNSSMService->m_uStdoutSharing, 0,
			pNSSMService->m_uStdoutDisposition, pNSSMService->m_uStdoutFlags);
		if (stdout_handle == INVALID_HANDLE_VALUE)
			return 4;
		pNSSMService->m_hStdoutInputPipe = NULL;

		if (pNSSMService->m_bUseStdoutPipe) {
			pNSSMService->m_hStdoutOutputPipe = pStartupInfo->hStdOutput = NULL;
			pNSSMService->m_hStdoutThread = create_logging_thread(
				pNSSMService->m_Name, pNSSMService->m_StdoutPathname,
				pNSSMService->m_uStdoutSharing,
				pNSSMService->m_uStdoutDisposition,
				pNSSMService->m_uStdoutFlags,
				&pNSSMService->m_hStdoutOutputPipe,
				&pNSSMService->m_hStdoutInputPipe, &stdout_handle,
				pNSSMService->m_uRotateBytesLow,
				pNSSMService->m_uRotateBytesHigh, pNSSMService->m_uRotateDelay,
				&pNSSMService->m_uStdoutTID,
				&pNSSMService->m_uRotateStdoutOnline,
				pNSSMService->m_bTimestampLog,
				pNSSMService->m_bStdoutCopyAndTruncate);
			if (!pNSSMService->m_hStdoutThread) {
				CloseHandle(pNSSMService->m_hStdoutOutputPipe);
				CloseHandle(pNSSMService->m_hStdoutInputPipe);
			}
		} else {
			pNSSMService->m_hStdoutThread = NULL;
		}

		if (!pNSSMService->m_hStdoutThread) {
			if (dup_handle(stdout_handle, &pNSSMService->m_hStdoutInputPipe,
					g_NSSMRegStdOut, L"stdout",
					DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
				return 4;
			}
			pNSSMService->m_uRotateStdoutOnline = NSSM_ROTATE_OFFLINE;
		}

		if (dup_handle(pNSSMService->m_hStdoutInputPipe,
				&pStartupInfo->hStdOutput, L"stdout_si", L"stdout")) {
			close_handle(&pNSSMService->m_hStdoutThread);
		}

		inherit_handles = true;
	}

	/* stderr */
	if (pNSSMService->m_StderrPathname[0]) {
		/* Same as stdout? */
		if (str_equiv(pNSSMService->m_StderrPathname,
				pNSSMService->m_StdoutPathname)) {
			pNSSMService->m_uStderrSharing = pNSSMService->m_uStdoutSharing;
			pNSSMService->m_uStderrDisposition =
				pNSSMService->m_uStdoutDisposition;
			pNSSMService->m_uStderrFlags = pNSSMService->m_uStdoutFlags;
			pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_OFFLINE;

			/* Two handles to the same file will create a race. */
			/* XXX: Here we assume that either both or neither handle must be a
			 * pipe. */
			if (dup_handle(pNSSMService->m_hStdoutInputPipe,
					&pNSSMService->m_hStderrInputPipe, L"stdout", L"stderr"))
				return 6;
		} else {
			if (pNSSMService->m_bRotateFiles)
				rotate_file(pNSSMService->m_Name,
					pNSSMService->m_StderrPathname,
					pNSSMService->m_uRotateSeconds,
					pNSSMService->m_uRotateBytesLow,
					pNSSMService->m_uRotateBytesHigh,
					pNSSMService->m_uRotateDelay,
					pNSSMService->m_bStderrCopyAndTruncate);
			HANDLE stderr_handle = write_to_file(pNSSMService->m_StderrPathname,
				pNSSMService->m_uStderrSharing, 0,
				pNSSMService->m_uStderrDisposition,
				pNSSMService->m_uStderrFlags);
			if (stderr_handle == INVALID_HANDLE_VALUE) {
				return 7;
			}
			pNSSMService->m_hStderrInputPipe = NULL;

			if (pNSSMService->m_bUseStderrPipe) {
				pNSSMService->m_hStderrOutputPipe = pStartupInfo->hStdError =
					NULL;
				pNSSMService->m_hStderrThread = create_logging_thread(
					pNSSMService->m_Name, pNSSMService->m_StderrPathname,
					pNSSMService->m_uStderrSharing,
					pNSSMService->m_uStderrDisposition,
					pNSSMService->m_uStderrFlags,
					&pNSSMService->m_hStderrOutputPipe,
					&pNSSMService->m_hStderrInputPipe, &stderr_handle,
					pNSSMService->m_uRotateBytesLow,
					pNSSMService->m_uRotateBytesHigh,
					pNSSMService->m_uRotateDelay, &pNSSMService->m_uStderrTID,
					&pNSSMService->m_uRotateStderrOnline,
					pNSSMService->m_bTimestampLog,
					pNSSMService->m_bStderrCopyAndTruncate);
				if (!pNSSMService->m_hStderrThread) {
					CloseHandle(pNSSMService->m_hStderrOutputPipe);
					CloseHandle(pNSSMService->m_hStderrInputPipe);
				}
			} else {
				pNSSMService->m_hStderrThread = NULL;
			}

			if (!pNSSMService->m_hStderrThread) {
				if (dup_handle(stderr_handle, &pNSSMService->m_hStderrInputPipe,
						g_NSSMRegStdErr, L"stderr",
						DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
					return 7;
				}
				pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_OFFLINE;
			}
		}

		if (dup_handle(pNSSMService->m_hStderrInputPipe,
				&pStartupInfo->hStdError, L"stderr_si", L"stderr")) {
			close_handle(&pNSSMService->m_hStderrThread);
		}

		inherit_handles = true;
	}

	/*
	  We need to set the startup_info flags to make the new handles
	  inheritable by the new process.
	*/
	if (inherit_handles) {
		pStartupInfo->dwFlags |= STARTF_USESTDHANDLES;
	}
	return 0;
}

/* Reuse output handles for a hook. */
int use_output_handles(nssm_service_t* pNSSMService, STARTUPINFOW* pStartupInfo)
{
	pStartupInfo->dwFlags &= ~STARTF_USESTDHANDLES;

	if (pNSSMService->m_hStdoutInputPipe) {
		if (dup_handle(pNSSMService->m_hStdoutInputPipe,
				&pStartupInfo->hStdOutput, L"stdout_pipe", L"hStdOutput")) {
			return 1;
		}
		pStartupInfo->dwFlags |= STARTF_USESTDHANDLES;
	}

	if (pNSSMService->m_hStderrInputPipe) {
		if (dup_handle(pNSSMService->m_hStderrInputPipe,
				&pStartupInfo->hStdError, L"stderr_pipe", L"hStdError")) {
			if (pStartupInfo->hStdOutput) {
				pStartupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
				CloseHandle(pStartupInfo->hStdOutput);
			}
			return 2;
		}
		pStartupInfo->dwFlags |= STARTF_USESTDHANDLES;
	}

	return 0;
}

void close_output_handles(STARTUPINFOW* pStartupInfo)
{
	if (pStartupInfo->hStdInput) {
		CloseHandle(pStartupInfo->hStdInput);
	}
	if (pStartupInfo->hStdOutput) {
		CloseHandle(pStartupInfo->hStdOutput);
	}
	if (pStartupInfo->hStdError) {
		CloseHandle(pStartupInfo->hStdError);
	}
}

void cleanup_loggers(nssm_service_t* pNSSMService)
{
	uint32_t interval = NSSM_CLEANUP_LOGGERS_DEADLINE;
	HANDLE thread_handle = INVALID_HANDLE_VALUE;

	close_handle(&pNSSMService->m_hStdoutThread, &thread_handle);
	/* Close write end of the data pipe so logging thread can finalise read. */
	close_handle(&pNSSMService->m_hStdoutInputPipe);
	/* Await logging thread then close read end. */
	if (thread_handle != INVALID_HANDLE_VALUE) {
		WaitForSingleObject(thread_handle, interval);
	}
	close_handle(&pNSSMService->m_hStdoutOutputPipe);

	thread_handle = INVALID_HANDLE_VALUE;
	close_handle(&pNSSMService->m_hStderrThread, &thread_handle);
	close_handle(&pNSSMService->m_hStderrInputPipe);
	if (thread_handle != INVALID_HANDLE_VALUE) {
		WaitForSingleObject(thread_handle, interval);
	}
	close_handle(&pNSSMService->m_hStderrOutputPipe);
}

/*
  Try multiple times to read from a file.
  Returns:  0 on success.
			1 on non-fatal error.
		   -1 on fatal error.
*/
static int try_read(logger_t* pLogger, void* pBuffer, uint32_t uBufferSize,
	uint32_t* pReadIn, int* pComplained)
{
	uint32_t error;
	int ret = 1;
	for (int tries = 0; tries < 5; tries++) {
		if (ReadFile(pLogger->m_hRead, pBuffer, uBufferSize,
				reinterpret_cast<DWORD*>(pReadIn), NULL)) {
			return 0;
		}

		error = GetLastError();
		switch (error) {
		/* Other end closed the pipe. */
		case ERROR_BROKEN_PIPE:
			ret = -1;
			goto complain_read;

		/* Couldn't lock the buffer. */
		case ERROR_NOT_ENOUGH_QUOTA:
			Sleep(2000U + static_cast<uint32_t>(tries) * 3000U);
			ret = 1;
			continue;

		/* Write was cancelled by the other end. */
		case ERROR_OPERATION_ABORTED:
			ret = 1;
			goto complain_read;

		default:
			ret = -1;
		}
	}

complain_read:
	/* Ignore the error if we've been requested to exit anyway. */
	if (*pLogger->m_pRotateOnline != NSSM_ROTATE_ONLINE) {
		return ret;
	}
	if (!(*pComplained & COMPLAINED_READ)) {
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_READFILE_FAILED,
			pLogger->m_pServiceName, pLogger->m_pPath, error_string(error),
			NULL);
	}
	*pComplained |= COMPLAINED_READ;
	return ret;
}

/*
  Try multiple times to write to a file.
  Returns:  0 on success.
			1 on non-fatal error.
		   -1 on fatal error.
*/
static int try_write(logger_t* pLogger, void* pBuffer, uint32_t uBufferSize,
	uint32_t* pWritten, int* pComplained)
{
	int ret = 1;
	unsigned long error;
	for (int tries = 0; tries < 5; tries++) {
		if (WriteFile(pLogger->m_hWrite, pBuffer, uBufferSize,
				reinterpret_cast<DWORD*>(pWritten), NULL)) {
			return 0;
		}

		error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			/* Operation was successful pending flush to disk. */
			return 0;
		}

		switch (error) {
		/* Other end closed the pipe. */
		case ERROR_BROKEN_PIPE:
			ret = -1;
			goto complain_write;

		/* Couldn't lock the buffer. */
		case ERROR_NOT_ENOUGH_QUOTA:
		/* Out of disk space. */
		case ERROR_DISK_FULL:
			Sleep(2000U + static_cast<uint32_t>(tries) * 3000U);
			ret = 1;
			continue;

		default:
			/* We'll lose this line but try to read and write subsequent ones.
			 */
			ret = 1;
		}
	}

complain_write:
	if (!(*pComplained & COMPLAINED_WRITE))
		log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_WRITEFILE_FAILED,
			pLogger->m_pServiceName, pLogger->m_pPath, error_string(error),
			NULL);
	*pComplained |= COMPLAINED_WRITE;
	return ret;
}

/* Note that the timestamp is created in UTF-8. */
static inline int write_timestamp(
	logger_t* pLogger, uint32_t uCharsize, uint32_t* pWritten, int* pComplained)
{
	char timestamp[TIMESTAMP_LEN + 1];

	SYSTEMTIME now;
	GetSystemTime(&now);
	snprintf(timestamp, RTL_NUMBER_OF(timestamp), TIMESTAMP_FORMAT, now.wYear,
		now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
		now.wMilliseconds);

	if (uCharsize == sizeof(char)) {
		return try_write(
			pLogger, timestamp, TIMESTAMP_LEN, pWritten, pComplained);
	}
	wchar_t* utf16;
	uint32_t utf16len;
	if (to_utf16(timestamp, &utf16, &utf16len)) {
		return -1;
	}
	int ret = try_write(
		pLogger, utf16, utf16len * sizeof(wchar_t), pWritten, pComplained);
	heap_free(utf16);
	return ret;
}

static int write_with_timestamp(logger_t* pLogger, void* pBuffer,
	uint32_t uBufferSize, uint32_t* pWritten, int* pComplained,
	uint32_t uCharsize)
{
	if (pLogger->m_bTimestampLog) {
		uint32_t log_out;
		int log_complained;
		uint32_t timestamp_out = 0;
		int timestamp_complained;
		if (!pLogger->m_uLineLength) {
			write_timestamp(
				pLogger, uCharsize, &timestamp_out, &timestamp_complained);
			pLogger->m_uLineLength += timestamp_out;
			*pWritten += timestamp_out;
			*pComplained |= timestamp_complained;
		}

		uint32_t i;
		void* line = pBuffer;
		uint32_t offset = 0;
		int ret;
		for (i = 0; i < uBufferSize; i++) {
			if (static_cast<const char*>(pBuffer)[i] == '\n') {
				ret = try_write(
					pLogger, line, i - offset + 1, &log_out, &log_complained);
				line = (void*)((char*)line + i - offset + 1);
				pLogger->m_uLineLength = 0LL;
				*pWritten += log_out;
				*pComplained |= log_complained;
				offset = i + 1;
				if (offset < uBufferSize) {
					write_timestamp(pLogger, uCharsize, &timestamp_out,
						&timestamp_complained);
					pLogger->m_uLineLength += timestamp_out;
					*pWritten += timestamp_out;
					*pComplained |= timestamp_complained;
				}
			}
		}

		if (offset < uBufferSize) {
			ret = try_write(
				pLogger, line, uBufferSize - offset, &log_out, &log_complained);
			*pWritten += log_out;
			*pComplained |= log_complained;
		}

		return ret;
	} else {
		return try_write(pLogger, pBuffer, uBufferSize, pWritten, pComplained);
	}
}

/***************************************

	Wrapper to be called in a new thread for logging.

	Called by CreateThread

***************************************/

unsigned long WINAPI log_and_rotate(void* pParam)
{
	logger_t* pLogger = static_cast<logger_t*>(pParam);
	if (!pLogger) {
		return 1;
	}

	uint64_t size;
	BY_HANDLE_FILE_INFORMATION info;

	/* Find initial file size. */
	if (!GetFileInformationByHandle(pLogger->m_hWrite, &info)) {
		pLogger->m_uSize = 0LL;
	} else {
		ULARGE_INTEGER l;
		l.HighPart = info.nFileSizeHigh;
		l.LowPart = info.nFileSizeLow;
		size = l.QuadPart;
	}

	char buffer[1024];
	void* address;
	uint32_t in, out;
	unsigned long charsize = 0;
	unsigned long error;
	int ret;
	int complained = 0;

	while (true) {
		/* Read data from the pipe. */
		address = &buffer;
		ret = try_read(pLogger, address, sizeof(buffer), &in, &complained);
		if (ret < 0) {
			close_handle(&pLogger->m_hRead);
			close_handle(&pLogger->m_hWrite);
			heap_free(pLogger);
			return 2;
		} else if (ret)
			continue;

		if (*pLogger->m_pRotateOnline == NSSM_ROTATE_ONLINE_ASAP ||
			(pLogger->m_uSize && (size + in) >= pLogger->m_uSize)) {
			/* Look for newline. */
			unsigned long i;
			for (i = 0; i < in; i++) {
				if (buffer[i] == '\n') {
					if (!charsize) {
						charsize = guess_charsize(address, in);
					}
					i += charsize;

					/* Write up to the newline. */
					ret = try_write(pLogger, address, i, &out, &complained);
					if (ret < 0) {
						close_handle(&pLogger->m_hRead);
						close_handle(&pLogger->m_hWrite);
						heap_free(pLogger);
						return 3;
					}
					size += out;

					/* Rotate. */
					*pLogger->m_pRotateOnline = NSSM_ROTATE_ONLINE;
					wchar_t rotated[PATH_LENGTH];
					rotated_filename(
						pLogger->m_pPath, rotated, RTL_NUMBER_OF(rotated), 0);

					/*
					  Ideally we'd try the rename first then close the handle
					  but MoveFile() will fail if the handle is still open so we
					  must risk losing everything.
					*/
					if (pLogger->m_bCopyAndTruncate) {
						FlushFileBuffers(pLogger->m_hWrite);
					}
					close_handle(&pLogger->m_hWrite);
					bool ok = true;
					wchar_t* function;
					if (pLogger->m_bCopyAndTruncate) {
						function = L"CopyFile()";
						if (CopyFileW(pLogger->m_pPath, rotated, TRUE)) {
							HANDLE file = write_to_file(pLogger->m_pPath,
								NSSM_STDOUT_SHARING, 0, NSSM_STDOUT_DISPOSITION,
								NSSM_STDOUT_FLAGS);
							Sleep(pLogger->m_uRotateDelay);
							SetFilePointer(file, 0, 0, FILE_BEGIN);
							SetEndOfFile(file);
							CloseHandle(file);
						} else {
							ok = false;
						}
					} else {
						function = L"MoveFile()";
						if (!MoveFileW(pLogger->m_pPath, rotated)) {
							ok = false;
						}
					}
					if (ok) {
						log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ROTATED,
							pLogger->m_pServiceName, pLogger->m_pPath, rotated,
							NULL);
						size = 0LL;
					} else {
						error = GetLastError();
						if (error != ERROR_FILE_NOT_FOUND) {
							if (!(complained & COMPLAINED_ROTATE))
								log_event(EVENTLOG_ERROR_TYPE,
									NSSM_EVENT_ROTATE_FILE_FAILED,
									pLogger->m_pServiceName, pLogger->m_pPath,
									function, rotated, error_string(error),
									NULL);
							complained |= COMPLAINED_ROTATE;
							/* We can at least try to re-open the existing file.
							 */
							pLogger->m_uDisposition = OPEN_ALWAYS;
						}
					}

					/* Reopen. */
					pLogger->m_hWrite =
						write_to_file(pLogger->m_pPath, pLogger->m_uSharing, 0,
							pLogger->m_uDisposition, pLogger->m_uFlags);
					if (pLogger->m_hWrite == INVALID_HANDLE_VALUE) {
						error = GetLastError();
						log_event(EVENTLOG_ERROR_TYPE,
							NSSM_EVENT_CREATEFILE_FAILED, pLogger->m_pPath,
							error_string(error), NULL);
						/* Oh dear.  Now we can't log anything further. */
						close_handle(&pLogger->m_hRead);
						close_handle(&pLogger->m_hWrite);
						heap_free(pLogger);
						return 4;
					}

					/* Resume writing after the newline. */
					address = (void*)((char*)address + i);
					in -= i;
				}
			}
		}

		if (!size || pLogger->m_bTimestampLog) {
			if (!charsize) {
				charsize = guess_charsize(address, in);
			}
		}
		if (!size) {
			/* Write a BOM to the new file. */
			if (charsize == sizeof(wchar_t)) {
				write_bom(pLogger, &out);
			}
			size += (__int64)out;
		}

		/* Write the data, if any. */
		if (!in) {
			continue;
		}

		ret = write_with_timestamp(
			pLogger, address, in, &out, &complained, charsize);
		size += out;
		if (ret < 0) {
			close_handle(&pLogger->m_hRead);
			close_handle(&pLogger->m_hWrite);
			heap_free(pLogger);
			return 3;
		}
	}

	close_handle(&pLogger->m_hRead);
	close_handle(&pLogger->m_hWrite);
	heap_free(pLogger);
	return 0;
}
