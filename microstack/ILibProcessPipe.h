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

/*! \file ILibNamedPipe.h
\brief MicroStack APIs for various functions and tasks related to named pipes
*/

#ifndef __ILibProcessPipe__
#define __ILibProcessPipe__

#include "ILibParsers.h"

typedef void* ILibProcessPipe_Manager;
typedef void* ILibProcessPipe_Process;
typedef void(*ILibProcessPipe_Process_OutputHandler)(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t* bytesConsumed, void* user);
typedef void(*ILibProcessPipe_Process_SendOKHandler)(ILibProcessPipe_Process sender, void* user);
typedef void(*ILibProcessPipe_Process_ExitHandler)(ILibProcessPipe_Process sender, int exitCode, void* user);

typedef void* ILibProcessPipe_Pipe;
typedef void(*ILibProcessPipe_Pipe_ReadHandler)(ILibProcessPipe_Pipe sender, char *buffer, size_t bufferLen, size_t* bytesConsumed);
typedef void(*ILibProcessPipe_Pipe_BrokenPipeHandler)(ILibProcessPipe_Pipe sender);

typedef enum ILibProcessPipe_SpawnTypes
{
	ILibProcessPipe_SpawnTypes_DEFAULT		= 0,
	ILibProcessPipe_SpawnTypes_USER			= 1,
	ILibProcessPipe_SpawnTypes_WINLOGON		= 2,
	ILibProcessPipe_SpawnTypes_TERM			= 3,
	ILibProcessPipe_SpawnTypes_DETACHED		= 4,
	ILibProcessPipe_SpawnTypes_SPECIFIED_USER = 5,
	ILibProcessPipe_SpawnTypes_POSIX_DETACHED = 0x8000
}ILibProcessPipe_SpawnTypes;

#ifdef WIN32
typedef void(*ILibProcessPipe_Pipe_ReadExHandler)(ILibProcessPipe_Pipe sender, void *user, DWORD errorCode, char *buffer, int bufferLen);
typedef void(*ILibProcessPipe_Pipe_WriteExHandler)(ILibProcessPipe_Pipe sender, void *user, DWORD errorCode, int bytesWritten);
typedef enum ILibProcessPipe_Pipe_ReaderHandleType
{
	ILibProcessPipe_Pipe_ReaderHandleType_NotOverLapped = 0,	//!< Spawn a I/O processing thread
	ILibProcessPipe_Pipe_ReaderHandleType_Overlapped = 1		//!< Use Overlapped I/O
}ILibProcessPipe_Pipe_ReaderHandleType;
HANDLE ILibProcessPipe_Manager_GetWorkerThread(ILibProcessPipe_Manager mgr);
#endif

ILibTransport_DoneState ILibProcessPipe_Pipe_Write(ILibProcessPipe_Pipe writePipe, char* buffer, int bufferLen, ILibTransport_MemoryOwnership ownership);
void ILibProcessPipe_Pipe_AddPipeReadHandler(ILibProcessPipe_Pipe targetPipe, int bufferSize, ILibProcessPipe_Pipe_ReadHandler OnReadHandler);
#ifdef WIN32
	int ILibProcessPipe_Pipe_CancelEx(ILibProcessPipe_Pipe targetPipe);
	int ILibProcessPipe_Pipe_ReadEx(ILibProcessPipe_Pipe targetPipe, char *buffer, int bufferLength, void *user, ILibProcessPipe_Pipe_ReadExHandler OnReadHandler);
	ILibTransport_DoneState ILibProcessPipe_Pipe_WriteEx(ILibProcessPipe_Pipe targetPipe, char *buffer, int bufferLength, void *user, ILibProcessPipe_Pipe_WriteExHandler OnWriteHandler);
	ILibProcessPipe_Pipe ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(ILibProcessPipe_Manager manager, HANDLE existingPipe, ILibProcessPipe_Pipe_ReaderHandleType handleType, int extraMemorySize);
	#define ILibProcessPipe_Pipe_CreateFromExisting(PipeManager, ExistingPipe, HandleType) ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(PipeManager, ExistingPipe, HandleType, 0)
#else
	ILibProcessPipe_Pipe ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(ILibProcessPipe_Manager manager, int existingPipe, int extraMemorySize);
	#define ILibProcessPipe_Pipe_CreateFromExisting(PipeManager, ExistingPipe) ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(PipeManager, ExistingPipe, 0)
#endif

void ILibProcessPipe_Pipe_SetBrokenPipeHandler(ILibProcessPipe_Pipe targetPipe, ILibProcessPipe_Pipe_BrokenPipeHandler handler);

ILibProcessPipe_Manager ILibProcessPipe_Manager_Create(void *chain);
int ILibProcessPipe_Process_IsDetached(ILibProcessPipe_Process p);
ILibProcessPipe_Process ILibProcessPipe_Manager_SpawnProcessEx4(ILibProcessPipe_Manager pipeManager, char* target, char* const* parameters, ILibProcessPipe_SpawnTypes spawnType, void *sessionId, void *envvars, int extraMemorySize);
#define ILibProcessPipe_Manager_SpawnProcess(pipeManager, target, parameters) ILibProcessPipe_Manager_SpawnProcessEx2(pipeManager, target, parameters, ILibProcessPipe_SpawnTypes_DEFAULT, 0)
#define ILibProcessPipe_Manager_SpawnProcessEx(pipeManager, target, parameters, spawnType) ILibProcessPipe_Manager_SpawnProcessEx2(pipeManager, target, parameters, spawnType, 0)
#define ILibProcessPipe_Manager_SpawnProcessEx2(pipeManager, target, parameters, spawnType, extraMemorySize) ILibProcessPipe_Manager_SpawnProcessEx3(pipeManager, target, parameters, spawnType, NULL, extraMemorySize)
#define ILibProcessPipe_Manager_SpawnProcessEx3(pipeManager, target, parameters, spawnType, sessionId, extraMemorySize) ILibProcessPipe_Manager_SpawnProcessEx4(pipeManager, target, parameters, spawnType, sessionId, NULL, extraMemorySize)
#define ILibProcessPipe_Manager_SpawnProcessWithExtraPipeMemory(pipeManager, target, parameters, memorySize) ILibProcessPipe_Manager_SpawnProcessEx2(pipeManager, target, parameters, ILibProcessPipe_SpawnTypes_DEFAULT, memorySize)
void ILibProcessPipe_Process_SoftKill(ILibProcessPipe_Process p);
void ILibProcessPipe_Process_HardKill(ILibProcessPipe_Process p);
void ILibProcessPipe_Process_AddHandlers(ILibProcessPipe_Process module, int bufferSize, ILibProcessPipe_Process_ExitHandler exitHandler, ILibProcessPipe_Process_OutputHandler stdOut, ILibProcessPipe_Process_OutputHandler stdErr, ILibProcessPipe_Process_SendOKHandler sendOk, void *user);
#ifdef WIN32
void ILibProcessPipe_Process_RemoveHandlers(ILibProcessPipe_Process module);
#endif
void ILibProcessPipe_Process_UpdateUserObject(ILibProcessPipe_Process module, void *userObj);
ILibTransport_DoneState ILibProcessPipe_Process_WriteStdIn(ILibProcessPipe_Process p, char* buffer, int bufferLen, ILibTransport_MemoryOwnership ownership);
void ILibProcessPipe_Process_CloseStdIn(ILibProcessPipe_Process p);
#ifdef WIN32
void ILibProcessPipe_Process_GetWaitHandles(ILibProcessPipe_Process p, HANDLE *hProcess, HANDLE *read, HANDLE *write, HANDLE *error);
#endif

#define ILibProcessPipe_Process_StartPipeReader(pipeObject, bufferSize, handler, user1, user2) ILibProcessPipe_Process_StartPipeReaderEx(pipeObject, bufferSize, handler, user1, user2, ILibChain_MetaData(__FILE__, __LINE__))

char *ILibProcessPipe_Process_GetMetadata(ILibProcessPipe_Process p);
void ILibProcessPipe_Process_ResetMetadata(ILibProcessPipe_Process p, char *metadata);
void ILibProcessPipe_Pipe_ResetMetadata(ILibProcessPipe_Pipe p, char *metadata);
void ILibProcessPipe_Pipe_Close(ILibProcessPipe_Pipe po);
void ILibProcessPipe_Pipe_Pause(ILibProcessPipe_Pipe pipeObject);
void ILibProcessPipe_Pipe_Resume(ILibProcessPipe_Pipe pipeObject);
ILibProcessPipe_Pipe ILibProcessPipe_Process_GetStdErr(ILibProcessPipe_Process p);
ILibProcessPipe_Pipe ILibProcessPipe_Process_GetStdOut(ILibProcessPipe_Process p);
#ifdef WIN32
DWORD ILibProcessPipe_Process_GetPID(ILibProcessPipe_Process p);
#else
pid_t ILibProcessPipe_Process_GetPID(ILibProcessPipe_Process p);
int ILibProcessPipe_Process_GetPTY(ILibProcessPipe_Process p);
#endif

#define ILibTransports_ProcessPipe 0x60
#endif
