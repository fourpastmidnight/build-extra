#define DEBUG
#define STRICT
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define BUFSIZE 65536

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

/// <summary>
///     Adds a string to a buffer representing a string table.
/// </summary>
/// <param name="strResource">
///     The string to add to the string table buffer.
/// </param>
/// <param name="cchResource">
///     The count of characters in <paramref name="strResource" />.
/// </param>
/// <param name="buffer">
///     The resource buffer representing the string table into which to add
///     <paramref name="strResource" />.
/// </param>
/// <param name="index">
///     The index in <paramref name="buffer" /> at which to insert <paramref name="strResource" />.
/// </param>
/// <returns>
///     The next <c>size_t</c> available index position where additional string
///     table string resources can be placed in <paramref name="buffer"/>.
/// </returns>
/// <remarks>
///     This method assumes that there is enough room left in the buffer to accommodate
///     <paramref name="strResource" /> and that a proper index has been specified.
/// </remars>
static size_t AddStringToStringTableBuffer(LPWSTR strResource, size_t cchResource, LPWSTR* buffer, size_t index)
{
	*buffer[index++] = (WCHAR) cchResource;
	if (cchResource > 0) {
		memcpy(*buffer + index, strResource, (cchResource * sizeof(WCHAR)));
		index += cchResource;
	}

	return index;
}

static BOOL UpdateStringTable(HANDLE hModule, int stringTable, LPVOID lpData, size_t cchData)
{
	return UpdateResource(hModule, RT_STRING, MAKEINTRESOURCE(stringTable),
				MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
				lpData, cchData * sizeof(WCHAR));
}

/// <summary>
///     Gets the current active terminal host
/// </summary>
/// <param name="hModule">
///     A handle to git-bash.exe
/// </param>
/// <param name="terminalHost">
///     A pointer to a buffer that will be populated with the currently active
///     terminal host upon successful execution of this function.
/// </param>
/// <returns>
///     Returns <c>ERROR_SUCCESS</c> (<c>0<=c>) if this function completes successfully,
///     otherwise, the error code. Use FormatMessage to get a description of
///     the returned error code.
/// </returns>
/// <remarks>
///     If git-bash.exe has never been modified by this program, then
///     string table 2 will contain no string entries. This means that MinTTY
///     is the currently active terminal host.
///
///     Otherwise, string entry 1 of string table 2 will contain the current
///     active terminal host for git-bash.exe.
/// </remarks>
static DWORD GetCurrentTerminalHost(HMODULE hModule, LPWSTR* terminalHost)
{
	LPWSTR curTermHost = calloc(BUFSIZE, 1);
	size_t cchCurTermHost = LoadString(hModule, 16, curTermHost, BUFSIZE);
	DWORD result = GetLastError();

	if (result == ERROR_SUCCESS && (wcsicmp(curTermHost, L"ConHost") == 0))
	{
		*terminalHost = L"ConHost";
	}
	else if ((cchCurTermHost > 0 && wcsicmp(curTermHost, L"MinTTY") == 0) || result == ERROR_RESOURCE_NAME_NOT_FOUND)
	{
		*terminalHost = L"MinTTY";
		result = ERROR_SUCCESS;
	}

	free(curTermHost);

	return result;
}

static DWORD SetActiveTerminalHost(LPWSTR pathToGitBash, LPWSTR terminalHost)
{
	HANDLE handle;
	HMODULE hGitBash;
	int minTTYCmdLineId = -1, conHostCmdLineId = -1, result = 0, cchMinTTYCmdLine, cchConHostCmdLine;
	size_t szResBuffer;
	LPWSTR curTermHost, minTTYCmdLine, conHostCmdLine, resBuffer;

	// Load git-bash.exe into memory so we can read it's existing resources.
	if (!(hGitBash = LoadLibrary(pathToGitBash)))
		return 1;

	if (GetCurrentTerminalHost(hGitBash, &curTermHost) != ERROR_SUCCESS)
	{
		FreeLibrary(hGitBash);
		return 1;
	}

	// If the requested terminal host is not the current terminal host,
	// continue, otherwise, we're done.'
	if (wcsicmp(terminalHost, curTermHost) != 0)
	{
		// Set the string entry IDs for the treminal host command lines based
		// on the currently active terminal host.
		minTTYCmdLineId = wcsicmp(curTermHost, L"MinTTY") == 0 ? 0 : 1;
		conHostCmdLineId = (1 + minTTYCmdLineId) % 2;

		// Get the command lines from string table 1
		minTTYCmdLine = calloc(BUFSIZE, 1);
		cchMinTTYCmdLine = LoadString(hGitBash, minTTYCmdLineId, minTTYCmdLine, BUFSIZE);

		conHostCmdLine = calloc(BUFSIZE, 1);
		cchConHostCmdLine = LoadString(hGitBash, conHostCmdLineId, conHostCmdLine, BUFSIZE);

		// Each string table stores 16 strings. String table strings are basically
		// BSTR strings, that is, the string is not NULL terminated, but is preceeded
		// by a character indicating the string's character length.
		// String tables must be updated in whole (you can't update individual string
		// entries). Therefore, the total amount of memory to be allocated is the total
		// number of characters to be stored, plus 16 "characters" (which would each
		// contain the length of a string table entry, in characters, to be stored in the
		// string table, or 0 if the string table entry is to be skipped).
		szResBuffer = cchMinTTYCmdLine + cchConHostCmdLine + 16;
		resBuffer = calloc(szResBuffer, sizeof(WCHAR));
		if (!resBuffer) {
			return 1;
		}

		// It's recommended to not modify resources of loaded libraries.
		// So, close git-bash.exe; we're all done reading it's resources.
		if(!FreeLibrary(hGitBash))
			return 3;

		// Swap the string resource Id's so that the second becomes first and the first second.
		minTTYCmdLineId = (minTTYCmdLineId + 1) % 2;
		conHostCmdLineId = (conHostCmdLineId + 1) % 2;

		for (int i = 0, index = 0; i < 2; ++i) {
			index = (i == minTTYCmdLineId) ? AddStringToStringTableBuffer(minTTYCmdLine, cchMinTTYCmdLine, &resBuffer, index)
										   : AddStringToStringTableBuffer(conHostCmdLine, cchConHostCmdLine, &resBuffer, index);
		}

		if (!(handle = BeginUpdateResource(pathToGitBash, FALSE)))
			return 2;

		// Update String Table 1 containing the git-bash.exe command lines
		if (!UpdateStringTable(handle, 1, resBuffer, szResBuffer))
			result = 3;

		if (!result)
		{
			// Store the new active terminal host in string entry 1 of string table 2.
			free(resBuffer);
			int cchNewTerminalHost = wcslen(terminalHost);
			szResBuffer = cchNewTerminalHost + 16;
			resBuffer = calloc(szResBuffer, sizeof(WCHAR));
			if (!resBuffer)
				return 1;

			if (!UpdateStringTable(handle, 2, resBuffer, szResBuffer))
				result = 3;
		}

		if (!EndUpdateResource(handle, FALSE))
			result = 4;
	}

	return result;
}

#ifdef STANDALONE_EXE
int main(int argc, char **argv)
{
	int wargc, result = 0;
	LPWSTR *wargv, newTermHost;

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
		fwprintf(stderr,
			L"\nUsage: %s <path-to-exe> <terminal-host>\n\n  <path-to-exe>\t\tPath to 'git-bash.exe'\n  <terminal-host>\tMinTTY | ConHost (case-insensitive)\n",
			wargv[0]);
		exit(1);
	}

	if (wcsicmp(wargv[2], L"MinTTY") == 0) {
		newTermHost = L"MinTTY";
	}
	else if (wcsicmp(wargv[2], L"ConHost") == 0) {
		newTermHost = L"ConHost";
	}
	else {
		fwprintf(stderr, L"Invalid <terminal-host> specified: '%s'\n<terminal-host> must be one of 'MinTTY' or 'ConHost'.\n", wargv[2]);
		exit(1);
	}

	result = SetActiveTerminalHost(wargv[1], newTermHost);

	if (result)
		fwprintf(stderr, L"Error editing %s: %d\n", wargv[1], result);

	return !!result;
}
#endif
