/***************************************

	NSSM GUI Manager

***************************************/

#include "gui.h"
#include "account.h"
#include "constants.h"
#include "env.h"
#include "event.h"
#include "hook.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "nssm_io.h"
#include "registry.h"
#include "resource.h"
#include "service.h"

#include <CommCtrl.h>
#include <CommDlg.h>
#include <wchar.h>

#include <strsafe.h>

static enum {
	NSSM_TAB_APPLICATION,
	NSSM_TAB_DETAILS,
	NSSM_TAB_LOGON,
	NSSM_TAB_DEPENDENCIES,
	NSSM_TAB_PROCESS,
	NSSM_TAB_SHUTDOWN,
	NSSM_TAB_EXIT,
	NSSM_TAB_IO,
	NSSM_TAB_ROTATION,
	NSSM_TAB_ENVIRONMENT,
	NSSM_TAB_HOOKS,
	NSSM_NUM_TABS
} nssm_tabs;

// List of controls in the dialog
static HWND g_Tablist[NSSM_NUM_TABS];

// Which tab is selected
static int g_iSelectedTab;

/***************************************

	Load a dialog resource

***************************************/

static HWND dialog(
	const wchar_t* pTemplate, HWND hParent, DLGPROC pFunction, LPARAM lParam)
{
	// The caller will deal with GetLastError()...
	HRSRC hResource = FindResourceExW(0, reinterpret_cast<LPWSTR>(RT_DIALOG),
		pTemplate, GetUserDefaultLangID());
	if (!hResource) {
		if (GetLastError() != ERROR_RESOURCE_LANG_NOT_FOUND) {
			return 0;
		}
		hResource = FindResourceExW(0, reinterpret_cast<LPWSTR>(RT_DIALOG),
			pTemplate, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
		if (!hResource) {
			return 0;
		}
	}

	// Load in the dialog
	HGLOBAL hGlobal = LoadResource(0, hResource);
	if (!hGlobal) {
		return 0;
	}

	// Create the dialog
	return CreateDialogIndirectParamW(
		0, reinterpret_cast<DLGTEMPLATE*>(hGlobal), hParent, pFunction, lParam);
}

/***************************************

	Load a dialog resource

	lParam is assumed 0

***************************************/

static HWND dialog(const wchar_t* pTemplate, HWND hParent, DLGPROC pFunction)
{
	return dialog(pTemplate, hParent, pFunction, 0);
}

/***************************************

	Enable controls for logon

***************************************/

static inline void set_logon_enabled(
	uint8_t bInteractEnabled, uint8_t bCredentialsEnabled)
{
	EnableWindow(
		GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_INTERACT), bInteractEnabled);
	EnableWindow(GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_USERNAME),
		bCredentialsEnabled);
	EnableWindow(GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD1),
		bCredentialsEnabled);
	EnableWindow(GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD2),
		bCredentialsEnabled);
}

/***************************************

	Main dialog handler for the GUI

***************************************/

int nssm_gui(int iResource, nssm_service_t* pNSSMService)
{
	/* Create window */
	HWND hDialog = dialog(MAKEINTRESOURCEW(iResource), 0, nssm_dlg,
		reinterpret_cast<LPARAM>(pNSSMService));
	if (!hDialog) {
		popup_message(0, MB_OK, NSSM_GUI_CREATEDIALOG_FAILED,
			error_string(GetLastError()));
		return 1;
	}

	// Load the icon.
	HANDLE hIcon =
		LoadImage(GetModuleHandleW(0), MAKEINTRESOURCE(IDI_NSSM), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (hIcon) {
		SendMessageW(
			hDialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
	}
	hIcon =
		LoadImage(GetModuleHandleW(0), MAKEINTRESOURCE(IDI_NSSM), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
	if (hIcon) {
		SendMessageW(
			hDialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
	}

	// Remember what the window is for.
	SetWindowLongPtrW(hDialog, GWLP_USERDATA, static_cast<LONG_PTR>(iResource));

	// Display the window
	center_window(hDialog);
	ShowWindow(hDialog, SW_SHOW);

	// Set service name if given
	if (pNSSMService->m_Name[0]) {
		SetDlgItemTextW(hDialog, IDC_NAME, pNSSMService->m_Name);
		// No point making user click remove if the name is already entered
		if (iResource == IDD_REMOVE) {
			HWND hButton = GetDlgItem(hDialog, IDC_REMOVE);
			if (hButton) {
				SendMessageW(hButton, WM_LBUTTONDOWN, 0, 0);
				SendMessageW(hButton, WM_LBUTTONUP, 0, 0);
			}
		}
	}

	if (iResource == IDD_EDIT) {
		// We'll need the service handle later.
		SetWindowLongPtrW(
			hDialog, DWLP_USER, reinterpret_cast<LONG_PTR>(pNSSMService));

		// Service name can't be edited.
		EnableWindow(GetDlgItem(hDialog, IDC_NAME), 0);
		SetFocus(GetDlgItem(hDialog, IDOK));

		// Set existing details.

		// Application tab.
		if (pNSSMService->m_bNative) {
			SetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_PATH,
				pNSSMService->m_NSSMExecutablePathname);
		} else {
			SetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_PATH,
				pNSSMService->m_ExecutablePath);
		}
		SetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_DIR,
			pNSSMService->m_WorkingDirectory);
		SetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_FLAGS,
			pNSSMService->m_AppParameters);

		// Details tab.
		SetDlgItemTextW(g_Tablist[NSSM_TAB_DETAILS], IDC_DISPLAYNAME,
			pNSSMService->m_DisplayName);
		SetDlgItemTextW(g_Tablist[NSSM_TAB_DETAILS], IDC_DESCRIPTION,
			pNSSMService->m_Description);
		HWND hCombo = GetDlgItem(g_Tablist[NSSM_TAB_DETAILS], IDC_STARTUP);
		SendMessageW(hCombo, CB_SETCURSEL, pNSSMService->m_uStartup, 0);

		// Log on tab.
		if (pNSSMService->m_pUsername) {
			if (is_virtual_account(
					pNSSMService->m_Name, pNSSMService->m_pUsername)) {
				CheckRadioButton(g_Tablist[NSSM_TAB_LOGON], IDC_LOCALSYSTEM,
					IDC_VIRTUAL_SERVICE, IDC_VIRTUAL_SERVICE);
				set_logon_enabled(0, 0);
			} else {
				CheckRadioButton(g_Tablist[NSSM_TAB_LOGON], IDC_LOCALSYSTEM,
					IDC_VIRTUAL_SERVICE, IDC_ACCOUNT);
				SetDlgItemTextW(g_Tablist[NSSM_TAB_LOGON], IDC_USERNAME,
					pNSSMService->m_pUsername);
				set_logon_enabled(0, 1);
			}
		} else {
			CheckRadioButton(g_Tablist[NSSM_TAB_LOGON], IDC_LOCALSYSTEM,
				IDC_VIRTUAL_SERVICE, IDC_LOCALSYSTEM);
			if (pNSSMService->m_uServiceTypes & SERVICE_INTERACTIVE_PROCESS) {
				SendDlgItemMessageW(g_Tablist[NSSM_TAB_LOGON], IDC_INTERACT,
					BM_SETCHECK, BST_CHECKED, 0);
			}
		}

		// Dependencies tab.
		if (pNSSMService->m_uDependenciesLength) {
			wchar_t* pFormatted;
			uintptr_t uFormattedLength;
			if (format_double_null(pNSSMService->m_pDependencies,
					pNSSMService->m_uDependenciesLength, &pFormatted,
					&uFormattedLength)) {
				popup_message(hDialog, MB_OK | MB_ICONEXCLAMATION,
					NSSM_EVENT_OUT_OF_MEMORY, L"dependencies", L"nssm_dlg()");
			} else {
				SetDlgItemTextW(g_Tablist[NSSM_TAB_DEPENDENCIES],
					IDC_DEPENDENCIES, pFormatted);
				heap_free(pFormatted);
			}
		}

		// Process tab.
		if (pNSSMService->m_uPriority) {
			uint32_t uPriorityIndex =
				priority_constant_to_index(pNSSMService->m_uPriority);
			hCombo = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_PRIORITY);
			SendMessageW(hCombo, CB_SETCURSEL, uPriorityIndex, 0);
		}

		if (pNSSMService->m_uAffinity) {
			HWND hList = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY);
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY_ALL,
				BM_SETCHECK, BST_UNCHECKED, 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY), 1);

			DWORD_PTR uAffinity;
			DWORD_PTR uSystemAffinity;
			if (GetProcessAffinityMask(
					GetCurrentProcess(), &uAffinity, &uSystemAffinity)) {
				if ((pNSSMService->m_uAffinity & uSystemAffinity) !=
					pNSSMService->m_uAffinity) {
					popup_message(hDialog, MB_OK | MB_ICONWARNING,
						NSSM_GUI_WARN_AFFINITY);
				}
			}

			for (int i = 0; i < num_cpus(); i++) {
				if (!(pNSSMService->m_uAffinity &
						(1ULL << static_cast<uint64_t>(i)))) {
					SendMessageW(hList, LB_SETSEL, 0, i);
				}
			}
		}

		if (pNSSMService->m_bDontSpawnConsole) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_PROCESS], IDC_CONSOLE,
				BM_SETCHECK, BST_UNCHECKED, 0);
		}

		// Shutdown tab.
		if (!(pNSSMService->m_uStopMethodFlags & NSSM_STOP_METHOD_CONSOLE)) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
				IDC_METHOD_CONSOLE, BM_SETCHECK, BST_UNCHECKED, 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_CONSOLE), 0);
		}
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_CONSOLE,
			pNSSMService->m_uKillConsoleDelay, 0);
		if (!(pNSSMService->m_uStopMethodFlags & NSSM_STOP_METHOD_WINDOW)) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_METHOD_WINDOW,
				BM_SETCHECK, BST_UNCHECKED, 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_WINDOW), 0);
		}
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_WINDOW,
			pNSSMService->m_uKillWindowDelay, 0);
		if (!(pNSSMService->m_uStopMethodFlags & NSSM_STOP_METHOD_THREADS)) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
				IDC_METHOD_THREADS, BM_SETCHECK, BST_UNCHECKED, 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_THREADS), 0);
		}
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_THREADS,
			pNSSMService->m_uKillThreadsDelay, 0);
		if (!(pNSSMService->m_uStopMethodFlags & NSSM_STOP_METHOD_TERMINATE)) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
				IDC_METHOD_TERMINATE, BM_SETCHECK, BST_UNCHECKED, 0);
		}
		if (!pNSSMService->m_bKillProcessTree) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
				IDC_KILL_PROCESS_TREE, BM_SETCHECK, BST_UNCHECKED, 0);
		}

		// Restart tab.
		SetDlgItemInt(g_Tablist[NSSM_TAB_EXIT], IDC_THROTTLE,
			pNSSMService->m_uThrottleDelay, 0);
		hCombo = GetDlgItem(g_Tablist[NSSM_TAB_EXIT], IDC_APPEXIT);
		SendMessageW(
			hCombo, CB_SETCURSEL, pNSSMService->m_uDefaultExitAction, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_EXIT], IDC_RESTART_DELAY,
			pNSSMService->m_uRestartDelay, 0);

		// I/O tab.
		SetDlgItemTextW(
			g_Tablist[NSSM_TAB_IO], IDC_STDIN, pNSSMService->m_StdinPathname);
		SetDlgItemTextW(
			g_Tablist[NSSM_TAB_IO], IDC_STDOUT, pNSSMService->m_StdoutPathname);
		SetDlgItemTextW(
			g_Tablist[NSSM_TAB_IO], IDC_STDERR, pNSSMService->m_StderrPathname);
		if (pNSSMService->m_bTimestampLog) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_IO], IDC_TIMESTAMP,
				BM_SETCHECK, BST_CHECKED, 0);
		}

		// Rotation tab.
		if (pNSSMService->m_uStdoutDisposition == CREATE_ALWAYS) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_ROTATION], IDC_TRUNCATE,
				BM_SETCHECK, BST_CHECKED, 0);
		}
		if (pNSSMService->m_bRotateFiles) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE,
				BM_SETCHECK, BST_CHECKED, 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_ONLINE), 1);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_SECONDS),
				1);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_BYTES_LOW),
				1);
		}

		if (pNSSMService->m_uRotateStdoutOnline ||
			pNSSMService->m_uRotateStderrOnline) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_ONLINE,
				BM_SETCHECK, BST_CHECKED, 0);
		}

		SetDlgItemInt(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_SECONDS,
			pNSSMService->m_uRotateSeconds, 0);
		if (!pNSSMService->m_uRotateBytesHigh) {
			SetDlgItemInt(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_BYTES_LOW,
				pNSSMService->m_uRotateBytesLow, 0);
		}

		// Hooks tab.
		if (pNSSMService->m_bHookShareOutputHandles) {
			SendDlgItemMessageW(g_Tablist[NSSM_TAB_HOOKS], IDC_REDIRECT_HOOK,
				BM_SETCHECK, BST_CHECKED, 0);
		}

		// Check if advanced settings are in use.
		if ((pNSSMService->m_uStdoutDisposition !=
				pNSSMService->m_uStderrDisposition) ||
			(pNSSMService->m_uStdoutDisposition &&
				(pNSSMService->m_uStdoutDisposition !=
					NSSM_STDOUT_DISPOSITION) &&
				(pNSSMService->m_uStdoutDisposition != CREATE_ALWAYS)) ||
			(pNSSMService->m_uStderrDisposition &&
				(pNSSMService->m_uStderrDisposition !=
					NSSM_STDERR_DISPOSITION) &&
				(pNSSMService->m_uStderrDisposition != CREATE_ALWAYS))) {
			popup_message(hDialog, MB_OK | MB_ICONWARNING, NSSM_GUI_WARN_STDIO);
		}
		if (pNSSMService->m_uRotateBytesHigh) {
			popup_message(
				hDialog, MB_OK | MB_ICONWARNING, NSSM_GUI_WARN_ROTATE_BYTES);
		}

		// Environment tab.
		wchar_t* pEnvironmentVariables;
		uintptr_t uEnvironmentVariablesLength;
		if (pNSSMService->m_uExtraEnvironmentVariablesLength) {
			pEnvironmentVariables = pNSSMService->m_pExtraEnvironmentVariables;
			uEnvironmentVariablesLength =
				pNSSMService->m_uExtraEnvironmentVariablesLength;
		} else {
			pEnvironmentVariables = pNSSMService->m_pEnvironmentVariables;
			uEnvironmentVariablesLength =
				pNSSMService->m_uEnvironmentVariablesLength;
			if (uEnvironmentVariablesLength) {
				SendDlgItemMessageW(g_Tablist[NSSM_TAB_ENVIRONMENT],
					IDC_ENVIRONMENT_REPLACE, BM_SETCHECK, BST_CHECKED, 0);
			}
		}

		if (uEnvironmentVariablesLength) {
			wchar_t* pFormatted;
			uintptr_t uFormattedLength;
			if (format_double_null(pEnvironmentVariables,
					uEnvironmentVariablesLength, &pFormatted,
					&uFormattedLength)) {
				popup_message(hDialog, MB_OK | MB_ICONEXCLAMATION,
					NSSM_EVENT_OUT_OF_MEMORY, L"environment", L"nssm_dlg()");
			} else {
				SetDlgItemTextW(g_Tablist[NSSM_TAB_ENVIRONMENT],
					IDC_ENVIRONMENT, pFormatted);
				heap_free(pFormatted);
			}
		}
		if (pNSSMService->m_uEnvironmentVariablesLength &&
			pNSSMService->m_uExtraEnvironmentVariablesLength)
			popup_message(
				hDialog, MB_OK | MB_ICONWARNING, NSSM_GUI_WARN_ENVIRONMENT);
	}

	// Show the dialog and wait for quit
	MSG TempMessage;
	while (GetMessageW(&TempMessage, 0, 0, 0)) {
		if (!IsDialogMessageW(hDialog, &TempMessage)) {
			TranslateMessage(&TempMessage);
			DispatchMessageW(&TempMessage);
		}
	}

	return static_cast<int>(TempMessage.wParam);
}

/***************************************

	Center the window on the desktop

***************************************/

void center_window(HWND hWindow)
{
	if (hWindow) {

		// Find window size
		RECT TempRect;
		if (GetWindowRect(hWindow, &TempRect)) {

			// Find desktop window
			HWND hDesktop = GetDesktopWindow();
			if (hDesktop) {

				// Find desktop window size
				RECT DesktopRect;
				if (GetWindowRect(hDesktop, &DesktopRect)) {

					// Center window
					int x = (DesktopRect.right - TempRect.right) / 2;
					int y = (DesktopRect.bottom - TempRect.bottom) / 2;
					MoveWindow(hWindow, x, y, TempRect.right - TempRect.left,
						TempRect.bottom - TempRect.top, 0);
				}
			}
		}
	}
}

/***************************************

	Check the stop method flags in m_uStopMethodFlags

***************************************/

static void check_stop_method(
	nssm_service_t* pNSSMService, uint32_t uMethod, uint32_t uControl)
{
	if (!SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
			static_cast<int>(uControl), BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		pNSSMService->m_uStopMethodFlags &= ~uMethod;
	}
}

/***************************************

	Convert a dialog value to an integer value

***************************************/

static void check_number(HWND hTab, uint32_t uControl, uint32_t* pOutput)
{
	BOOL bTranslated;
	UINT uConfigured =
		GetDlgItemInt(hTab, static_cast<int>(uControl), &bTranslated, false);

	// Success?
	if (bTranslated) {
		// Save the value
		*pOutput = uConfigured;
	}
}

/***************************************

	Convert a dialog value to an integer value

	Enable/disable dependent if checked

***************************************/

static void set_timeout_enabled(uint32_t uControl, uint32_t uDependent)
{
	BOOL bEnabled = false;
	if (SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN],
			static_cast<int>(uControl), BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		bEnabled = true;
	}
	EnableWindow(
		GetDlgItem(g_Tablist[NSSM_TAB_SHUTDOWN], static_cast<int>(uDependent)),
		bEnabled);
}

/***************************************

	Enable/disable affinity control

***************************************/

static void set_affinity_enabled(BOOL bEnabled)
{
	EnableWindow(
		GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY), bEnabled);
}

/***************************************

	Enable/disable rotation control

***************************************/

static void set_rotation_enabled(BOOL bEnabled)
{
	EnableWindow(
		GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_ONLINE), bEnabled);
	EnableWindow(
		GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_SECONDS), bEnabled);
	EnableWindow(GetDlgItem(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_BYTES_LOW),
		bEnabled);
}

/***************************************

	Create the NSSM_HOOK_*_* environment variable name

	Returns less than zero on error

***************************************/

static inline int hook_env(const wchar_t* pHookEvent,
	const wchar_t* pHookAction, wchar_t* pOutput, uint32_t uOutputLength)
{
	return StringCchPrintfW(
		pOutput, uOutputLength, L"NSSM_HOOK_%s_%s", pHookEvent, pHookAction);
}

/***************************************

	Set the dialog items for the Hook tab

***************************************/

static void set_hook_tab(
	uint32_t uEventIndex, uint32_t uActionIndex, bool bChanged)
{
	HWND hCombo = GetDlgItem(g_Tablist[NSSM_TAB_HOOKS], IDC_HOOK_EVENT);
	SendMessageW(hCombo, CB_SETCURSEL, uEventIndex, 0);

	hCombo = GetDlgItem(g_Tablist[NSSM_TAB_HOOKS], IDC_HOOK_ACTION);
	SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

	const wchar_t* pHookEvent = g_HookEventStrings[uEventIndex];
	const wchar_t* pHookAction;
	uint32_t i;
	switch (uEventIndex + NSSM_GUI_HOOK_EVENT_START) {
	case NSSM_GUI_HOOK_EVENT_ROTATE:
		i = 0;
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_ROTATE_PRE));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPre;
		}
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_ROTATE_POST));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPost;
		}
		break;

	case NSSM_GUI_HOOK_EVENT_START:
		i = 0;
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_START_PRE));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPre;
		}
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_START_POST));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPost;
		}
		break;

	case NSSM_GUI_HOOK_EVENT_STOP:
		i = 0;
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_STOP_PRE));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPre;
		}
		break;

	case NSSM_GUI_HOOK_EVENT_EXIT:
		i = 0;
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_EXIT_POST));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionPost;
		}
		break;

	case NSSM_GUI_HOOK_EVENT_POWER:
		i = 0;
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_POWER_CHANGE));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionChange;
		}
		SendMessageW(hCombo, CB_INSERTSTRING, i,
			(LPARAM)message_string(NSSM_GUI_HOOK_ACTION_POWER_RESUME));
		if (uActionIndex == i++) {
			pHookAction = g_NSSMHookActionResume;
		}
		break;
	}

	SendMessageW(hCombo, CB_SETCURSEL, uActionIndex, 0);

	// Get the environment variable
	wchar_t HookName[HOOK_NAME_LENGTH];
	hook_env(pHookEvent, pHookAction, HookName, RTL_NUMBER_OF(HookName));

	if (HookName[0]) {
		wchar_t CommandLine[CMD_LENGTH];
		if (bChanged) {
			GetDlgItemTextW(g_Tablist[NSSM_TAB_HOOKS], IDC_HOOK, CommandLine,
				RTL_NUMBER_OF(CommandLine));
			SetEnvironmentVariableW(HookName, CommandLine);
		} else {
			if (!GetEnvironmentVariableW(
					HookName, CommandLine, RTL_NUMBER_OF(CommandLine))) {
				CommandLine[0] = 0;
			}
			SetDlgItemTextW(g_Tablist[NSSM_TAB_HOOKS], IDC_HOOK, CommandLine);
		}
	}
}

/***************************************

	Update a hook control based on an environment variable

***************************************/

static int update_hook(wchar_t* pServiceName, const wchar_t* pHookEvent,
	const wchar_t* pHookAction)
{
	// Get the environment variable
	wchar_t HookName[HOOK_NAME_LENGTH];
	if (hook_env(pHookEvent, pHookAction, HookName, RTL_NUMBER_OF(HookName)) <
		0) {
		return 1;
	}

	wchar_t CommandLine[CMD_LENGTH];
	ZeroMemory(CommandLine, sizeof(CommandLine));
	GetEnvironmentVariableW(HookName, CommandLine, RTL_NUMBER_OF(CommandLine));
	if (set_hook(pServiceName, pHookEvent, pHookAction, CommandLine)) {
		return 2;
	}
	return 0;
}

/***************************************

	Update all hook controls based on environment variables

	Return 0 if no error

***************************************/

static int update_hooks(wchar_t* pServiceName)
{
	int ret =
		update_hook(pServiceName, g_NSSMHookEventStart, g_NSSMHookActionPre);
	ret +=
		update_hook(pServiceName, g_NSSMHookEventStart, g_NSSMHookActionPost);
	ret += update_hook(pServiceName, g_NSSMHookEventStop, g_NSSMHookActionPre);
	ret += update_hook(pServiceName, g_NSSMHookEventExit, g_NSSMHookActionPost);
	ret +=
		update_hook(pServiceName, g_NSSMHookEventPower, g_NSSMHookActionChange);
	ret +=
		update_hook(pServiceName, g_NSSMHookEventPower, g_NSSMHookActionResume);
	ret +=
		update_hook(pServiceName, g_NSSMHookEventRotate, g_NSSMHookActionPre);
	ret +=
		update_hook(pServiceName, g_NSSMHookEventRotate, g_NSSMHookActionPost);
	return ret;
}

/***************************************

	Check an IO tab control

***************************************/

static void check_io(HWND hOwner, wchar_t* pName, wchar_t* pBuffer,
	uint32_t uBufferLength, uint32_t uControl)
{
	if (SendMessageW(
			GetDlgItem(g_Tablist[NSSM_TAB_IO], static_cast<int>(uControl)),
			WM_GETTEXTLENGTH, 0, 0)) {
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_IO], static_cast<int>(uControl),
				pBuffer, static_cast<int>(uBufferLength))) {

			popup_message(hOwner, MB_OK | MB_ICONEXCLAMATION,
				NSSM_MESSAGE_PATH_TOO_LONG, pName);
			ZeroMemory(pBuffer, uBufferLength * sizeof(wchar_t));
		}
	}
}

/***************************************

	Set service parameters.

	If editing, use pNSSMOriginal for the current records

***************************************/

int configure(HWND hWindow, nssm_service_t* pNSSMService,
	const nssm_service_t* pNSSMOriginal)
{
	// Sanity check
	if (!pNSSMService) {
		return 1;
	}

	// Set all the defaults
	set_nssm_service_defaults(pNSSMService);

	if (pNSSMOriginal) {
		pNSSMService->m_bNative = pNSSMOriginal->m_bNative;
		pNSSMService->m_hServiceControlManager =
			pNSSMOriginal->m_hServiceControlManager;
	}

	// Get service name.
	if (!GetDlgItemTextW(hWindow, IDC_NAME, pNSSMService->m_Name,
			RTL_NUMBER_OF(pNSSMService->m_Name))) {
		popup_message(
			hWindow, MB_OK | MB_ICONEXCLAMATION, NSSM_GUI_MISSING_SERVICE_NAME);
		cleanup_nssm_service(pNSSMService);
		return 2;
	}

	// Get executable name
	if (!pNSSMService->m_bNative) {
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_PATH,
				pNSSMService->m_ExecutablePath,
				RTL_NUMBER_OF(pNSSMService->m_ExecutablePath))) {
			popup_message(
				hWindow, MB_OK | MB_ICONEXCLAMATION, NSSM_GUI_MISSING_PATH);
			return 3;
		}

		// Get startup directory.
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_DIR,
				pNSSMService->m_WorkingDirectory,
				RTL_NUMBER_OF(pNSSMService->m_WorkingDirectory))) {
			StringCchPrintfW(pNSSMService->m_WorkingDirectory,
				RTL_NUMBER_OF(pNSSMService->m_WorkingDirectory), L"%s",
				pNSSMService->m_ExecutablePath);
			// Remove the .exe name, and keep the directory
			strip_basename(pNSSMService->m_WorkingDirectory);
		}

		// Get flags.
		if (SendMessageW(GetDlgItem(g_Tablist[NSSM_TAB_APPLICATION], IDC_FLAGS),
				WM_GETTEXTLENGTH, 0, 0)) {
			if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_APPLICATION], IDC_FLAGS,
					pNSSMService->m_AppParameters,
					RTL_NUMBER_OF(pNSSMService->m_AppParameters))) {
				popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
					NSSM_GUI_INVALID_OPTIONS);
				return 4;
			}
		}
	}

	// Get details.
	if (SendMessageW(GetDlgItem(g_Tablist[NSSM_TAB_DETAILS], IDC_DISPLAYNAME),
			WM_GETTEXTLENGTH, 0, 0)) {
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_DETAILS], IDC_DISPLAYNAME,
				pNSSMService->m_DisplayName,
				RTL_NUMBER_OF(pNSSMService->m_DisplayName))) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_INVALID_DISPLAYNAME);
			return 5;
		}
	}

	if (SendMessageW(GetDlgItem(g_Tablist[NSSM_TAB_DETAILS], IDC_DESCRIPTION),
			WM_GETTEXTLENGTH, 0, 0)) {
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_DETAILS], IDC_DESCRIPTION,
				pNSSMService->m_Description,
				RTL_NUMBER_OF(pNSSMService->m_Description))) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_INVALID_DESCRIPTION);
			return 5;
		}
	}

	HWND hCombo = GetDlgItem(g_Tablist[NSSM_TAB_DETAILS], IDC_STARTUP);
	pNSSMService->m_uStartup =
		static_cast<uint32_t>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
	if (static_cast<LONG>(pNSSMService->m_uStartup) == CB_ERR) {
		pNSSMService->m_uStartup = NSSM_STARTUP_AUTOMATIC;
	}

	// Get logon stuff.

	// Local system user
	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_LOGON], IDC_LOCALSYSTEM, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		if (SendDlgItemMessageW(
				g_Tablist[NSSM_TAB_LOGON], IDC_INTERACT, BM_GETCHECK, 0, 0) &
			BST_CHECKED) {
			pNSSMService->m_uServiceTypes |= SERVICE_INTERACTIVE_PROCESS;
		}
		if (pNSSMService->m_pUsername) {
			heap_free(pNSSMService->m_pUsername);
		}
		pNSSMService->m_pUsername = NULL;
		pNSSMService->m_uUsernameLength = 0;
		if (pNSSMService->m_pPassword) {
			RtlSecureZeroMemory(pNSSMService->m_pPassword,
				pNSSMService->m_uPasswordLength * sizeof(wchar_t));
			heap_free(pNSSMService->m_pPassword);
		}
		pNSSMService->m_pPassword = NULL;
		pNSSMService->m_uPasswordLength = 0;

		// Virtual service?
	} else if (SendDlgItemMessageW(g_Tablist[NSSM_TAB_LOGON],
				   IDC_VIRTUAL_SERVICE, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		if (pNSSMService->m_pUsername) {
			heap_free(pNSSMService->m_pUsername);
		}
		pNSSMService->m_pUsername = virtual_account(pNSSMService->m_Name);
		if (!pNSSMService->m_pUsername) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"account name", L"install()");
			return 6;
		}
		pNSSMService->m_uUsernameLength = wcslen(pNSSMService->m_pUsername) + 1;
		pNSSMService->m_pPassword = NULL;
		pNSSMService->m_uPasswordLength = 0;
	} else {

		// Username and password
		pNSSMService->m_uUsernameLength = static_cast<uintptr_t>(
			SendMessageW(GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_USERNAME),
				WM_GETTEXTLENGTH, 0, 0));
		if (!pNSSMService->m_uUsernameLength) {
			popup_message(
				hWindow, MB_OK | MB_ICONEXCLAMATION, NSSM_GUI_MISSING_USERNAME);
			return 6;
		}
		// Ensure the terminating zero is accounted for
		pNSSMService->m_uUsernameLength++;

		pNSSMService->m_pUsername = static_cast<wchar_t*>(
			heap_alloc(pNSSMService->m_uUsernameLength * sizeof(wchar_t)));
		if (!pNSSMService->m_pUsername) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"account name", L"install()");
			return 6;
		}
		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_LOGON], IDC_USERNAME,
				pNSSMService->m_pUsername,
				static_cast<int>(pNSSMService->m_uUsernameLength))) {
			heap_free(pNSSMService->m_pUsername);
			pNSSMService->m_pUsername = NULL;
			pNSSMService->m_uUsernameLength = 0;
			popup_message(
				hWindow, MB_OK | MB_ICONEXCLAMATION, NSSM_GUI_INVALID_USERNAME);
			return 6;
		}

		/*
		  Special case for well-known accounts.
		  Ignore the password if we're editing and the username hasn't changed.
		*/
		const wchar_t* pWellKnownUsername =
			well_known_username(pNSSMService->m_pUsername);
		if (pWellKnownUsername) {
			if (str_equiv(pWellKnownUsername, g_NSSMLocalSystemAccount)) {
				heap_free(pNSSMService->m_pUsername);
				pNSSMService->m_pUsername = NULL;
				pNSSMService->m_uUsernameLength = 0;
			} else {

				// Add in the terminating zero to the length
				pNSSMService->m_uUsernameLength =
					wcslen(pWellKnownUsername) + 1;
				pNSSMService->m_pUsername = static_cast<wchar_t*>(heap_alloc(
					pNSSMService->m_uUsernameLength * sizeof(wchar_t)));
				if (!pNSSMService->m_pUsername) {
					print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"canon",
						L"install()");
					return 6;
				}
				memmove(pNSSMService->m_pUsername, pWellKnownUsername,
					pNSSMService->m_uUsernameLength * sizeof(wchar_t));
			}
		} else {
			// Password.
			pNSSMService->m_uPasswordLength =
				static_cast<uintptr_t>(SendMessageW(
					GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD1),
					WM_GETTEXTLENGTH, 0, 0));
			uintptr_t uPasswordLength = static_cast<uintptr_t>(SendMessageW(
				GetDlgItem(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD2),
				WM_GETTEXTLENGTH, 0, 0));

			if (!pNSSMOriginal || !pNSSMOriginal->m_pUsername ||
				!str_equiv(
					pNSSMService->m_pUsername, pNSSMOriginal->m_pUsername) ||
				pNSSMService->m_uPasswordLength || uPasswordLength) {

				if (!pNSSMService->m_uPasswordLength) {
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_GUI_MISSING_PASSWORD);
					return 6;
				}

				if (uPasswordLength != pNSSMService->m_uPasswordLength) {
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_GUI_MISSING_PASSWORD);
					return 6;
				}
				// Include the terminating zero
				pNSSMService->m_uPasswordLength++;

				// Temporary buffer for password validation.
				wchar_t* pPassword = static_cast<wchar_t*>(heap_alloc(
					pNSSMService->m_uPasswordLength * sizeof(wchar_t)));
				if (!pPassword) {
					heap_free(pNSSMService->m_pUsername);
					pNSSMService->m_pUsername = NULL;
					pNSSMService->m_uPasswordLength = 0;
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_EVENT_OUT_OF_MEMORY, L"password confirmation",
						L"install()");
					return 6;
				}

				/* Actual password buffer. */
				pNSSMService->m_pPassword = static_cast<wchar_t*>(heap_alloc(
					pNSSMService->m_uPasswordLength * sizeof(wchar_t)));
				if (!pNSSMService->m_pPassword) {
					heap_free(pPassword);
					heap_free(pNSSMService->m_pUsername);
					pNSSMService->m_pUsername = NULL;
					pNSSMService->m_uUsernameLength = 0;
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_EVENT_OUT_OF_MEMORY, L"password", L"install()");
					return 6;
				}

				// Get first password.
				if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD1,
						pNSSMService->m_pPassword,
						static_cast<int>(pNSSMService->m_uPasswordLength))) {
					heap_free(pPassword);
					RtlSecureZeroMemory(pNSSMService->m_pPassword,
						pNSSMService->m_uPasswordLength * sizeof(wchar_t));
					heap_free(pNSSMService->m_pPassword);
					pNSSMService->m_pPassword = NULL;
					pNSSMService->m_uPasswordLength = 0;
					heap_free(pNSSMService->m_pUsername);
					pNSSMService->m_pUsername = NULL;
					pNSSMService->m_uUsernameLength = 0;
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_GUI_INVALID_PASSWORD);
					return 6;
				}

				// Get confirmation.
				if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_LOGON], IDC_PASSWORD2,
						pPassword,
						static_cast<int>(pNSSMService->m_uPasswordLength))) {
					RtlSecureZeroMemory(pPassword,
						pNSSMService->m_uPasswordLength * sizeof(wchar_t));
					heap_free(pPassword);
					RtlSecureZeroMemory(pNSSMService->m_pPassword,
						pNSSMService->m_uPasswordLength * sizeof(wchar_t));
					heap_free(pNSSMService->m_pPassword);
					pNSSMService->m_pPassword = NULL;
					pNSSMService->m_uPasswordLength = 0;
					heap_free(pNSSMService->m_pUsername);
					pNSSMService->m_pUsername = NULL;
					pNSSMService->m_uUsernameLength = 0;
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_GUI_INVALID_PASSWORD);
					return 6;
				}

				// Compare.
				if (wcsncmp(pPassword, pNSSMService->m_pPassword,
						pNSSMService->m_uPasswordLength)) {
					popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
						NSSM_GUI_MISSING_PASSWORD);
					RtlSecureZeroMemory(pPassword,
						pNSSMService->m_uPasswordLength * sizeof(wchar_t));
					heap_free(pPassword);
					RtlSecureZeroMemory(pNSSMService->m_pPassword,
						pNSSMService->m_uPasswordLength * sizeof(wchar_t));
					heap_free(pNSSMService->m_pPassword);
					pNSSMService->m_pPassword = NULL;
					pNSSMService->m_uPasswordLength = 0;
					heap_free(pNSSMService->m_pUsername);
					pNSSMService->m_pUsername = NULL;
					pNSSMService->m_uUsernameLength = 0;
					return 6;
				}
			}
		}
	}

	// Get dependencies.
	uint32_t uDependenciesLength = static_cast<uint32_t>(SendMessageW(
		GetDlgItem(g_Tablist[NSSM_TAB_DEPENDENCIES], IDC_DEPENDENCIES),
		WM_GETTEXTLENGTH, 0, 0));
	if (uDependenciesLength) {

		// Double zero termination
		wchar_t* pDependencies = static_cast<wchar_t*>(
			heap_calloc((uDependenciesLength + 2) * sizeof(wchar_t)));
		if (!pDependencies) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"dependencies", L"install()");
			cleanup_nssm_service(pNSSMService);
			return 6;
		}

		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_DEPENDENCIES], IDC_DEPENDENCIES,
				pDependencies, static_cast<int>(uDependenciesLength) + 1)) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_INVALID_DEPENDENCIES);
			heap_free(pDependencies);
			cleanup_nssm_service(pNSSMService);
			return 6;
		}

		if (unformat_double_null(pDependencies, uDependenciesLength,
				&pNSSMService->m_pDependencies,
				&pNSSMService->m_uDependenciesLength)) {
			heap_free(pDependencies);
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"dependencies", L"install()");
			cleanup_nssm_service(pNSSMService);
			return 6;
		}

		heap_free(pDependencies);
	}

	// Remaining tabs are only for services we manage.
	if (pNSSMService->m_bNative) {
		return 0;
	}

	// Get process stuff.
	hCombo = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_PRIORITY);
	pNSSMService->m_uPriority = priority_index_to_constant(
		static_cast<uint32_t>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0)));

	pNSSMService->m_uAffinity = 0LL;
	if (!(SendDlgItemMessageW(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY_ALL,
			  BM_GETCHECK, 0, 0) &
			BST_CHECKED)) {
		HWND hList = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY);
		LONG_PTR iSelected = SendMessageW(hList, LB_GETSELCOUNT, 0, 0);
		LONG_PTR iCount = SendMessageW(hList, LB_GETCOUNT, 0, 0);
		if (!iSelected) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_WARN_AFFINITY_NONE);
			return 5;
		} else if (iSelected < iCount) {
			for (LONG_PTR i = 0; i < iCount; i++) {
				if (SendMessageW(hList, LB_GETSEL, static_cast<WPARAM>(i), 0)) {
					pNSSMService->m_uAffinity |=
						(1ULL << static_cast<uint64_t>(i));
				}
			}
		}
	}

	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_PROCESS], IDC_CONSOLE, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		pNSSMService->m_bDontSpawnConsole = false;
	} else {
		pNSSMService->m_bDontSpawnConsole = true;
	}

	// Get stop method stuff.
	check_stop_method(
		pNSSMService, NSSM_STOP_METHOD_CONSOLE, IDC_METHOD_CONSOLE);
	check_stop_method(pNSSMService, NSSM_STOP_METHOD_WINDOW, IDC_METHOD_WINDOW);
	check_stop_method(
		pNSSMService, NSSM_STOP_METHOD_THREADS, IDC_METHOD_THREADS);
	check_stop_method(
		pNSSMService, NSSM_STOP_METHOD_TERMINATE, IDC_METHOD_TERMINATE);
	check_number(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_CONSOLE,
		&pNSSMService->m_uKillConsoleDelay);
	check_number(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_WINDOW,
		&pNSSMService->m_uKillWindowDelay);
	check_number(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_THREADS,
		&pNSSMService->m_uKillThreadsDelay);

	if (SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_PROCESS_TREE,
			BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		pNSSMService->m_bKillProcessTree = true;
	} else {
		pNSSMService->m_bKillProcessTree = false;
	}

	// Get exit action stuff.
	check_number(g_Tablist[NSSM_TAB_EXIT], IDC_THROTTLE,
		&pNSSMService->m_uThrottleDelay);
	hCombo = GetDlgItem(g_Tablist[NSSM_TAB_EXIT], IDC_APPEXIT);
	pNSSMService->m_uDefaultExitAction =
		static_cast<uint32_t>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
	if (pNSSMService->m_uDefaultExitAction == CB_ERR) {
		pNSSMService->m_uDefaultExitAction = NSSM_EXIT_RESTART;
	}

	check_number(g_Tablist[NSSM_TAB_EXIT], IDC_RESTART_DELAY,
		&pNSSMService->m_uRestartDelay);

	// Get I/O stuff.
	check_io(hWindow, L"stdin", pNSSMService->m_StdinPathname,
		RTL_NUMBER_OF(pNSSMService->m_StdinPathname), IDC_STDIN);
	check_io(hWindow, L"stdout", pNSSMService->m_StdoutPathname,
		RTL_NUMBER_OF(pNSSMService->m_StdoutPathname), IDC_STDOUT);
	check_io(hWindow, L"stderr", pNSSMService->m_StderrPathname,
		RTL_NUMBER_OF(pNSSMService->m_StderrPathname), IDC_STDERR);
	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_IO], IDC_TIMESTAMP, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		pNSSMService->m_bTimestampLog = true;
	} else {
		pNSSMService->m_bTimestampLog = false;
	}

	// Override stdout and/or stderr.
	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_ROTATION], IDC_TRUNCATE, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		if (pNSSMService->m_StdoutPathname[0]) {
			pNSSMService->m_uStdoutDisposition = CREATE_ALWAYS;
		}
		if (pNSSMService->m_StderrPathname[0]) {
			pNSSMService->m_uStderrDisposition = CREATE_ALWAYS;
		}
	}

	// Get rotation stuff.
	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE, BM_GETCHECK, 0, 0) &
		BST_CHECKED) {
		pNSSMService->m_bRotateFiles = true;
		if (SendDlgItemMessageW(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_ONLINE,
				BM_GETCHECK, 0, 0) &
			BST_CHECKED)
			pNSSMService->m_uRotateStdoutOnline =
				pNSSMService->m_uRotateStderrOnline = NSSM_ROTATE_ONLINE;
		check_number(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_SECONDS,
			&pNSSMService->m_uRotateSeconds);
		check_number(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_BYTES_LOW,
			&pNSSMService->m_uRotateBytesLow);
	}

	// Get hook stuff.
	if (SendDlgItemMessageW(
			g_Tablist[NSSM_TAB_HOOKS], IDC_REDIRECT_HOOK, BM_GETCHECK, 0, 0) &
		BST_CHECKED)
		pNSSMService->m_bHookShareOutputHandles = true;

	// Get environment.
	uintptr_t uEnvironmentLength = static_cast<uintptr_t>(SendMessageW(
		GetDlgItem(g_Tablist[NSSM_TAB_ENVIRONMENT], IDC_ENVIRONMENT),
		WM_GETTEXTLENGTH, 0, 0));
	if (uEnvironmentLength) {
		wchar_t* pEnvironmentVariables = static_cast<wchar_t*>(
			heap_calloc((uEnvironmentLength + 2) * sizeof(wchar_t)));
		if (!pEnvironmentVariables) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"environment", L"install()");
			cleanup_nssm_service(pNSSMService);
			return 5;
		}

		if (!GetDlgItemTextW(g_Tablist[NSSM_TAB_ENVIRONMENT], IDC_ENVIRONMENT,
				pEnvironmentVariables,
				static_cast<int>(uEnvironmentLength + 1))) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_INVALID_ENVIRONMENT);
			heap_free(pEnvironmentVariables);
			cleanup_nssm_service(pNSSMService);
			return 5;
		}

		// Convert from a CR/LF parsed string to a double null terminated list
		// of "C" strings
		wchar_t* pParsedEnvironmentVariables;
		uintptr_t uParsedEnvironmentVariablesLength;
		if (unformat_double_null(pEnvironmentVariables, uEnvironmentLength,
				&pParsedEnvironmentVariables,
				&uParsedEnvironmentVariablesLength)) {
			heap_free(pEnvironmentVariables);
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_EVENT_OUT_OF_MEMORY, L"environment", L"install()");
			cleanup_nssm_service(pNSSMService);
			return 5;
		}

		heap_free(pEnvironmentVariables);

		// Use the double null terminated list
		pEnvironmentVariables = pParsedEnvironmentVariables;
		uEnvironmentLength = uParsedEnvironmentVariablesLength;

		// Test the environment is valid.
		if (test_environment(pEnvironmentVariables)) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_INVALID_ENVIRONMENT);
			heap_free(pEnvironmentVariables);
			cleanup_nssm_service(pNSSMService);
			return 5;
		}

		// Query the checkbox for append or replacement of environment variables
		if (SendDlgItemMessageW(g_Tablist[NSSM_TAB_ENVIRONMENT],
				IDC_ENVIRONMENT_REPLACE, BM_GETCHECK, 0, 0) &
			BST_CHECKED) {
			pNSSMService->m_pEnvironmentVariables = pEnvironmentVariables;
			pNSSMService->m_uEnvironmentVariablesLength = uEnvironmentLength;
		} else {
			pNSSMService->m_pExtraEnvironmentVariables = pEnvironmentVariables;
			pNSSMService->m_uExtraEnvironmentVariablesLength =
				uEnvironmentLength;
		}
	}
	// All good
	return 0;
}

/***************************************

	Install the service.

***************************************/

int install(HWND hWindow)
{
	// Sanity check
	if (!hWindow) {
		return 1;
	}

	nssm_service_t* pNSSMService = alloc_nssm_service();
	if (pNSSMService) {
		int iResult = configure(hWindow, pNSSMService, NULL);
		if (iResult) {
			return iResult;
		}
	}

	// See if it works.
	// Note: install_service() handles if pNSSMService is NULL
	switch (install_service(pNSSMService)) {
	case 1:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_EVENT_OUT_OF_MEMORY, L"service", L"install()");
		cleanup_nssm_service(pNSSMService);
		return 1;

	case 2:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 2;

	case 3:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_MESSAGE_PATH_TOO_LONG, g_NSSM);
		cleanup_nssm_service(pNSSMService);
		return 3;

	case 4:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_OUT_OF_MEMORY_FOR_IMAGEPATH);
		cleanup_nssm_service(pNSSMService);
		return 4;

	case 5:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_INSTALL_SERVICE_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 5;

	case 6:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_CREATE_PARAMETERS_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 6;
	}

	// It's good, update the hooks
	update_hooks(pNSSMService->m_Name);

	popup_message(
		hWindow, MB_OK, NSSM_MESSAGE_SERVICE_INSTALLED, pNSSMService->m_Name);
	cleanup_nssm_service(pNSSMService);
	return 0;
}

/***************************************

	Remove the service

***************************************/

int remove(HWND hWindow)
{
	// Sanity check
	if (!hWindow) {
		return 1;
	}

	// See if it works
	nssm_service_t* pNSSMService = alloc_nssm_service();
	if (pNSSMService) {
		// Get service name
		if (!GetDlgItemTextW(hWindow, IDC_NAME, pNSSMService->m_Name,
				RTL_NUMBER_OF(pNSSMService->m_Name))) {
			popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
				NSSM_GUI_MISSING_SERVICE_NAME);
			cleanup_nssm_service(pNSSMService);
			return 2;
		}

		// Confirm
		if (popup_message(hWindow, MB_YESNO, NSSM_GUI_ASK_REMOVE_SERVICE,
				pNSSMService->m_Name) != IDYES) {
			cleanup_nssm_service(pNSSMService);
			return 0;
		}
	}

	switch (remove_service(pNSSMService)) {
	case 1:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_EVENT_OUT_OF_MEMORY, L"service", L"remove()");
		cleanup_nssm_service(pNSSMService);
		return 1;

	case 2:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 2;

	case 3:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_SERVICE_NOT_INSTALLED);
		cleanup_nssm_service(pNSSMService);
		return 3;

	case 4:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_REMOVE_SERVICE_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 4;
	}

	// Gone
	popup_message(
		hWindow, MB_OK, NSSM_MESSAGE_SERVICE_REMOVED, pNSSMService->m_Name);
	cleanup_nssm_service(pNSSMService);
	return 0;
}

/***************************************

	Edit a service

***************************************/

int edit(HWND hWindow, const nssm_service_t* pNSSMOriginal)
{
	if (!hWindow) {
		return 1;
	}

	nssm_service_t* pNSSMService = alloc_nssm_service();
	if (pNSSMService) {
		int iResult = configure(hWindow, pNSSMService, pNSSMOriginal);
		if (iResult) {
			return iResult;
		}
	}

	switch (edit_service(pNSSMService, true)) {
	case 1:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_EVENT_OUT_OF_MEMORY, L"service", L"edit()");
		cleanup_nssm_service(pNSSMService);
		return 1;

	case 3:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_MESSAGE_PATH_TOO_LONG, g_NSSM);
		cleanup_nssm_service(pNSSMService);
		return 3;

	case 4:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_OUT_OF_MEMORY_FOR_IMAGEPATH);
		cleanup_nssm_service(pNSSMService);
		return 4;

	case 5:
	case 6:
		popup_message(hWindow, MB_OK | MB_ICONEXCLAMATION,
			NSSM_GUI_EDIT_PARAMETERS_FAILED);
		cleanup_nssm_service(pNSSMService);
		return 6;
	}

	// Update the hooks for the edited service
	update_hooks(pNSSMService->m_Name);

	popup_message(
		hWindow, MB_OK, NSSM_MESSAGE_SERVICE_EDITED, pNSSMService->m_Name);
	cleanup_nssm_service(pNSSMService);
	return 0;
}

/***************************************

	Browse filter for items of interest

***************************************/

static wchar_t* browse_filter(int iMessage)
{
	switch (iMessage) {
	case NSSM_GUI_BROWSE_FILTER_APPLICATIONS:
		return L"*.exe;*.bat;*.cmd";
	case NSSM_GUI_BROWSE_FILTER_DIRECTORIES:
		return L".";
	case NSSM_GUI_BROWSE_FILTER_ALL_FILES: /* Fall through. */
	default:
		return L"*.*";
	}
}

/***************************************

	Browse filter dialog

***************************************/

static UINT_PTR CALLBACK browse_hook(
	HWND /* hDialog */, UINT iMessage, WPARAM /* w */, LPARAM /* l */)
{
	switch (iMessage) {
	case WM_INITDIALOG:
		return 1;
	}
	return 0;
}

/***************************************

	Browse for application

***************************************/

void browse(
	HWND hWindow, const wchar_t* pWorkingDirectory, uint32_t uFlags, ...)
{
	if (!hWindow) {
		return;
	}

	va_list arg;
	const uintptr_t uBufferSize = 256U;

	OPENFILENAMEW ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter =
		static_cast<wchar_t*>(heap_alloc(uBufferSize * sizeof(wchar_t)));

	/* XXX: Escaping nulls with FormatMessage is tricky */
	if (ofn.lpstrFilter) {
		ZeroMemory(const_cast<wchar_t*>(ofn.lpstrFilter), uBufferSize);
		uintptr_t uLength = 0;
		// "Applications" + NULL + "*.exe" + NULL
		va_start(arg, uFlags);

		int i;
		while ((i = va_arg(arg, int)) != 0) {
			wchar_t* pLocalized = message_string(static_cast<uint32_t>(i));
			StringCchPrintfW(const_cast<wchar_t*>(ofn.lpstrFilter) + uLength,
				uBufferSize - uLength, pLocalized);
			uLength += wcslen(pLocalized) + 1;
			LocalFree(pLocalized);
			wchar_t* filter = browse_filter(i);
			StringCchPrintfW(const_cast<wchar_t*>(ofn.lpstrFilter) + uLength,
				uBufferSize - uLength, L"%s", filter);
			uLength += wcslen(filter) + 1;
		}
		va_end(arg);
		/* Remainder of the buffer is already zeroed */
	}

	ofn.lpstrFile =
		static_cast<wchar_t*>(heap_alloc(PATH_LENGTH * sizeof(wchar_t)));
	if (ofn.lpstrFile) {
		if (uFlags & OFN_NOVALIDATE) {
			// Directory hack.
			StringCchPrintfW(ofn.lpstrFile, PATH_LENGTH, L":%s:",
				message_string(NSSM_GUI_BROWSE_FILTER_DIRECTORIES));
			ofn.nMaxFile = DIR_LENGTH;
		} else {
			StringCchPrintfW(
				ofn.lpstrFile, PATH_LENGTH, L"%s", pWorkingDirectory);
			ofn.nMaxFile = PATH_LENGTH;
		}
	}
	ofn.lpstrTitle = message_string(NSSM_GUI_BROWSE_TITLE);
	ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | uFlags;

	if (GetOpenFileNameW(&ofn)) {
		// Directory hack.
		if (uFlags & OFN_NOVALIDATE) {
			strip_basename(ofn.lpstrFile);
		}
		SendMessageW(
			hWindow, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(ofn.lpstrFile));
	}
	if (ofn.lpstrFilter) {
		heap_free(const_cast<wchar_t*>(ofn.lpstrFilter));
	}
	if (ofn.lpstrFile) {
		heap_free(ofn.lpstrFile);
	}
}

/***************************************

	Tab handler for dialog

***************************************/

static INT_PTR CALLBACK tab_dlg(
	HWND hTabControl, UINT uMessage, WPARAM w, LPARAM /* l */)
{
	wchar_t TempBuffer[PATH_LENGTH];
	HWND hDialog;
	BOOL bEnabled;

	// Which message to process?
	switch (uMessage) {
	case WM_INITDIALOG:
		return 1;

	/* Button was pressed or control was controlled. */
	case WM_COMMAND:

		switch (LOWORD(w)) {
			// Browse for application.
		case IDC_BROWSE:
			hDialog = GetDlgItem(hTabControl, IDC_PATH);
			GetDlgItemTextW(
				hTabControl, IDC_PATH, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			browse(hDialog, TempBuffer, OFN_FILEMUSTEXIST,
				NSSM_GUI_BROWSE_FILTER_APPLICATIONS,
				NSSM_GUI_BROWSE_FILTER_ALL_FILES, 0);

			// Fill in startup directory if it wasn't already specified.
			GetDlgItemTextW(
				hTabControl, IDC_DIR, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			if (!TempBuffer[0]) {
				GetDlgItemTextW(hTabControl, IDC_PATH, TempBuffer,
					RTL_NUMBER_OF(TempBuffer));
				strip_basename(TempBuffer);
				SetDlgItemTextW(hTabControl, IDC_DIR, TempBuffer);
			}
			break;

			// Browse for startup directory.
		case IDC_BROWSE_DIR:
			hDialog = GetDlgItem(hTabControl, IDC_DIR);
			GetDlgItemTextW(
				hTabControl, IDC_DIR, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			browse(hDialog, TempBuffer, OFN_NOVALIDATE,
				NSSM_GUI_BROWSE_FILTER_DIRECTORIES, 0);
			break;

			// Log on.
		case IDC_LOCALSYSTEM:
			set_logon_enabled(1, 0);
			break;

		case IDC_VIRTUAL_SERVICE:
			set_logon_enabled(0, 0);
			break;

		case IDC_ACCOUNT:
			set_logon_enabled(0, 1);
			break;

			// Affinity.
		case IDC_AFFINITY_ALL:
			if (SendDlgItemMessageW(hTabControl, LOWORD(w), BM_GETCHECK, 0, 0) &
				BST_CHECKED) {
				bEnabled = 0;
			} else {
				bEnabled = 1;
			}
			set_affinity_enabled(bEnabled);
			break;

			// Shutdown methods.
		case IDC_METHOD_CONSOLE:
			set_timeout_enabled(LOWORD(w), IDC_KILL_CONSOLE);
			break;

		case IDC_METHOD_WINDOW:
			set_timeout_enabled(LOWORD(w), IDC_KILL_WINDOW);
			break;

		case IDC_METHOD_THREADS:
			set_timeout_enabled(LOWORD(w), IDC_KILL_THREADS);
			break;

			// Browse for stdin.
		case IDC_BROWSE_STDIN:
			hDialog = GetDlgItem(hTabControl, IDC_STDIN);
			GetDlgItemTextW(
				hTabControl, IDC_STDIN, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			browse(hDialog, TempBuffer, 0, NSSM_GUI_BROWSE_FILTER_ALL_FILES, 0);
			break;

			// Browse for stdout.
		case IDC_BROWSE_STDOUT:
			hDialog = GetDlgItem(hTabControl, IDC_STDOUT);
			GetDlgItemTextW(
				hTabControl, IDC_STDOUT, TempBuffer, RTL_NUMBER_OF(TempBuffer));

			browse(hDialog, TempBuffer, 0, NSSM_GUI_BROWSE_FILTER_ALL_FILES, 0);

			// Fill in stderr if it wasn't already specified.
			GetDlgItemTextW(
				hTabControl, IDC_STDERR, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			if (!TempBuffer[0]) {
				GetDlgItemTextW(hTabControl, IDC_STDOUT, TempBuffer,
					RTL_NUMBER_OF(TempBuffer));
				SetDlgItemTextW(hTabControl, IDC_STDERR, TempBuffer);
			}
			break;

			// Browse for stderr.
		case IDC_BROWSE_STDERR:
			hDialog = GetDlgItem(hTabControl, IDC_STDERR);
			GetDlgItemTextW(
				hTabControl, IDC_STDERR, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			browse(hDialog, TempBuffer, 0, NSSM_GUI_BROWSE_FILTER_ALL_FILES, 0);
			break;

			// Rotation.
		case IDC_ROTATE:
			if (SendDlgItemMessageW(hTabControl, LOWORD(w), BM_GETCHECK, 0, 0) &
				BST_CHECKED) {
				bEnabled = 1;
			} else {
				bEnabled = 0;
			}
			set_rotation_enabled(bEnabled);
			break;

			// Hook event.
		case IDC_HOOK_EVENT:
			if (HIWORD(w) == CBN_SELCHANGE) {
				set_hook_tab(static_cast<uint32_t>(SendMessageW(
								 GetDlgItem(hTabControl, IDC_HOOK_EVENT),
								 CB_GETCURSEL, 0, 0)),
					0, false);
			}
			break;

		// Hook action.
		case IDC_HOOK_ACTION:
			if (HIWORD(w) == CBN_SELCHANGE) {
				set_hook_tab(static_cast<uint32_t>(SendMessageW(
								 GetDlgItem(hTabControl, IDC_HOOK_EVENT),
								 CB_GETCURSEL, 0, 0)),
					static_cast<uint32_t>(
						SendMessageW(GetDlgItem(hTabControl, IDC_HOOK_ACTION),
							CB_GETCURSEL, 0, 0)),
					false);
			}
			break;

		// Browse for hook.
		case IDC_BROWSE_HOOK:
			hDialog = GetDlgItem(hTabControl, IDC_HOOK);
			GetDlgItemTextW(
				hTabControl, IDC_HOOK, TempBuffer, RTL_NUMBER_OF(TempBuffer));
			browse(hDialog, L"", OFN_FILEMUSTEXIST,
				NSSM_GUI_BROWSE_FILTER_ALL_FILES, 0);
			break;

		// Hook.
		case IDC_HOOK:
			set_hook_tab(static_cast<uint32_t>(SendMessageW(
							 GetDlgItem(hTabControl, IDC_HOOK_EVENT),
							 CB_GETCURSEL, 0, 0)),
				static_cast<uint32_t>(
					SendMessageW(GetDlgItem(hTabControl, IDC_HOOK_ACTION),
						CB_GETCURSEL, 0, 0)),
				true);
			break;
		default:
			break;
		}
		return 1;

	// Ignore the rest
	default:
		break;
	}

	return 0;
}

/***************************************

	Install/remove dialogue callback

***************************************/

INT_PTR CALLBACK nssm_dlg(HWND hWindow, UINT message, WPARAM w, LPARAM l)
{

	switch (message) {
		/* Creating the dialogue */
	case WM_INITDIALOG: {
		nssm_service_t* pNSSMService = reinterpret_cast<nssm_service_t*>(l);

		SetFocus(GetDlgItem(hWindow, IDC_NAME));

		HWND hTabsControl = GetDlgItem(hWindow, IDC_TAB1);
		if (!hTabsControl) {
			return 0;
		}

		// Set up tabs.
		TCITEMW tab;
		ZeroMemory(&tab, sizeof(tab));
		tab.mask = TCIF_TEXT;

		g_iSelectedTab = 0;

		// Application tab.
		if (pNSSMService->m_bNative) {
			tab.pszText = message_string(NSSM_GUI_TAB_NATIVE);
		} else {
			tab.pszText = message_string(NSSM_GUI_TAB_APPLICATION);
		}
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_APPLICATION, (LPARAM)&tab);
		if (pNSSMService->m_bNative) {
			g_Tablist[NSSM_TAB_APPLICATION] =
				dialog(MAKEINTRESOURCEW(IDD_NATIVE), hWindow, tab_dlg);
			EnableWindow(g_Tablist[NSSM_TAB_APPLICATION], 0);
			EnableWindow(
				GetDlgItem(g_Tablist[NSSM_TAB_APPLICATION], IDC_PATH), 0);
		} else {
			g_Tablist[NSSM_TAB_APPLICATION] =
				dialog(MAKEINTRESOURCEW(IDD_APPLICATION), hWindow, tab_dlg);
		}
		ShowWindow(g_Tablist[NSSM_TAB_APPLICATION], SW_SHOW);

		// Details tab.
		tab.pszText = message_string(NSSM_GUI_TAB_DETAILS);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(hTabsControl, TCM_INSERTITEM, NSSM_TAB_DETAILS,
			reinterpret_cast<LPARAM>(&tab));
		g_Tablist[NSSM_TAB_DETAILS] =
			dialog(MAKEINTRESOURCEW(IDD_DETAILS), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_DETAILS], SW_HIDE);

		// Set defaults.
		HWND hCombo = GetDlgItem(g_Tablist[NSSM_TAB_DETAILS], IDC_STARTUP);
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_STARTUP_AUTOMATIC,
			(LPARAM)message_string(NSSM_GUI_STARTUP_AUTOMATIC));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_STARTUP_DELAYED,
			(LPARAM)message_string(NSSM_GUI_STARTUP_DELAYED));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_STARTUP_MANUAL,
			(LPARAM)message_string(NSSM_GUI_STARTUP_MANUAL));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_STARTUP_DISABLED,
			(LPARAM)message_string(NSSM_GUI_STARTUP_DISABLED));
		SendMessageW(hCombo, CB_SETCURSEL, NSSM_STARTUP_AUTOMATIC, 0);

		// Logon tab.
		tab.pszText = message_string(NSSM_GUI_TAB_LOGON);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_LOGON, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_LOGON] =
			dialog(MAKEINTRESOURCEW(IDD_LOGON), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_LOGON], SW_HIDE);

		// Set defaults.
		CheckRadioButton(g_Tablist[NSSM_TAB_LOGON], IDC_LOCALSYSTEM,
			IDC_ACCOUNT, IDC_LOCALSYSTEM);
		set_logon_enabled(1, 0);

		// Dependencies tab.
		tab.pszText = message_string(NSSM_GUI_TAB_DEPENDENCIES);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_DEPENDENCIES, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_DEPENDENCIES] =
			dialog(MAKEINTRESOURCEW(IDD_DEPENDENCIES), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_DEPENDENCIES], SW_HIDE);

		// Remaining tabs are only for services we manage.
		if (pNSSMService->m_bNative) {
			return 1;
		}

		// Process tab.
		tab.pszText = message_string(NSSM_GUI_TAB_PROCESS);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_PROCESS, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_PROCESS] =
			dialog(MAKEINTRESOURCEW(IDD_PROCESS), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_PROCESS], SW_HIDE);

		// Set defaults.
		hCombo = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_PRIORITY);
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_REALTIME_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_REALTIME_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_HIGH_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_HIGH_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_ABOVE_NORMAL_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_ABOVE_NORMAL_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_NORMAL_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_NORMAL_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_BELOW_NORMAL_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_BELOW_NORMAL_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_IDLE_PRIORITY,
			(LPARAM)message_string(NSSM_GUI_IDLE_PRIORITY_CLASS));
		SendMessageW(hCombo, CB_SETCURSEL, NSSM_NORMAL_PRIORITY, 0);

		SendDlgItemMessageW(g_Tablist[NSSM_TAB_PROCESS], IDC_CONSOLE,
			BM_SETCHECK, BST_CHECKED, 0);

		HWND hList = GetDlgItem(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY);
		int n = num_cpus();
		SendMessageW(hList, LB_SETCOLUMNWIDTH, 16, 0);
		int i;
		for (i = 0; i < n; i++) {
			wchar_t SmallBuffer[4];
			StringCchPrintfW(SmallBuffer, RTL_NUMBER_OF(SmallBuffer), L"%d", i);
			SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)SmallBuffer);
		}

		/*
		  Size to fit.
		  The box is high enough for four rows.  It is wide enough for eight
		  columns without scrolling.  With scrollbars it shrinks to two rows.
		  Note that the above only holds if we set the column width BEFORE
		  adding the strings.
		*/
		if (n < 32) {
			int columns = (n - 1) / 4;
			RECT TempRect;
			GetWindowRect(hList, &TempRect);
			int width = TempRect.right - TempRect.left;
			width -= (7 - columns) * 16;
			int height = TempRect.bottom - TempRect.top;
			if (n < 4) {
				height -= static_cast<int>(
							  SendMessageW(hList, LB_GETITEMHEIGHT, 0, 0)) *
					(4 - n);
			}
			SetWindowPos(
				hList, 0, 0, 0, width, height, SWP_NOMOVE | SWP_NOOWNERZORDER);
		}
		SendMessageW(hList, LB_SELITEMRANGE, 1, MAKELPARAM(0, n));

		SendDlgItemMessageW(g_Tablist[NSSM_TAB_PROCESS], IDC_AFFINITY_ALL,
			BM_SETCHECK, BST_CHECKED, 0);
		set_affinity_enabled(0);

		// Shutdown tab.
		tab.pszText = message_string(NSSM_GUI_TAB_SHUTDOWN);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_SHUTDOWN, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_SHUTDOWN] =
			dialog(MAKEINTRESOURCEW(IDD_SHUTDOWN), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_SHUTDOWN], SW_HIDE);

		// Set defaults.
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_METHOD_CONSOLE,
			BM_SETCHECK, BST_CHECKED, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_CONSOLE,
			NSSM_KILL_CONSOLE_GRACE_PERIOD, 0);
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_METHOD_WINDOW,
			BM_SETCHECK, BST_CHECKED, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_WINDOW,
			NSSM_KILL_WINDOW_GRACE_PERIOD, 0);
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_METHOD_THREADS,
			BM_SETCHECK, BST_CHECKED, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_THREADS,
			NSSM_KILL_THREADS_GRACE_PERIOD, 0);
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_METHOD_TERMINATE,
			BM_SETCHECK, BST_CHECKED, 0);
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_SHUTDOWN], IDC_KILL_PROCESS_TREE,
			BM_SETCHECK, BST_CHECKED, 1);

		// Restart tab.
		tab.pszText = message_string(NSSM_GUI_TAB_EXIT);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText));
		SendMessageW(hTabsControl, TCM_INSERTITEM, NSSM_TAB_EXIT, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_EXIT] =
			dialog(MAKEINTRESOURCEW(IDD_APPEXIT), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_EXIT], SW_HIDE);

		// Set defaults.
		SetDlgItemInt(g_Tablist[NSSM_TAB_EXIT], IDC_THROTTLE,
			NSSM_RESET_THROTTLE_RESTART, 0);
		hCombo = GetDlgItem(g_Tablist[NSSM_TAB_EXIT], IDC_APPEXIT);
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_EXIT_RESTART,
			(LPARAM)message_string(NSSM_GUI_EXIT_RESTART));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_EXIT_IGNORE,
			(LPARAM)message_string(NSSM_GUI_EXIT_IGNORE));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_EXIT_REALLY,
			(LPARAM)message_string(NSSM_GUI_EXIT_REALLY));
		SendMessageW(hCombo, CB_INSERTSTRING, NSSM_EXIT_UNCLEAN,
			(LPARAM)message_string(NSSM_GUI_EXIT_UNCLEAN));
		SendMessageW(hCombo, CB_SETCURSEL, NSSM_EXIT_RESTART, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_EXIT], IDC_RESTART_DELAY, 0, 0);

		// I/O tab.
		tab.pszText = message_string(NSSM_GUI_TAB_IO);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText)) + 1;
		SendMessageW(hTabsControl, TCM_INSERTITEM, NSSM_TAB_IO, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_IO] =
			dialog(MAKEINTRESOURCEW(IDD_IO), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_IO], SW_HIDE);

		// Set defaults.
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_IO], IDC_TIMESTAMP, BM_SETCHECK,
			BST_UNCHECKED, 0);

		// Rotation tab.
		tab.pszText = message_string(NSSM_GUI_TAB_ROTATION);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText)) + 1;
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_ROTATION, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_ROTATION] =
			dialog(MAKEINTRESOURCEW(IDD_ROTATION), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_ROTATION], SW_HIDE);

		// Set defaults.
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_ONLINE,
			BM_SETCHECK, BST_UNCHECKED, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_SECONDS, 0, 0);
		SetDlgItemInt(g_Tablist[NSSM_TAB_ROTATION], IDC_ROTATE_BYTES_LOW, 0, 0);
		set_rotation_enabled(0);

		// Environment tab.
		tab.pszText = message_string(NSSM_GUI_TAB_ENVIRONMENT);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText)) + 1;
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_ENVIRONMENT, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_ENVIRONMENT] =
			dialog(MAKEINTRESOURCEW(IDD_ENVIRONMENT), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_ENVIRONMENT], SW_HIDE);

		// Hooks tab.
		tab.pszText = message_string(NSSM_GUI_TAB_HOOKS);
		tab.cchTextMax = static_cast<int>(wcslen(tab.pszText)) + 1;
		SendMessageW(
			hTabsControl, TCM_INSERTITEM, NSSM_TAB_HOOKS, (LPARAM)&tab);
		g_Tablist[NSSM_TAB_HOOKS] =
			dialog(MAKEINTRESOURCEW(IDD_HOOKS), hWindow, tab_dlg);
		ShowWindow(g_Tablist[NSSM_TAB_HOOKS], SW_HIDE);

		// Set defaults.
		hCombo = GetDlgItem(g_Tablist[NSSM_TAB_HOOKS], IDC_HOOK_EVENT);
		SendMessageW(hCombo, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			(LPARAM)message_string(NSSM_GUI_HOOK_EVENT_START));
		SendMessageW(hCombo, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			(LPARAM)message_string(NSSM_GUI_HOOK_EVENT_STOP));
		SendMessageW(hCombo, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			(LPARAM)message_string(NSSM_GUI_HOOK_EVENT_EXIT));
		SendMessageW(hCombo, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			(LPARAM)message_string(NSSM_GUI_HOOK_EVENT_POWER));
		SendMessageW(hCombo, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			(LPARAM)message_string(NSSM_GUI_HOOK_EVENT_ROTATE));
		SendDlgItemMessageW(g_Tablist[NSSM_TAB_HOOKS], IDC_REDIRECT_HOOK,
			BM_SETCHECK, BST_UNCHECKED, 0);

		if (pNSSMService->m_Name[0]) {
			wchar_t HookName[HOOK_NAME_LENGTH];
			wchar_t CommandLine[CMD_LENGTH];
			for (i = 0; g_HookEventStrings[i]; i++) {
				const wchar_t* pHookEvent = g_HookEventStrings[i];
				int j;
				for (j = 0; g_HookActionStrings[j]; j++) {
					const wchar_t* pHookAction = g_HookActionStrings[j];
					if (!valid_hook_name(pHookEvent, pHookAction, true)) {
						continue;
					}
					if (get_hook(pNSSMService->m_Name, pHookEvent, pHookAction,
							CommandLine, sizeof(CommandLine))) {
						continue;
					}
					if (hook_env(pHookEvent, pHookAction, HookName,
							RTL_NUMBER_OF(HookName)) < 0) {
						continue;
					}
					SetEnvironmentVariableW(HookName, CommandLine);
				}
			}
		}
		set_hook_tab(0, 0, false);
		return 1;
	}

	/* Tab change. */
	case WM_NOTIFY: {
		const NMHDR* pNotification = (NMHDR*)l;
		switch (pNotification->code) {
		case TCN_SELCHANGE: {
			HWND hTabs2 = GetDlgItem(hWindow, IDC_TAB1);
			if (!hTabs2) {
				return 0;
			}
			int iSelection =
				static_cast<int>(SendMessageW(hTabs2, TCM_GETCURSEL, 0, 0));
			if (iSelection != g_iSelectedTab) {
				ShowWindow(g_Tablist[g_iSelectedTab], SW_HIDE);
				ShowWindow(g_Tablist[iSelection], SW_SHOWDEFAULT);
				SetFocus(GetDlgItem(hWindow, IDOK));
				g_iSelectedTab = iSelection;
			}
			return 1;
		}
		default:
			break;
		}
		return 0;
	}

	// Button was pressed or control was controlled
	case WM_COMMAND:
		switch (LOWORD(w)) {
		// OK button
		case IDOK:
			if (GetWindowLongPtrW(hWindow, GWLP_USERDATA) == IDD_EDIT) {
				if (!edit(hWindow,
						(nssm_service_t*)GetWindowLongPtrW(
							hWindow, DWLP_USER))) {
					PostQuitMessage(0);
				}
			} else if (!install(hWindow)) {
				PostQuitMessage(0);
			}
			break;

		// Cancel button
		case IDCANCEL:
			DestroyWindow(hWindow);
			break;

		// Remove button
		case IDC_REMOVE:
			if (!remove(hWindow)) {
				PostQuitMessage(0);
			}
			break;
		}
		return 1;

	// Window closing
	case WM_CLOSE:
		DestroyWindow(hWindow);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
	}
	return 0;
}
