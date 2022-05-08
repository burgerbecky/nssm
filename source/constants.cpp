/***************************************

	String constants

***************************************/

#include "constants.h"

#ifndef NULL
#define NULL 0
#endif

#ifdef _WIN64
#define NSSM_ARCHITECTURE L"64-bit"
#else
#define NSSM_ARCHITECTURE L"32-bit"
#endif
#ifdef _DEBUG
#define NSSM_DEBUG L" debug"
#else
#define NSSM_DEBUG L""
#endif

// String constants

const wchar_t g_NSSM[] = L"NSSM";

const wchar_t g_NSSMConfiguration[] = NSSM_ARCHITECTURE NSSM_DEBUG;
const wchar_t g_NSSMVersion[] = L"2.25";
const wchar_t g_NSSMDate[] = L"Tue 05/03/2022";

// Affinity.
const wchar_t g_AffinityAll[] = L"All";

// Registry strings
const wchar_t g_NSSMRegistry[] = L"SYSTEM\\CurrentControlSet\\Services\\%s";
const wchar_t g_NSSMRegistryParameters[] =
	L"SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters";
const wchar_t g_NSSMRegistryParameters2[] =
	L"SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters\\%s";
const wchar_t g_NSSMRegistryGroups[] =
	L"SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder";
const wchar_t g_NSSMRegGroups[] = L"List";
const wchar_t g_NSSMRegExe[] = L"Application";
const wchar_t g_NSSMRegFlags[] = L"AppParameters";
const wchar_t g_NSSMRegDir[] = L"AppDirectory";
const wchar_t g_NSSMRegEnv[] = L"AppEnvironment";
const wchar_t g_NSSMRegEnvExtra[] = L"AppEnvironmentExtra";
const wchar_t g_NSSMRegExit[] = L"AppExit";
const wchar_t g_NSSMRegRestartDelay[] = L"AppRestartDelay";
const wchar_t g_NSSMRegThrottle[] = L"AppThrottle";
const wchar_t g_NSSMRegStopMethodSkip[] = L"AppStopMethodSkip";
const wchar_t g_NSSMRegKillConsoleGracePeriod[] = L"AppStopMethodConsole";
const wchar_t g_NSSMRegKillWindowGracePeriod[] = L"AppStopMethodWindow";
const wchar_t g_NSSMRegKillThreadsGracePeriod[] = L"AppStopMethodThreads";
const wchar_t g_NSSMRegKillProcessTree[] = L"AppKillProcessTree";
const wchar_t g_NSSMRegStdIn[] = L"AppStdin";
const wchar_t g_NSSMRegStdInSharing[] = L"AppStdinShareMode";
const wchar_t g_NSSMRegStdInDisposition[] = L"AppStdinCreationDisposition";
const wchar_t g_NSSMRegStdInFlags[] = L"AppStdinFlagsAndAttributes";
const wchar_t g_NSSMRegStdOut[] = L"AppStdout";
const wchar_t g_NSSMRegStdOutSharing[] = L"AppStdoutShareMode";
const wchar_t g_NSSMRegStdOutDisposition[] = L"AppStdoutCreationDisposition";
const wchar_t g_NSSMRegStdOutFlags[] = L"AppStdoutFlagsAndAttributes";
const wchar_t g_NSSMRegStdOutCopyAndTruncate[] = L"AppStdoutCopyAndTruncate";
const wchar_t g_NSSMRegStdErr[] = L"AppStderr";
const wchar_t g_NSSMRegStdErrSharing[] = L"AppStderrShareMode";
const wchar_t g_NSSMRegStdErrDisposition[] = L"AppStderrCreationDisposition";
const wchar_t g_NSSMRegStdErrFlags[] = L"AppStderrFlagsAndAttributes";
const wchar_t g_NSSMRegStdErrCopyAndTruncate[] = L"AppStderrCopyAndTruncate";
const wchar_t g_NSSMRegStdIOSharing[] = L"ShareMode";
const wchar_t g_NSSMRegStdIODisposition[] = L"CreationDisposition";
const wchar_t g_NSSMRegStdIOFlags[] = L"FlagsAndAttributes";
const wchar_t g_NSSMRegStdIOCopyAndTruncate[] = L"CopyAndTruncate";
const wchar_t g_NSSMRegHookShareOutputHandles[] = L"AppRedirectHook";
const wchar_t g_NSSMRegRotate[] = L"AppRotateFiles";
const wchar_t g_NSSMRegRotateOnline[] = L"AppRotateOnline";
const wchar_t g_NSSMRegRotateSeconds[] = L"AppRotateSeconds";
const wchar_t g_NSSMRegRotateBytesLow[] = L"AppRotateBytes";
const wchar_t g_NSSMRegRotateBytesHigh[] = L"AppRotateBytesHigh";
const wchar_t g_NSSMRegRotateDelay[] = L"AppRotateDelay";
const wchar_t g_NSSMRegTimeStampLog[] = L"AppTimestampLog";
const wchar_t g_NSSMRegPriority[] = L"AppPriority";
const wchar_t g_NSSMRegAffinity[] = L"AppAffinity";
const wchar_t g_NSSMRegNoConsole[] = L"AppNoConsole";
const wchar_t g_NSSMRegHook[] = L"AppEvents";
const wchar_t g_NSSMDefaultString[] = L"Default";

const wchar_t g_NSSMHookEventStart[] = L"Start";
const wchar_t g_NSSMHookEventStop[] = L"Stop";
const wchar_t g_NSSMHookEventExit[] = L"Exit";
const wchar_t g_NSSMHookEventPower[] = L"Power";
const wchar_t g_NSSMHookEventRotate[] = L"Rotate";

const wchar_t g_NSSMHookActionPre[] = L"Pre";
const wchar_t g_NSSMHookActionPost[] = L"Post";
const wchar_t g_NSSMHookActionChange[] = L"Change";
const wchar_t g_NSSMHookActionResume[] = L"Resume";

const wchar_t g_NSSMHookEnvVersion[] = L"NSSM_HOOK_VERSION";
const wchar_t g_NSSMHookEnvImagePath[] = L"NSSM_EXE";
const wchar_t g_NSSMHookEnvNSSMConfiguration[] = L"NSSM_CONFIGURATION";
const wchar_t g_NSSMHookEnvNSSMVersion[] = L"NSSM_VERSION";
const wchar_t g_NSSMHookEnvBuildDate[] = L"NSSM_BUILD_DATE";
const wchar_t g_NSSMHookEnvPID[] = L"NSSM_PID";
const wchar_t g_NSSMHookEnvDeadline[] = L"NSSM_DEADLINE";
const wchar_t g_NSSMHookEnvServiceName[] = L"NSSM_SERVICE_NAME";
const wchar_t g_NSSMHookEnvServiceDisplayName[] = L"NSSM_SERVICE_DISPLAYNAME";
const wchar_t g_NSSMHookEnvCommandLine[] = L"NSSM_COMMAND_LINE";
const wchar_t g_NSSMHookEnvApplicationPID[] = L"NSSM_APPLICATION_PID";
const wchar_t g_NSSMHookEnvEvent[] = L"NSSM_EVENT";
const wchar_t g_NSSMHookEnvAction[] = L"NSSM_ACTION";
const wchar_t g_NSSMHookEnvTrigger[] = L"NSSM_TRIGGER";
const wchar_t g_NSSMHookEnvLastControl[] = L"NSSM_LAST_CONTROL";
const wchar_t g_NSSMHookEnvStartRequestedCount[] =
	L"NSSM_START_REQUESTED_COUNT";
const wchar_t g_NSSMHookEnvStartCount[] = L"NSSM_START_COUNT";
const wchar_t g_NSSMHookEnvThrottleCount[] = L"NSSM_THROTTLE_COUNT";
const wchar_t g_NSSMHookEnvExitCount[] = L"NSSM_EXIT_COUNT";
const wchar_t g_NSSMHookEnvExitCode[] = L"NSSM_EXITCODE";
const wchar_t g_NSSMHookEnvRuntime[] = L"NSSM_RUNTIME";
const wchar_t g_NSSMHookEnvApplicationRuntime[] = L"NSSM_APPLICATION_RUNTIME";

const wchar_t g_NSSMNativeDependOnGroup[] = L"DependOnGroup";
const wchar_t g_NSSMNativeDependOnService[] = L"DependOnService";
const wchar_t g_NSSMNativeDescription[] = L"Description";
const wchar_t g_NSSMNativeDisplayName[] = L"DisplayName";
const wchar_t g_NSSMNativeEnvironment[] = L"Environment";
const wchar_t g_NSSMNativeImagePath[] = L"ImagePath";
const wchar_t g_NSSMNativeName[] = L"Name";
const wchar_t g_NSSMNativeObjectName[] = L"ObjectName";
const wchar_t g_NSSMNativeStartup[] = L"Start";
const wchar_t g_NSSMNativeType[] = L"Type";

const wchar_t g_NSSMKernelDriver[] = L"SERVICE_KERNEL_DRIVER";
const wchar_t g_NSSMFileSystemDriver[] = L"SERVICE_FILE_SYSTEM_DRIVER";
const wchar_t g_NSSMWin32OwnProcess[] = L"SERVICE_WIN32_OWN_PROCESS";
const wchar_t g_NSSMWin32ShareProcess[] = L"SERVICE_WIN32_SHARE_PROCESS";
const wchar_t g_NSSMInteractiveProcess[] = L"SERVICE_INTERACTIVE_PROCESS";
const wchar_t g_NSSMShareInteractiveProcess[] =
	L"SERVICE_WIN32_SHARE_PROCESS|SERVICE_INTERACTIVE_PROCESS";
const wchar_t g_NSSMUnknown[] = L"?";

// Account strings

// Not really an account.  The canonical name is NT Authority\System.
const wchar_t g_NSSMLocalSystemAccount[] = L"LocalSystem";

// Other well-known accounts which can start a service without a password.
const wchar_t g_NSSMLocalServiceAccount[] = L"NT Authority\\LocalService";
const wchar_t g_NSSMNetworkServiceAccount[] = L"NT Authority\\NetworkService";

// Virtual service accounts.
const wchar_t g_NSSMVirtualServiceAccountDomain[] = L"NT Service";
const wchar_t g_NSSMVirtualServiceAccountDomainSlash[] = L"NT Service\\";

// This is explicitly a wide string. */
const wchar_t g_NSSMLogonAsServiceRight[] = L"SeServiceLogonRight";

// String groups
const wchar_t* g_ExitActionStrings[5] = {
	L"Restart", L"Ignore", L"Exit", L"Suicide", NULL};

const wchar_t* g_StartupStrings[5] = {L"SERVICE_AUTO_START",
	L"SERVICE_DELAYED_AUTO_START", L"SERVICE_DEMAND_START", L"SERVICE_DISABLED",
	NULL};

const wchar_t* g_PriorityStrings[7] = {L"REALTIME_PRIORITY_CLASS",
	L"HIGH_PRIORITY_CLASS", L"ABOVE_NORMAL_PRIORITY_CLASS",
	L"NORMAL_PRIORITY_CLASS", L"BELOW_NORMAL_PRIORITY_CLASS",
	L"IDLE_PRIORITY_CLASS", NULL};

const wchar_t* g_HookEventStrings[6] = {g_NSSMHookEventStart,
	g_NSSMHookEventStop, g_NSSMHookEventExit, g_NSSMHookEventPower,
	g_NSSMHookEventRotate, NULL};

const wchar_t* g_HookActionStrings[5] = {g_NSSMHookActionPre,
	g_NSSMHookActionPost, g_NSSMHookActionChange, g_NSSMHookActionResume, NULL};

// Globals
int g_bIsAdmin;
