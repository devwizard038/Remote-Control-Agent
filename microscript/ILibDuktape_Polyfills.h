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

#ifndef __ILibDuktape_Polyfills__
#define __ILibDuktape_Polyfills__

#include "duktape.h"

extern int g_displayStreamPipeMessages;
extern int g_displayFinalizerMessages;
void ILibDuktape_Polyfills_Init(duk_context *ctx);
void ILibDuktape_Polyfills_JS_Init(duk_context *ctx);

#endif
