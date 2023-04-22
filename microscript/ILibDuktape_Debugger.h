/*
Copyright 2006 - 2017 Intel Corporation

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

#ifndef __DUKTAPE_DEBUGGER__
#define __DUKTAPE_DEBUGGER__

#include "duktape.h"

void ILibDuktape_Debugger_Init(duk_context *ctx, unsigned short debugPort);
void ILibDuktape_Debugger_SetScript(char *js, int jsLen, char *fileName, int fileNameLen);

#endif

