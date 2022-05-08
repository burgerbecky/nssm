/***************************************

	String constants

***************************************/

#ifndef __CONSTANTS_H__
#define __CONSTANTS_H__

/*
  MSDN says, basically, that the maximum length of a path is 260 characters,
  which is represented by the constant MAX_PATH.  Except when it isn't.

  The maximum length of a directory path is MAX_PATH - 12 because it must be
  possible to create a file in 8.3 format under any valid directory.

  Unicode versions of filesystem API functions accept paths up to 32767
  characters if the first four (wide) characters are L"\\?\" and each component
  of the path, separated by L"\", does not exceed the value of
  lpMaximumComponentLength returned by GetVolumeInformation(), which is
  probably 255.  But might not be.

  Relative paths are always limited to MAX_PATH because the L"\\?\" prefix
  is not valid for a relative path.

  Note that we don't care about the last two paragraphs because we're only
  concerned with allocating buffers big enough to store valid paths.  If the
  user tries to store invalid paths they will fit in the buffers but the
  application will fail.  The reason for the failure will end up in the
  event log and the user will realise the mistake.

  So that's that cleared up, then.
*/

#define PATH_LENGTH 32767
#define DIR_LENGTH PATH_LENGTH - 12

/*
  MSDN says the commandline in CreateProcess() is limited to 32768 characters
  and the application name to MAX_PATH.
  A service name and service display name are limited to 256 characters.
  A registry key is limited to 255 characters.
  A registry value is limited to 16383 characters.
  Therefore we limit the service name to accommodate the path under HKLM.
*/
#define EXE_LENGTH PATH_LENGTH
#define CMD_LENGTH 32768
#define KEY_LENGTH 255
#define VALUE_LENGTH 16383
#define SERVICE_NAME_LENGTH 256

#define ACTION_LEN 16

// Hook name will be "<service> (<event>/<action>)"
#define HOOK_NAME_LENGTH SERVICE_NAME_LENGTH * 2

/*
  Throttle the restart of the service if it stops before this many
  milliseconds have elapsed since startup.  Override in registry.
*/
#define NSSM_RESET_THROTTLE_RESTART 1500

/*
  How many milliseconds to wait for the application to die after sending
  a Control-C event to its console.  Override in registry.
*/
#define NSSM_KILL_CONSOLE_GRACE_PERIOD 1500

/*
  How many milliseconds to wait for the application to die after posting to
  its windows' message queues.  Override in registry.
*/
#define NSSM_KILL_WINDOW_GRACE_PERIOD 1500

/*
  How many milliseconds to wait for the application to die after posting to
  its threads' message queues.  Override in registry.
*/
#define NSSM_KILL_THREADS_GRACE_PERIOD 1500

// How many milliseconds to pause after rotating logs.
#define NSSM_ROTATE_DELAY 0

// Margin of error for service status wait hints in milliseconds.
#define NSSM_WAITHINT_MARGIN 2000

// Methods used to try to stop the application.
#define NSSM_STOP_METHOD_CONSOLE (1 << 0)
#define NSSM_STOP_METHOD_WINDOW (1 << 1)
#define NSSM_STOP_METHOD_THREADS (1 << 2)
#define NSSM_STOP_METHOD_TERMINATE (1 << 3)

// How many milliseconds to wait before updating service status.
#define NSSM_SERVICE_STATUS_DEADLINE 20000

// User-defined service controls can be in the range 128-255.
#define NSSM_SERVICE_CONTROL_START 0
#define NSSM_SERVICE_CONTROL_ROTATE 128

// How many milliseconds to wait for a hook.
#define NSSM_HOOK_DEADLINE 60000

// How many milliseconds to wait for outstanding hooks.
#define NSSM_HOOK_THREAD_DEADLINE 80000

// How many milliseconds to wait for closing logging thread.
#define NSSM_CLEANUP_LOGGERS_DEADLINE 1500

extern const wchar_t g_NSSM[];
extern const wchar_t g_NSSMConfiguration[];
extern const wchar_t g_NSSMVersion[];
extern const wchar_t g_NSSMDate[];
extern const wchar_t g_AffinityAll[];
extern const wchar_t g_NSSMRegistry[];
extern const wchar_t g_NSSMRegistryParameters[];
extern const wchar_t g_NSSMRegistryParameters2[];
extern const wchar_t g_NSSMRegistryGroups[];
extern const wchar_t g_NSSMRegGroups[];
extern const wchar_t g_NSSMRegExe[];
extern const wchar_t g_NSSMRegFlags[];
extern const wchar_t g_NSSMRegDir[];
extern const wchar_t g_NSSMRegEnv[];
extern const wchar_t g_NSSMRegEnvExtra[];
extern const wchar_t g_NSSMRegExit[];
extern const wchar_t g_NSSMRegRestartDelay[];
extern const wchar_t g_NSSMRegThrottle[];
extern const wchar_t g_NSSMRegStopMethodSkip[];
extern const wchar_t g_NSSMRegKillConsoleGracePeriod[];
extern const wchar_t g_NSSMRegKillWindowGracePeriod[];
extern const wchar_t g_NSSMRegKillThreadsGracePeriod[];
extern const wchar_t g_NSSMRegKillProcessTree[];
extern const wchar_t g_NSSMRegStdIn[];
extern const wchar_t g_NSSMRegStdInSharing[];
extern const wchar_t g_NSSMRegStdInDisposition[];
extern const wchar_t g_NSSMRegStdInFlags[];
extern const wchar_t g_NSSMRegStdOut[];
extern const wchar_t g_NSSMRegStdOutSharing[];
extern const wchar_t g_NSSMRegStdOutDisposition[];
extern const wchar_t g_NSSMRegStdOutFlags[];
extern const wchar_t g_NSSMRegStdOutCopyAndTruncate[];
extern const wchar_t g_NSSMRegStdErr[];
extern const wchar_t g_NSSMRegStdErrSharing[];
extern const wchar_t g_NSSMRegStdErrDisposition[];
extern const wchar_t g_NSSMRegStdErrFlags[];
extern const wchar_t g_NSSMRegStdErrCopyAndTruncate[];
extern const wchar_t g_NSSMRegStdIOSharing[];
extern const wchar_t g_NSSMRegStdIODisposition[];
extern const wchar_t g_NSSMRegStdIOFlags[];
extern const wchar_t g_NSSMRegStdIOCopyAndTruncate[];
extern const wchar_t g_NSSMRegHookShareOutputHandles[];
extern const wchar_t g_NSSMRegRotate[];
extern const wchar_t g_NSSMRegRotateOnline[];
extern const wchar_t g_NSSMRegRotateSeconds[];
extern const wchar_t g_NSSMRegRotateBytesLow[];
extern const wchar_t g_NSSMRegRotateBytesHigh[];
extern const wchar_t g_NSSMRegRotateDelay[];
extern const wchar_t g_NSSMRegTimeStampLog[];
extern const wchar_t g_NSSMRegPriority[];
extern const wchar_t g_NSSMRegAffinity[];
extern const wchar_t g_NSSMRegNoConsole[];
extern const wchar_t g_NSSMRegHook[];
extern const wchar_t g_NSSMDefaultString[];

extern const wchar_t g_NSSMHookEventStart[];
extern const wchar_t g_NSSMHookEventStop[];
extern const wchar_t g_NSSMHookEventExit[];
extern const wchar_t g_NSSMHookEventPower[];
extern const wchar_t g_NSSMHookEventRotate[];

extern const wchar_t g_NSSMHookActionPre[];
extern const wchar_t g_NSSMHookActionPost[];
extern const wchar_t g_NSSMHookActionChange[];
extern const wchar_t g_NSSMHookActionResume[];

extern const wchar_t g_NSSMHookEnvVersion[];
extern const wchar_t g_NSSMHookEnvImagePath[];
extern const wchar_t g_NSSMHookEnvNSSMConfiguration[];
extern const wchar_t g_NSSMHookEnvNSSMVersion[];
extern const wchar_t g_NSSMHookEnvBuildDate[];
extern const wchar_t g_NSSMHookEnvPID[];
extern const wchar_t g_NSSMHookEnvDeadline[];
extern const wchar_t g_NSSMHookEnvServiceName[];
extern const wchar_t g_NSSMHookEnvServiceDisplayName[];
extern const wchar_t g_NSSMHookEnvCommandLine[];
extern const wchar_t g_NSSMHookEnvApplicationPID[];
extern const wchar_t g_NSSMHookEnvEvent[];
extern const wchar_t g_NSSMHookEnvAction[];
extern const wchar_t g_NSSMHookEnvTrigger[];
extern const wchar_t g_NSSMHookEnvLastControl[];
extern const wchar_t g_NSSMHookEnvStartRequestedCount[];
extern const wchar_t g_NSSMHookEnvStartCount[];
extern const wchar_t g_NSSMHookEnvThrottleCount[];
extern const wchar_t g_NSSMHookEnvExitCount[];
extern const wchar_t g_NSSMHookEnvExitCode[];
extern const wchar_t g_NSSMHookEnvRuntime[];
extern const wchar_t g_NSSMHookEnvApplicationRuntime[];

extern const wchar_t g_NSSMNativeDependOnGroup[];
extern const wchar_t g_NSSMNativeDependOnService[];
extern const wchar_t g_NSSMNativeDescription[];
extern const wchar_t g_NSSMNativeDisplayName[];
extern const wchar_t g_NSSMNativeEnvironment[];
extern const wchar_t g_NSSMNativeImagePath[];
extern const wchar_t g_NSSMNativeName[];
extern const wchar_t g_NSSMNativeObjectName[];
extern const wchar_t g_NSSMNativeStartup[];
extern const wchar_t g_NSSMNativeType[];

extern const wchar_t g_NSSMLocalSystemAccount[];
extern const wchar_t g_NSSMLocalServiceAccount[];
extern const wchar_t g_NSSMNetworkServiceAccount[];
extern const wchar_t g_NSSMVirtualServiceAccountDomain[];
extern const wchar_t g_NSSMVirtualServiceAccountDomainSlash[];
extern const wchar_t g_NSSMLogonAsServiceRight[];

extern const wchar_t g_NSSMKernelDriver[];
extern const wchar_t g_NSSMFileSystemDriver[];
extern const wchar_t g_NSSMWin32OwnProcess[];
extern const wchar_t g_NSSMWin32ShareProcess[];
extern const wchar_t g_NSSMInteractiveProcess[];
extern const wchar_t g_NSSMShareInteractiveProcess[];
extern const wchar_t g_NSSMUnknown[];

// Exit actions.
#define NSSM_EXIT_RESTART 0
#define NSSM_EXIT_IGNORE 1
#define NSSM_EXIT_REALLY 2
#define NSSM_EXIT_UNCLEAN 3
#define NSSM_NUM_EXIT_ACTIONS 4
extern const wchar_t* g_ExitActionStrings[5];

// Startup types.
#define NSSM_STARTUP_AUTOMATIC 0
#define NSSM_STARTUP_DELAYED 1
#define NSSM_STARTUP_MANUAL 2
#define NSSM_STARTUP_DISABLED 3
extern const wchar_t* g_StartupStrings[5];

// Process priority.
#define NSSM_REALTIME_PRIORITY 0
#define NSSM_HIGH_PRIORITY 1
#define NSSM_ABOVE_NORMAL_PRIORITY 2
#define NSSM_NORMAL_PRIORITY 3
#define NSSM_BELOW_NORMAL_PRIORITY 4
#define NSSM_IDLE_PRIORITY 5
extern const wchar_t* g_PriorityStrings[7];

extern const wchar_t* g_HookEventStrings[6];
extern const wchar_t* g_HookActionStrings[5];

extern int g_bIsAdmin;

#endif
