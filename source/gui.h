/***************************************

	NSSM GUI Manager

***************************************/

#ifndef __GUI_H__
#define __GUI_H__

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

struct nssm_service_t;

extern int nssm_gui(int iResource, nssm_service_t* pNSSMService);
extern void center_window(HWND hWindow);
extern int configure(HWND hWindow, nssm_service_t* pNSSMService,
	const nssm_service_t* pNSSMOriginal);
extern int install(HWND hWindow);
extern int remove(HWND hWindow);
extern int edit(HWND hWindow, const nssm_service_t* pNSSMOriginal);
extern void browse(
	HWND hWindow, const wchar_t* pWorkingDirectory, uint32_t uFlags, ...);
extern INT_PTR CALLBACK nssm_dlg(
	HWND hWindow, UINT uMessage, WPARAM w, LPARAM l);

#endif
