# Change log

## Changes since 2.27

* Numerious small bugs and HKEY leaks fixed

## Changes since 2.24

* Allow skipping kill_process_tree().

* NSSM can now sleep a configurable amount of time after
    rotating output files.

* NSSM can now rotate log files by calling CopyFile()
    followed by SetEndOfFile(), allowing it to rotate files
    which other processes hold open.

* NSSM now sets the service environment before querying
    parameters from the registry, so paths and arguments
    can reference environment configured in AppEnvironment
    or AppEnvironmentExtra.

## Changes since 2.23

* NSSM once again calls TerminateProcess() correctly.

## Changes since 2.22

* NSSM no longer clutters the event log with "The specified
    procedure could not be found" on legacy Windows releases.

* Fixed failure to set a local username to run the service.

## Changes since 2.21

* Existing services can now be managed using the GUI
    or on the command line.

* NSSM can now set the priority class and processor
    affinity of the managed application.

* NSSM can now apply an unconditional delay before
    restarting the application.

* NSSM can now optionally rotate existing files when
    redirecting I/O.

* Unqualified path names are now relative to the
    application startup directory when redirecting I/O.

* NSSM can now set the service display name, description,
    startup type and log on details.

* All services now receive a standard console window,
    allowing them to read input correctly (if running in
    interactive mode).

## Changes since 2.20

* Services installed from the GUI no longer have incorrect
    AppParameters set in the registry.

## Changes since 2.19

* Services installed from the commandline without using the
    GUI no longer have incorrect AppStopMethod* registry
    entries set.

## Changes since 2.18

* Support AppEnvironmentExtra to append to the environment
    instead of replacing it.

* The GUI is significantly less sucky.

## Changes since 2.17

* Timeouts for each shutdown method can be configured in
    the registry.

* The GUI is slightly less sucky.

## Changes since 2.16

* NSSM can now redirect the service's I/O streams to any path
    capable of being opened by CreateFile().

* Allow building on Visual Studio Express.

* Silently ignore INTERROGATE control.

* Try to send Control-C events to console applications when
    shutting them down.

## Changes since 2.15

* Fixed case where NSSM could kill unrelated processes when
    shutting down.

## Changes since 2.14

* NSSM is now translated into Italian.

* Fixed GUI not allowing paths longer than 256 characters.

## Changes since 2.13

* Fixed default GUI language being French not English.

## Changes since 2.12

* Fixed failure to run on Windows 2000.

## Changes since 2.11

* NSSM is now translated into French.

* Really ensure systems recovery actions can happen.

    The change supposedly introduced in v2.4 to allow service recovery
    actions to be activated when the application exits gracefully with
    a non-zero error code didn't actually work.

## Changes since 2.10

* Support AppEnvironment for compatibility with srvany.

## Changes since 2.9

* Fixed failure to compile messages.mc in paths containing spaces.

* Fixed edge case with CreateProcess().

    Correctly handle the case where the application executable is under
    a path which contains space and an executable sharing the initial
    part of that path (up to a space) exists.

## Changes since 2.8

* Fixed failure to run on Windows versions prior to Vista.

## Changes since 2.7

* Read Application, AppDirectory and AppParameters before each restart so
    a change to any one doesn't require restarting NSSM itself.

* Fixed messages not being sent to the event log correctly in some
    cases.

* Try to handle (strictly incorrect) quotes in AppDirectory.

    Windows directories aren't allowed to contain quotes so CreateProcess()
    will fail if the AppDirectory is quoted.  Note that it succeeds even if
    Application itself is quoted as the application plus parameters are
    interpreted as a command line.

* Fixed failed to write full arguments to AppParameters when
    installing a service.

* Throttle restarts.

    Back off from restarting the application immediately if it starts
    successfully but exits too soon.  The default value of "too soon" is
    1500 milliseconds.  This can be configured by adding a DWORD value
    AppThrottle to the registry.

    Handle resume messages from the service console to restart the
    application immediately even if it is throttled.

* Try to kill the process tree gracefully.

    Before calling TerminateProcess() on all processes assocatiated with
    the monitored application, enumerate all windows and threads and
    post appropriate messages to them.  If the application bothers to
    listen for such messages it has a chance to shut itself down gracefully.

## Changes since 2.6

* Handle missing registry values.

    Warn if AppParameters is missing.  Warn if AppDirectory is missing or
    unset and choose a fallback directory.
    First try to find the parent directory of the application.  If that
    fails, eg because the application path is just "notepad" or something,
    start in the Windows directory.

* Kill process tree when stopping service.

    Ensure that all child processes of the monitored application are
    killed when the service stops by recursing through all running
    processes and terminating those whose parent is the application
    or one of its descendents.

## Changes since 2.5

* Removed incorrect ExpandEnvironmentStrings() error.

    A log_event() call was inadvertently left in the code causing an error
    to be set to the eventlog saying that ExpandEnvironmentStrings() had
    failed when it had actually succeeded.

## Changes since 2.4

* Allow use of REG_EXPAND_SZ values in the registry.

* Don't suicide on exit status 0 by default.

    Suiciding when the application exits 0 will cause recovery actions to be
    taken.  Usually this is inappropriate.  Only suicide if there is an
    explicit AppExit value for 0 in the registry.

    Technically such behaviour could be abused to do something like run a
    script after successful completion of a service but in most cases a
    suicide is undesirable when no actual failure occurred.

* Don't hang if startup parameters couldn't be determined.
    Instead, signal that the service entered the STOPPED state.
    Set START_PENDING state prior to actual startup.

## Changes since 2.3

* Ensure systems recovery actions can happen.

    In Windows versions earlier than Vista the service manager would only
    consider a service failed (and hence eligible for recovery action) if
    the service exited without setting its state to SERVICE_STOPPED, even if
    it signalled an error exit code.
    In Vista and later the service manager can be configured to treat a
    graceful shutdown with error code as a failure but this is not the
    default behaviour.

    Try to configure the service manager to use the new behaviour when
    starting the service so users who set AppExit to Exit can use recovery
    actions as expected.

    Also recognise the new AppExit option Suicide for use on pre-Vista
    systems.  When AppExit is Suicide don't stop the service but exit
    inelegantly, which should be seen as a failure.

## Changes since 2.2

* Send properly formatted messages to the event log.

* Fixed truncation of very long path lengths in the registry.

## Changes since 2.1

* Decide how to handle application exit.

    When the service exits with exit code n look in
    HKLM\SYSTEM\CurrentControlSet\Services\<service>\Parameters\AppExit\<n>,
    falling back to the unnamed value if no such code is listed.  Parse the
    (string) value of this entry as follows:

        Restart: Start the application again (NSSM default).
        Ignore:  Do nothing (srvany default).
        Exit:    Stop the service.

## Changes since 2.0

* Added support for building a 64-bit executable.

* Added project files for newer versions of Visual Studio.
