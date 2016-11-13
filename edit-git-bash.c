//#define DEBUG
#define STRICT
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdlib.h>

#ifdef STANDALONE_EXE
#include <shellapi.h>
#include <stdio.h>
#elif defined(_WIN64)
#error "The .dll needs to be 32-bit to match InnoSetup's architecture"
#endif

#ifdef DEBUG
static void print_error(LPCWSTR prefix, DWORD error_number)
{
	LPWSTR buffer = NULL;
	DWORD count = 0;

	count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
			| FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, error_number, LANG_NEUTRAL,
			(LPTSTR)&buffer, 0, NULL);
	if (count < 1)
		count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
				| FORMAT_MESSAGE_FROM_STRING
				| FORMAT_MESSAGE_ARGUMENT_ARRAY,
				L"Code 0x%1!08x!",
				0, LANG_NEUTRAL, (LPTSTR)&buffer, 0,
				(va_list*)&error_number);
	MessageBox(NULL, buffer, prefix, MB_OK);
	LocalFree((HLOCAL)buffer);
}
#endif

#ifdef STANDALONE_EXE
static
#else
WINAPI __declspec(dllexport)
#endif
int edit_git_bash(LPWSTR git_bash_path, LPWSTR new_command_line)
{
	HANDLE handle;
	int result = 0;
	size_t cchNewCommandLine, alloc;
	WCHAR *buffer;

	// When updating string table resources, you must update the entire string table.
	// Strings in string tables are stored as (LENGTH, STRING) pairs, where STRING is a
	// Unicode string and LENGTH is a WCHAR which contains the count of chracters in STRING.
	//
	// Each string table contains 16 strings.
	//
	// To skip string entries, simply provide an empty entry (e.g. "", or 0x0000).
	//
	// Therefore, count up the total number of characters of the strings in the string table
	// and add 16 (one WCHAR for each string in the table for recording the size of the strings)
	// This is the size of WCHARs you must allocate for your buffer.
	cchNewCommandLine = wcslen(new_command_line);
	alloc = cchNewCommandLine + 16;
	buffer = calloc(alloc, sizeof(WCHAR));

	if (!buffer)
		return 1;

	buffer[0] = (WCHAR) cchNewCommandLine;
	memcpy(buffer + 1, new_command_line, (cchNewCommandLine * sizeof(WCHAR)));

	if (!(handle = BeginUpdateResource(git_bash_path, FALSE)))
		return 2;

	if (!UpdateResource(handle, RT_STRING, MAKEINTRESOURCE(1),
			MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			buffer, alloc * sizeof(WCHAR)))
		result = 3;

	if (!EndUpdateResource(handle, FALSE))
		return 4;

	return result;
}

#ifdef STANDALONE_EXE
int main(int argc, char **argv)
{
	int wargc, result;
	LPWSTR *wargv;
	LPWSTR new_command_line;

#ifdef DEBUG
	LPTSTR cmdLine = GetCommandLineW();
	fwprintf(stderr, L"Command Line: %s\n", cmdLine);

	wargv = CommandLineToArgvW(cmdLine, &wargc);

	for (int i = 1; i < wargc; ++i)
		fwprintf(stderr, L"Arg %d: %s\n", i, wargv[i]);
#else
	wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
#endif

	if (wargc != 3) {
		fwprintf(stderr, L"\nUsage: %s <path-to-exe> <terminal-host>\n\n  <path-to-exe>\t\tPath to 'git-bash.exe'\n  <terminal-host>\tMinTTY | ConHost (case-insensitive)\n",
			wargv[0]);
		exit(1);
	}

	// Default to MinTTY.
	new_command_line = L"APP_ID=GitForWindows.Bash ALLOC_CONSOLE=1 usr\\bin\\mintty.exe -o AppID=GitForWindows.Bash -o AppLaunchCmd=\"@@EXEPATH@@\\git-bash.exe\" -o AppName=\"Git Bash\" -i \"@@EXEPATH@@\\git-bash.exe\" --stare-taskbar-properties -- /usr/bin/bash --login -i";
	if (wcsicmp(wargv[2], L"ConHost") == 0) {
		new_command_line = L"SHOW_CONSOLE=1 APPEND_QUOTE=1 @@COMSPEC@@ /S /C \"\"@@EXEPATH@@\\usr\\bin\\bash.exe\" --login -i";
	}

	if (wcsicmp(wargv[2], L"MinTTY") != 0) {
		fwprintf(stderr, L"Invalid Git Bash terminal host selection: '%s'.\nValid terminal hosts are 'MinTTY' or 'ConHost'.\n", wargv[2]);
		exit(1);
	}

	result = edit_git_bash(wargv[1], new_command_line);

	if (result)
		fwprintf(stderr, L"Error editing %s: %d\n", wargv[1], result);

	return !!result;
}
#endif
