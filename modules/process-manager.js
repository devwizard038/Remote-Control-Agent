/*
Copyright 2018 Intel Corporation

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


var GM = require('_GenericMarshal');
var TH32CS_SNAPPROCESS = 0x02;
var TH32CS_SNAPMODULE32 = 0x10;
var TH32CS_SNAPMODULE = 0x08;
var PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;


var promise = require('promise');
function defPromiseHandler(res, rej)
{
    this._res = res;
    this._rej = rej;
}

// Used on Windows and Linux to get information about running processes
function processManager() {
    this._ObjectID = 'process-manager'; // Used for debugging, allows you to get the object type at runtime.
    
    // Setup the platform specific calls.
    switch (process.platform)
    {
        case 'win32':
            this._kernel32 = GM.CreateNativeProxy('kernel32.dll');
            this._kernel32.CreateMethod('CloseHandle');
            this._kernel32.CreateMethod('GetLastError');
            this._kernel32.CreateMethod('CreateToolhelp32Snapshot');
            this._kernel32.CreateMethod('Module32FirstW');
            this._kernel32.CreateMethod('Module32NextW');
            this._kernel32.CreateMethod('OpenProcess');
            this._kernel32.CreateMethod('Process32FirstW');
            this._kernel32.CreateMethod('Process32NextW');
            this._kernel32.CreateMethod('QueryFullProcessImageNameW');
            break;
	case 'freebsd':
        case 'linux':
        case 'darwin':
            this._childProcess = require('child_process');
            break;
        default:
            throw (process.platform + ' not supported');
            break;
    }
    this.enumerateProcesses = function enumerateProcesses()
    {
        var promise = require('promise');
        var ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
        ret.callback = function callback(ps)
        {
            callback.prom._res(ps);
        }
        ret.callback.prom = ret;
        this.getProcesses(ret.callback);
        return (ret);
    }
    // Return a object of: pid -> process information.
    this.getProcesses = function getProcesses(callback)
    {
        switch(process.platform)
        {
            default:
                throw ('Enumerating processes on ' + process.platform + ' not supported');
                break;
            case 'win32': // Windows processes
                var pid;
                var retVal = {};
                var h = this._kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                var info = GM.CreateVariable(GM.PointerSize == 8 ? 568 : 556);
                var fullpath = GM.CreateVariable(2048);
                var pathSize = GM.CreateVariable(4);
                var ph;

                info.toBuffer().writeUInt32LE(info._size, 0);
                var nextProcess = this._kernel32.Process32FirstW(h, info);
                while (nextProcess.Val) 
                {
                    pid = info.Deref(8, 4).toBuffer().readUInt32LE(0);
                    retVal[pid] = { pid: pid, cmd: info.Deref(GM.PointerSize == 4 ? 36 : 44, 260).Wide2UTF8 };

                    if ((ph = this._kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid)).Val != -1)
                    {
                        pathSize.toBuffer().writeUInt32LE(fullpath._size);
                        if (this._kernel32.QueryFullProcessImageNameW(ph, 0, fullpath, pathSize).Val != 0)
                        {
                            retVal[pid].path = fullpath.Wide2UTF8;
                        }
                        this._kernel32.CloseHandle(ph);
                    }
                 
                    try
                    {
                        retVal[pid].user = require('user-sessions').getProcessOwnerName(pid).name;
                    }
                    catch(ee)
                    {
                    }
                    
                    nextProcess = this._kernel32.Process32NextW(h, info);
                }
                this._kernel32.CloseHandle(h);
                if (callback) { callback.apply(this, [retVal]); }
                break;
            case 'linux': // Linux processes
                var fallback = false;
                var users = require('fs').existsSync('/etc/login.defs') ? 'user:99' : 'user';
                var p = require('child_process').execFile('/bin/sh', ['sh']);
                p.stdout.str = ''; p.stdout.on('data', function (c) { this.str += c.toString(); });
                p.stderr.str = ''; p.stderr.on('data', function (c) { this.str += c.toString(); });
                p.stdin.write('ps -e -o pid -o ' + users + ' -o args | tr ' + "'\\n' '\\t' | awk -F" + '"\\t" \'');
                p.stdin.write('{');
                p.stdin.write('   printf "{"; ');
                p.stdin.write('   for(i=1;i<NF;++i)');
                p.stdin.write('   { ');
                p.stdin.write('       split($i,A," "); ');

                p.stdin.write('       gsub(/[ \\t]*[0-9]+[ \\t]*[^ ^\\t]+[ \\t]+/,"",$i);');
                p.stdin.write('       gsub(/\\\\/,"\\\\\\\\",$i);');
                p.stdin.write('       gsub(/"/,"\\\\\\"",$i);');
                p.stdin.write('       if($i !~ /^awk /)');
                p.stdin.write('       {');
                p.stdin.write('          printf "%s\\"%s\\":{\\"pid\\":\\"%s\\",\\"user\\":\\"%s\\",\\"cmd\\":\\"%s\\"}",(i==1?"":","),A[1],A[1],A[2],$i;');
                //                                     PID               PID                 USER               command     
                p.stdin.write('       }');
                p.stdin.write('   }');
                p.stdin.write('   printf "}";');
                p.stdin.write("}'\nexit\n");
                p.waitExit();

                if (p.stderr.str.trim() != '')
                {
                    fallback = true;
                    var p = require('child_process').execFile('/bin/sh', ['sh']);
                    p.stdout.str = ''; p.stdout.on('data', function (c) { this.str += c.toString(); });
                    p.stderr.str = ''; p.stderr.on('data', function (c) { this.str += c.toString(); });
                    p.stdin.write("ps | tr '\\n' '`' | awk -F'`' '");
                    p.stdin.write('{');
                    p.stdin.write('   len=split($1,A," "); X=index($1,A[len]);');
                    p.stdin.write('   printf "{"; ');
                    p.stdin.write('   for(i=2;i<NF;++i)');
                    p.stdin.write('   {');
                    p.stdin.write('      split($i,TOK," ");');
                    p.stdin.write('      cmd=substr($i,X);');
                    p.stdin.write('      gsub(/\\\\/,"\\\\\\\\",cmd);');
                    p.stdin.write('      gsub(/"/,"\\\\\\"",cmd);');
                    p.stdin.write('      printf "%s\\"%s\\":{\\"pid\\":\\"%s\\",\\"user\\":\\"%s\\",\\"cmd\\":\\"%s\\"}",(i==2?"":","),TOK[1],TOK[1],TOK[2],cmd;');
                    //                                  PID               PID                 USER               command     
                    p.stdin.write('   }');
                    p.stdin.write('   printf "}";');
                    p.stdin.write("}'\nexit\n");
                    p.waitExit();
                }

                var J = JSON.parse(p.stdout.str);
                if (callback)
                {
                    p.args = [];
                    for (var i = 1; i < arguments.length; ++i) { p.args.push(arguments[i]); }

                    if (fallback)
                    {
                        for(pid in J)
                        {
                            
                            try
                            {
                                if (isNaN(parseInt(J[pid].pid))) { delete J[pid]; continue; }
                                var c = require('fs').readFileSync('/proc/' + pid + '/cmdline');
                                for(i in c)
                                {
                                    if (c[i] == 0) { c[i] = 32; }
                                }
                                c = c.toString().trim();
                                if (c != '' && J[pid].cmd != c) { J[pid].cmd = c; }
                            }
                            catch(ee)
                            {
                            }
                        }
                    }

                    p.args.unshift(J);
                    callback.apply(this, p.args);
                }

                break;
            case 'darwin':
            case 'freebsd':
                var p = require('child_process').execFile('/bin/sh', ['sh']);
                p.stdout.str = ''; p.stdout.on('data', function (c) { this.str += c.toString(); });
                p.stderr.str = ''; p.stderr.on('data', function (c) { this.str += c.toString(); });
                p.stdin.write('ps -axo pid -o user -o command | tr ' + "'\\n' '\\t' | awk -F" + '"\\t" \'{ printf "{"; for(i=2;i<NF;++i) { gsub(/^[ ]+/,"",$i); split($i,tok," "); pid=tok[1]; user=tok[2]; cmd=substr($i,length(tok[1])+length(tok[2])+2); gsub(/\\\\/,"\\\\\\\\&",cmd); gsub(/"/,"\\\\\\\\&",cmd); gsub(/^[ ]+/,"",cmd); printf "%s\\"%s\\":{\\"pid\\":\\"%s\\",\\"user\\":\\"%s\\",\\"cmd\\":\\"%s\\"}",(i!=2?",":""),pid,pid,user,cmd; } printf "}"; }\'\nexit\n');
                p.waitExit();

                if (callback)
                {
                    var J = JSON.parse(p.stdout.str);
                    p.args = [];
                    for (var i = 1; i < arguments.length; ++i) { p.args.push(arguments[i]); }
                    if (process.platform == 'freebsd')
                    {
                        for (pid in J)
                        {
                            try
                            {
                                var c = require('fs').readFileSync('/proc/' + pid + '/cmdline');
                                for (i in c)
                                {
                                    if (c[i] == 0) { c[i] = 32; }
                                }
                                if (c.toString().trim() != '' && J[pid].cmd != c.toString()) { J[pid].cmd = c.toString(); }
                            }
                            catch (ee)
                            {
                            }
                        }
                    }
                    p.args.unshift(J);
                    callback.apply(this, p.args);
                }

                break;
        }
    };

    // Get information about a specific process on Linux
    this.getProcessInfo = function getProcessInfo(pid)
    {
        switch(process.platform)
        {
            default:
                throw ('getProcessInfo() not supported for ' + process.platform);
                break;
            case 'linux':
                var status = require('fs').readFileSync('/proc/' + pid + '/status');
                var info = {};
                var lines = status.toString().split('\n');
                for(var i=0;i<lines.length;++i)
                {
                    var tokens = lines[i].split(':');
                    if (tokens.length > 1) { tokens[1] = tokens[1].trim(); }
                    info[tokens[0]] = tokens[1];
                }
                return (info);
                break;
            case 'win32':
                var ret = new promise(defPromiseHandler);
                ret.server = require('net').createServer();
                ret._clientpath = 'mesh-' + require('uuid/v4')();
                ret._path = '\\\\.\\pipe\\' + ret._clientpath;
                try { ret.server.listen({ path: ret._path }); } catch (e) { throw ('ProcessManagerError: Cannot create connection'); }
                ret.server.on('connection', function (c)
                {
                    c.str = '';
                    this._connection = c;
                    c.on('data', function (chunk)
                    {
                        if (chunk.toString() == '{{{X}}}\r\n')
                        {
                            if (this.str == '') { ret._rej('ProcessManagerError: PID(' + pid + ') NOT FOUND'); return; }
                            var rj = {};
                            var J = this.str.split('\r\n');
                            var keys = J[0].split(',');
                            var values = J[1].split(',');
                            for (var i = 0; i < keys.length; ++i)
                            {
                                if (keys[i][0] == '"')
                                {
                                    keys[i] = keys[i].substring(1, keys[i].length - 1);
                                }
                                if (values[i][0] == '"')
                                {
                                    values[i] = values[i].substring(1, values[i].length - 1);
                                }
                                if (values[i] != '')
                                {
                                    rj[keys[i]] = values[i];
                                }
                            }
                            ret._res(rj);
                            return;
                        }
                        this.str += chunk.toString();
                    });
                });

                ret.child = require('child_process').execFile(process.env['windir'] + '\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', ['powershell', '-noprofile', '-nologo', '-command', '-'], {});
                ret.child.descriptorMetadata = 'process-manager';
                ret.child.stdout.str = ''; ret.child.stdout.on('data', function (c) { this.str += c.toString(); });
                ret.child.stderr.str = ''; ret.child.stderr.on('data', function (c) { this.str += c.toString(); });

                ret.child.stdin.write('[reflection.Assembly]::LoadWithPartialName("system.core")\r\n');
                ret.child.stdin.write('$pipe = new-object System.IO.Pipes.NamedPipeClientStream(".", "' + ret._clientpath + '", 3);\r\n');
                ret.child.stdin.write('$pipe.Connect(); \r\n');
                ret.child.stdin.write('$sw = new-object System.IO.StreamWriter($pipe);\r\n');
                ret.child.stdin.write('$X = Get-Process -IncludeUserName -id ' + pid + ' | ConvertTo-CSV -NoTypeInformation\r\n');
                ret.child.stdin.write('$sw.WriteLine($X[0]); $sw.Flush();\r\n');
                ret.child.stdin.write('$sw.WriteLine($X[1]); $sw.Flush();\r\n');
                ret.child.stdin.write('$sw.WriteLine("{{{X}}}"); $sw.Flush();\r\n');
                ret.child.stdin.write('exit\r\n');

                return (require('promise').wait(ret));
                break;
        }
    };

    if(process.platform != 'win32')
    {
        Object.defineProperty(this, '_pgrep', {
            value: (function ()
            {
                var child = require('child_process').execFile('/bin/sh', ['sh']);
                child.stdout.str = '';
                child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                child.stdin.write("whereis pgrep | awk '{ print $2 }'\nexit\n");
                child.waitExit();
                return (child.stdout.str.trim());
            })()
        });

        if (this._pgrep != '')
        {
            this.getProcess = function getProcess(cmd)
            {
                var child = require('child_process').execFile('/bin/sh', ['sh']);
                child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
                child.stderr.str = ''; child.stderr.on('data', function (c) { this.str += c.toString(); });
                child.stdin.write("pgrep gnome-session | tr '\\n' '\\t' |" + ' awk -F"\\t" \'{ printf "["; for(i=1;i<NF;++i) { if(i>1) { printf ","; } printf "%d", $i; } printf "]"; }\'');
                child.stdin.write('\nexit\n');
                child.waitExit();
                if (child.stderr.str != '') { throw (child.stderr.str.trim()); }
                if (child.stdout.str.trim() == '') { throw (cmd + ' not found'); }

                return (JSON.parse(child.stdout.str.trim()));
            };
        }
    }
    this.getProcessEx = function getProcessEx(cmd, options)
    {
        if (process.platform == 'win32')
        {
            var result = [];
            this.getProcesses(function (j)
            {
                var i;
                for(i in j)
                {
                    if(j[i].cmd.toLowerCase() == cmd.toLowerCase())
                    {
                        if (options == null || options.path.toLowerCase() == j[i].path.toLowerCase())
                        {
                            result.push(j[i].pid);
                        }
                    }
                }
            });
            return (result);
        }

        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stderr.str = ''; child.stderr.on('data', function (c) { this.str += c.toString(); });
        child.stdin.write('ps -ax -o pid -o command | grep ' + cmd + " | tr '\\n' '\\t' | awk -F" + '"\\t" \'{ printf "["; for(i=1;i<NF;++i) { split($i,r," "); if(r[2]!="grep") { if(i>1) { printf ","; } printf "%s", r[1]; } } printf "]"; }\'');
        child.stdin.write('\nexit\n');
        child.waitExit();

        if (child.stdout.str.trim() == '')
        {
            throw (cmd + ' not found');
        }
        else
        {
            return (JSON.parse(child.stdout.str.trim()));
        }
    }
}

module.exports = new processManager();
