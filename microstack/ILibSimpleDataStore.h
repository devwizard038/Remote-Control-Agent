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

/*
This is a simple data store that implements a hash table in a file. New keys are appended at the end of the file and the file can be compacted when needed.
The store will automatically create a hash of each value and store the hash. Upon reading the data, the hash is always checked.
*/

#ifndef __ILIBSIMPLEDATASTORE__
#define __ILIBSIMPLEDATASTORE__

#include "ILibParsers.h"

#if defined(WIN32) && defined(EXPORT_LIB)
#define __EXPORT_TYPE __declspec(dllexport)
#else
#define __EXPORT_TYPE 
#endif

#define ILibSimpleDataStore_MaxKeyLength 1024
#define ILibSimpleDataStore_MaxUnspecifiedValueLen 16384
#define ILibSimpleDataStore_MaxFilePath 4096

typedef void* ILibSimpleDataStore;

const extern int ILibMemory_SimpleDataStore_CONTAINERSIZE;

typedef void(*ILibSimpleDataStore_KeyEnumerationHandler)(ILibSimpleDataStore sender, char* Key, int KeyLen, void *user);
typedef void(*ILibSimpleDataStore_SizeWarningHandler)(ILibSimpleDataStore sender, uint64_t size, void *user);
typedef void(*ILibSimpleDataStore_WriteErrorHandler)(ILibSimpleDataStore sender, void *user);
typedef void(*ILibSimpleDataStore_GetValuesHandler)(ILibSimpleDataStore sender, char* Key, size_t KeyLen, char* Value, size_t ValueLen, void *user);


// Create the data store.
__EXPORT_TYPE ILibSimpleDataStore ILibSimpleDataStore_CreateEx2(char* filePath, int userExtraMemorySize, int readonly);
#define ILibSimpleDataStore_Create(filePath) ILibSimpleDataStore_CreateEx2(filePath, 0, 0)
#define ILibSimpleDataStore_CreateEx(filePath, userExtraMemorySize) ILibSimpleDataStore_CreateEx2(filePath, userExtraMemorySize, 0)
#define ILibSimpleDataStore_CreateCachedOnly() ILibSimpleDataStore_Create(NULL)
void ILibSimpleDataStore_ReOpenReadOnly(ILibSimpleDataStore dataStore, char* filePath);
int ILibSimpleDataStore_IsCacheOnly(ILibSimpleDataStore ds);
int ILibSimpleDataStore_WasCreatedAsNew(ILibSimpleDataStore ds);

// Check if the data store exists
int ILibSimpleDataStore_Exists(char *filePath);

// Close the data store.
__EXPORT_TYPE void ILibSimpleDataStore_Close(ILibSimpleDataStore dataStore);
__EXPORT_TYPE void ILibSimpleDataStore_CachedEx(ILibSimpleDataStore dataStore, char* key, size_t keyLen, char* value, size_t valueLen, char *vhash);
#define ILibSimpleDataStore_Cached(dataStore, key, keyLen, value, valueLen) ILibSimpleDataStore_CachedEx(dataStore, key, keyLen, value, valueLen, NULL)
__EXPORT_TYPE int ILibSimpleDataStore_Cached_GetValues(ILibSimpleDataStore dataStore, ILibSimpleDataStore_GetValuesHandler handler, void *user);

__EXPORT_TYPE void ILibSimpleDataStore_ConfigCompact(ILibSimpleDataStore dataStore, uint64_t minimumDirtySize);
__EXPORT_TYPE void ILibSimpleDataStore_ConfigSizeLimit(ILibSimpleDataStore dataStore, uint64_t sizeLimit, ILibSimpleDataStore_SizeWarningHandler handler, void *user);
void ILibSimpleDataStore_ConfigWriteErrorHandler(ILibSimpleDataStore dataStore, ILibSimpleDataStore_WriteErrorHandler handler, void *user);

__EXPORT_TYPE int ILibSimpleDataStore_PutEx2(ILibSimpleDataStore dataStore, char* key, size_t keyLen, char* value, size_t valueLen, char *hash);
#define ILibSimpleDataStore_Put(dataStore, key, value) ILibSimpleDataStore_PutEx(dataStore, key, strnlen_s(key, ILibSimpleDataStore_MaxKeyLength), value, strnlen_s(value, ILibSimpleDataStore_MaxUnspecifiedValueLen))
#define ILibSimpleDataStore_PutEx(dataStore, key, keyLen, value, valueLen) ILibSimpleDataStore_PutEx2(dataStore, key, keyLen, value, valueLen, NULL)
int ILibSimpleDataStore_PutCompressed(ILibSimpleDataStore dataStore, char* key, size_t keyLen, char* value, size_t valueLen);

// Get a value from the datastore of given a key.
__EXPORT_TYPE int ILibSimpleDataStore_GetEx(ILibSimpleDataStore dataStore, char* key, size_t keyLen, char* buffer, size_t bufferLen);
#define ILibSimpleDataStore_Get(dataStore, key, buffer, bufferLen) ILibSimpleDataStore_GetEx(dataStore, key, strnlen_s(key, ILibSimpleDataStore_MaxKeyLength), buffer, bufferLen)
__EXPORT_TYPE int ILibSimpleDataStore_GetInt(ILibSimpleDataStore dataStore, char* key, int defaultValue);

// Get the SHA384 hash value from the datastore for a given a key.
__EXPORT_TYPE char* ILibSimpleDataStore_GetHashEx(ILibSimpleDataStore dataStore, char* key, size_t keyLen);
#define ILibSimpleDataStore_GetHash(dataStore, key) ILibSimpleDataStore_GetHashEx(dataStore, key, strnlen_s(key, ILibSimpleDataStore_MaxKeyLength))
int ILibSimpleDataStore_GetHashSize();

// Delete a key from the data store
__EXPORT_TYPE int ILibSimpleDataStore_DeleteEx(ILibSimpleDataStore dataStore, char* key, size_t keyLen);
#define ILibSimpleDataStore_Delete(dataStore, key) ILibSimpleDataStore_DeleteEx(dataStore, key, strnlen_s(key, ILibSimpleDataStore_MaxKeyLength))

// Enumerate all keys from the data store
__EXPORT_TYPE void ILibSimpleDataStore_EnumerateKeys(ILibSimpleDataStore dataStore, ILibSimpleDataStore_KeyEnumerationHandler handler, void *user);

// Compacts the data store
__EXPORT_TYPE int ILibSimpleDataStore_Compact(ILibSimpleDataStore dataStore);

// Lock and unlock the data store. This is useful if we need to access this store from many threads.
__EXPORT_TYPE void ILibSimpleDataStore_Lock(ILibSimpleDataStore dataStore);
__EXPORT_TYPE void ILibSimpleDataStore_UnLock(ILibSimpleDataStore dataStore);

#endif
