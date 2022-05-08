/***************************************

	Event logging

***************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "event.h"
#include "constants.h"
#include "memorymanager.h"
#include "resource.h"

#include <Windows.h>
#include <wchar.h>

#include <stdlib.h>
#include <strsafe.h>

// Maximum number of event string to post
#define NSSM_MAX_EVENT_STRINGS 16

// Buffer size to create an error message in
#define NSSM_ERROR_BUFSIZE 65535

static const wchar_t g_NSSM_SOURCE[] = L"nssm";

// Thread Local Storage used for event manager
static uint32_t g_TLSIndex = TLS_OUT_OF_INDEXES;

/***************************************

	Use the thread local storage to store a buffer to use across threads

***************************************/

void setup_event(void)
{
	// Is there an index set?
	if (g_TLSIndex == TLS_OUT_OF_INDEXES) {
		// Allocate one
		g_TLSIndex = TlsAlloc();
	}
}

/***************************************

	Release the memory in the thread local storage

***************************************/

void unsetup_event(void)
{
	// Is it in use?
	if (g_TLSIndex != TLS_OUT_OF_INDEXES) {

		// Get the value stored
		void* pBuffer = TlsGetValue(g_TLSIndex);
		if (pBuffer) {
			// Release it
			LocalFree(pBuffer);
			TlsSetValue(g_TLSIndex, NULL);
		}

		// Release the thread index
		TlsFree(g_TLSIndex);
		g_TLSIndex = TLS_OUT_OF_INDEXES;
	}
}

/***************************************

	Convert error code to error string

***************************************/

wchar_t* error_string(uint32_t uErrorCode)
{
	// Get the thread safe buffer
	wchar_t* pErrorMessage = static_cast<wchar_t*>(TlsGetValue(g_TLSIndex));
	if (!pErrorMessage) {

		// Allocate the buffer
		pErrorMessage = static_cast<wchar_t*>(
			LocalAlloc(LPTR, NSSM_ERROR_BUFSIZE * sizeof(wchar_t)));

		// If memory can't be allocated, just use this message
		if (!pErrorMessage) {
			return L"<out of memory for error message>";
		}
		// Save the buffer, forever
		TlsSetValue(g_TLSIndex, pErrorMessage);
	}

	// Convert the error code into a message in the native language
	if (!FormatMessageW(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
			uErrorCode, GetUserDefaultLangID(), pErrorMessage,
			NSSM_ERROR_BUFSIZE, NULL)) {

		// Use English
		if (!FormatMessageW(
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
				uErrorCode, 0, pErrorMessage, NSSM_ERROR_BUFSIZE, NULL)) {

			// Okay... Unknown error
			if (FAILED(StringCchPrintfW(pErrorMessage, NSSM_ERROR_BUFSIZE,
					L"system error 0x%08X",
					static_cast<unsigned int>(uErrorCode)))) {
				return L"StringCchPrintfW failed in error_string()";
			}
		}
	}
	return pErrorMessage;
}

/***************************************

	Get the localized string from messages.mc

	Note: the pointer returned must be freed by LocalFree

***************************************/

wchar_t* message_string(uint32_t uMessageCode)
{
	wchar_t* pMessage = NULL;

	// Try localized
	if (!FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			0, uMessageCode, GetUserDefaultLangID(),
			reinterpret_cast<LPWSTR>(&pMessage), NSSM_ERROR_BUFSIZE, NULL)) {

		// Try English
		if (!FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
					FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
				0, uMessageCode, 0, reinterpret_cast<LPWSTR>(&pMessage),
				NSSM_ERROR_BUFSIZE, NULL)) {

			// Generic message
			pMessage =
				static_cast<wchar_t*>(LocalAlloc(LPTR, 32 * sizeof(wchar_t)));
			if (pMessage) {
				if (FAILED(StringCchPrintfW(pMessage, 32, L"message 0x%08X",
						static_cast<unsigned int>(uMessageCode)))) {
					pMessage[0] = 0;
				}
			}
		}
	}
	return pMessage;
}

/***************************************

	Log a message to the Event Log

	The 3rd and higher parameters must all be wchar_t string pointers with a
	NULL being the final parameter

***************************************/

void log_event(uint16_t uMessageType, uint32_t uMessageID, ...)
{
	// Create the event source
	HANDLE hEvent = RegisterEventSourceW(NULL, g_NSSM_SOURCE);
	if (hEvent) {

		const wchar_t* TempStringArray[NSSM_MAX_EVENT_STRINGS];

		// Capture the strings into an array
		// All entries are assumed to be wchar_t *

		va_list arg;
		va_start(arg, uMessageID);
		wchar_t* s;
		unsigned int uCount = 0;
		// Iterate until the NULL pointer is found
		while ((s = va_arg(arg, wchar_t*)) &&
			uCount < (NSSM_MAX_EVENT_STRINGS - 1)) {
			TempStringArray[uCount++] = s;
		}
		TempStringArray[uCount] = NULL;
		va_end(arg);

		// Report to the event log
		ReportEventW(hEvent, uMessageType, 0, uMessageID, NULL,
			static_cast<WORD>(uCount), 0,
			reinterpret_cast<LPCWSTR*>(TempStringArray), NULL);

		// Release the source
		DeregisterEventSource(hEvent);
	}
}

/***************************************

	Log a message to the console, stdout or stderr

***************************************/

void print_message(FILE* fp, uint32_t uMessageID, ...)
{
	wchar_t* pMessage = message_string(uMessageID);
	if (pMessage) {
		va_list arg;
		va_start(arg, uMessageID);
		vfwprintf(fp, pMessage, arg);
		va_end(arg);
		LocalFree(pMessage);
	}
}

/***************************************

	Show a GUI dialog

***************************************/

int popup_message(HWND hWindow, uint32_t uMessageType, uint32_t uMessageID, ...)
{
	wchar_t* pMessage = message_string(uMessageID);
	if (!pMessage) {
		return MessageBoxW(NULL,
			L"The message which was supposed to go here is missing!", g_NSSM,
			MB_OK | MB_ICONEXCLAMATION);
	}

	wchar_t TempBuffer[2048];
	va_list arg;
	va_start(arg, uMessageID);
	int iResult;
	if (FAILED(StringCchPrintfW(TempBuffer, 2048, pMessage, arg))) {

		iResult = MessageBoxW(NULL,
			L"The message which was supposed to go here is too big!", g_NSSM,
			MB_OK | MB_ICONEXCLAMATION);
	} else {

		MSGBOXPARAMSW BoxParams;
		ZeroMemory(&BoxParams, sizeof(BoxParams));
		BoxParams.cbSize = sizeof(BoxParams);
		BoxParams.hInstance = GetModuleHandleW(0);
		BoxParams.hwndOwner = hWindow;
		BoxParams.lpszText = TempBuffer;
		BoxParams.lpszCaption = g_NSSM;
		BoxParams.dwStyle = uMessageType;
		if (uMessageType == MB_OK) {
			BoxParams.dwStyle |= MB_USERICON;
			BoxParams.lpszIcon = MAKEINTRESOURCEW(IDI_NSSM);
		}
		iResult = MessageBoxIndirectW(&BoxParams);
	}
	va_end(arg);
	LocalFree(pMessage);
	return iResult;
}
