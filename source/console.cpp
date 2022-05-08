/***************************************

	Function to test if the application was launched from a console

***************************************/

#include "console.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x500
#endif

#include <Windows.h>

/***************************************

	See if we were launched from a console window.
	If not, release any attached console so it can run as a service

***************************************/

bool check_console(void)
{
	// If we're running in a service context there will be no console window.
	HWND hConsoleWindow = GetConsoleWindow();
	if (hConsoleWindow) {

		// Get the process ID
		DWORD pid;
		if (GetWindowThreadProcessId(hConsoleWindow, &pid)) {

			// If the process associated with the console window handle is the
			// same as this process, we were not launched from an existing
			// console.  The user probably double-clicked our executable.
			if (GetCurrentProcessId() != pid) {
				return true;
			}

			// We close our new console so that subsequent messages appear in a
			// popup.
			FreeConsole();
		}
	}
	// No console is attached
	return false;
}
