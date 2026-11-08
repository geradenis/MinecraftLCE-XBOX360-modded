#pragma once

// Crash diagnostics for the Windows64 build.
//
// Install() sets an unhandled-exception filter that writes:
//   - H:\Temp\<app>_crash.log   (exception + symbolized stack)
//   - H:\Temp\crashes\<app>_<pid>_<time>.dmp (minidump)
// and redirects CRT asserts to the log instead of a modal dialog.
// LogDebug() is called from app.DebugPrintf so the log contains the last
// game/server events before the crash.
class ServerCrashHandler
{
public:
	static void Install(const char* appName);
	static void LogDebug(const char* message);
	static void Log(const char* fmt, ...);
};
