/*
Copyright 2006 - 2022 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "duktape.h"

#if defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#define WIN_FAKE_O_NONBLOCK 128
#endif

#include "microstack/ILibParsers.h"
#include "microstack/ILibProcessPipe.h"
#include "ILibDuktape_Helpers.h"
#include "ILibDuktapeModSearch.h"
#include "ILibDuktape_fs.h"
#include "ILibDuktape_WritableStream.h"
#include "ILibDuktape_ReadableStream.h"
#include "ILibDuktape_EventEmitter.h"
#include "../microstack/ILibRemoteLogging.h"

#ifndef WIN32
#include <dirent.h>
#endif

#ifdef _POSIX
#include <sys/stat.h>
#if !defined(_NOFSWATCHER) && !defined(__APPLE__) && !defined(_FREEBSD)
	#include <sys/inotify.h>
#endif
#endif
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#define KEVENTBLOCKSIZE 16
#endif


#define FS_NextFD			"\xFF_NextFD"
#define FS_FDS				"\xFF_FDS"
#define FS_WRITESTREAM		"\xFF_WriteStream"
#define FS_WRITESTREAM_2FS	"\xFF_WriteStream2FS"
#define FS_READSTREAM		"\xFF_ReadStream"
#define FS_READSTREAM_2FS	"\xFF_ReadStream2FS"
#define FS_READSTREAM_BUFFERSIZE	4096
#define FS_STAT_METHOD_RETVAL		"\xFF_RetVal"
#define FS_WATCHER_DATA_PTR			"\xFF_FSWatcherPtr"
#define FS_WATCHER_2_FS				"\xFF_FSWatcher2FS"
#define FS_PIPEMANAGER_PTR			"\xFF_FSWatcher_PipeMgrPtr"
#define FS_NOTIFY_DISPATCH_PTR		"\xFF_FSWatcher_NotifyDispatchPtr"
#define FS_CHAIN_PTR				"\xFF_FSWatcher_ChainPtr"
#define FS_WATCH_PATH				"\xFF_FSWatcher_Path"
#define FS_EVENT_R_DESCRIPTORS		"\xFF_FSEventReadDescriptors"
#define FS_EVENT_W_DESCRIPTORS		"\xFF_FSEventWriteDescriptors"
#define FS_EVENT_DESCRIPTORS_IO		"\xFF_FSEventDescriptors_IO"
#define FS_WINDOWS_HANDLES			"\xFF_FSWindowsHandles"
#define FS_WINDOWS_DataPTR			"\xFF_FSWindowHandles_DataPTR"
#define FS_WINDOWS_ReadCallback		"\xFF_FSWindowsHandles_ReadCallback"
#define FS_WINDOWS_WriteCallback	"\xFF_FSWindowsHandles_WriteCallback"
#define FS_WINDOWS_UserBuffer		"\xFF_FSWindowsHandles_UserBuffer"
#define FS_WINDOWS_WriteUserBuffer	"\xFF_FSWindowsHandles_WriteUserBuffer"
#define FS_BUFFER_DESCRIPTOR_PENDING "\xFF_FS_BUFFER_DESCRIPTOR_PENDING"

#if defined(_POSIX) && !defined(__APPLE__)
typedef struct ILibDuktape_fs_linuxWatcher
{
	ILibChain_Link chainLink;
	ILibHashtable watchTable;
	int fd;
	void *ctx;
}ILibDuktape_fs_linuxWatcher;
#endif

#ifdef __APPLE__
typedef enum ILibDuktape_fs_descriptorFlags
{
	ILibDuktape_fs_descriptorFlags_ADD = 1,
	ILibDuktape_fs_descriptorFlags_REMOVE = 2
}ILibDuktape_fs_descriptorFlags;
typedef struct ILibDuktape_fs_descriptorInfo
{
	void* descriptor;
	void* user;
	ILibDuktape_fs_descriptorFlags flags;
}ILibDuktape_fs_descriptorInfo;
typedef struct ILibDuktape_fs_appleWatcher
{
	int exit;
	void *chain;
	void *kthread;									// Worker thread running kevent()
	int kq;											// kqueue() object
	sem_t exitWaiter;
	sem_t inputWaiter;								// Semaphore used to dispatch between Microstack Event Loop and kevent loop.
	int unblocker[2];								// pipe used to unblock thread
	ILibDuktape_fs_descriptorInfo **descriptors;
}ILibDuktape_fs_appleWatcher;
#endif

typedef struct ILibDuktape_fs_writeStreamData
{
	duk_context *ctx;					// Duktape Context Object
	ILibDuktape_EventEmitter *emitter;	// Event Emitter
	void *fsObject;						// 'fs' Object
	void *WriteStreamObject;			// stream.Writable
	FILE *fPtr;							// FILE* handle
	int fd;								// descriptor
	int autoClose;
	ILibDuktape_WritableStream *stream;
}ILibDuktape_fs_writeStreamData;

typedef struct ILibDuktape_fs_readStreamData
{
	duk_context *ctx;						// Duktape Context Object
	void *ReadStreamObject;					// stream.readable
	void *fsObject;							// 'fs' object
	ILibDuktape_EventEmitter *emitter;		// Event Emitter
	FILE *fPtr;								// FILE* handle
	int fd;									// descriptor
	int autoClose;
	ILibDuktape_readableStream *stream;
	int bytesRead;							// Number of bytes read
	int bytesLeft;							// Number of bytes left
	int readLoopActive;						// Event Dispatch thread is actively reading
	int unshiftedBytes;						// Number of bytes to mark as unread
	char buffer[FS_READSTREAM_BUFFERSIZE];
}ILibDuktape_fs_readStreamData;

#ifdef WIN32
typedef struct ILibDuktape_WindowsHandle_Data
{
	duk_context *ctx;					// Duktape Context Object
	HANDLE *H;							// File Handle from CreateFileW()

	// read
	void *callback;
	void *userBuffer;					// heap pointer to user provided buffer object
	OVERLAPPED p;						// Read Overlapped Structure
	char *buffer;						// Read Buffer
	size_t bufferSize;

	//write
	void *write_callback;
	void *write_userBuffer;				// heap pointer to user provided write buffer object
	OVERLAPPED write_p;					// overlapped structure for write
	char *write_buffer;					// Write Buffer
	size_t write_bufferSize;
}ILibDuktape_WindowsHandle_Data;
#endif

#ifndef _NOFSWATCHER
typedef struct ILibDuktape_fs_watcherData
{
	duk_context *ctx;							// Duktape Context Object
	void *object;								// fs.Watcher object
	void *parent;								// fs
	ILibDuktape_EventEmitter *emitter;			// Event Emitter
#if defined(WIN32)
	int recursive;								// Indicates whether all subdirectories should be watched, or only the current directory.
	HANDLE h;									// File Handle from CreateFileW() for watched folder
	struct _OVERLAPPED overlapped;				// Overlapped structure for event handling on Windows
	void *chain;
	void *pipeManager;
	char results[4096];
#elif defined(_POSIX)
#ifndef __APPLE__
	ILibDuktape_fs_linuxWatcher* linuxWatcher;	// Linux specific data structure, holding the descriptors
#endif
	union { int i; void *p; } wd;
#endif
}ILibDuktape_fs_watcherData;
#endif

#ifndef WIN32

// Helper method used to manipulate linux paths, so that it's high level behavior matches Windows.
char ILibDuktape_fs_linuxPath[1024];
char* ILibDuktape_fs_fixLinuxPath(char *path)
{
	int start = 0;
	int end = strnlen_s(path, sizeof(ILibDuktape_fs_linuxPath));
	int len = end;
	if (end > (sizeof(ILibDuktape_fs_linuxPath)-1)) { return(NULL); }

	//if (path[0] == '/') { start = 1; }	else { ++len; }
	if (path[end - 1] == '*') { --end; --len; }

	ILibDuktape_fs_linuxPath[0] = '/';
	memcpy_s(ILibDuktape_fs_linuxPath, sizeof(ILibDuktape_fs_linuxPath), path + start, end);
	ILibDuktape_fs_linuxPath[len] = 0;	// Klocwork is being retarded, as it is too stupid to notice the size check at the top of this func
	return(ILibDuktape_fs_linuxPath);
}
#endif

// Helper method to retrive FILE* from supplied integer value
FILE* ILibDuktape_fs_getFilePtr(duk_context *ctx, int fd)
{
	FILE *retVal = NULL;
	char *key = ILibScratchPad;
	sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "%d", fd);

	duk_push_this(ctx);						// [fs]
	duk_get_prop_string(ctx, -1, FS_FDS);	// [fs][fds]   (File Descriptor Table)
	if (duk_has_prop_string(ctx, -1, key))
	{
		duk_get_prop_string(ctx, -1, key);	// [fs][fds][ptr]
		retVal = (FILE*)duk_get_pointer(ctx, -1);
		duk_pop_3(ctx);						// ...
	}
	else
	{
		duk_pop_2(ctx);						// ...
	}
	return retVal;
}

// Closes file descriptor
duk_ret_t ILibDuktape_fs_closeSync(duk_context *ctx)
{
	if (duk_is_object(ctx, 0) && strcmp(Duktape_GetStringPropertyValue(ctx, 0, "_ObjectID", ""), "fs.bufferDescriptor") == 0)
	{
		return(0);
	}
#ifdef WIN32
	if (duk_is_number(ctx, 0))
	{
		void *tmp = (void*)(uintptr_t)duk_require_uint(ctx, 0);
		duk_push_this(ctx);									// [fs]
		duk_get_prop_string(ctx, -1, FS_WINDOWS_HANDLES);	// [fs][table]
		duk_push_pointer(ctx, tmp);							// [fs][table][H]
		duk_get_prop(ctx, -2);								// [fs][table][container]
		if (!duk_is_null_or_undefined(ctx, -1))
		{
			ILibDuktape_WindowsHandle_Data *data = (ILibDuktape_WindowsHandle_Data*)Duktape_GetBufferProperty(ctx, -1, FS_WINDOWS_DataPTR);
			if (data != NULL)
			{
				// Free all the resources associated with this
				CloseHandle(data->H);
				CloseHandle(data->p.hEvent);
				CloseHandle(data->write_p.hEvent);
			}
			duk_pop(ctx);									// [fs][table]
			duk_push_pointer(ctx, tmp);						// [ts][table][H]
			duk_del_prop(ctx, -2);
			return(0);
		}
	}
#endif

	// if fd is < 65535, then it is the actual descriptor
	int fd = duk_require_int(ctx, 0);
	if (fd < 65535)
	{
#ifdef WIN32
		_close(fd);
#else
		close(fd);
#endif
		return(0);
	}

	// fd is mapped, so we have to fetch it
	FILE *f;
	char *key = ILibScratchPad;
	sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "%d", fd);

	duk_push_this(ctx);						// [fs]
	duk_get_prop_string(ctx, -1, FS_FDS);	// [fs][fds]
	if (duk_has_prop_string(ctx, -1, key))
	{
		duk_get_prop_string(ctx, -1, key);	// [fs][fds][ptr]
		f = (FILE*)duk_get_pointer(ctx, -1);
		duk_del_prop_string(ctx, -2, key);
		if (f != NULL)
		{
			fclose(f);
		}
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Invalid FD"));
	}
	return 0;
}

// Helper method to open a file and map the FILE* to an integral descriptor
int ILibDuktape_fs_openSyncEx(duk_context *ctx, char *path, char *flags, char *mode)
{
	int retVal;
	FILE *f;
	char *key = ILibScratchPad;

	duk_push_this(ctx);													// [fs]
	duk_get_prop_string(ctx, -1, FS_NextFD);							// [fs][fd]
	retVal = duk_get_int(ctx, -1) + 1;
	duk_pop(ctx);														// [fs]

	sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "%d", retVal);
#ifdef WIN32
	_wfopen_s(&f, (const wchar_t*)ILibDuktape_String_UTF8ToWide(ctx, path), (const wchar_t*)ILibDuktape_String_UTF8ToWide(ctx, flags));
#else
	f = fopen(path, flags);
#endif
	if (f != NULL)
	{
		// MAP FILE* to FD
		duk_get_prop_string(ctx, -1, FS_FDS);							// [fs][fds]
		duk_push_pointer(ctx, f);										// [fs][fds][ptr]
		duk_put_prop_string(ctx, -2, key);								// [fs][fds]
		duk_pop(ctx);													// [fs]
		duk_push_int(ctx, retVal);										// [fs][nextFD]
		duk_put_prop_string(ctx, -2, FS_NextFD);						// [fs]
		duk_pop(ctx);													// ...            
		return retVal; // Klocwork is being retarded, because f is saved six lines above
	}
	else
	{																	// [fs]
		duk_pop(ctx);													// ...
		return 0;
	}
}

// Opens a file and returns a descriptor
duk_ret_t ILibDuktape_fs_openSync(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
#ifdef WIN32
	char *path = (char*)duk_require_string(ctx, 0);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
#endif
	if (duk_is_string(ctx, 1))
	{
		// If flags are passed in as a string, then the Descriptor returned is mapped from a FILE*
		char *flags = (char*)duk_require_string(ctx, 1);
		int retVal = -1;

		if (nargs < 2) { return(ILibDuktape_Error(ctx, "Too few arguments")); }

		retVal = ILibDuktape_fs_openSyncEx(ctx, path, flags, NULL);
		if (retVal > 0)
		{
			duk_push_int(ctx, retVal);
			return 1;
		}
		else
		{
			return(ILibDuktape_Error(ctx, "fs.openSync(): Error opening '%s'", path));
		}
	}


	// If flags are passed in numerically, then the returned FD is the actual descriptor
	int flags = (int)duk_require_int(ctx, 1);
#ifdef WIN32
	HANDLE fd = NULL;
	DWORD dwDesiredAccess = 0;
	DWORD dwCreationMode = 0;
	ILibDuktape_WindowsHandle_Data *data;
	if (((flags & _O_RDONLY) == _O_RDONLY) || ((flags & _O_RDWR) == _O_RDWR)) { dwDesiredAccess |= GENERIC_READ; }
	if (((flags & _O_WRONLY) == _O_WRONLY) || ((flags & _O_RDWR) == _O_RDWR)) { dwDesiredAccess |= GENERIC_WRITE; }
	if ((dwDesiredAccess & GENERIC_WRITE) == GENERIC_WRITE)
	{
		dwCreationMode = OPEN_ALWAYS;
	}
	else
	{
		dwCreationMode = OPEN_EXISTING;
	}
	fd = CreateFileW((wchar_t*)ILibDuktape_String_UTF8ToWide(ctx, path), dwDesiredAccess, 0, NULL, dwCreationMode, FILE_FLAG_OVERLAPPED, NULL);
	if (fd == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		return(ILibDuktape_Error(ctx, "Open Error: %u", err));
	}

	duk_push_this(ctx);															// [fs]
	duk_get_prop_string(ctx, -1, FS_WINDOWS_HANDLES);							// [fs][table]
	duk_push_pointer(ctx, fd);													// [fs][table][HANDLE]
	duk_push_object(ctx);														// [fs][table][HANDLE][container]
	data = (ILibDuktape_WindowsHandle_Data*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_WindowsHandle_Data));//..][data]
	duk_put_prop_string(ctx, -2, FS_WINDOWS_DataPTR);							// [fs][table][HANDLE][container]
	duk_put_prop(ctx, -3);														// [fs][table]
	duk_pop_2(ctx);																// ...

	data->ctx = ctx;
	data->H = fd;
	data->p.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	data->write_p.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	duk_push_uint(ctx, (duk_uint_t)(uintptr_t)fd);
#else
	int fd = open(path, flags);
	duk_push_int(ctx, fd);
#ifdef _POSIX
	if (fd >= 0 && (flags & O_NONBLOCK) == O_NONBLOCK)
	{
		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, O_NONBLOCK | flags);
	}
#endif
#endif
	return(1);
}

// Synchronously read from a descriptor
duk_ret_t ILibDuktape_fs_readSync(duk_context *ctx)
{
	int narg = (int)duk_get_top(ctx);
	if (narg > 3)
	{
		// Convert to Options Format
		duk_push_this(ctx);								// [fs]
		duk_prepare_method_call(ctx, -1, "readSync");	// [fs][readSync][this]
		duk_dup(ctx, 0); duk_dup(ctx, 1);				// [fs][readSync][this][fd][buffer]
		duk_push_object(ctx);							// [fs][readSync][this][fd][buffer][options]
		duk_dup(ctx, 2); duk_put_prop_string(ctx, -2, "offset");
		duk_dup(ctx, 3); duk_put_prop_string(ctx, -2, "length");
		duk_dup(ctx, 4); duk_put_prop_string(ctx, -2, "position");
		duk_call_method(ctx, 3);
		return(1);
	}

	duk_size_t bufferSize;
	char *buffer = Duktape_GetBuffer(ctx, 1, &bufferSize);
	int offset = narg > 2 ? Duktape_GetIntPropertyValue(ctx, 2, "offset", 0) : 0;
	int length = narg > 2 ? Duktape_GetIntPropertyValue(ctx, 2, "length", (int)bufferSize) : (int)bufferSize;
	int position = narg > 2 ? Duktape_GetIntPropertyValue(ctx, 2, "position", -1) : -1;
	int bytesRead;
	FILE *f = ILibDuktape_fs_getFilePtr(ctx, duk_require_int(ctx, 0));

	if (length > (int)bufferSize) { return(ILibDuktape_Error(ctx, "fs.readSync(): Buffer of size: %llu bytes, but attempting to read %d bytes", (uint64_t)bufferSize, length)); }
	if (f != NULL)
	{
		if (position >= 0) { fseek(f, position, SEEK_SET); }
		bytesRead = (int)fread(buffer + offset, 1, length, f);
		duk_push_int(ctx, bytesRead);
		return 1;
	}

	return(ILibDuktape_Error(ctx, "FS I/O Error"));
}

// Event Callback from DescriptorEvents, when readset is triggered
duk_ret_t ILibDuktape_fs_read_readsetSink(duk_context *ctx)
{
	int fd = duk_require_int(ctx, 0);
	duk_push_this(ctx);										// [DescriptorEvents]
	duk_get_prop_string(ctx, -1, FS_EVENT_DESCRIPTORS_IO);	// [DescriptorEvents][pending]
	duk_array_pop(ctx, -1);									// [DescriptorEvents][pending][options]
	
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBufferPropertyEx(ctx, -1, "buffer", &bufferLen);
	duk_size_t offset = (duk_size_t)Duktape_GetIntPropertyValue(ctx, -1, "offset", 0);
	duk_size_t length = (duk_size_t)Duktape_GetIntPropertyValue(ctx, -1, "length", (int)bufferLen);
#ifdef WIN32
	int bytesRead = _read(fd, buffer + offset, (unsigned int)length);
#else
	int bytesRead = read(fd, buffer + offset, length);
#endif
	int status = bytesRead < 0 ? errno : 0;

	duk_get_prop_string(ctx, -1, "callback");				// [DescriptorEvents][pending][options][callback]
	duk_eval_string(ctx, "require('fs');");						// ..............[pending][options][callback][this]
	duk_push_int(ctx, status);									// ..............[pending][options][callback][this][err/status]
	duk_push_int(ctx, bytesRead);								// ..............[pending][options][callback][this][err/status][bytesRead]
	duk_get_prop_string(ctx, -5, "buffer");						// ..............[pending][options][callback][this][err/status][bytesRead][buffer]
	duk_dup(ctx, -6);											// ..............[pending][options][callback][this][err/status][bytesRead][buffer][options]
	if (duk_pcall_method(ctx, 4) != 0)						// [DescriptorEvents][pending][options][val]
	{
		ILibDuktape_Process_UncaughtExceptionEx(ctx, "fs.read() Callback Error: %s ", duk_safe_to_string(ctx, -1));
	}
	duk_pop_2(ctx);											// [DescriptorEvents][pending]
	if (duk_get_length(ctx, -1) == 0)
	{
		// No more pending read I/O operations, so we can cleanup the DescriptorEvents
		duk_eval_string(ctx, "require('DescriptorEvents');");	// .....[DescriptorEvents]
		duk_get_prop_string(ctx, -1, "removeDescriptor");		// .....[DescriptorEvents][removeDescriptor]
		duk_swap_top(ctx, -2);									// .....[removeDescriptor][this]
		duk_push_int(ctx, fd);									// .....[removeDescriptor][this][fd]
		duk_push_object(ctx);									// .....[removeDescriptor][this][fd][options]
		duk_push_true(ctx); duk_put_prop_string(ctx, -2, "readset");//..[removeDescriptor][this][fd][options]
		duk_pcall_method(ctx, 2); duk_pop(ctx);				// [DescriptorEvents][pending]

		duk_eval_string(ctx, "require('fs');");				// [DescriptorEvents][pending][FS]
		duk_get_prop_string(ctx, -1,FS_EVENT_R_DESCRIPTORS);// [DescriptorEvents][pending][FS][table]
		duk_del_prop_index(ctx, -1, fd);
	}
	return(0);
}

// Event callback from DescriptorEvents when writeset triggers
duk_ret_t ILibDuktape_fs_write_writeset_sink(duk_context *ctx)
{
	int fd = (int)duk_require_int(ctx, 0);
	duk_push_this(ctx);											// [events]
	duk_get_prop_string(ctx, -1, FS_EVENT_DESCRIPTORS_IO);		// [events][pending]
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBufferPropertyEx(ctx, -1, "buffer", &bufferLen);
	int performCleanup = 0;
#ifdef WIN32
	int bytesWritten = _write(fd, buffer, (unsigned int)bufferLen);
#else
	int bytesWritten = write(fd, buffer, bufferLen);
#endif
	if (bytesWritten == bufferLen)
	{
		// Complete
		duk_del_prop_string(ctx, -2, FS_EVENT_DESCRIPTORS_IO);
		duk_get_prop_string(ctx, -1, "callback");				// [events][pending][callback]
		duk_eval_string(ctx, "require('fs');");					// [events][pending][callback][this]
		duk_push_int(ctx, 0);									// [events][pending][callback][this][status]
		duk_get_prop_string(ctx, -5, "original");				// [events][pending][callback][this][status][buffer]
		duk_get_length(ctx, -1);								// [events][pending][callback][this][status][buffer][length]
		duk_swap_top(ctx, -2);									// [events][pending][callback][this][status][bytesWritten][buffer]
		duk_get_prop_string(ctx, -6, "opt");					// [events][pending][callback][this][status][bytesWritten][buffer][options]
		if (duk_pcall_method(ctx, 4) != 0)						// [events][pending][ret]
		{
			ILibDuktape_Process_UncaughtExceptionEx(ctx, "fs.write() Callback Error: %s ", duk_safe_to_string(ctx, -1));
		}
		duk_pop_2(ctx);											// [events]
		if (!duk_has_prop_string(ctx, -1, FS_EVENT_DESCRIPTORS_IO))
		{
			performCleanup = 1;
		}
	}
	else
	{
		if (bytesWritten > 0)										// [events][pending]
		{
			duk_get_prop_string(ctx, -1, "buffer");					// [events][pending][buffer]
			duk_buffer_slice(ctx, -1, bytesWritten, (int)bufferLen - bytesWritten);	// ....][buffer][sliced]
			duk_put_prop_string(ctx, -3, "buffer");					// [events][pending][oldBuffer]
		}
		else
		{
			int e = errno;
			if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR)		// [events][pending]
			{
				// Error occured
				duk_del_prop_string(ctx, -2, FS_EVENT_DESCRIPTORS_IO);
				performCleanup = 1;

				duk_get_prop_string(ctx, -1, "callback");			// [events][pending][callback]
				duk_eval_string(ctx, "require('fs');");				// [events][pending][callback][this]
				duk_push_int(ctx, e);								// [events][pending][callback][this][status]
				duk_get_prop_string(ctx, -5, "original");			// [events][pending][callback][this][status][buffer]
				duk_push_int(ctx, 0);								// [events][pending][callback][this][status][buffer][written]
				duk_swap_top(ctx, -2);								// [events][pending][callback][this][status][bytesWritten][buffer]
				duk_get_prop_string(ctx, -6, "opt");				// [events][pending][callback][this][status][bytesWritten][buffer][options]
				if (duk_pcall_method(ctx, 4) != 0)					// [events][pending][ret]
				{
					ILibDuktape_Process_UncaughtExceptionEx(ctx, "fs.write() Callback Error: %s ", duk_safe_to_string(ctx, -1));
				}
			}
		}
	}
	if (performCleanup != 0)
	{
		// No more pending writes, so we can do some cleanup
		duk_eval_string(ctx, "require('fs');");					// ... [fs]
		duk_get_prop_string(ctx, -1, FS_EVENT_W_DESCRIPTORS);	// ... [fs][table]
		duk_del_prop_index(ctx, -1, fd);						// ... [fs][table]

																// And we can also unhook ourselves
		duk_eval_string(ctx, "require('EventDescriptors');");	// ... [eventdescriptors]
		duk_get_prop_string(ctx, -1, "removeDescriptor");		// ... [eventdescriptors][remove]
		duk_swap_top(ctx, -2);									// ... [remove][this]
		duk_push_int(ctx, fd);									// ... [remove][this][fd]
		duk_push_object(ctx);									// ... [remove][this][fd][options]
		duk_push_true(ctx); duk_put_prop_string(ctx, -2, "writeset");//[remove][this][fd][options]
		duk_call_method(ctx, 2);								// ... [ret]
	}
	return(0);
}
#ifdef WIN32
// Windows Overlapped callback on WRITE
BOOL ILibDuktape_fs_write_WindowsSink(void *chain, HANDLE h, ILibWaitHandle_ErrorStatus status, DWORD bytesWritten, void* user)
{
	ILibDuktape_WindowsHandle_Data *data = (ILibDuktape_WindowsHandle_Data*)user;
	if (ILibMemory_CanaryOK(data) && duk_ctx_is_alive(data->ctx))
	{
		// Advance the position pointer, because Windows won't do it for us
		LARGE_INTEGER i64;
		i64.LowPart = data->write_p.Offset;
		i64.HighPart = data->write_p.OffsetHigh;
		i64.QuadPart += (int64_t)bytesWritten;
		data->write_p.Offset = i64.LowPart;
		data->write_p.OffsetHigh = i64.HighPart;

		duk_context *ctx = data->ctx;
		duk_push_heapptr(data->ctx, data->write_callback);	// [callback]
		duk_eval_string(data->ctx, "require('fs');");		// [callback][this]
		duk_push_int(data->ctx, status);					// [callback][this][err]
		duk_push_uint(data->ctx, bytesWritten);				// [callback][this][err][bytesRead]
		duk_push_heapptr(data->ctx, data->write_userBuffer);// [callback][this][err][bytesRead][buffer]
		data->write_callback = NULL;
		data->write_userBuffer = NULL;
		if (duk_pcall_method(data->ctx, 3) != 0) { ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.write.onCallack(): "); }
		duk_pop(ctx);										// ...
	}
	return(FALSE);
}
#endif

// fs.write()  Writes to a descriptor
duk_ret_t ILibDuktape_fs_write(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 1, &bufferLen);
	int cbx = 2;
	int offset = 0, length = (int)bufferLen;
	int position = -1;

	// If offset and length are specified, use those values
	if (duk_is_number(ctx, 2)) { offset = (int)duk_require_int(ctx, 2); cbx++; }
	if (duk_is_number(ctx, 3)) { length = (int)duk_require_int(ctx, 3); cbx++; }
	if (duk_is_number(ctx, 4))
	{
		position = (int)duk_require_int(ctx, 4);
		cbx++;
	}
	if (!duk_is_function(ctx, cbx)) { return(ILibDuktape_Error(ctx, "Invalid Parameters")); }

#ifdef WIN32
	HANDLE H = (HANDLE)(uintptr_t)duk_require_uint(ctx, 0);
	ILibDuktape_WindowsHandle_Data *data = NULL;

	// Windows handles are mapped, so we need to look it up
	duk_push_this(ctx);											// [fs]
	duk_get_prop_string(ctx, -1, FS_WINDOWS_HANDLES);			// [fs][table]
	duk_push_pointer(ctx, H);									// [fs][table][key]
	duk_get_prop(ctx, -2);										// [fs][table][value]
	if (duk_is_null_or_undefined(ctx, -1)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	data = (ILibDuktape_WindowsHandle_Data*)Duktape_GetBufferProperty(ctx, -1, FS_WINDOWS_DataPTR);
	if (data->write_callback != NULL) { return(ILibDuktape_Error(ctx, "Operation Already in progress")); }
	duk_dup(ctx, cbx);											// [fs][table][value][callback]
	duk_put_prop_string(ctx, -2, FS_WINDOWS_WriteCallback);		// [fs][table][value]
	duk_get_prop_string(ctx, 1, "buffer");						// [fs][table][value][userbuffer]
	data->write_userBuffer = duk_get_heapptr(ctx, -1);
	duk_put_prop_string(ctx, -2, FS_WINDOWS_WriteUserBuffer);	// [fs][table][value]
	data->write_callback = duk_require_heapptr(ctx, cbx);

	if (position >= 0)
	{
		// If position was specified, we need to set the current position in a 64bit manner
		DWORD highorder = 0;
		DWORD loworder = SetFilePointer(data->H, (LONG)position, (LONG*)&highorder, FILE_BEGIN);
		if (loworder == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) { return(ILibDuktape_Error(ctx, "Unable to seek to Position")); }
		data->write_p.Offset = loworder;
		data->write_p.OffsetHigh = highorder;
	}

	data->write_buffer = buffer + offset;
	data->write_bufferSize = length;
	ILibChain_WriteEx2(duk_ctx_chain(ctx), data->H, &(data->write_p), data->write_buffer, (DWORD)data->write_bufferSize, ILibDuktape_fs_write_WindowsSink, data, "fs.write()");
	return(0);

#else
	int e;
	int fd = (int)duk_require_int(ctx, 0); // On non-Windows platforms, the descriptor is directly passed
	if (position >= 0)
	{
		// If position was specified, we need to seek to it
		if (lseek(fd, (off_t)position, SEEK_SET) < 0) { return(ILibDuktape_Error(ctx, "Unable to seek to Position")); }
	}
	int bytesWritten = write(fd, buffer + offset, length);
	if (bytesWritten == length)
	{
		// Completed
		duk_require_function(ctx, cbx);							
		duk_dup(ctx, cbx);										// [func]
		duk_push_this(ctx);										// [func][this]
		duk_push_int(ctx, 0);									// [func][this][ERR]
		duk_push_int(ctx, bytesWritten);						// [func][this][ERR][bytesWritten]
		duk_dup(ctx, 1);										// [func][this][ERR][bytesWritten][buffer]
		duk_dup(ctx, cbx + 1);									// [func][this][ERR][bytesWritten][buffer][options]
		duk_call_method(ctx, 4);
		return(0);
	}
	if (bytesWritten > 0 || (e = errno) == EAGAIN || e == EWOULDBLOCK || e == EINTR)
	{
		// Partial Write
		duk_push_this(ctx);											// [fs]
		duk_get_prop_string(ctx, -1, FS_EVENT_W_DESCRIPTORS);		// [fs][table]
		if (!duk_has_prop_index(ctx, -1, fd))
		{
			//
			// We're going to stick our descriptor in DescriptorEvents, so we can get asyncronously notified
			//
			duk_eval_string(ctx, "require('DescriptorEvents');");	// [fs][table][DescriptorEvents]
			duk_get_prop_string(ctx, -1, "addDescriptor");			// [fs][table][DescriptorEvents][add]
			duk_swap_top(ctx, -2);									// [fs][table][add][this]
			duk_push_int(ctx, fd);									// [fs][table][add][this][fd]
			duk_push_object(ctx);									// [fs][table][add][this][fd][options]
			duk_push_true(ctx); duk_put_prop_string(ctx, -2, "writeset");	// ...[add][this][fd][options]
			if (duk_is_object(ctx, cbx + 1) && duk_has_prop_string(ctx, cbx + 1, "metadata"))
			{
				duk_push_string(ctx, "fs.write(), ");				// [fs][table][add][this][fd][options][str1]
				duk_get_prop_string(ctx, cbx + 1, "metadata");		// [fs][table][add][this][fd][options][str1][str2]
				duk_string_concat(ctx, -2); duk_remove(ctx, -2);	// [fs][table][add][this][fd][options][metadata]
			}
			else
			{
				duk_push_string(ctx, "fs.write()");					// [fs][table][add][this][fd][options][metadata]
			}
			duk_put_prop_string(ctx, -2, "metadata");				// [fs][table][add][this][fd][options]
			duk_call_method(ctx, 2);								// [fs][table][events]
			ILibDuktape_EventEmitter_SetupOn(ctx, duk_get_heapptr(ctx, -1), "writeset");	// [on][this][writeset]
			duk_push_c_function(ctx, ILibDuktape_fs_write_writeset_sink, DUK_VARARGS);		// [on][this][writeset][func]
			duk_call_method(ctx, 2); duk_pop(ctx);					// [fs][table][events]  .............................
			duk_put_prop_index(ctx, -2, fd);						// [fs][table]
		}
		//
		// We're going to update the pending buffer
		//
		duk_get_prop_index(ctx, -1, fd);							// [fs][table][events]
		duk_push_object(ctx);										// [fs][table][events][pending]
		if (duk_is_object(ctx, cbx + 1)) { duk_dup(ctx, cbx + 1); duk_put_prop_string(ctx, -2, "opt"); }
		duk_dup(ctx, 1);											// [fs][table][events][pending][buffer]
		duk_buffer_slice(ctx, -1, offset + length, length - offset);// [fs][table][events][pending][buffer][sliced]
		duk_dup(ctx, -1);											// [fs][table][events][pending][buffer][sliced][dup]
		duk_put_prop_string(ctx, -4, "buffer");						// [fs][table][events][pending][buffer][sliced]
		duk_put_prop_string(ctx, -3, "original");					// [fs][table][events][pending][buffer]
		duk_pop(ctx);												// [fs][table][events][pending]
		duk_dup(ctx, cbx); duk_put_prop_string(ctx, -2, "callback");// [fs][table][events][pending]
		duk_put_prop_string(ctx, -2, FS_EVENT_DESCRIPTORS_IO);		// [fs][table][events]
		return(0);
	}

	// ERROR
	duk_require_function(ctx, cbx);							
	duk_dup(ctx, cbx);										// [func]
	duk_push_this(ctx);										// [func][this]
	duk_push_int(ctx, e);									// [func][this][ERR]
	duk_push_int(ctx, bytesWritten);						// [func][this][ERR][bytesWritten]
	duk_dup(ctx, 1);										// [func][this][ERR][bytesWritten][buffer]
	duk_dup(ctx, cbx + 1);									// [func][this][ERR][bytesWritten][buffer][options]
	duk_call_method(ctx, 4);
#endif
	return(0);
}

#ifdef WIN32
// Overlapped callback for Windows Read
BOOL ILibDuktape_fs_read_WindowsSink(void *chain, HANDLE h, ILibWaitHandle_ErrorStatus status, char *buffer, DWORD bytesRead, void* user)
{
	ILibDuktape_WindowsHandle_Data *data = (ILibDuktape_WindowsHandle_Data*)user;
	if (ILibMemory_CanaryOK(data) && duk_ctx_is_alive(data->ctx))
	{
		// Advance the position pointer, because Windows won't do it for us
		LARGE_INTEGER i64;
		i64.LowPart = data->p.Offset;
		i64.HighPart = data->p.OffsetHigh;
		i64.QuadPart += (int64_t)bytesRead;
		data->p.Offset = i64.LowPart;
		data->p.OffsetHigh = i64.HighPart;

		duk_context *ctx = data->ctx;
		duk_push_heapptr(data->ctx, data->callback);		// [callback]
		duk_eval_string(data->ctx, "require('fs');");		// [callback][this]
		duk_push_int(data->ctx, status);					// [callback][this][err]
		duk_push_uint(data->ctx, bytesRead);				// [callback][this][err][bytesRead]
		duk_push_heapptr(data->ctx, data->userBuffer);		// [callback][this][err][bytesRead][buffer]
		data->callback = NULL;
		data->userBuffer = NULL;
		if (duk_pcall_method(data->ctx, 3) != 0) { ILibDuktape_Process_UncaughtExceptionEx(ctx, "fs.read.onCallack(): "); }
		duk_pop(ctx);										// ...
	}
	return(FALSE);
}
#endif

// Helper method to do a read from a buffer, after unraveling the event loop, to prevent stack overflow
void ILibDuktape_fs_buffer_fd_read(duk_context *ctx, void ** args, int argsLen)
{
	duk_idx_t top = duk_get_top(ctx);

	duk_eval_string(ctx, "require('fs');");							// [fs]
	duk_get_prop_string(ctx, -1, FS_BUFFER_DESCRIPTOR_PENDING);		// [fs][array]
	while (duk_get_length(ctx, -1) > 0)
	{
		duk_array_shift(ctx, -1);									// [fs][array][obj]

		duk_get_prop_string(ctx, -1, "func");						// [fs][array][obj][func]
		duk_eval_string(ctx, "require('fs');");						// [fs][array][obj][func][this]
		duk_get_prop_string(ctx, -3, "err");						// [fs][array][obj][func][this][err]
		duk_get_prop_string(ctx, -4, "bytesRead");					// [fs][array][obj][func][this][err][bytesRead]
		duk_get_prop_string(ctx, -5, "buffer");						// [fs][array][obj][func][this][err][bytesRead][buffer]
		duk_get_prop_string(ctx, -6, "options");					// [fs][array][obj][func][this][err][bytesRead][buffer][options]
		duk_remove(ctx, -7);										// [fs][array][obj][func][this][err][bytesRead][buffer][options]
		if (duk_pcall_method(ctx, 4) != 0)
		{
			ILibDuktape_Process_UncaughtExceptionEx(ctx, "fs.read.bufferFD.callback() ");
		}
		duk_pop(ctx);												// [fs][array][obj]
	}
	duk_set_top(ctx, top);
}

// fs.read()  Perform a read from a descriptor
duk_ret_t ILibDuktape_fs_read(duk_context *ctx)
{
	int top = duk_get_top(ctx);
	if (top > 2 && (!duk_is_function(ctx, 2)))
	{
		// Simplify flow, by converting to an options object
		duk_push_this(ctx);										// [fs]
		duk_get_prop_string(ctx, -1, "read");					// [fs][read]
		duk_swap_top(ctx, -2);									// [read][this]
		duk_dup(ctx, 0);										// [read][this][fd]
		duk_push_object(ctx);									// [read][this][fd][options]
		duk_dup(ctx, 1); duk_put_prop_string(ctx, -2, "buffer");
		duk_dup(ctx, 2); duk_put_prop_string(ctx, -2, "offset");
		duk_dup(ctx, 3); duk_put_prop_string(ctx, -2, "length");
		duk_dup(ctx, 4); duk_put_prop_string(ctx, -2, "position");
		duk_dup(ctx, 5);										// [read][this][fd][options][callback]
		duk_call_method(ctx, 3);
		return(1);
	}
	if (top == 2 && duk_is_function(ctx, 1))
	{
		// Simplify flow, by converting to a default options object
		duk_push_this(ctx);										// [fs]
		duk_get_prop_string(ctx, -1, "read");					// [fs][read]
		duk_swap_top(ctx, -2);									// [read][this]
		duk_dup(ctx, 0);										// [read][this][fd]
		duk_push_object(ctx);									// [read][this][fd][options]
		duk_push_fixed_buffer(ctx, 16384);						// [read][this][fd][options][buffer]
		duk_push_buffer_object(ctx, -1, 0, 16384, DUK_BUFOBJ_NODEJS_BUFFER); 
		duk_remove(ctx, -2);									// [read][this][fd][options][buffer]
		duk_put_prop_string(ctx, -2, "buffer");					// [read][this][fd][options]
		duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, "offset");
		duk_push_int(ctx, 16384); duk_put_prop_string(ctx, -2, "length");
		duk_push_null(ctx); duk_put_prop_string(ctx, -2, "position");
		duk_dup(ctx, 1);										// [read][this][fd][options][callback]
		duk_call_method(ctx, 3);
		return(1);
	}

	// Options object was specified, with an object instead of descriptor
	if (duk_is_object(ctx, 0) && duk_is_object(ctx, 1) && duk_is_function(ctx, 2))
	{
		if (strcmp(Duktape_GetStringPropertyValue(ctx, 0, "_ObjectID", ""), "fs.bufferDescriptor") != 0) { return(ILibDuktape_Error(ctx, "Invalid Parameter")); }
		duk_size_t wrbufLen;
		char *wrbuf = (char*)Duktape_GetBufferPropertyEx(ctx, 1, "buffer", &wrbufLen);
		duk_dup(ctx, 0);												// [bufferDescriptor]
		int dpos = Duktape_GetIntPropertyValue(ctx, -1, "position", 0);
		dpos = Duktape_GetIntPropertyValue(ctx, 1, "position", dpos);
		duk_get_prop_string(ctx, -1, "buffer");							// [bufferDescriptor][buffer]
		int bufferLength = (int)duk_get_length(ctx, -1);
		int length = Duktape_GetIntPropertyValue(ctx, 1, "length", bufferLength);
		int offset = Duktape_GetIntPropertyValue(ctx, 1, "offset", 0);
		int bytesRead = length < (bufferLength - dpos) ? length : (bufferLength - dpos);
		if (bytesRead > ((int)wrbufLen - offset)) { bytesRead = (int)wrbufLen - offset; }
		duk_size_t srbufLen;
		char *srbuf = (char*)Duktape_GetBufferPropertyEx(ctx, 0, "buffer", &srbufLen);
		memcpy_s(wrbuf + offset, bytesRead, srbuf + dpos, bytesRead);

		duk_push_int(ctx, bytesRead + dpos);								// [bufferDescriptor][buffer][position]
		duk_put_prop_string(ctx, -3, "position");							// [bufferDescriptor][buffer]
		duk_push_this(ctx);													// [bufferDescriptor][buffer][fs]
		duk_get_prop_string(ctx, -1, FS_BUFFER_DESCRIPTOR_PENDING);			// [bufferDescriptor][buffer][fs][array]
		duk_push_object(ctx);												// [bufferDescriptor][buffer][fs][array][object]
		duk_dup(ctx, 2); duk_put_prop_string(ctx, -2, "func");
		duk_push_int(ctx, bytesRead); duk_put_prop_string(ctx, -2, "bytesRead");
		duk_get_prop_string(ctx, 1, "buffer"); duk_put_prop_string(ctx, -2, "buffer");
		duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, "err");
		duk_array_push(ctx, -2);											// [bufferDescriptor][buffer][fs][array]
		ILibDuktape_Immediate(ctx, NULL, 0, ILibDuktape_fs_buffer_fd_read);	// Use an immediate, to unravel the callstack, so we don't risk a stack overflow, if this ends up recursing
		return(0);
	}

	// If we get here, we are going to do an actual read
	if (!(duk_is_number(ctx, 0) && duk_is_object(ctx, 1) && duk_is_function(ctx, 2))) { return(ILibDuktape_Error(ctx, "Invalid Parameters")); }
#ifdef WIN32
	HANDLE H = (HANDLE)(uintptr_t)duk_require_uint(ctx, 0);
	ILibDuktape_WindowsHandle_Data *data = NULL;
#else
	int fd = (int)duk_require_int(ctx, 0);
#endif

#ifdef WIN32
	duk_push_this(ctx);										// [fs]
	duk_get_prop_string(ctx, -1, FS_WINDOWS_HANDLES);		// [fs][table]
	duk_push_pointer(ctx, H);								// [fs][table][key]
	duk_get_prop(ctx, -2);									// [fs][table][value]
	if (duk_is_null_or_undefined(ctx, -1)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	data = (ILibDuktape_WindowsHandle_Data*)Duktape_GetBufferProperty(ctx, -1, FS_WINDOWS_DataPTR);
	if (data->callback != NULL) { return(ILibDuktape_Error(ctx, "Operation Already in progress")); }
	duk_dup(ctx, 2);										// [fs][table][value][callback]
	duk_put_prop_string(ctx, -2, FS_WINDOWS_ReadCallback);	// [fs][table][value]
	duk_get_prop_string(ctx, 1, "buffer");					// [fs][table][value][userbuffer]
	data->userBuffer = duk_get_heapptr(ctx, -1);
	duk_put_prop_string(ctx, -2, FS_WINDOWS_UserBuffer);	// [fs][table][value]
	data->callback = duk_require_heapptr(ctx, 2);
#endif

	// First, we'll attempt to read, and see if it completes or is pending
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBufferPropertyEx(ctx, 1, "buffer", &bufferLen);
	duk_size_t offset = (duk_size_t)Duktape_GetIntPropertyValue(ctx, 1, "offset", 0);
	duk_size_t length = (duk_size_t)Duktape_GetIntPropertyValue(ctx, 1, "length", (int)bufferLen);
	int position = Duktape_GetIntPropertyValue(ctx, 1, "position", -1);
	if (position >= 0)
	{
#ifdef WIN32
		DWORD highorder = 0;
		DWORD loworder = SetFilePointer(data->H, (LONG)position, (LONG*)&highorder, FILE_BEGIN);
		if (loworder == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) { return(ILibDuktape_Error(ctx, "Unable to seek to Position")); }
		data->p.Offset = loworder;
		data->p.OffsetHigh = highorder;
#else
		if (lseek(fd, (off_t)position, SEEK_SET) < 0) { return(ILibDuktape_Error(ctx, "Unable to seek to Position")); }
#endif
	}
#ifdef WIN32
	// On Windows, we'll use overlapped I/O
	data->buffer = buffer + offset;
	data->bufferSize = length;
	ILibChain_ReadEx2(duk_ctx_chain(ctx), data->H, &(data->p), data->buffer, (DWORD)data->bufferSize, ILibDuktape_fs_read_WindowsSink, data, "fs.read()");
	return(0);
#else
	int bytesRead = read(fd, buffer + offset, length);
	if (bytesRead >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
	{
		// Completed
		int errStatus = bytesRead >= 0 ? 0 : errno;

		duk_push_this(ctx);												// [fs]
		duk_get_prop_string(ctx, -1, FS_BUFFER_DESCRIPTOR_PENDING);		// [fs][array]
		duk_push_object(ctx);											// [fs][array][object]
		duk_dup(ctx, 2); duk_put_prop_string(ctx, -2, "func");
		duk_push_int(ctx, bytesRead); duk_put_prop_string(ctx, -2, "bytesRead");
		duk_get_prop_string(ctx, 1, "buffer"); duk_put_prop_string(ctx, -2, "buffer");
		duk_push_int(ctx, errStatus); duk_put_prop_string(ctx, -2, "err");
		duk_dup(ctx, 3); duk_put_prop_string(ctx, -2, "options");
		duk_array_push(ctx, -2);										// [fs][array]
		ILibDuktape_Immediate(ctx, NULL, 0, ILibDuktape_fs_buffer_fd_read);
		return(0);
	}

	//
	// Setup the EventDescriptor listener, because the read did not complete
	//
	duk_push_this(ctx);											// [fs]
	duk_get_prop_string(ctx, -1, FS_EVENT_R_DESCRIPTORS);		// [fs][table]
	if (!duk_has_prop_index(ctx, -1, fd))
	{
		duk_eval_string(ctx, "require('DescriptorEvents');");	// [fs][table][DE]
		duk_get_prop_string(ctx, -1, "addDescriptor");			// [fs][table][DE][addDescriptor]
		duk_swap_top(ctx, -2);									// [fs][table][addDescriptor][this]
		duk_push_int(ctx, fd);									// [fs][table][addDescriptor][this][fd]
		duk_push_object(ctx);									// [fs][table][addDescriptor][this][fd][options]
		duk_push_true(ctx); duk_put_prop_string(ctx, -2, "readset");
		if (duk_has_prop_string(ctx, 1, "metadata"))
		{
			duk_push_string(ctx, "fs.read(), ");				// [fs][table][addDescriptor][this][fd][options][str]
			duk_get_prop_string(ctx, 1, "metadata");			// [fs][table][addDescriptor][this][fd][options][str][str2]
			duk_string_concat(ctx, -2); duk_remove(ctx, -2);	// [fs][table][addDescriptor][this][fd][options][metadata]
			duk_put_prop_string(ctx, -2, "metadata");			// [fs][table][addDescriptor][this][fd][options]
		}
		else
		{
			duk_push_string(ctx, "fs.read()"); duk_put_prop_string(ctx, -2, "metadata");
		}
		duk_call_method(ctx, 2);								// [fs][table][descriptorEvent]
		ILibDuktape_EventEmitter_SetupOn(ctx, duk_get_heapptr(ctx, -1), "readset");	// ........[on][this][readset]
		duk_push_c_function(ctx, ILibDuktape_fs_read_readsetSink, DUK_VARARGS);		// ........[on][this][readset][func]
		duk_call_method(ctx, 2); duk_pop(ctx);					// [fs][table][descriptorEvent]
		duk_push_array(ctx); duk_put_prop_string(ctx, -2, FS_EVENT_DESCRIPTORS_IO);
		duk_put_prop_index(ctx, -2, fd);						// [fs][table]
	}
	duk_get_prop_index(ctx, -1, fd);							// [fs][table][desriptorEvent]
	duk_get_prop_string(ctx, -1, FS_EVENT_DESCRIPTORS_IO);		// [fs][table][desriptorEvent][pending]
	duk_dup(ctx, 1);											// [fs][table][desriptorEvent][pending][options]
	duk_dup(ctx, 2); duk_put_prop_string(ctx, -2, "callback");	// [fs][table][desriptorEvent][pending][options]
	duk_array_unshift(ctx, -2);									// [fs][table][desriptorEvent][pending]
	return(0);
#endif
}

// Syncronously write to a descriptor
duk_ret_t ILibDuktape_fs_writeSync(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	duk_size_t length;
	char *buffer = Duktape_GetBuffer(ctx, 1, &length);
	FILE *f;
	int bytesWritten;

	if (nargs > 2) { buffer = buffer + duk_require_int(ctx, 2); }
	if (nargs > 3) { length = (duk_size_t)duk_require_int(ctx, 3); }

	f = ILibDuktape_fs_getFilePtr(ctx, duk_require_int(ctx, 0));
	if (f != NULL)
	{
		if (nargs > 4) { fseek(f, duk_require_int(ctx, 4), SEEK_SET); printf("Write: Seeking to %d\n", duk_require_int(ctx, 4)); }
		bytesWritten = (int)fwrite(buffer, 1, length, f);
		duk_push_int(ctx, bytesWritten);
		return 1;
	}

	return(ILibDuktape_Error(ctx, "FS I/O Error"));
}

// Helper function to call closeSync()
int ILibduktape_fs_CloseFD(duk_context *ctx, void *fs, int fd)
{
	int retVal = 1;
	duk_push_heapptr(ctx, fs);													// [fs]
	duk_get_prop_string(ctx, -1, "closeSync");									// [fs][func]
	duk_swap_top(ctx, -2);														// [func][this]
	duk_push_int(ctx, fd);														// [func][this][fd]
	retVal = duk_pcall_method(ctx, 1);
	duk_pop(ctx);																// ...
	return retVal;
}

// Write handler called by stream.Writable.write()
ILibTransport_DoneState ILibDuktape_fs_writeStream_writeHandler(struct ILibDuktape_WritableStream *stream, char *buffer, int bufferLen, void *user)
{
	ILibDuktape_fs_writeStreamData *data = (ILibDuktape_fs_writeStreamData*)user;
	int bytesWritten = 0;
	ILibTransport_DoneState retVal = ILibTransport_DoneState_ERROR;

	if (data->fPtr != NULL)
	{
		bytesWritten = (int)fwrite(buffer, 1, bufferLen, data->fPtr);
		if (bytesWritten > 0)
		{
			retVal = ILibTransport_DoneState_COMPLETE;
		}
	}
	return retVal;
}

// End handler called by stream.Writable.end()
void ILibDuktape_fs_writeStream_endHandler(struct ILibDuktape_WritableStream *stream, void *user)
{
	ILibDuktape_fs_writeStreamData *data = (ILibDuktape_fs_writeStreamData*)user;
	sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "%d", data->fd);

	// If AutoClose is specified, then when the stream is ended, we will close the descriptor
	if (data->autoClose != 0 && data->fPtr != NULL)
	{
		if (ILibduktape_fs_CloseFD(data->ctx, data->fsObject, data->fd) != 0)
		{
			ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.writeStream.end(): Error closing FD: %d", data->fd);
		}
		data->fd = 0;
		data->fPtr = NULL;
	}


	// Call the 'close' event on the WriteStream
	duk_push_heapptr(data->ctx, data->WriteStreamObject);	// [this]
	duk_get_prop_string(data->ctx, -1, "emit");				// [this][emit]
	duk_swap_top(data->ctx, -2);							// [emit][this]
	duk_push_string(data->ctx, "close");					// [emit][this][close]
	if (duk_pcall_method(data->ctx, 1) != 0) { ILibDuktape_Process_UncaughtException(data->ctx); }
	duk_pop(data->ctx);										// ...
}

// Called by Garbage Collector
duk_ret_t ILibDuktape_fs_writeStream_finalizer(duk_context *ctx)
{
	ILibDuktape_fs_writeStreamData *data;

	duk_get_prop_string(ctx, 0, FS_WRITESTREAM);
	data = (ILibDuktape_fs_writeStreamData*)Duktape_GetBuffer(ctx, -1, NULL);

	// If AutoClose was specified, then the descriptor will be explicitely closed
	if (data->autoClose != 0 && data->fPtr != NULL)
	{
		if (ILibduktape_fs_CloseFD(data->ctx, data->fsObject, data->fd) != 0)
		{
			ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.writeStream._finalizer(): Error closing FD: %d", data->fd);
		}
		
		data->fPtr = NULL;
		data->fd = 0;
	}

	return 0;
}

// fs.createWriteStream()
duk_ret_t ILibDuktape_fs_createWriteStream(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
#ifdef WIN32
	char *path = (char*)duk_require_string(ctx, 0);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
#endif
	char *flags = "w";
	int fd = 0;
	FILE *f;
	ILibDuktape_fs_writeStreamData *data;
	int autoClose = 1;

	if (nargs > 1)
	{
		if (duk_has_prop_string(ctx, 1, "fd"))
		{
			// File Descriptor is set
			duk_get_prop_string(ctx, 1, "fd");
			fd = duk_get_int(ctx, -1);
		}
		if (duk_has_prop_string(ctx, 1, "flags"))
		{
			duk_get_prop_string(ctx, 1, "flags");			// [flags]
			flags = (char*)duk_get_string(ctx, -1);
		}
		if (duk_has_prop_string(ctx, 1, "autoClose"))
		{
			duk_get_prop_string(ctx, 1, "autoClose");
			autoClose = (int)duk_get_boolean(ctx, -1);
		}
	}

	if (fd == 0)
	{
		// If a descriptor is not set, then we'll open the file first
		fd = ILibDuktape_fs_openSyncEx(ctx, path, flags, NULL);
	}
	f = ILibDuktape_fs_getFilePtr(ctx, fd);
	if (f != NULL)
	{
		//
		// Create the stream object
		//
		duk_push_object(ctx);													// [writeStream]
		ILibDuktape_WriteID(ctx, "fs.writeStream");
		duk_push_fixed_buffer(ctx, sizeof(ILibDuktape_fs_writeStreamData));		// [writeStream][buffer]
		data = (ILibDuktape_fs_writeStreamData*)Duktape_GetBuffer(ctx, -1, NULL);
		memset(data, 0, sizeof(ILibDuktape_fs_writeStreamData));
		duk_put_prop_string(ctx, -2, FS_WRITESTREAM);							// [writeStream]
		duk_push_this(ctx);														// [writeStream][fs]
		data->fsObject = duk_get_heapptr(ctx, -1);
		duk_put_prop_string(ctx, -2, FS_WRITESTREAM_2FS);						// [writeStream]
		data->ctx = ctx;
		data->fd = fd;
		data->fPtr = f;
		data->autoClose = autoClose;
		data->WriteStreamObject = duk_get_heapptr(ctx, -1);
		data->emitter = ILibDuktape_EventEmitter_Create(ctx);
		data->stream = ILibDuktape_WritableStream_Init(ctx, ILibDuktape_fs_writeStream_writeHandler, ILibDuktape_fs_writeStream_endHandler, data);

		ILibDuktape_EventEmitter_CreateEventEx(data->emitter, "close");
		ILibDuktape_CreateFinalizer(ctx, ILibDuktape_fs_writeStream_finalizer);
		return 1;
	}
	else
	{
		// Descriptor Error
		return(ILibDuktape_Error(ctx, "FS CreateWriteStream Error"));
	}
}
void ILibDuktape_fs_readStream_Pause(struct ILibDuktape_readableStream *sender, void *user)
{
	UNREFERENCED_PARAMETER(user);
	sender->paused = 1;
}

//
// The stream.resume() contains the main processing loop.
//
void ILibDuktape_fs_readStream_Resume(struct ILibDuktape_readableStream *sender, void *user)
{
	if (!ILibMemory_CanaryOK(user)) { return; }

	ILibDuktape_fs_readStreamData *data = (ILibDuktape_fs_readStreamData*)user;
	int bytesToRead;

	// If this is set, it means this thread is trying to re-enter this processing loop
	if (data->readLoopActive != 0) { return; }
	data->readLoopActive = 1;
	sender->paused = 0;

	if (data->bytesRead == -1) { data->bytesRead = 1; }
	data->unshiftedBytes = 0;
	while (sender->paused == 0 && data->bytesRead > 0 && (data->bytesLeft < 0 || data->bytesLeft > 0))
	{
		// The main read processing loop. we'll read as much as we can until we can't read anymore
		bytesToRead = data->bytesLeft < 0 ? (int)sizeof(data->buffer) : (data->bytesLeft > ((int)sizeof(data->buffer) - data->unshiftedBytes) ? (int)sizeof(data->buffer) - data->unshiftedBytes : data->bytesLeft);
		data->bytesRead = (int)fread(data->buffer + data->unshiftedBytes, 1, bytesToRead, data->fPtr);
		if (data->bytesRead > 0)
		{
			if (data->bytesLeft > 0) { data->bytesLeft -= data->bytesRead; }
			data->bytesRead += data->unshiftedBytes; data->unshiftedBytes = 0;
			do
			{
				int preshift = data->unshiftedBytes == 0 ? data->bytesRead : data->unshiftedBytes;
				ILibDuktape_readableStream_WriteData(sender, data->buffer, data->unshiftedBytes>0 ? data->unshiftedBytes : data->bytesRead);
				if (data->unshiftedBytes > 0 && data->unshiftedBytes != preshift) { memmove(data->buffer, data->buffer + (preshift - data->unshiftedBytes), data->unshiftedBytes); }
			} while (data->unshiftedBytes != 0 && data->unshiftedBytes != data->bytesRead);
			data->unshiftedBytes = 0;
			if (data->bytesLeft == 0) { data->bytesRead = 0; }
		}
	}
	if (sender->paused == 0 && data->bytesRead == 0)
	{
		// We aren't paused, but the read resulted in a graceful end
		ILibDuktape_readableStream_WriteEnd(sender);

		if (data->autoClose != 0 && data->fPtr != NULL)
		{
			if (ILibduktape_fs_CloseFD(data->ctx, data->fsObject, data->fd) != 0)
			{
				ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.readStream._CloseFD(): Error closing FD: %d", data->fd);
			}
			data->fd = 0;
			data->fPtr = NULL;

			if (data->ctx != NULL && data->ReadStreamObject != NULL)
			{
				duk_push_heapptr(data->ctx, data->ReadStreamObject);					// [this]
				duk_get_prop_string(data->ctx, -1, "emit");								// [this][emit]
				duk_swap_top(data->ctx, -2);											// [emit][this]
				duk_push_string(data->ctx, "close");									// [emit][this][close]
				if (duk_pcall_method(data->ctx, 1) != 0) { ILibDuktape_Process_UncaughtException(data->ctx); }
				duk_pop(data->ctx);														// ...
			}
		}
	}
	data->readLoopActive = 0;
}

// Destructor called by Garbage Collector
duk_ret_t ILibDuktape_fs_readStream_finalizer(duk_context *ctx)
{
	ILibDuktape_fs_readStreamData *data;
	duk_get_prop_string(ctx, 0, FS_READSTREAM);
	data = (ILibDuktape_fs_readStreamData*)Duktape_GetBuffer(ctx, -1, NULL);

	if (data->autoClose != 0 && data->fPtr != NULL)
	{
		// If autoclose was specified, we need to close the descriptor
		if (ILibduktape_fs_CloseFD(data->ctx, data->fsObject, data->fd) != 0)
		{
			ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.readStream._finalizer(): Error closing FD: %d", data->fd);
		}
		data->fd = 0;
		data->fPtr = NULL;
	}

	return 0;
}

// If the user can't process all the data, they call unshift() to mark some of the data as unread. The actual logic is handled in the main processing loop in the resume() function
int ILibDuktape_fs_readStream_unshift(struct ILibDuktape_readableStream *sender, int unshiftBytes, void *user)
{
	if (!ILibMemory_CanaryOK(user)) { return(unshiftBytes); }
	ILibDuktape_fs_readStreamData *data = (ILibDuktape_fs_readStreamData*)user;
	data->unshiftedBytes = unshiftBytes;
	return(unshiftBytes);
}

// fs.createReadStream() will create a stream.readable. The main processing loop will start when resume() is called, either directly or indirectly.
duk_ret_t ILibDuktape_fs_createReadStream(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
#ifdef WIN32
	char *path = (char*)duk_require_string(ctx, 0);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
#endif
	char *flags = "r";
	int fd = 0;
	FILE *f;
	ILibDuktape_fs_readStreamData *data;
	int autoClose = 1;
	int start = 0;
	int end = -1;

	// If an options object was specified, fetch some properties
	if (nargs > 1)
	{
		fd = Duktape_GetIntPropertyValue(ctx, 1, "fd", 0);
		flags = Duktape_GetStringPropertyValue(ctx, 1, "flags", "r");
		if (duk_has_prop_string(ctx, 1, "autoClose"))
		{
			duk_get_prop_string(ctx, 1, "autoClose");
			autoClose = (int)duk_get_boolean(ctx, -1);
		}
		start = Duktape_GetIntPropertyValue(ctx, 1, "start", 0);
		end = Duktape_GetIntPropertyValue(ctx, 1, "end", -1);
	}

	if (fd == 0)
	{
		fd = ILibDuktape_fs_openSyncEx(ctx, path, flags, NULL);
	}
	f = ILibDuktape_fs_getFilePtr(ctx, fd);
	if (f == NULL)
	{
		// Could not find a mapped descriptor
		return(ILibDuktape_Error(ctx, "FS CreateReadStream Error"));
	}

	// create the stream object
	duk_push_object(ctx);													// [readStream]
	ILibDuktape_WriteID(ctx, "fs.readStream");
	data = (ILibDuktape_fs_readStreamData*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_fs_readStreamData));
	duk_put_prop_string(ctx, -2, FS_READSTREAM);							// [readStream]
	duk_push_this(ctx);														// [readStream][fs]
	data->fsObject = duk_get_heapptr(ctx, -1);
	duk_put_prop_string(ctx, -2, FS_READSTREAM_2FS);						// [readStream]
	data->ctx = ctx;
	data->emitter = ILibDuktape_EventEmitter_Create(ctx);
	data->fd = fd;
	data->fPtr = f;
	data->autoClose = autoClose;
	data->ReadStreamObject = duk_get_heapptr(ctx, -1);
	data->bytesLeft = end < 0 ? end : (end - start + 1);
	data->bytesRead = -1;
	//data->stream = ILibDuktape_ReadableStream_Init(ctx, ILibDuktape_fs_readStream_Pause, ILibDuktape_fs_readStream_Resume, data);
	data->stream = ILibDuktape_ReadableStream_InitEx(ctx, ILibDuktape_fs_readStream_Pause, ILibDuktape_fs_readStream_Resume, ILibDuktape_fs_readStream_unshift, data);
	data->stream->paused = 1; // The stream starts in the paused state... All the work is done in the resume() function

	//printf("readStream [start: %d, end: %d\n", start, end);

	ILibDuktape_EventEmitter_CreateEventEx(data->emitter, "close");
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_fs_readStream_finalizer);

	if (start != 0)
	{
		// If a starting position was specified, we need to seek to it
		fseek(f, start, SEEK_SET);
	}

	return 1;
}

// Desctructor called by Garbage Collector
duk_ret_t ILibDuktape_fs_Finalizer(duk_context *ctx)
{
	if (duk_has_prop_string(ctx, 0, FS_PIPEMANAGER_PTR) && duk_has_prop_string(ctx, 0, FS_CHAIN_PTR))
	{
		duk_get_prop_string(ctx, 0, FS_PIPEMANAGER_PTR);		// [pipeMgr]
		duk_get_prop_string(ctx, 0, FS_CHAIN_PTR);				// [pipeMgr][chain]
		ILibChain_SafeRemove(duk_get_pointer(ctx, -1), duk_get_pointer(ctx, -2));
	}
	if (duk_has_prop_string(ctx, 0, FS_NOTIFY_DISPATCH_PTR))
	{
#ifdef _POSIX
#if !defined(__APPLE__) && !defined(_FREEBSD)
		void *ptr = Duktape_GetPointerProperty(ctx, 0, FS_NOTIFY_DISPATCH_PTR);
		ILibChain_SafeRemove(duk_ctx_chain(ctx), ptr);
#endif
#ifdef __APPLE__
		duk_get_prop_string(ctx, 0, FS_NOTIFY_DISPATCH_PTR);
		ILibDuktape_fs_appleWatcher *watcher = (ILibDuktape_fs_appleWatcher*)Duktape_GetBuffer(ctx, -1, NULL);
		watcher->exit = 1;
		watcher->descriptors = NULL;
		write(watcher->unblocker[1], " ", 1);
		sem_wait(&(watcher->exitWaiter));

		sem_destroy(&(watcher->exitWaiter));
		sem_destroy(&(watcher->inputWaiter));
		close(watcher->kq);
		close(watcher->unblocker[0]);
		close(watcher->unblocker[1]);
#else
#endif
#endif
	}
	return 0;
}


// Enumerate a folder path
duk_ret_t ILibDuktape_fs_readdirSync(duk_context *ctx)
{
	int i = 0;
#ifdef WIN32
	HANDLE h;
	WIN32_FIND_DATAW data;
	duk_size_t pathLen;

	char *path = (char*)ILibDuktape_String_AsWide(ctx, 0, &pathLen);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
	struct dirent *dir;
	DIR *d;
#endif

	duk_push_array(ctx);								// [retVal]

#ifdef WIN32
	h = FindFirstFileW((LPCWSTR)path, &data);
	if (h == INVALID_HANDLE_VALUE)
	{
		// Check if this was a Junction (Symbolic Link)
		if (((wchar_t*)path)[pathLen - 2] == '*')
		{
			((wchar_t*)path)[pathLen - 2] = 0;
			((wchar_t*)path)[pathLen - 3] = 0;
			HANDLE tmp = CreateFileW((LPCWSTR)path, 0, 0, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
			if (tmp != INVALID_HANDLE_VALUE)
			{
				DWORD tmpSZlen = 3+GetFinalPathNameByHandleW(tmp, NULL, 0, 0);
				wchar_t *tmpSZ = (wchar_t*)ILibMemory_AllocateTemp(Duktape_GetChain(ctx), tmpSZlen * 2);
				tmpSZlen = GetFinalPathNameByHandleW(tmp, tmpSZ, tmpSZlen, 0);
				wcscpy_s(tmpSZ + tmpSZlen, 3, L"\\*");
				CloseHandle(tmp);
				h = FindFirstFileW((LPCWSTR)tmpSZ+4, &data);
			}
		}

	}
	if (h != INVALID_HANDLE_VALUE)
	{
		if (wcscmp(data.cFileName, L".") != 0)
		{
			ILibDuktape_String_PushWideString(ctx, (char*)data.cFileName, 0);	// [retVal][val]
			duk_put_prop_index(ctx, -2, i++);									// [retVal]
		}
		while (FindNextFileW(h, &data))
		{
			if (wcscmp(data.cFileName, L"..") != 0)
			{
				ILibDuktape_String_PushWideString(ctx, (char*)data.cFileName, 0);	// [retVal][val]
				duk_put_prop_index(ctx, -2, i++);									// [retVal]
			}
		}
		FindClose(h);
	}
	
#else
	d = opendir(path);
	if (d != NULL)
	{
		while ((dir = readdir(d)) != NULL)
		{
			if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0)
			{
				duk_push_string(ctx, dir->d_name);
				duk_put_prop_index(ctx, -2, i++);
			}
		}
		closedir(d);
	}
#endif

	return 1;
}

// Helper function to populate IsFile() and IsDirectory()
duk_ret_t ILibDuktape_fs_statSyncEx(duk_context *ctx)
{
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, FS_STAT_METHOD_RETVAL);
	return 1;
}

// Helper function to convert file time to JS time
#ifdef WIN32
char *ILibDuktape_fs_convertTime(SYSTEMTIME *st, char *dest, int destLen)
#else
char *ILibDuktape_fs_convertTime(uint64_t st, char *dest, int destLen)
#endif
{
	int len;
#ifdef WIN32
	struct tm x;
	memset(&x, 0, sizeof(struct tm));

	x.tm_hour = st->wHour;
	x.tm_min = st->wMinute;
	x.tm_sec = st->wSecond;
	x.tm_mday = st->wDay;
	x.tm_mon = st->wMonth - 1;
	x.tm_year = st->wYear - 1900;
	
	len = (int)strftime(dest, destLen, "%Y-%m-%dT%H:%M:%SZ", &x);
#else
	len = (int)strftime(dest, destLen, "%Y-%m-%dT%H:%M:%SZ", localtime((time_t*)&(st)));
#endif
	dest[len] = 0;
	return(dest);
}

// Fetch various attributes from the file system about a file/folder
duk_ret_t ILibDuktape_fs_statSync(duk_context *ctx)
{
#ifdef WIN32	
	char *path = ILibDuktape_String_AsWide(ctx, 0, NULL);
	char data[4096];
	WIN32_FILE_ATTRIBUTE_DATA *attr = (WIN32_FILE_ATTRIBUTE_DATA*)data;
	SYSTEMTIME stime;
	
	if(GetFileAttributesExW((LPCWSTR)path, GetFileExInfoStandard, (void*)data) == 0)
	{
		return(ILibDuktape_Error(ctx, "fs.statSync(): Invalid Path"));
	}

	duk_push_object(ctx);	

	duk_push_number(ctx, (double)((((uint64_t)attr->nFileSizeHigh) << 32) + ((uint64_t)attr->nFileSizeLow)));
	duk_put_prop_string(ctx, -2, "size");

	if (FileTimeToSystemTime(&(attr->ftCreationTime), &stime) != 0)
	{
		duk_push_string(ctx, ILibDuktape_fs_convertTime(&stime, ILibScratchPad, sizeof(ILibScratchPad)));
		duk_put_prop_string(ctx, -2, "ctime");
	}
	if (FileTimeToSystemTime(&(attr->ftLastWriteTime), &stime) != 0)
	{
		duk_push_string(ctx, ILibDuktape_fs_convertTime(&stime, ILibScratchPad, sizeof(ILibScratchPad)));
		duk_put_prop_string(ctx, -2, "mtime");
	}
	if (FileTimeToSystemTime(&(attr->ftLastAccessTime), &stime) != 0)
	{
		duk_push_string(ctx, ILibDuktape_fs_convertTime(&stime, ILibScratchPad, sizeof(ILibScratchPad)));
		duk_put_prop_string(ctx, -2, "atime");
	}
	ILibDuktape_CreateInstanceMethodWithBooleanProperty(ctx, FS_STAT_METHOD_RETVAL, (attr->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY ? 1 : 0, "isDirectory", ILibDuktape_fs_statSyncEx, 0);
	ILibDuktape_CreateInstanceMethodWithBooleanProperty(ctx, FS_STAT_METHOD_RETVAL, (attr->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY ? 0 : 1, "isFile", ILibDuktape_fs_statSyncEx, 0);
	return 1;
#else
	struct stat result;
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
	memset(&result, 0, sizeof(struct stat));
	if (stat(path, &result) != 0) { return(ILibDuktape_Error(ctx, "fs.statSync(): Path Error [%s]", path)); }

	duk_push_object(ctx);
	duk_push_number(ctx, result.st_size);
	duk_put_prop_string(ctx, -2, "size");

	duk_push_string(ctx, ILibDuktape_fs_convertTime(result.st_ctime, ILibScratchPad, sizeof(ILibScratchPad)));
	duk_put_prop_string(ctx, -2, "ctime");

	duk_push_string(ctx, ILibDuktape_fs_convertTime(result.st_mtime, ILibScratchPad, sizeof(ILibScratchPad)));
	duk_put_prop_string(ctx, -2, "mtime");

	duk_push_string(ctx, ILibDuktape_fs_convertTime(result.st_atime, ILibScratchPad, sizeof(ILibScratchPad)));
	duk_put_prop_string(ctx, -2, "atime");

	duk_push_int(ctx, (int)result.st_uid); duk_put_prop_string(ctx, -2, "uid");
	duk_push_int(ctx, (int)result.st_gid); duk_put_prop_string(ctx, -2, "gid");

	duk_push_int(ctx, result.st_mode); 
	ILibDuktape_CreateReadonlyProperty(ctx, "mode");

	ILibDuktape_CreateInstanceMethodWithBooleanProperty(ctx, FS_STAT_METHOD_RETVAL, S_ISDIR(result.st_mode) || S_ISBLK(result.st_mode) ? 1 : 0, "isDirectory", ILibDuktape_fs_statSyncEx, 0);
	ILibDuktape_CreateInstanceMethodWithBooleanProperty(ctx, FS_STAT_METHOD_RETVAL, S_ISREG(result.st_mode) ? 1 : 0, "isFile", ILibDuktape_fs_statSyncEx, 0);


	return 1;
#endif
}


#ifdef WIN32
// Override for toString() for Windows
duk_ret_t ILibDuktape_fs_readDrivesSync_result_toString(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, "name");
	return 1;
}

// Pushes the result object for readDrivesSync() on Windows
int ILibDuktape_fs_readDrivesSync_result_PUSH(duk_context *ctx, char *volumeName)
{
	char driveName[1024];
	int driveNameLen;
	unsigned int driveType;
	uint64_t freeBytes;
	uint64_t totalBytes;
	uint64_t totalFreeBytes;

	if (GetVolumePathNamesForVolumeName(volumeName, driveName, sizeof(driveName), &driveNameLen) && driveName[0] != 0)
	{
		duk_push_object(ctx);								// [obj]
		duk_push_string(ctx, driveName);					// [obj][name]
		duk_put_prop_string(ctx, -2, "name");				// [obj]
		driveType = GetDriveType(driveName);
		if (GetDiskFreeSpaceEx(driveName, (PULARGE_INTEGER)&freeBytes, (PULARGE_INTEGER)&totalBytes, (PULARGE_INTEGER)&totalFreeBytes) != 0)
		{
			duk_push_number(ctx, (duk_double_t)totalBytes);
			duk_put_prop_string(ctx, -2, "size");
			duk_push_number(ctx, (duk_double_t)totalFreeBytes);
			duk_put_prop_string(ctx, -2, "free");
		}
		switch (driveType)
		{
		case 2:
			duk_push_string(ctx, "REMOVABLE");
			break;
		case 3:
			duk_push_string(ctx, "FIXED");
			break;
		case 4:
			duk_push_string(ctx, "REMOTE");
			break;
		case 5:
			duk_push_string(ctx, "CDROM");
			break;
		case 6:
			duk_push_string(ctx, "RAMDISK");
			break;
		default:
			duk_push_string(ctx, "UNKNOWN");
			break;
		}
		duk_put_prop_string(ctx, -2, "type");
		ILibDuktape_CreateInstanceMethod(ctx, "toString", ILibDuktape_fs_readDrivesSync_result_toString, 0);
		return 1;
	}
	return 0;
}
#endif

// Fetches drive information on Windows
duk_ret_t ILibDuktape_fs_readDrivesSync(duk_context *ctx)
{
	duk_push_array(ctx);

#ifdef WIN32
	char volumeName[1024];
	int i = 0;
	HANDLE h = FindFirstVolume(volumeName, sizeof(volumeName));

	if (h == INVALID_HANDLE_VALUE)
	{
		return(ILibDuktape_Error(ctx, "fs.readDrivesSync(): Unknown Error"));
	}
	if (ILibDuktape_fs_readDrivesSync_result_PUSH(ctx, volumeName) != 0) { duk_put_prop_index(ctx, -2, i++); }

	while (FindNextVolume(h, volumeName, sizeof(volumeName)))
	{
		if (ILibDuktape_fs_readDrivesSync_result_PUSH(ctx, volumeName) != 0) { duk_put_prop_index(ctx, -2, i++); }
	}
	FindVolumeClose(h);
#endif

	return 1;
}

#ifndef _NOFSWATCHER
// Closes a file watcher
duk_ret_t ILibDuktape_fs_watcher_close(duk_context *ctx)
{
	ILibDuktape_fs_watcherData *data;

	duk_push_this(ctx);																	// [fsWatcher]
	if (!duk_has_prop_string(ctx, -1, FS_WATCHER_DATA_PTR)) { return(0); }
	duk_get_prop_string(ctx, -1, FS_WATCHER_DATA_PTR);
	data = (ILibDuktape_fs_watcherData*)Duktape_GetBuffer(ctx, -1, NULL);

#if defined(WIN32)
	// On windows, we have to cancel pending I/O on the handle
	int r = CancelIo(data->h);
	ILibChain_RemoveWaitHandle(data->chain, data->overlapped.hEvent);
	CloseHandle(data->h);
	data->h = NULL;
#elif defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
	// On Linux, we have to remove the watcher on the descriptor
	ILibHashtable_Remove(data->linuxWatcher->watchTable, data->wd.p, NULL, 0);
	if (inotify_rm_watch(data->linuxWatcher->fd, data->wd.i) != 0) { ILibRemoteLogging_printf(ILibChainGetLogger(Duktape_GetChain(ctx)), ILibRemoteLogging_Modules_Agent_GuardPost | ILibRemoteLogging_Modules_ConsolePrint, ILibRemoteLogging_Flags_VerbosityLevel_1, "FSWatcher.close(): Error removing wd[%d] from fd[%d]", data->wd.i, data->linuxWatcher->fd); }
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(Duktape_GetChain(ctx)), ILibRemoteLogging_Modules_Agent_GuardPost, ILibRemoteLogging_Flags_VerbosityLevel_1, "FSWatcher.close(): Success removing wd[%d] from fd[%d]", data->wd.i, data->linuxWatcher->fd);
	}
	data->wd.p = NULL;
#elif defined(__APPLE__)
	// On macOS we have to signal to the kevent() loop to exit
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, FS_WATCHER_2_FS);
	duk_get_prop_string(ctx, -1, FS_NOTIFY_DISPATCH_PTR);

	ILibDuktape_fs_appleWatcher *watcher = (ILibDuktape_fs_appleWatcher*)Duktape_GetBuffer(ctx, -1, NULL);
	void **d = ILibMemory_Init(alloca(ILibMemory_Init_Size(2 * sizeof(void*), sizeof(ILibDuktape_fs_descriptorInfo))), 2 * sizeof(void*), sizeof(ILibDuktape_fs_descriptorInfo), ILibMemory_Types_STACK);
	d[0] = ILibMemory_Extra(d);
	d[1] = NULL;
	((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->descriptor = data->wd.p;
	((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->user = data;
	((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->flags = ILibDuktape_fs_descriptorFlags_REMOVE;
	watcher->descriptors = (ILibDuktape_fs_descriptorInfo**)d;
	write(watcher->unblocker[1], " ", 1);
	sem_wait(&(watcher->inputWaiter));

	close(data->wd.i);
#endif
	
	duk_push_this(ctx);									// [fsWatcher]
	duk_del_prop_string(ctx, -1, FS_WATCHER_DATA_PTR);
	return 0;
}
#endif

#ifdef WIN32
// Windows Callback for file watcher
BOOL ILibDuktape_fs_watch_iocompletion(void *chain, HANDLE h, ILibWaitHandle_ErrorStatus errors, void *user)
{
	if (errors != ILibWaitHandle_ErrorStatus_NONE || !ILibMemory_CanaryOK(user)) { return(FALSE); }
	ILibDuktape_fs_watcherData *data = (ILibDuktape_fs_watcherData*)user;
	FILE_NOTIFY_INFORMATION *n = (FILE_NOTIFY_INFORMATION*)data->results;
	char filename[4096];
	int changed = 0, renamed = 0;
	BOOL ret = FALSE;
	
	duk_push_object(data->ctx);										// [detail]

	while (n != NULL)
	{
		ILibWideToUTF8_stupidEx(n->FileName, n->FileNameLength, filename, (int)sizeof(filename));
		switch (n->Action)
		{
			case FILE_ACTION_RENAMED_OLD_NAME:
				duk_push_string(data->ctx, filename);
				duk_put_prop_string(data->ctx, -2, "oldname");
				renamed = 1;
				break;
			case FILE_ACTION_RENAMED_NEW_NAME:
				duk_push_string(data->ctx, filename);
				duk_put_prop_string(data->ctx, -2, "newname");
				renamed = 1;
				break;
			case FILE_ACTION_ADDED:
				duk_push_string(data->ctx, "ADDED");
				duk_put_prop_string(data->ctx, -2, "changeType");
				duk_push_string(data->ctx, filename);
				duk_put_prop_string(data->ctx, -2, "\xFF_FileName");
				changed = 1;
				break;
			case FILE_ACTION_REMOVED:
				duk_push_string(data->ctx, "REMOVED");
				duk_put_prop_string(data->ctx, -2, "changeType");
				duk_push_string(data->ctx, filename);
				duk_put_prop_string(data->ctx, -2, "\xFF_FileName");
				changed = 1;
				break;
			case FILE_ACTION_MODIFIED:
				duk_push_string(data->ctx, "MODIFIED");
				duk_put_prop_string(data->ctx, -2, "changeType");
				duk_push_string(data->ctx, filename);
				duk_put_prop_string(data->ctx, -2, "\xFF_FileName");
				changed = 1;
				break;
		}
		n = (n->NextEntryOffset != 0) ? ((FILE_NOTIFY_INFORMATION*)((char*)n + n->NextEntryOffset)) : NULL;
	}

	// Emit change event
	duk_push_heapptr(data->ctx, data->object);						// [detail][fsWatcher]
	duk_get_prop_string(data->ctx, -1, "emit");						// [detail][fsWatcher][emit]
	duk_swap_top(data->ctx, -2);									// [detail][emit][this]
	duk_push_string(data->ctx, "change");							// [detail][emit][this][change]
	duk_push_string(data->ctx, changed == 0 ? "rename" : "change");	// [detail][emit][this][change][type]
	if (changed == 0)
	{
		duk_get_prop_string(data->ctx, -4, "oldname");				// [detail][emit][this][change][type][fileName]
	}
	else
	{
		duk_get_prop_string(data->ctx, -4, "\xFF_FileName");		// [detail][emit][this][change][type][fileName]
	}
	duk_dup(data->ctx, -6);											// [detail][emit][this][change][type][fileName][detail]
	if (duk_pcall_method(data->ctx, 4) != 0) { ILibDuktape_Process_UncaughtException(data->ctx); }
	duk_pop_2(data->ctx);											// ...

	memset(data->results, 0, sizeof(data->results));
	if (data->h != NULL)
	{
		if (ReadDirectoryChangesW(data->h, data->results, sizeof(data->results), data->recursive, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS, NULL, &(data->overlapped), NULL) == 0)
		{
			duk_push_string(data->ctx, "fs.fsWatcher.change: Could not reset watcher");
			ILibDuktape_Process_UncaughtException(data->ctx);
			duk_pop(data->ctx);
		}
		else
		{
			ret = TRUE;
		}
	}
	return(ret);
}
#endif

#ifndef _NOFSWATCHER
// Called by Garbage Collector
duk_ret_t ILibDuktape_fs_watcher_finalizer(duk_context *ctx)
{
	duk_get_prop_string(ctx, 0, "close");		// [close]
	duk_dup(ctx, 0);							// [close][this]
	duk_call_method(ctx, 0);					// [ret]

	return 0;
}

// This a helper function that is indirectly called by the Microstack Event loop, to query metadata
void ILibDuktape_fs_notifyDispatcher_QueryEx(ILibHashtable sender, void *Key1, char* Key2, int Key2Len, void *Data, void *user)
{
	// We're going to push the metadata into the supplied array
	ILibDuktape_fs_watcherData *data = (ILibDuktape_fs_watcherData*)Data;
	duk_push_heapptr(data->ctx, user);					// [array]
	duk_push_heapptr(data->ctx, data->object);			// [array][watcher]
	duk_get_prop_string(data->ctx, -1, FS_WATCH_PATH);	// [array][watcher][path]
	duk_remove(data->ctx, -2);							// [array][path]
	duk_array_push(data->ctx, -2);						// [array];
	duk_pop(data->ctx);									// ...
}

#if defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
// This function is called by the Microstack Event loop, to query metadata about a descriptor
char* ILibDuktape_fs_notifyDispatcher_Query(void* chain, void *object, int fd, size_t *dataLen)
{
	ILibDuktape_fs_linuxWatcher *data = (ILibDuktape_fs_linuxWatcher*)object;

	if (duk_ctx_is_alive(data->ctx) == 0) 
	{
		// Context isn't valid, so just return generic metadata
		return(" (ILibDuktape_fs_linuxWatcher)"); 
	}

	if (data->fd != fd) { return(((ILibChain_Link*)object)->MetaData); }
	int top = duk_get_top(data->ctx);
	duk_push_array(data->ctx);																				// [array]
	ILibHashtable_Enumerate(data->watchTable, ILibDuktape_fs_notifyDispatcher_QueryEx, duk_get_heapptr(data->ctx, -1));
	duk_array_join(data->ctx, -1, ", ");																	// [array][str]

	duk_size_t tmpLen;
	char *tmp = (char*)duk_get_lstring(data->ctx, -1, &tmpLen);
	char *tmp2 = ILibMemory_AllocateTemp(chain, tmpLen + 12);
	*dataLen = sprintf_s(tmp2, tmpLen + 12, "fs.watch(%s)", tmp); // Populate metadata with what we are watching
	duk_set_top(data->ctx, top);
	return tmp2;
}

// Called by Microstack event loop, to add our descriptors
void ILibDuktape_fs_notifyDispatcher_PreSelect(void* object, fd_set *readset, fd_set *writeset, fd_set *errorset, int* blocktime)
{
	ILibDuktape_fs_linuxWatcher *data = (ILibDuktape_fs_linuxWatcher*)object;
	FD_SET(data->fd, readset);
}

// Called by Microstack event loop, to check our descriptors
void ILibDuktape_fs_notifyDispatcher_PostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	struct inotify_event *evt;
	int i = 0;
	int len;
	char buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
	ILibDuktape_fs_linuxWatcher *data = (ILibDuktape_fs_linuxWatcher*)object;
	ILibDuktape_fs_watcherData *watcher;
	union { int i; void *p; } wd;

	if (!FD_ISSET(data->fd, readset)) { return; }

	// We were signaled that data is available, so let's get it
	while ((len = read(data->fd, buffer, sizeof(buffer))) > 0)
	{
		while (i < len)
		{
			int changed = 0;
			evt = (struct inotify_event*)(buffer + i);
			i += (sizeof(struct inotify_event) + evt->len);

			wd.p = NULL;
			wd.i = evt->wd;
			watcher = (ILibDuktape_fs_watcherData*)ILibHashtable_Get(data->watchTable, wd.p, NULL, 0);
			if (watcher == NULL || ILibDuktape_EventEmitter_HasListeners(watcher->emitter, "change") == 0) { continue; } // If nobody is listening for this event, no need to waste cycles processing it

			duk_push_object(watcher->ctx);					// [detail]

			if ((evt->mask & IN_CREATE) == IN_CREATE)
			{
				changed = 1;
				duk_push_string(watcher->ctx, "ADDED");
				duk_put_prop_string(watcher->ctx, -2, "changeType");
				duk_push_string(watcher->ctx, evt->name);
				duk_put_prop_string(watcher->ctx, -2, "\xFF_FileName");
			}
			if ((evt->mask & IN_DELETE) == IN_DELETE)
			{
				changed = 1;
				duk_push_string(watcher->ctx, "REMOVED");
				duk_put_prop_string(watcher->ctx, -2, "changeType");
				duk_push_string(watcher->ctx, evt->name);
				duk_put_prop_string(watcher->ctx, -2, "\xFF_FileName");
			}

			// emit the event
			ILibDuktape_EventEmitter_SetupEmit(watcher->ctx, watcher->object, "change");// [detail][emit][this][change]
			duk_push_string(watcher->ctx, changed == 0 ? "rename" : "change");			// [detail][emit][this][change][type]
			if (changed == 0)
			{
				duk_get_prop_string(watcher->ctx, -5, "oldname");						// [detail][emit][this][change][type][fileName]
			}
			else
			{
				duk_get_prop_string(watcher->ctx, -5, "\xFF_FileName");					// [detail][emit][this][change][type][fileName]
			}
			duk_dup(watcher->ctx, -6);													// [detail][emit][this][change][type][fileName][detail]
			if (duk_pcall_method(watcher->ctx, 4) != 0) { ILibDuktape_Process_UncaughtException(watcher->ctx); }
			duk_pop_2(watcher->ctx);													// ...
		}
	}
}

// Called by the microstack event loop to cleanup
void ILibDuktape_fs_notifyDispatcher_Destroy(void *object)
{
	ILibHashtable_Destroy(((ILibDuktape_fs_linuxWatcher*)object)->watchTable);
}
#endif

#ifdef __APPLE__
// Dispatched callback from kevent(), to be emitted on JS event loop
void ILibduktape_fs_watch_appleWorker_MODIFIED(void *chain, void *user, char *changeType)
{
	ILibDuktape_fs_watcherData *data = (ILibDuktape_fs_watcherData*)user;
	if (ILibMemory_CanaryOK(data))
	{
		ILibDuktape_EventEmitter_SetupEmit(data->ctx, data->object, "change");	// [emit][this][change]
		duk_push_string(data->ctx, "change");									// [emit][this][change][eventType]
		duk_get_prop_string(data->ctx, -3, "\xFF_FileName");					// [emit][this][change][eventType][fileName]
		duk_push_object(data->ctx);												// [emit][this][change][eventType][fileName][detail]
		duk_push_string(data->ctx, changeType); duk_put_prop_string(data->ctx, -2, "changeType");
		if (duk_pcall_method(data->ctx, 4) != 0) { ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.fsWatch.onChange(): "); }
		duk_pop(data->ctx);														// ...
	}
}
void ILibduktape_fs_watch_appleWorker_DELETE(void *chain, void *user)
{
	ILibduktape_fs_watch_appleWorker_MODIFIED(chain, user, "DELETE");
}
void ILibduktape_fs_watch_appleWorker_EXTEND(void *chain, void *user)
{
	ILibduktape_fs_watch_appleWorker_MODIFIED(chain, user, "MODIFIED_EXTEND");
}
void ILibduktape_fs_watch_appleWorker_ATTRIB(void *chain, void *user)
{
	ILibduktape_fs_watch_appleWorker_MODIFIED(chain, user, "MODIFIED_ATTRIB");
}
void ILibduktape_fs_watch_appleWorker_RENAME(void *chain, void *user)
{
	ILibDuktape_fs_watcherData *data = (ILibDuktape_fs_watcherData*)user;
	if (ILibMemory_CanaryOK(data))
	{
		if (duk_peval_string(data->ctx, "require('fs');")==0)
		{
			duk_get_prop_string(data->ctx, -1, "_fdToName");							// [fs][_fdToName]
			duk_swap_top(data->ctx, -2);												// [_fdToName][this]
			duk_push_int(data->ctx, data->wd.i);										// [_fdToName][this][fd]
			if (duk_pcall_method(data->ctx, 1) == 0)
			{
				ILibDuktape_EventEmitter_SetupEmit(data->ctx, data->object, "change");	// [NAME][emit][this][change]
				duk_push_string(data->ctx, "rename");									// [NAME][emit][this][change][eventType]
				duk_get_prop_string(data->ctx, -3, "\xFF_FileName");					// [NAME][emit][this][change][eventType][oldName]
				duk_push_object(data->ctx);												// [NAME][emit][this][change][eventType][oldName][detail]
				duk_push_string(data->ctx, "rename"); duk_put_prop_string(data->ctx, -2, "changeType");				
				duk_dup(data->ctx, -7);	duk_put_prop_string(data->ctx, -2, "newname");	// [NAME][emit][this][change][eventType][oldName][detail]
				duk_get_prop_string(data->ctx, -5, "\xFF_FileName"); duk_put_prop_string(data->ctx, -2, "oldname");		
				duk_remove(data->ctx, -7);												// [emit][this][change][eventType][oldName][detail]
				if (duk_pcall_method(data->ctx, 4) != 0) { ILibDuktape_Process_UncaughtExceptionEx(data->ctx, "fs.fsWatch.onChange(): "); }
				duk_pop(data->ctx);														// ...
			}
			duk_pop(data->ctx);										// ...
		}
		else
		{
			duk_pop(data->ctx);
		}
	}
}
void ILibduktape_fs_watch_appleWorker_WRITE(void *chain, void *user)
{
	ILibduktape_fs_watch_appleWorker_MODIFIED(chain, user, "MODIFIED_WRITE");
}
void ILibduktape_fs_watch_appleWorker_LINK(void *chain, void *user)
{
	ILibduktape_fs_watch_appleWorker_MODIFIED(chain, user, "MODIFIED_LINK");
}

// macOS kevent loop
void ILibduktape_fs_watch_appleWorker(void *obj)
{
	ILibDuktape_fs_appleWatcher *watcher = (ILibDuktape_fs_appleWatcher*)obj;
	struct kevent change[KEVENTBLOCKSIZE];
	struct kevent event;
	int inCount = 1;
	int n, i;
	char tmp[255];

	EV_SET(&(change[0]), watcher->unblocker[0], EVFILT_READ, EV_ADD | EV_CLEAR,	0, 0, 0);
	while (watcher->exit == 0)
	{
		n = kevent(watcher->kq, change, inCount, &event, 1, NULL); inCount = 0;
		if (n > 0)
		{
			if (event.ident == watcher->unblocker[0]) // Check our 'wakeup' descriptor, which we use to force unblock, so we can add other descriptors
			{
				// Force Unblock
				read(watcher->unblocker[0], tmp, event.data < sizeof(tmp) ? event.data : sizeof(tmp));
				if (watcher->exit != 0) { continue; }

				for(i=0; (watcher->descriptors != NULL && watcher->descriptors[i] != NULL); ++i)
				{
					if ((watcher->descriptors[i]->flags & ILibDuktape_fs_descriptorFlags_ADD) == ILibDuktape_fs_descriptorFlags_ADD)
					{
						// Add Descriptor
						EV_SET(&(change[inCount++]), (uintptr_t)watcher->descriptors[i]->descriptor, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_LINK, 0, watcher->descriptors[i]->user);
						if (inCount == KEVENTBLOCKSIZE)
						{
							// Change List is full, let's set it to kevent now
							kevent(watcher->kq, change, inCount, &event, 0, NULL); // This will return immediately, becuase we set eventsize to 0
							inCount = 0;
						}
					}
					if ((watcher->descriptors[i]->flags & ILibDuktape_fs_descriptorFlags_REMOVE) == ILibDuktape_fs_descriptorFlags_REMOVE)
					{
						// Remove Descriptor
						EV_SET(&(change[inCount++]), (uintptr_t)watcher->descriptors[i]->descriptor, EVFILT_VNODE, EV_DELETE | EV_DISABLE, 0, 0, NULL);
						if (inCount == KEVENTBLOCKSIZE)
						{
							// Change List is full, let's set it to kevent now
							kevent(watcher->kq, change, inCount, &event, 0, NULL); // This will return immediately, becuase we set eventsize to 0
							inCount = 0;
						}
					}
				}
				sem_post(&(watcher->inputWaiter));
			}
			else
			{
				// One of the descriptors triggered!
				if ((event.fflags & NOTE_ATTRIB) == NOTE_ATTRIB)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_ATTRIB, event.udata);
				}
				if ((event.fflags & NOTE_DELETE) == NOTE_DELETE)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_DELETE, event.udata);
				}
				if ((event.fflags & NOTE_EXTEND) == NOTE_EXTEND)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_EXTEND, event.udata);
				}
				if ((event.fflags & NOTE_RENAME) == NOTE_RENAME)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_RENAME, event.udata);
				}
				if ((event.fflags & NOTE_WRITE) == NOTE_WRITE)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_WRITE, event.udata);
				}	
				if ((event.fflags & NOTE_LINK) == NOTE_LINK)
				{
					ILibChain_RunOnMicrostackThreadEx(watcher->chain, ILibduktape_fs_watch_appleWorker_LINK, event.udata);
				}
			}
		}
	}
	sem_post(&(watcher->exitWaiter));
}
#endif

duk_ret_t ILibDuktape_fs_watch(duk_context *ctx)
{
#ifdef WIN32
	WCHAR *path = (WCHAR*)ILibDuktape_String_AsWide(ctx, 0, NULL);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
#endif
	int nargs = duk_get_top(ctx);
	int i;
	ILibDuktape_fs_watcherData *data;
#ifndef __APPLE__
	void *chain = Duktape_GetChain(ctx);
#endif

#if defined(WIN32)
	int recursive = 0;
	ILibProcessPipe_Manager pipeMgr;

	// Setup an ILibProcessPipe_Manager object, that we can use for dispatching if necessary
	duk_push_this(ctx);														// [fs]
	if (duk_has_prop_string(ctx, -1, FS_PIPEMANAGER_PTR))
	{
		duk_get_prop_string(ctx, -1, FS_PIPEMANAGER_PTR);
		pipeMgr = (ILibProcessPipe_Manager)duk_get_pointer(ctx, -1);		// [fs][ptr]
		duk_pop_2(ctx);														// ...
	}
	else
	{
		pipeMgr = ILibProcessPipe_Manager_Create(chain);
		duk_push_pointer(ctx, pipeMgr);										// [fs][ptr]
		duk_put_prop_string(ctx, -2, FS_PIPEMANAGER_PTR);					// [fs]
		duk_pop(ctx);														// ...
	}
#elif defined(_POSIX)
#if !defined(__APPLE__) && !defined(_FREEBSD)
	// Linux
	ILibDuktape_fs_linuxWatcher *notifyDispatcher = NULL;
	duk_push_this(ctx);																// [fs]
	if (duk_has_prop_string(ctx, -1, FS_NOTIFY_DISPATCH_PTR))
	{
		duk_get_prop_string(ctx, -1, FS_NOTIFY_DISPATCH_PTR);						// [fs][ptr]
		notifyDispatcher = (ILibDuktape_fs_linuxWatcher*)duk_get_pointer(ctx, -1);
		duk_pop_2(ctx);																// ...
	}
	else
	{
		// if a notifyDispatcher wasn't setup, let's setup a new one and hook it up to the event loop, so we can add descriptors
		notifyDispatcher = ILibMemory_Allocate(sizeof(ILibDuktape_fs_linuxWatcher), 0, NULL, NULL);
		notifyDispatcher->chainLink.MetaData = ILibMemory_SmartAllocate_FromString("ILibDuktape_fs_linuxWatcher");
		notifyDispatcher->chainLink.PreSelectHandler = ILibDuktape_fs_notifyDispatcher_PreSelect;
		notifyDispatcher->chainLink.PostSelectHandler = ILibDuktape_fs_notifyDispatcher_PostSelect;
		notifyDispatcher->chainLink.DestroyHandler = ILibDuktape_fs_notifyDispatcher_Destroy;
		notifyDispatcher->chainLink.QueryHandler = ILibDuktape_fs_notifyDispatcher_Query;
		notifyDispatcher->watchTable = ILibHashtable_Create();
		notifyDispatcher->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (notifyDispatcher->fd == 0)
		{
			notifyDispatcher->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
			close(0);
		}
		notifyDispatcher->ctx = ctx;
		ILibAddToChain(chain, notifyDispatcher);
		duk_push_pointer(ctx, notifyDispatcher);							// [fs][ptr]
		duk_put_prop_string(ctx, -2, FS_NOTIFY_DISPATCH_PTR);				// [fs]
		duk_pop(ctx);														// ...
	}
#elif defined(__APPLE__)
	// MacOS
	ILibDuktape_fs_appleWatcher *watcher = NULL;
	duk_push_this(ctx);																// [fs]
	if (duk_has_prop_string(ctx, -1, FS_NOTIFY_DISPATCH_PTR))
	{
		duk_get_prop_string(ctx, -1, FS_NOTIFY_DISPATCH_PTR);						// [fs][buffer]
		watcher = (ILibDuktape_fs_appleWatcher*)Duktape_GetBuffer(ctx, -1, NULL);
		duk_pop_2(ctx);																// ...
	}
	else
	{
		watcher = (ILibDuktape_fs_appleWatcher*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_fs_appleWatcher));
		duk_put_prop_string(ctx, -2, FS_NOTIFY_DISPATCH_PTR);
		duk_pop(ctx);																// ...

		// Since this is newly created, we must take care of a few housekeeping things
		if ((watcher->kq = kqueue()) == -1)
		{
			return(ILibDuktape_Error(ctx, "Could not create kq"));
		}		
		sem_init(&(watcher->exitWaiter), 0, 0);
		sem_init(&(watcher->inputWaiter), 0, 0);
		pipe(watcher->unblocker); // Setup a pipe, so we can use it to force unblock the child, so it can add descriptors
		watcher->chain = Duktape_GetChain(ctx);
		watcher->kthread = ILibSpawnNormalThread(ILibduktape_fs_watch_appleWorker, watcher); // Spawn a thread to act as the kevent loop
	}
#endif
#endif
	
	duk_push_object(ctx);													// [FSWatcher]
	ILibDuktape_WriteID(ctx, "fs.fsWatcher");
	data = (ILibDuktape_fs_watcherData*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_fs_watcherData));
	duk_put_prop_string(ctx, -2, FS_WATCHER_DATA_PTR);						// [FSWatcher]
	duk_push_this(ctx); duk_put_prop_string(ctx, -2, FS_WATCHER_2_FS);
	duk_dup(ctx, 0); duk_put_prop_string(ctx, -2, FS_WATCH_PATH);
	data->emitter = ILibDuktape_EventEmitter_Create(ctx);
	data->ctx = ctx;
	data->object = duk_get_heapptr(ctx, -1);
#if defined(WIN32)
	data->chain = chain;
	data->pipeManager = pipeMgr;
	data->recursive = recursive;
#elif defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
	data->linuxWatcher = notifyDispatcher;
#endif
#ifdef __APPLE__
	duk_dup(ctx, 0);
	duk_put_prop_string(ctx, -2, "\xFF_FileName");
#endif

	ILibDuktape_CreateInstanceMethod(ctx, "close", ILibDuktape_fs_watcher_close, 0);
	ILibDuktape_EventEmitter_CreateEventEx(data->emitter, "change");
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_fs_watcher_finalizer);

	for (i = 1; i < nargs; ++i)
	{
		if (duk_is_function(ctx, i))
		{
			// listener callback
			ILibDuktape_EventEmitter_AddOn(data->emitter, "change", duk_require_heapptr(ctx, i));
			break;
		}
	}

	// Setup the file system watcher
#if defined(WIN32)
	if ((data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL)) == NULL) { return(ILibDuktape_Error(ctx, "Could not create handle")); }
	data->h = CreateFileW(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	if (data->h == INVALID_HANDLE_VALUE) { return(ILibDuktape_Error(ctx, "fs.watch(): Invalid Path or Access Denied")); }

	if (ReadDirectoryChangesW(data->h, data->results, sizeof(data->results), recursive, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS, NULL, &(data->overlapped), NULL) == 0)
	{
		return(ILibDuktape_Error(ctx, "fs.watch(): Error creating watcher"));
	}
	ILibChain_AddWaitHandle(data->chain, data->overlapped.hEvent, -1, ILibDuktape_fs_watch_iocompletion, data);
#elif defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
	data->wd.i = inotify_add_watch(data->linuxWatcher->fd, path, IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);
	if (data->wd.i < 0)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(chain), ILibRemoteLogging_Modules_Agent_GuardPost | ILibRemoteLogging_Modules_ConsolePrint, ILibRemoteLogging_Flags_VerbosityLevel_1, "fs.watch(): Error setting watch on [%s] errno = %d", path, errno);
	}
	else
	{
		ILibHashtable_Put(data->linuxWatcher->watchTable, data->wd.p, NULL, 0, data);
	}
#elif defined(__APPLE__)
	if ((data->wd.i = open(path, O_RDONLY)) < 0)
	{
		return(ILibDuktape_Error(ctx, "Could not create watcher for: %s", path));
	}
	else
	{
		void **d = ILibMemory_Init(alloca(ILibMemory_Init_Size(2 * sizeof(void*), sizeof(ILibDuktape_fs_descriptorInfo))), 2 * sizeof(void*), sizeof(ILibDuktape_fs_descriptorInfo), ILibMemory_Types_STACK);
		d[0] = ILibMemory_Extra(d);
		d[1] = NULL;
		((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->descriptor = data->wd.p;
		((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->user = data;
		((ILibDuktape_fs_descriptorInfo*)ILibMemory_Extra(d))->flags = ILibDuktape_fs_descriptorFlags_ADD;
		watcher->descriptors = (ILibDuktape_fs_descriptorInfo**)d;
		write(watcher->unblocker[1], " ", 1);
		sem_wait(&(watcher->inputWaiter));
	}
#endif

	return 1;
}
#endif

// fs.renameSync() to rename a file
duk_ret_t ILibDuktape_fs_rename(duk_context *ctx)
{
	char *oldPath = (char*)duk_require_string(ctx, 0);
	char *newPath = (char*)duk_require_string(ctx, 1);

#ifdef WIN32
	if (_wrename((LPCWSTR)ILibDuktape_String_UTF8ToWide(ctx, oldPath), (LPCWSTR)ILibDuktape_String_UTF8ToWide(ctx, newPath)) != 0)
#else
	if (rename(oldPath, newPath) != 0)
#endif
	{
		sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "fs.renameSync(): Error renaming %s to %s", oldPath, newPath);
		return(ILibDuktape_Error(ctx, "%s", ILibScratchPad));
	}
	return 0;
}

// fs.unlinkSync() to delete a file
duk_ret_t ILibDuktape_fs_unlink(duk_context *ctx)
{
#ifdef WIN32
	char *path = ILibDuktape_String_AsWide(ctx, 0, NULL);
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
#endif

#ifdef WIN32
	if(_wremove((const wchar_t*)path) != 0)
#else
	if (remove(path) != 0)
#endif
	{
#ifdef WIN32
		if (RemoveDirectoryW((LPCWSTR)path) != 0) { return 0; }
#endif
		sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "fs.unlinkSync(): Error trying to unlink: %s", ILibDuktape_String_WideToUTF8(ctx, path));
		return(ILibDuktape_Error(ctx, "%s", ILibScratchPad));
	}
	return 0;
}

// fs.rmdirSync() to delete a folder
duk_ret_t ILibDuktape_fs_rmdirSync(duk_context *ctx)
{
#ifdef WIN32
	char *path = ILibDuktape_String_AsWide(ctx, 0, NULL);
	ILibDuktape_String_WideToUTF8(ctx, path);
	if (_wrmdir((const wchar_t*)path) != 0)
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
	if (rmdir(path) != 0)
#endif
	{
		return(ILibDuktape_Error(ctx, "fs.rmdirSync(): Unable to remove dir: %s", ILibDuktape_String_WideToUTF8(ctx, path)));
	}
	return 0;
}

// fs.mkdirSync() to create a folder
duk_ret_t ILibDuktape_fs_mkdirSync(duk_context *ctx)
{
	//int nargs = duk_get_top(ctx);

#ifdef WIN32
	char *path = ILibDuktape_String_AsWide(ctx, 0, NULL);
	ILibDuktape_String_WideToUTF8(ctx, path);
	if (_wmkdir((const wchar_t*)path) != 0)
#else
	char *path = ILibDuktape_fs_fixLinuxPath((char*)duk_require_string(ctx, 0));
	if (mkdir(path, 0777) != 0)
#endif
	{
		return(ILibDuktape_Error(ctx, "fs.mkdirSync(): Unable to create dir: %s", ILibDuktape_String_WideToUTF8(ctx, path)));
	}
	return 0;
}

// fs.readFileSync() to read an entire file into a buffer
duk_ret_t ILibDuktape_fs_readFileSync(duk_context *ctx)
{
	char *filePath = (char*)duk_require_string(ctx, 0);
	FILE *f;
	long fileLen;

#ifdef WIN32
	char *flags = "rbN";
#else
	char *flags = "rb";
#endif

	if (duk_is_object(ctx, 1))
	{
		flags = Duktape_GetStringPropertyValue(ctx, 1, "flags", flags);
	}

#ifdef WIN32
	_wfopen_s(&f, (const wchar_t*)ILibDuktape_String_UTF8ToWide(ctx, filePath), (const wchar_t*)ILibDuktape_String_UTF8ToWide(ctx, flags));
#else
	f = fopen(filePath, flags);
#endif

	if (f == NULL) { return(ILibDuktape_Error(ctx, "fs.readFileSync(): File [%s] not found", filePath)); }

	fseek(f, 0, SEEK_END);
	fileLen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(fileLen > 0)
	{
		// If the filesystem gives us the file length, we'll allocate the buffer first, and then fill it up
		duk_push_fixed_buffer(ctx, (duk_size_t)fileLen);
		ignore_result(fread(Duktape_GetBuffer(ctx, -1, NULL), 1, (size_t)fileLen, f));
		fclose(f);
		duk_push_buffer_object(ctx, -1, 0, (duk_size_t)fileLen, DUK_BUFOBJ_NODEJS_BUFFER);
	}
	else
	{
		// If the filesystem can't tell us the file length, we'll need to use a dynamic buffer, so it will just realloc as we go along
		duk_size_t bufferSize = 1024;
		char *buffer = (char*)duk_push_dynamic_buffer(ctx, bufferSize);				// [dynamicBuffer]
		size_t bytesRead = 0;
		size_t len = 0;
		while ((bytesRead = fread(buffer + len, 1,(bufferSize-len)>1024?1024:(bufferSize-len), f)) > 0)
		{
			len += bytesRead;
			if (bytesRead == 1024)
			{
				buffer = duk_resize_buffer(ctx, -1, bufferSize + 1024);
				bufferSize += 1024;
			}
		}
		fclose(f);
		duk_push_buffer_object(ctx, -1, 0, (duk_size_t)len, DUK_BUFOBJ_NODEJS_BUFFER);
	}

	return(1);
}

// fs.existsSync() to determine if a path exists
duk_ret_t ILibDuktape_fs_existsSync(duk_context *ctx)
{
	duk_push_this(ctx);							// [fs]
	duk_get_prop_string(ctx, -1, "statSync");	// [fs][statSync]
	duk_swap_top(ctx, -2);						// [statSync][this]
	duk_dup(ctx, 0);							// [statSync][this][path]
	if (duk_pcall_method(ctx, 1) != 0) 
	{ 
		duk_push_false(ctx); 
	}
	else
	{
		duk_push_true(ctx);
	}
	return(1);
}
#ifdef _POSIX
// fs.chmodSync()
duk_ret_t ILibduktape_fs_chmodSync(duk_context *ctx)
{
	if(chmod((char*)duk_require_string(ctx, 0), (mode_t)duk_require_int(ctx, 1)) != 0)
	{
		return(ILibDuktape_Error(ctx, "Error calling chmod(), errno=%d", errno));
	}
	else
	{
		return(0);
	}
}
// fs.chownSync()
duk_ret_t ILibDuktape_fs_chownSync(duk_context *ctx)
{
	if (chown((char*)duk_require_string(ctx, 0), (uid_t)duk_require_int(ctx, 1), (gid_t)duk_require_int(ctx, 2)) != 0)
	{
		return(ILibDuktape_Error(ctx, "Error calling chown(), errno=%d", errno));
	}
	else
	{
		return(0);
	}
}
#endif
#ifdef WIN32
// Windows helper to convert file time to system time
duk_ret_t ILibDuktape_fs_convertFileTime(duk_context *ctx)
{
	if (!(duk_is_object(ctx, 0) && duk_has_prop_string(ctx, -1, "_ptr"))) { return(ILibDuktape_Error(ctx, "Invalid Input Parameters")); }
	FILETIME *ft = (FILETIME*)Duktape_GetPointerProperty(ctx, 0, "_ptr");
	SYSTEMTIME st;
	if (ft == NULL) { return(ILibDuktape_Error(ctx, "Invalid Input Parameters")); }

	if (FileTimeToSystemTime(ft, &st) != 0)
	{
		duk_push_string(ctx, ILibDuktape_fs_convertTime(&st, ILibScratchPad, sizeof(ILibScratchPad)));
		return(1);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Error converting time"));
	}
}
#endif
void ILibDuktape_fs_PUSH(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);						// [fs]
	ILibDuktape_WriteID(ctx, "fs");
	duk_push_pointer(ctx, chain);				// [fs][chain]
	duk_put_prop_string(ctx, -2, FS_CHAIN_PTR);	// [fs]

	duk_push_int(ctx, 65535);					// [fs][nextFD]
	duk_put_prop_string(ctx, -2, FS_NextFD);	// [fs]

	duk_push_object(ctx);						// [fs][descriptors]
	duk_put_prop_string(ctx, -2, FS_FDS);		// [fs]

	duk_push_array(ctx);
	duk_put_prop_string(ctx, -2, FS_BUFFER_DESCRIPTOR_PENDING);

	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, FS_WINDOWS_HANDLES);

	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, FS_EVENT_R_DESCRIPTORS);
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, FS_EVENT_W_DESCRIPTORS);

	ILibDuktape_CreateInstanceMethod(ctx, "closeSync", ILibDuktape_fs_closeSync, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "openSync", ILibDuktape_fs_openSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "readSync", ILibDuktape_fs_readSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "writeSync", ILibDuktape_fs_writeSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "read", ILibDuktape_fs_read, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "write", ILibDuktape_fs_write, DUK_VARARGS);
#ifdef WIN32
	ILibDuktape_CreateInstanceMethod(ctx, "_readdirSync", ILibDuktape_fs_readdirSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "_statSync", ILibDuktape_fs_statSync, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "convertFileTime", ILibDuktape_fs_convertFileTime, 1);
#else
	ILibDuktape_CreateInstanceMethod(ctx, "readdirSync", ILibDuktape_fs_readdirSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "statSync", ILibDuktape_fs_statSync, 1);
#endif
	ILibDuktape_CreateInstanceMethod(ctx, "createWriteStream", ILibDuktape_fs_createWriteStream, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "createReadStream", ILibDuktape_fs_createReadStream, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "readDrivesSync", ILibDuktape_fs_readDrivesSync, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "readFileSync", ILibDuktape_fs_readFileSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "existsSync", ILibDuktape_fs_existsSync, 1);
#ifdef _POSIX
	ILibDuktape_CreateInstanceMethod(ctx, "chmodSync", ILibduktape_fs_chmodSync, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "chownSync", ILibDuktape_fs_chownSync, 3);
#endif
#ifndef _NOFSWATCHER
	ILibDuktape_CreateInstanceMethod(ctx, "watch", ILibDuktape_fs_watch, DUK_VARARGS);
#endif
	ILibDuktape_CreateInstanceMethod(ctx, "renameSync", ILibDuktape_fs_rename, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "unlinkSync", ILibDuktape_fs_unlink, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "mkdirSync", ILibDuktape_fs_mkdirSync, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "rmdirSync", ILibDuktape_fs_rmdirSync, 1);

	duk_push_object(ctx);
#ifdef WIN32
	duk_push_int(ctx, _O_RDONLY); duk_put_prop_string(ctx, -2, "O_RDONLY");
	duk_push_int(ctx, _O_WRONLY); duk_put_prop_string(ctx, -2, "O_WRONLY");
	duk_push_int(ctx, _O_RDWR); duk_put_prop_string(ctx, -2, "O_RDWR");
	duk_push_int(ctx, WIN_FAKE_O_NONBLOCK); duk_put_prop_string(ctx, -2, "O_NONBLOCK");
#else
	duk_push_int(ctx, O_RDONLY); duk_put_prop_string(ctx, -2, "O_RDONLY");
	duk_push_int(ctx, O_WRONLY); duk_put_prop_string(ctx, -2, "O_WRONLY");
	duk_push_int(ctx, O_RDWR); duk_put_prop_string(ctx, -2, "O_RDWR");
	duk_push_int(ctx, O_NONBLOCK); duk_put_prop_string(ctx, -2, "O_NONBLOCK");
#endif
	duk_put_prop_string(ctx, -2, "constants");

	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_fs_Finalizer);

	// JavaScript Polyfills
	char copyFile[] = "exports.copyFile = function copyFile(src, dest)\
						{\
							var ss = this.createReadStream(src, {flags: 'rb'});\
							var ds = this.createWriteStream(dest, {flags: 'wb'});\
							ss.fs = this;\
							ss.pipe(ds);\
							ds.ss = ss;\
							if(!this._copyStreams){this._copyStreams = {};this._copyStreamID = 0;}\
							ss.id = this._copyStreamID++;\
							this._copyStreams[ss.id] = ss;\
							if(arguments.length == 3 && typeof arguments[2] === 'function')\
							{\
								ds.on('close', arguments[2]);\
							}\
							else if(arguments.length == 4 && typeof arguments[3] === 'function')\
							{\
								ds.on('close', arguments[3]);\
							}\
							ds.on('close', function onCopyFileDone(){delete this.ss.fs._copyStreams[this.ss.id];});\
						};\
						exports.copyFileSync = function copyFileSync(src, dest)\
						{\
							var buffer = this.readFileSync(src, {flags: 'rb'});\
							this.writeFileSync(dest, buffer, {flags: 'wb'});\
						};\
						exports.writeFileSync = function writeFileSync(dest, data, options)\
						{\
							var fd = this.openSync(dest, options?options.flags:'wb');\
							this.writeSync(fd, data);\
							this.closeSync(fd);\
							if(options && options.mode != null && process.platform != 'win32') { this.chmodSync(dest, options.mode);}\
						};\
						exports.CHMOD_MODES = {S_IRUSR: 0o400, S_IWUSR: 0o200, S_IXUSR: 0o100, S_IRGRP: 0o40, S_IWGRP: 0o20, S_IXGRP: 0o10, S_IROTH: 0o4, S_IWOTH: 0o2, S_IXOTH: 0o1};\
						if(process.platform == 'darwin')\
						{\
							exports._fdToName = function _fdToName(req_fd, pid)\
							{\
								var child = require('child_process').execFile('/bin/sh', ['sh']);\
								child.stdout._lines = '';\
								child.stdout.on('data', function(chunk) { this._lines += chunk.toString(); });\
								child.stdin.write('lsof -p ' + (pid ? pid : process.pid) + '\\nexit\\n');\
								child.waitExit();\
								var lines = child.stdout._lines.split('\\n');\
								var nx = lines[0].indexOf('NAME');\
								var fdx = lines[0].indexOf('FD') + 2;\
								for (var i = 1; i < lines.length; ++i)\
								{\
									var name = lines[i].substring(nx).trim();\
									var fd = lines[i].substring(0, fdx).split(' ');\
									fd = fd[fd.length - 1];\
									if (req_fd == fd)\
									{\
										return (name);\
									}\
								}\
								throw ('not found');\
							}\
						}\
						if(process.platform == 'win32')\
						{\
							exports._fixwinpath = function _fixwinpath(p) { return(p.split('/').join('\\\\')); };\
							exports.statSync = function statSync(pathstr) { return(this._statSync(this._fixwinpath(pathstr))); };\
							exports.readdirSync = function readdirSync(pathstr)\
							{\
								var sysnative=false;\
								pathstr = exports._fixwinpath(pathstr);\
								if(require('os').arch()=='x64' && require('_GenericMarshal').PointerSize==4 && pathstr.split('\\\\*').join('').toLowerCase()==process.env['windir'].toLowerCase()) { sysnative=true; }\
								if(!pathstr.endsWith('*'))\
								{\
									if(pathstr.endsWith('\\\\'))\
									{\
										pathstr += '*';\
									}\
									else\
									{\
										pathstr += '\\\\*';\
									}\
								}\
								var ret = exports._readdirSync(pathstr);\
								if(sysnative) { ret.push('sysnative'); }\
								return(ret);\
							};\
							exports.readdirSync.version=1;\
						}";
	ILibDuktape_ModSearch_AddHandler_AlsoIncludeJS(ctx, copyFile, sizeof(copyFile) - 1);
}

void ILibDuktape_fs_init(duk_context * ctx)
{
	ILibDuktape_ModSearch_AddHandler(ctx, "fs", ILibDuktape_fs_PUSH);
}

#ifdef __DOXY__
/*!
\brief File I/O is provided by simple wrappers around standard POSIX functions. <b>Note:</b> To use, must <b>require('fs')</b>
*/
class fs
{
public:
	/*!
	\brief Synchronous close
	\param fd <Integer> File Descriptor
	*/
	void closeSync(fd);
	/*!
	\brief Synchronous file open
	\param path \<String\|Buffer\> 
	\param flags \<String\|Number\>
	\param mode <Integer> 
	\return <Integer> File Descriptor
	*/
	Integer openSync(path, flags[, mode]);
	/*!
	\brief Synchronously read data from File Descriptor
	\param fd <Integer>
	\param buffer \<Buffer\> 
	\param offset <Integer> where to start writing
	\param length <Integer> number of bytes to read
	\param position <Integer|NULL> where in file to start reading. NULL = current position
	\return <Integer> number of bytes written into Buffer
	*/
	Integer readSync(fd, buffer, offset, length, position);
	/*!
	\brief Synchronously writes data to a file, replacing the file if it already exists
	\param fd <Integer> File descriptor
	\param offset <Integer> 
	\param length <Integer>
	\param position <Integer>
	\return <Integer> Number of bytes written
	*/
	Integer writeSync(fd, buffer[, offset[, length[, position]]]);
	/*!
	\brief Synchronously reads the contents of a directoy
	\param path \<String\> directory to read
	\param options \<String\|Object\> \n
	<b>encoding</b> \<String\> <b>Default:</b> 'utf8'\n
	\return Array\<String\> contents of the folder, excluding '.' and '..'
	*/
	Array<String> readdirSync(path[, options]);
	/*!
	\brief Returns a new WritableStream
	\param path \<String\> 
	\param options <Object> has the following defaults:\n
	<b>flags</b> \<String\> 'w'\n
	<b>encoding</b> \<String\> 'utf8'\n
	<b>fd</b> <Integer> NULL\n
	<b>mode</b> <Integer> 0o666\n
	<b>autoClose</b> <boolean> true\n
	\return \<WritableStream\>
	*/
	WritableStream createWriteStream(path[, options]);
	/*!
	\brief Returns a new ReadableStream
	\param path \<String\>
	\param options <Object> has the following defaults:\n
	<b>flags</b> \<String\> 'r'\n
	<b>encoding</b> \<String\> NULL\n
	<b>fd</b> <Integer> NULL\n
	<b>mode</b> <Integer> 0o666\n
	<b>autoClose</b> <boolean> true\n
	\return \<ReadableStream\>
	*/
	ReadableStream createReadStream(path[, options]);
	/*!
	\brief Synchronously gets file statistics
	\param path \<String]>
	\return \<Stats\>
	*/
	Stats statSync(path);
	/*!
	\brief Synchronously fetches an Array of mounted drive letters <b>Note:</b> Windows Only
	\return Array\<String\>
	*/
	Array<String> readDrivesSync();
	/*!
	\brief Synchronously reads the contents of a file
	\param path \<String\>
	\param options <Object> Optional options with the following defaults:\n
	<b>encoding</b> \<String\> NULL\n
	<b>flag</b> \<String\> 'r'\n
	\return \<Buffer\>
	*/
	Buffer readFileSync(path[, options]);
	/*!
	\brief Watch for changes on filename, where filename is either a file or a directory.
	\param filename \<String\>
	\param options <Object> Optional with the following values:\n
	<b>persistent</b> <boolean> Indicates whether the process should continue to run as long as files are being watched. <b>Default:</b> true\n
	<b>recursive</b> <boolean> Indicates whether all subdirectories should be watched, or only the current directory. This applies when a directory is specified, and only on supported platforms. <b>Default:</b> false\n
	<b>encoding</b> \<String\> Specifies the character encoding to be used for the filename passed to the listener. <b>Default:</b> 'utf8'\n
	\return \<FSWatcher\>
	*/
	FSWatcher watch(filename[, options][, listener]);
	/*!
	\brief Synchronously renames oldPath to newPath
	\param oldPath \<String\>
	\param newPath \<String\>
	*/
	void renameSync(oldPath, newPath);
	/*!
	\brief Synchronously unlinks or removes the specified path
	\param path \<String\>
	*/
	void unlinkSync(path);
	/*!
	\brief Synchronously creates the directory specified by path
	\param path \<String\>
	\param mode <Integer> Optional. <b>Default:</b> 0o777
	*/
	void mkdirSync(path[, mode]);
	/*!
	\brief File System Statistics
	*/
	class Stats
	{
	public:
		/*!
		\brief File Size
		*/
		Number size;
		/*!
		\brief Access Time  -  Time when file was last accessed.
		*/
		String atime;
		/*!
		\brief Modified Time  -  Time when file data last modified
		*/
		String mtime;
		/*!
		\brief Change Time  -  Time when file status was last changed
		*/
		String ctime;
		/*!
		\brief Birth Time  -  Time of file creation
		*/
		String birthtime;
	};
	/*!
	\implements EventEmitter
	\brief File System Watch object
	*/
	class FSWatcher
	{
	public:
		/*!
		\brief Event emitted when something changes
		\param eventType \<String\> The type of fs change
		\param filename \<String\> The filename that changed (if relevant/available)
		*/
		void change;
		/*!
		\brief Event emitted when an error occurs
		\param err <Error>
		*/
		void error;
		
		/*!
		\brief Stop watching for changes
		*/
		void close();
	};
};
#endif
