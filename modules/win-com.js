/*
Copyright 2021 Intel Corporation
@author Bryan Roe

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

const CLSCTX_INPROC_SERVER = 1;
const CLSCTX_LOCAL_SERVER = 4;
const EOAC_NONE = 0;
const RPC_C_AUTHN_LEVEL_DEFAULT = 0;
const RPC_C_IMP_LEVEL_IMPERSONATE = 3;
const COINIT_MULTITHREADED = 0;
const IUnknownMethods = ['QueryInterface', 'AddRef', 'Release'];

var GM = require('_GenericMarshal');
var ole32 = GM.CreateNativeProxy('ole32.dll');
ole32.CreateMethod('CLSIDFromString');          // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-clsidfromstring
ole32.CreateMethod('CoCreateInstance');         // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
ole32.CreateMethod('CoInitializeSecurity');     // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializesecurity
ole32.CreateMethod('CoInitialize');             // https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-coinitialize
ole32.CreateMethod('CoInitializeEx');           // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
ole32.CreateMethod('CoUninitialize');           // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-couninitialize
ole32.CreateMethod('IIDFromString');            // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-iidfromstring
ole32.CreateMethod('StringFromCLSID');          // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-stringfromclsid
ole32.CreateMethod('StringFromIID');            // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-stringfromiid


function createInstance_finalizer()
{
    console.info1('CoUninitialize()');
    ole32.CoUninitialize();
}
function createInstance(RFCLSID, RFIID, options)
{
    // Start by initializing the Windows COM Library
    console.info1('CoInitializeEx()');
    ole32.CoInitializeEx(0, COINIT_MULTITHREADED);

    // Set default Security Values for COM
    ole32.CoInitializeSecurity(0, -1, 0, 0, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE, 0);

    // Create Instance of COM Object
    var ppv = GM.CreatePointer();
    var h;
    if ((h = ole32.CoCreateInstance(RFCLSID, 0, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, RFIID,ppv)).Val == 0)
    {
        var ret = ppv.Deref();
        ret.once('~', createInstance_finalizer);
        return (ret);
    }
    else
    {
        // If it fails, we can tear down the COM library
        ole32.CoUninitialize();
    }
    throw ('Error calling CoCreateInstance(' + h.Val + ')');
}

// Convert from STRING to CLSID
function CLSIDFromString(CLSIDString)
{
    var v = GM.CreateVariable(CLSIDString, { wide: true });
    var rfclsid = GM.CreateVariable(16);

    if(ole32.CLSIDFromString(v, rfclsid).Val == 0)
    {
        return (rfclsid);
    }
    else
    {
        throw ('Error Converting CLSIDString');
    }
}

// Convert from STRING to IID
function IIDFromString(IIDString)
{
    var v = GM.CreateVariable(IIDString, { wide: true });
    var rfiid = GM.CreateVariable(16);

    if(ole32.IIDFromString(v, rfiid).Val==0)
    {
        return (rfiid);
    }
    else
    {
        throw ('Error Converting IIDString');
    }
}

// Create an array of functions, from the definitions
function marshalFunctions(obj, arr)
{
    return (GM.MarshalFunctions(obj.Deref(), arr));
}

// Implement a COM interface, and attach it to the local vtable
function marshalInterface(arr)
{
    if (GM.PointerSize == 4)
    {
        // For 32 bit, we need to check to make sure custom handlers were used
        for (var i = 0; i < arr.length; ++i)
        {
            if (arr[i].cx == null)
            {
                throw ('Not supported on 32bit platforms, becuase ellipses function cannot be __stdcall');
            }
        }
    }
    var vtbl = GM.CreateVariable(arr.length * GM.PointerSize);
    var obj = GM.CreatePointer();
    vtbl.pointerBuffer().copy(obj.toBuffer());
    obj._gcallbacks = [];

    // Cleanup function, which clears all the callbacks
    obj.cleanup = function ()
    {
        var v;
        while (this._gcallbacks.length > 0)
        {
            v = this._gcallbacks.pop();
            v.removeAllListeners('GlobalCallback');
            GM.PutGenericGlobalCallbackEx(v);
        }
    };


    // Enumerate the functions, and put them in the local vtable for the COM interface
    for (var i = 0; i < arr.length; ++i)
    {
        _hide(GM.GetGenericGlobalCallbackEx(arr[i].parms, GM.PointerSize == 4 ? arr[i].cx : null)); // Only 32 bit needs custom handlers
        _hide()._ObjectID = 'GlobalCallback_' + arr[i].name;
        obj._gcallbacks.push(_hide());
        _hide().obj = arr[i];
        _hide().pointerBuffer().copy(vtbl.Deref(i * GM.PointerSize, GM.PointerSize).toBuffer());
        _hide(true).on('GlobalCallback', function ()
        {
            if (arguments[0]._ptr == obj._ptr)
            {
                var args = [];
                for (var i in arguments)
                {
                    args.push(arguments[i]);
                }
                obj.callbackDispatched = this.callbackDispatched;
                return (this.obj.func.apply(obj, args));
            }
        });
    }

    return (obj);
}
module.exports = { createInstance: createInstance, marshalFunctions: marshalFunctions, marshalInterface: marshalInterface, CLSIDFromString: CLSIDFromString, IIDFromString: IIDFromString, IID_IUnknown: IIDFromString('{00000000-0000-0000-C000-000000000046}') };
