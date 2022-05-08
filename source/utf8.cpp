/***************************************

	UTF 8 translation functions

***************************************/

#include "utf8.h"
#include "memorymanager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

// Previous code page used by the console
static UINT g_OldCodePage;

/***************************************

	Ensure we write in UTF-8 mode, so that non-ASCII characters don't get
	mangled.  If we were compiled in ANSI mode it won't work.

***************************************/

void setup_utf8(void)
{
	g_OldCodePage = GetConsoleOutputCP();

	// Set to use the Unicode text output
	SetConsoleOutputCP(CP_UTF8);

	// Not all libraries support _O_U8TEXT
#if defined(_O_U8TEXT)
	_setmode(_fileno(stdout), _O_U8TEXT);
	_setmode(_fileno(stderr), _O_U8TEXT);
#endif
}

/***************************************

	Restore the previous code page

***************************************/

void unsetup_utf8(void)
{
	if (g_OldCodePage) {
		SetConsoleOutputCP(g_OldCodePage);
		g_OldCodePage = 0;
	}
}

/***************************************

	Conversion functions.

	to_utf8/16() converts a string which may be either utf8 or utf16 to
	the desired format.  If no conversion is needed a new string is still
	allocated and the old string is copied.

	from_utf8/16() converts a string which IS in the specified format to
	whichever format is needed according to whether UNICODE is defined.
	It simply wraps the appropriate to_utf8/16() function.

	Therefore the caller must ALWAYS free the destination pointer after a
	successful (return code 0) call to one of these functions.

	The length pointer is optional.  Pass NULL if you don't care about
	the length of the converted string.

	Both the destination and the length, if supplied, will be zeroed if
	no conversion was done.

***************************************/

/***************************************

	Convert a "C" wchar_t string to utf8

***************************************/

int to_utf8(const wchar_t* pInput, char** ppOutput, uint32_t* pOutputLength)
{
	// Initialize the output
	*ppOutput = 0;
	if (pOutputLength) {
		*pOutputLength = 0;
	}

	int iDataSize =
		WideCharToMultiByte(CP_UTF8, 0, pInput, -1, NULL, 0, NULL, NULL);
	int iResult;
	if (!iDataSize) {
		iResult = 1;
	} else {

		// Get the buffer
		char* pOutput =
			static_cast<char*>(heap_alloc(static_cast<uintptr_t>(iDataSize)));
		if (!pOutput) {
			iResult = 2;
		} else {

			// Convert the text to UTF8
			if (!WideCharToMultiByte(
					CP_UTF8, 0, pInput, -1, pOutput, iDataSize, NULL, NULL)) {
				heap_free(pOutput);
				iResult = 3;
			} else {
				// Save the output, no errors
				*ppOutput = pOutput;
				if (pOutputLength) {
					*pOutputLength = static_cast<uint32_t>(strlen(pOutput));
				}
				iResult = 0;
			}
		}
	}
	return iResult;
}

/***************************************

	Convert a utf8 "C" string to a utf8 string

	Effectively just copy a utf8 string

***************************************/

int to_utf8(const char* pInput, char** ppOutput, uint32_t* pOutputLength)
{
	// Initialize the output
	*ppOutput = 0;
	if (pOutputLength) {
		*pOutputLength = 0;
	}

	size_t uInputLength = strlen(pInput);
	int iResult;
	char* pOutput = static_cast<char*>(heap_alloc(uInputLength + 1));
	if (!pOutput) {

		// Out of memory
		iResult = 2;

	} else {
		// Save the new length and pointer
		if (pOutputLength) {
			*pOutputLength = static_cast<uint32_t>(uInputLength);
		}
		*ppOutput = pOutput;

		// Copy the string
		memmove(pOutput, pInput, uInputLength + 1);
		iResult = 0;
	}
	return iResult;
}

/***************************************

	Convert a utf8 "C" string to a utf16 string

***************************************/

int to_utf16(const char* pInput, wchar_t** ppOutput, uint32_t* pOutputLength)
{
	// Initialize the output
	*ppOutput = 0;
	if (pOutputLength) {
		*pOutputLength = 0;
	}

	// Determine the buffer needed for the UTF16 version of the string
	int iResult;
	int iNewSize = MultiByteToWideChar(CP_UTF8, 0, pInput, -1, NULL, 0);
	if (!iNewSize) {

		// Can't convert?
		iResult = 1;
	} else {

		// Allocate the space for the new string
		wchar_t* pOutput = static_cast<wchar_t*>(
			heap_alloc(static_cast<uintptr_t>(iNewSize * sizeof(wchar_t))));
		if (!pOutput) {
			iResult = 2;
		} else {

			// Do the conversion
			if (!MultiByteToWideChar(
					CP_UTF8, 0, pInput, -1, pOutput, iNewSize)) {

				// Error? Clean up and report
				heap_free(pOutput);
				iResult = 3;
			} else {

				// Return the buffer
				*ppOutput = pOutput;
				if (pOutputLength) {
					*pOutputLength = static_cast<uint32_t>(wcslen(pOutput));
				}
				iResult = 0;
			}
		}
	}
	return iResult;
}

/***************************************

	Convert a utf16 "C" string to a utf16 string

	Effectively just copy a utf16 string

***************************************/

int to_utf16(const wchar_t* pInput, wchar_t** ppOutput, uint32_t* pOutputLength)
{
	// Initialize the output
	*ppOutput = 0;
	if (pOutputLength) {
		*pOutputLength = 0;
	}

	size_t uInputLength = wcslen(pInput);
	int iResult;
	wchar_t* pOutput =
		static_cast<wchar_t*>(heap_alloc((uInputLength + 1) * sizeof(wchar_t)));
	if (!pOutput) {

		// Out of memory
		iResult = 2;

	} else {
		// Save the new length and pointer
		if (pOutputLength) {
			*pOutputLength = static_cast<uint32_t>(uInputLength);
		}
		*ppOutput = pOutput;

		// Copy the string
		memmove(pOutput, pInput, (uInputLength + 1) * sizeof(wchar_t));
		iResult = 0;
	}
	return iResult;
}

/***************************************

	Convert a utf8 "C" string to a utf16 string

***************************************/

int from_utf8(const char* pInput, wchar_t** ppOutput, uint32_t* pOutputLength)
{
	return to_utf16(pInput, ppOutput, pOutputLength);
}

/***************************************

	Convert a utf16 "C" string to a utf16 string

	Effectively just copy a utf16 string

***************************************/

int from_utf16(
	const wchar_t* pInput, wchar_t** ppOutput, uint32_t* pOutputLength)
{
	return to_utf16(pInput, ppOutput, pOutputLength);
}
