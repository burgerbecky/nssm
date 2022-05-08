/***************************************

	Main entry and common subroutines

***************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500
#endif

#include "nssm.h"
#include "console.h"
#include "constants.h"
#include "event.h"
#include "imports.h"
#include "memorymanager.h"
#include "messages.h"
#include "registry.h"
#include "service.h"
#include "utf8.h"

#include <tchar.h>
#include <wchar.h>
#include <windows.h>

#include <ShellApi.h>
#include <Shlwapi.h>
#include <stdlib.h>
#include <strsafe.h>

// These are very large buffers (32K elements each)
static wchar_t unquoted_imagepath[PATH_LENGTH];
static wchar_t imagepath[PATH_LENGTH];
static wchar_t imageargv0[PATH_LENGTH];

/***************************************

	Exit NSSM

***************************************/

void nssm_exit(int iStatus)
{
	free_imports();

	// Free up the event manager
	unsetup_event();

	// Undo the UTF8 code page output
	unsetup_utf8();
	exit(iStatus);
}

/***************************************

	Are two strings case-insensitively equivalent?

	Return true if they match

***************************************/

int str_equiv(const wchar_t* pInput1, const wchar_t* pInput2)
{
	uintptr_t len = wcslen(pInput1);
	if (wcslen(pInput2) != len) {
		return 0;
	}
	if (_wcsnicmp(pInput1, pInput2, len)) {
		return 0;
	}
	return 1;
}

/***************************************

	Convert a string to a number.

	Return 0 if okay, 1 if pString is NULL, 2 if wcstoul() failed

***************************************/

int str_number(const wchar_t* pString, uint32_t* pNumber, wchar_t** ppEnd)
{
	if (!pString) {
		return 1;
	}

	*pNumber = wcstoul(pString, ppEnd, 0);
	if (**ppEnd) {
		return 2;
	}
	return 0;
}

/***************************************

	User requested us to print our version.

	Check the input string for /v, -v, -version and --version

	Return true if the string is a command line switch for version

***************************************/

static bool is_version(const wchar_t* pInput)
{
	if (!pInput || !*pInput) {
		return false;
	}
	/* /version */
	if (*pInput == '/') {
		pInput++;
	} else if (*pInput == '-') {
		/* -v, -V, -version, --version */
		pInput++;
		if (*pInput == '-') {
			pInput++;
		} else if (str_equiv(pInput, L"v")) {
			return true;
		}
	}
	if (str_equiv(pInput, L"version")) {
		return true;
	}
	return false;
}

/***************************************

	String to number, simple version

***************************************/

int str_number(const wchar_t* pString, uint32_t* pNumber)
{
	wchar_t* pEnd;
	return str_number(pString, pNumber, &pEnd);
}

/***************************************

	Does a char need to be escaped?

***************************************/

static bool needs_escape(wchar_t c)
{
	if (c == L'"') {
		return true;
	}
	if (c == L'&') {
		return true;
	}
	if (c == L'%') {
		return true;
	}
	if (c == L'^') {
		return true;
	}
	if (c == L'<') {
		return true;
	}
	if (c == L'>') {
		return true;
	}
	if (c == L'|') {
		return true;
	}
	return false;
}

/***************************************

	Does a char need to be quoted?

***************************************/

static bool needs_quote(wchar_t c)
{
	if (c == L' ') {
		return true;
	}
	if (c == L'\t') {
		return true;
	}
	if (c == L'\n') {
		return true;
	}
	if (c == L'\v') {
		return true;
	}
	if (c == L'"') {
		return true;
	}
	if (c == L'*') {
		return true;
	}
	return needs_escape(c);
}

/***************************************

	https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/

	http://www.robvanderwoude.com/escapechars.php

***************************************/

int quote(const wchar_t* pUnquoted, wchar_t* pBuffer, uintptr_t uBufferLength)
{
	uintptr_t i, j, n;
	uintptr_t len = wcslen(pUnquoted);
	if (len > uBufferLength - 1) {
		return 1;
	}

	bool bEscape = false;
	bool bQuotes = false;

	for (i = 0; i < len; i++) {
		if (needs_escape(pUnquoted[i])) {
			bEscape = bQuotes = true;
			break;
		}
		if (needs_quote(pUnquoted[i])) {
			bQuotes = true;
		}
	}
	if (!bQuotes) {
		memmove(pBuffer, pUnquoted, (len + 1) * sizeof(wchar_t));
		return 0;
	}

	/* "" */
	uintptr_t quoted_len = 2;
	if (bEscape) {
		quoted_len += 2;
	}
	for (i = 0;; i++) {
		n = 0;

		while (i != len && pUnquoted[i] == L'\\') {
			i++;
			n++;
		}

		if (i == len) {
			quoted_len += n * 2;
			break;
		} else if (pUnquoted[i] == L'"') {
			quoted_len += n * 2 + 2;
		} else {
			quoted_len += n + 1;
		}
		if (needs_escape(pUnquoted[i])) {
			quoted_len += n;
		}
	}
	if (quoted_len > uBufferLength - 1) {
		return 1;
	}

	wchar_t* s = pBuffer;
	if (bEscape) {
		*s++ = L'^';
	}
	*s++ = L'"';

	for (i = 0;; i++) {
		n = 0;

		while (i != len && pUnquoted[i] == L'\\') {
			i++;
			n++;
		}

		if (i == len) {
			for (j = 0; j < n * 2; j++) {
				if (bEscape) {
					*s++ = L'^';
				}
				*s++ = L'\\';
			}
			break;
		} else if (pUnquoted[i] == L'"') {
			for (j = 0; j < n * 2 + 1; j++) {
				if (bEscape) {
					*s++ = L'^';
				}
				*s++ = L'\\';
			}
			if (bEscape && needs_escape(pUnquoted[i])) {
				*s++ = L'^';
			}
			*s++ = pUnquoted[i];
		} else {
			for (j = 0; j < n; j++) {
				if (bEscape) {
					*s++ = L'^';
				}
				*s++ = L'\\';
			}
			if (bEscape && needs_escape(pUnquoted[i])) {
				*s++ = L'^';
			}
			*s++ = pUnquoted[i];
		}
	}
	if (bEscape) {
		*s++ = L'^';
	}
	*s++ = L'"';
	*s++ = 0;

	return 0;
}

/***************************************

	Remove basename of a path, leaving only the parent directory

***************************************/

void strip_basename(wchar_t* pBuffer)
{
	uintptr_t len = wcslen(pBuffer);
	uintptr_t i;
	for (i = len; i && (pBuffer[i] != L'\\') && (pBuffer[i] != L'/'); i--) {
	}

	/* X:\ is OK. */
	if (i && (pBuffer[i - 1] == L':')) {
		i++;
	}
	pBuffer[i] = 0;
}

/***************************************

	How to use me correctly

***************************************/

int usage(int iResult)
{
	if ((!GetConsoleWindow() || !GetStdHandle(STD_OUTPUT_HANDLE)) &&
		GetProcessWindowStation()) {
		popup_message(0, MB_OK, NSSM_MESSAGE_USAGE, g_NSSMVersion,
			g_NSSMConfiguration, g_NSSMDate);
	} else {
		print_message(stderr, NSSM_MESSAGE_USAGE, g_NSSMVersion,
			g_NSSMConfiguration, g_NSSMDate);
	}
	return iResult;
}

static void check_admin(void)
{
	g_bIsAdmin = false;

	/* Lifted from MSDN examples */
	PSID AdministratorsGroup;
	SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
	if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
			DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &AdministratorsGroup)) {
		return;
	}
	CheckTokenMembership(0, AdministratorsGroup, /*XXX*/ &g_bIsAdmin);
	FreeSid(AdministratorsGroup);
}

static int elevate(int /* iArgc */, wchar_t** ppArgv, uint32_t uMessage)
{
	print_message(stderr, uMessage);

	SHELLEXECUTEINFOW sei;
	ZeroMemory(&sei, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.lpVerb = L"runas";
	sei.lpFile = nssm_imagepath();

	wchar_t* args =
		static_cast<wchar_t*>(heap_calloc(EXE_LENGTH * sizeof(wchar_t)));
	if (!args) {
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"GetCommandLine()",
			L"elevate()");
		return 111;
	}

	/* Get command line, which includes the path to NSSM, and skip that part. */
	StringCchPrintfW(args, EXE_LENGTH, L"%s", GetCommandLine());
	uintptr_t s = wcslen(ppArgv[0]) + 1;
	if (args[0] == L'"') {
		s += 2;
	}
	while (isspace(args[s])) {
		s++;
	}

	sei.lpParameters = args + s;
	sei.nShow = SW_SHOW;

	int iExitcode = 0;
	if (!ShellExecuteExW(&sei)) {
		iExitcode = 100;
	}

	heap_free(args);
	return iExitcode;
}

int num_cpus(void)
{
	DWORD_PTR i, affinity, system_affinity;
	if (!GetProcessAffinityMask(
			GetCurrentProcess(), &affinity, &system_affinity)) {
		return 64;
	}
	for (i = 0; system_affinity & (1LL << i); i++) {
		if (i == 64) {
			break;
		}
	}
	return static_cast<int>(i);
}

const wchar_t* nssm_unquoted_imagepath(void)
{
	return unquoted_imagepath;
}

const wchar_t* nssm_imagepath(void)
{
	return imagepath;
}

const wchar_t* nssm_exe(void)
{
	return imageargv0;
}

/***************************************

	Main entry point

***************************************/

int _tmain(int iArgc, wchar_t** argv)
{
	// Is there a console available?
	if (check_console()) {
		// Force using UTF8 text output
		setup_utf8();
	}

	/* Remember if we are admin */
	check_admin();

	/* Set up function pointers. */
	if (get_imports())
		nssm_exit(111);

	/* Remember our path for later. */
	StringCchPrintfW(imageargv0, RTL_NUMBER_OF(imageargv0), L"%s", argv[0]);
	PathQuoteSpacesW(imageargv0);
	GetModuleFileNameW(
		0, unquoted_imagepath, RTL_NUMBER_OF(unquoted_imagepath));
	GetModuleFileNameW(0, imagepath, RTL_NUMBER_OF(imagepath));
	PathQuoteSpacesW(imagepath);

	/* Elevate */
	if (iArgc > 1) {
		/*
		  Valid commands are:
		  start, stop, pause, continue, install, edit, get, set, reset, unset,
		  remove status, statuscode, rotate, list, processes, version
		*/
		if (is_version(argv[1])) {
			wprintf(L"%s %s %s %s\n", g_NSSM, g_NSSMVersion,
				g_NSSMConfiguration, g_NSSMDate);
			nssm_exit(0);
		}
		if (str_equiv(argv[1], L"start")) {
			nssm_exit(control_service(
				NSSM_SERVICE_CONTROL_START, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"stop")) {
			nssm_exit(
				control_service(SERVICE_CONTROL_STOP, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"restart")) {
			int ret =
				control_service(SERVICE_CONTROL_STOP, iArgc - 2, argv + 2);
			if (ret) {
				nssm_exit(ret);
			}
			nssm_exit(control_service(
				NSSM_SERVICE_CONTROL_START, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"pause")) {
			nssm_exit(
				control_service(SERVICE_CONTROL_PAUSE, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"continue")) {
			nssm_exit(
				control_service(SERVICE_CONTROL_CONTINUE, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"status")) {
			nssm_exit(control_service(
				SERVICE_CONTROL_INTERROGATE, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"statuscode")) {
			nssm_exit(control_service(
				SERVICE_CONTROL_INTERROGATE, iArgc - 2, argv + 2, true));
		}
		if (str_equiv(argv[1], L"rotate")) {
			nssm_exit(control_service(
				NSSM_SERVICE_CONTROL_ROTATE, iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"install")) {
			if (!g_bIsAdmin) {
				nssm_exit(elevate(iArgc, argv,
					NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL));
			}
			create_messages();
			nssm_exit(pre_install_service(iArgc - 2, argv + 2));
		}
		if (str_equiv(argv[1], L"edit") || str_equiv(argv[1], L"get") ||
			str_equiv(argv[1], L"set") || str_equiv(argv[1], L"reset") ||
			str_equiv(argv[1], L"unset") || str_equiv(argv[1], L"dump")) {
			int ret = pre_edit_service(iArgc - 1, argv + 1);
			if (ret == 3 && !g_bIsAdmin && iArgc == 3) {
				nssm_exit(elevate(
					iArgc, argv, NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_EDIT));
			}
			/* There might be a password here. */
			for (int i = 0; i < iArgc; i++) {
				RtlSecureZeroMemory(argv[i], wcslen(argv[i]) * sizeof(wchar_t));
			}
			nssm_exit(ret);
		}
		if (str_equiv(argv[1], L"list"))
			nssm_exit(list_nssm_services(iArgc - 2, argv + 2));
		if (str_equiv(argv[1], L"processes"))
			nssm_exit(service_process_tree(iArgc - 2, argv + 2));
		if (str_equiv(argv[1], L"remove")) {
			if (!g_bIsAdmin) {
				nssm_exit(elevate(
					iArgc, argv, NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_REMOVE));
			}
			nssm_exit(pre_remove_service(iArgc - 2, argv + 2));
		}
	}

	/* Thread local storage for error message buffer */
	setup_event();

	/* Register messages */
	if (g_bIsAdmin) {
		create_messages();
	}

	/*
	  Optimisation for Windows 2000:
	  When we're run from the command line the StartServiceCtrlDispatcher() call
	  will time out after a few seconds on Windows 2000.  On newer versions the
	  call returns instantly.  Check for stdin first and only try to call the
	  function if there's no input stream found.  Although it's possible that
	  we're running with input redirected it's much more likely that we're
	  actually running as a service.
	  This will save time when running with no arguments from a command prompt.
	*/
	if (!GetStdHandle(STD_INPUT_HANDLE)) {
		/* Start service magic */
		SERVICE_TABLE_ENTRYW table[] = {
			{const_cast<wchar_t*>(g_NSSM), service_main}, {0, 0}};
		if (!StartServiceCtrlDispatcherW(table)) {
			DWORD error = GetLastError();
			/* User probably ran nssm with no argument */
			if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
				nssm_exit(usage(1));
			}
			log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_DISPATCHER_FAILED,
				error_string(error), NULL);
			nssm_exit(100);
		}
	} else {
		nssm_exit(usage(1));
	}
	/* And nothing more to do */
	nssm_exit(0);
	return 0;
}
