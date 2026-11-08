#include "stdafx.h"
#include "ServerCrashHandler.h"

#include <dbghelp.h>
#include <crtdbg.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <share.h>

#pragma comment(lib, "dbghelp.lib")

namespace
{
	FILE *g_log = NULL;
	FILE *g_debugLog = NULL;
	CRITICAL_SECTION g_logCS;
	bool g_installed = false;
	char g_appName[32] = "server";
	char g_crashPath[MAX_PATH] = {0};
	char g_debugPath[MAX_PATH] = {0};

	typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
		PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
	typedef BOOL (WINAPI *PFN_SymInitialize)(HANDLE, PCSTR, BOOL);
	typedef BOOL (WINAPI *PFN_SymCleanup)(HANDLE);
	typedef BOOL (WINAPI *PFN_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
	typedef BOOL (WINAPI *PFN_SymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
	typedef PVOID (WINAPI *PFN_SymFunctionTableAccess64)(HANDLE, DWORD64);
	typedef DWORD64 (WINAPI *PFN_SymGetModuleBase64)(HANDLE, DWORD64);
	typedef BOOL (WINAPI *PFN_StackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
		PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
		PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);

	PFN_MiniDumpWriteDump pMiniDumpWriteDump = NULL;
	PFN_SymInitialize pSymInitialize = NULL;
	PFN_SymCleanup pSymCleanup = NULL;
	PFN_SymFromAddr pSymFromAddr = NULL;
	PFN_SymGetLineFromAddr64 pSymGetLineFromAddr64 = NULL;
	PFN_SymFunctionTableAccess64 pSymFunctionTableAccess64 = NULL;
	PFN_SymGetModuleBase64 pSymGetModuleBase64 = NULL;
	PFN_StackWalk64 pStackWalk64 = NULL;

	void EnsureDirectory(const wchar_t *path)
	{
		CreateDirectoryW(path, NULL);
	}

	void BuildPath(char *out, size_t outSize, const char *fileName)
	{
		// Prefer H:\Temp (development machine). On a server without H:, fall
		// back to the executable directory so diagnostics are never lost.
		if (GetFileAttributesW(L"H:\\Temp") != INVALID_FILE_ATTRIBUTES)
		{
			snprintf(out, outSize, "H:\\Temp\\%s", fileName);
			return;
		}
		wchar_t modulePath[MAX_PATH] = {0};
		GetModuleFileNameW(NULL, modulePath, MAX_PATH);
		wchar_t *slash = wcsrchr(modulePath, L'\\');
		if (slash != NULL) *slash = 0;
		char narrow[MAX_PATH] = {0};
		WideCharToMultiByte(CP_ACP, 0, modulePath, -1, narrow, MAX_PATH, NULL, NULL);
		snprintf(out, outSize, "%s\\%s", narrow, fileName);
	}

	void WriteTimestamp(FILE *f)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [tid %lu] ",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			(unsigned long)GetCurrentThreadId());
	}

	std::string ModuleAndOffset(DWORD64 address)
	{
		char result[512] = {0};
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery((LPCVOID)(ULONG_PTR)address, &mbi, sizeof(mbi)) != 0 && mbi.AllocationBase != NULL)
		{
			wchar_t modulePath[MAX_PATH] = {0};
			if (GetModuleFileNameW((HMODULE)mbi.AllocationBase, modulePath, MAX_PATH) != 0)
			{
				const wchar_t *name = wcsrchr(modulePath, L'\\');
				name = (name != NULL) ? name + 1 : modulePath;
				char narrow[MAX_PATH] = {0};
				WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, MAX_PATH, NULL, NULL);
				snprintf(result, sizeof(result), "%s+0x%llX", narrow,
					(unsigned long long)(address - (DWORD64)(ULONG_PTR)mbi.AllocationBase));
				return std::string(result);
			}
		}
		snprintf(result, sizeof(result), "0x%llX", (unsigned long long)address);
		return std::string(result);
	}

	BOOL CALLBACK ReadProcessMemory64(HANDLE hProcess, DWORD64 base, PVOID buffer, DWORD size, LPDWORD bytesRead)
	{
		SIZE_T read = 0;
		BOOL ok = ReadProcessMemory(hProcess, (LPCVOID)(ULONG_PTR)base, buffer, size, &read);
		if (bytesRead != NULL) *bytesRead = (DWORD)read;
		return ok;
	}

	void LoadDbgHelp()
	{
		HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
		if (dbghelp == NULL) return;
		pMiniDumpWriteDump = (PFN_MiniDumpWriteDump)GetProcAddress(dbghelp, "MiniDumpWriteDump");
		pSymInitialize = (PFN_SymInitialize)GetProcAddress(dbghelp, "SymInitialize");
		pSymCleanup = (PFN_SymCleanup)GetProcAddress(dbghelp, "SymCleanup");
		pSymFromAddr = (PFN_SymFromAddr)GetProcAddress(dbghelp, "SymFromAddr");
		pSymGetLineFromAddr64 = (PFN_SymGetLineFromAddr64)GetProcAddress(dbghelp, "SymGetLineFromAddr64");
		pSymFunctionTableAccess64 = (PFN_SymFunctionTableAccess64)GetProcAddress(dbghelp, "SymFunctionTableAccess64");
		pSymGetModuleBase64 = (PFN_SymGetModuleBase64)GetProcAddress(dbghelp, "SymGetModuleBase64");
		pStackWalk64 = (PFN_StackWalk64)GetProcAddress(dbghelp, "StackWalk64");
	}

	void WriteDump(EXCEPTION_POINTERS *exceptionPointers)
	{
		if (pMiniDumpWriteDump == NULL) return;

		EnsureDirectory(L"H:\\Temp\\crashes");
		SYSTEMTIME st;
		GetLocalTime(&st);
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "H:\\Temp\\crashes\\%s_%lu_%04d%02d%02d_%02d%02d%02d.dmp",
			g_appName, (unsigned long)GetCurrentProcessId(),
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

		HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			ServerCrashHandler::Log("MiniDump: CreateFile failed for %s (error %lu)", path, GetLastError());
			return;
		}

		MINIDUMP_EXCEPTION_INFORMATION info;
		info.ThreadId = GetCurrentThreadId();
		info.ExceptionPointers = exceptionPointers;
		info.ClientPointers = FALSE;

		BOOL ok = pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
			(MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithDataSegs),
			&info, NULL, NULL);
		CloseHandle(file);
		ServerCrashHandler::Log("MiniDump: %s (%s)", path, ok ? "ok" : "failed");
	}

	void SymbolizeStack(HANDLE process, HANDLE thread, EXCEPTION_POINTERS *exceptionPointers)
	{
		if (pSymInitialize == NULL || pStackWalk64 == NULL || exceptionPointers == NULL || exceptionPointers->ContextRecord == NULL)
		{
			ServerCrashHandler::Log("Stack: dbghelp unavailable");
			return;
		}

		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		if (!pSymInitialize(process, NULL, TRUE))
		{
			ServerCrashHandler::Log("Stack: SymInitialize failed (error %lu)", GetLastError());
			return;
		}

		CONTEXT *context = exceptionPointers->ContextRecord;
		STACKFRAME64 frame;
		ZeroMemory(&frame, sizeof(frame));
		frame.AddrPC.Offset = context->Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrStack.Offset = context->Rsp;
		frame.AddrStack.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = context->Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;

		for (int i = 0; i < 40; ++i)
		{
			if (!pStackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context,
				ReadProcessMemory64, pSymFunctionTableAccess64, pSymGetModuleBase64, NULL))
			{
				break;
			}
			if (frame.AddrPC.Offset == 0) break;

			char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
			SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbolBuffer;
			ZeroMemory(symbol, sizeof(symbolBuffer));
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;

			DWORD64 displacement = 0;
			bool hasSymbol = pSymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
			const char *name = hasSymbol ? symbol->Name : "<unknown>";
			unsigned long long offset = hasSymbol ? (unsigned long long)displacement : 0;

			IMAGEHLP_LINE64 line;
			ZeroMemory(&line, sizeof(line));
			line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			DWORD lineDisplacement = 0;
			bool hasLine = pSymGetLineFromAddr64 != NULL &&
				pSymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != FALSE;

			if (hasLine)
			{
				ServerCrashHandler::Log("  #%02d %s!%s+0x%llX  (%s:%lu)",
					i, ModuleAndOffset(frame.AddrPC.Offset).c_str(), name, offset,
					line.FileName, line.LineNumber);
			}
			else
			{
				ServerCrashHandler::Log("  #%02d %s!%s+0x%llX",
					i, ModuleAndOffset(frame.AddrPC.Offset).c_str(), name, offset);
			}
		}

		if (pSymCleanup != NULL) pSymCleanup(process);
	}

	LONG WINAPI UnhandledExceptionFilterProc(EXCEPTION_POINTERS *exceptionPointers)
	{
		EXCEPTION_RECORD *record = exceptionPointers != NULL ? exceptionPointers->ExceptionRecord : NULL;
		DWORD code = record != NULL ? record->ExceptionCode : 0;
		DWORD64 address = record != NULL ? (DWORD64)(ULONG_PTR)record->ExceptionAddress : 0;

		ServerCrashHandler::Log("==============================================");
		ServerCrashHandler::Log("CRASH in %s.exe", g_appName);
		ServerCrashHandler::Log("exception=0x%08lX address=%s", code, ModuleAndOffset(address).c_str());
		if (record != NULL)
		{
			for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
			{
				ServerCrashHandler::Log("  param[%lu]=0x%llX", i, (unsigned long long)record->ExceptionInformation[i]);
			}
		}
		ServerCrashHandler::Log("dump follows; stack:");

		WriteDump(exceptionPointers);
		SymbolizeStack(GetCurrentProcess(), GetCurrentThread(), exceptionPointers);
		ServerCrashHandler::Log("==============================================");
		fflush(g_log);

		return EXCEPTION_EXECUTE_HANDLER;
	}

	int __cdecl CrtReportHook(int reportType, char *message, int *returnValue)
	{
		const char *type = "CRT";
		if (reportType == _CRT_ASSERT) type = "ASSERT";
		else if (reportType == _CRT_ERROR) type = "ERROR";
		ServerCrashHandler::Log("[%s] %s", type, message != NULL ? message : "");
		// Suppress the modal dialog. The message is already in our log; keep
		// the server alive if the caller can continue.
		if (returnValue != NULL) *returnValue = 0;
		return TRUE;
	}
}

LONG CALLBACK VectoredBreakpointHandler(PEXCEPTION_POINTERS exceptionPointers)
{
	if (exceptionPointers != NULL && exceptionPointers->ExceptionRecord != NULL &&
		exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
	{
		ServerCrashHandler::Log("BREAKPOINT at %p (Debug assert/debugbreak)",
			exceptionPointers->ExceptionRecord->ExceptionAddress);
		WriteDump(exceptionPointers);
		SymbolizeStack(GetCurrentProcess(), GetCurrentThread(), exceptionPointers);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void ServerCrashHandler::Install(const char* appName)
{
	if (g_installed) return;
	g_installed = true;

	strncpy_s(g_appName, sizeof(g_appName), appName != NULL ? appName : "server", _TRUNCATE);

	InitializeCriticalSection(&g_logCS);

	char crashName[64];
	char debugName[64];
	if (strcmp(g_appName, "client") == 0)
	{
		// Several clients may run on one test machine; keep their logs apart.
		snprintf(crashName, sizeof(crashName), "client_%lu_crash.log", (unsigned long)GetCurrentProcessId());
		snprintf(debugName, sizeof(debugName), "client_%lu_debug.log", (unsigned long)GetCurrentProcessId());
	}
	else
	{
		snprintf(crashName, sizeof(crashName), "%s_crash.log", g_appName);
		snprintf(debugName, sizeof(debugName), "%s_debug.log", g_appName);
	}
	BuildPath(g_crashPath, sizeof(g_crashPath), crashName);
	BuildPath(g_debugPath, sizeof(g_debugPath), debugName);

	g_log = _fsopen(g_crashPath, "w", _SH_DENYNO);
	g_debugLog = _fsopen(g_debugPath, "w", _SH_DENYNO);

	// Resolve dbghelp entry points now, while the process is healthy. Loading
	// a DLL from a crash handler is unreliable if the heap is already damaged.
	LoadDbgHelp();

	Log("MinecraftLCE crash handler installed: app=%s pid=%lu", g_appName, (unsigned long)GetCurrentProcessId());
	Log("crash log: %s", g_crashPath);
	Log("debug log: %s", g_debugPath);

	SetUnhandledExceptionFilter(UnhandledExceptionFilterProc);
	// Debug asserts use int 3 / __debugbreak, which does not go through the
	// unhandled-exception filter. Capture it with a stack and a dump before
	// Windows terminates the process.
	AddVectoredExceptionHandler(1, VectoredBreakpointHandler);

	// CRT asserts/errors go to the log, not to a modal dialog.
	_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CrtReportHook);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}

void ServerCrashHandler::Log(const char* fmt, ...)
{
	if (!g_installed || g_log == NULL || fmt == NULL) return;

	char message[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	EnterCriticalSection(&g_logCS);
	WriteTimestamp(g_log);
	fprintf(g_log, "%s\n", message);
	fflush(g_log);
	LeaveCriticalSection(&g_logCS);
}

void ServerCrashHandler::LogDebug(const char* message)
{
	if (!g_installed || g_debugLog == NULL || message == NULL) return;

	EnterCriticalSection(&g_logCS);
	WriteTimestamp(g_debugLog);
	fputs(message, g_debugLog);
	size_t len = strlen(message);
	if (len == 0 || message[len - 1] != '\n') fputc('\n', g_debugLog);
	fflush(g_debugLog);
	LeaveCriticalSection(&g_logCS);
}
