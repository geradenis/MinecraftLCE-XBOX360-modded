#pragma once

#include <string>
#include <deque>
#include <windows.h>

// Java-style server console for the Windows64 TCP prototype.
//
// The game is a GUI-subsystem application, so --server attaches to the parent
// console (or allocates one) and redirects stdin/stdout. A small input thread
// queues commands; they are executed on the Minecraft server thread by
// MinecraftServer::handleConsoleInputs(), which keeps world access safe.
class ServerConsole
{
public:
	static void Init();
	static bool IsActive() { return s_active; }

	// Log line (adds a newline if missing).
	static void PrintA(const char* fmt, ...);

	// In-place progress line, e.g. "[Generating spawn area] 42%".
	static void Progress(const char* stage, int percent);

	// Queue a command from the console input thread.
	static void QueueCommand(const std::string& line);
	static void QueueCommandW(const std::wstring& line);

	// Called from the server thread once per tick.
	static void ProcessPendingCommands();

	static bool StopRequested() { return s_stopRequested; }

	// Print local IPv4 addresses and the configured server port.
	// includeVirtual=false shows only physical Ethernet/Wi-Fi adapters.
	static void PrintServerAddresses(bool includeVirtual = true);

private:
	static DWORD WINAPI InputThread(LPVOID param);
	static void EnsureCs();
	static void PollCommandFile();
	static void ProcessCommand(const std::wstring& line);
	static void PrintHelp();
	static void PrintPlayerList();
	static std::string WstringToUtf8(const std::wstring& value);

	static bool s_active;
	static volatile bool s_stopRequested;
	static CRITICAL_SECTION s_cs;
	static std::deque<std::wstring> s_commands;
	static int s_lastProgress;
};
