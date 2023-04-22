/*
Copyright 2019 - 2022 Intel Corporation

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

try
{
    Object.defineProperty(Array.prototype, 'getParameterEx',
        {
            value: function (name, defaultValue)
            {
                var i, ret;
                for (i = 0; i < this.length; ++i)
                {
                    if (this[i].startsWith(name + '='))
                    {
                        ret = this[i].substring(name.length + 1);
                        if (ret.startsWith('"')) { ret = ret.substring(1, ret.length - 1); }
                        return (ret);
                    }
                }
                return (defaultValue);
            }
        });
    Object.defineProperty(Array.prototype, 'getParameter',
        {
            value: function (name, defaultValue)
            {
                return (this.getParameterEx('--' + name, defaultValue));
            }
        });
}
catch (x)
{ }

function getAutoProxyDomain()
{
    var domain = null;
    try
    {
        domain = _MSH().autoproxy;
    }
    catch (e) { }
    if (domain == null)
    {
        domain = process.argv.getParameter('autoproxy');
    }

    if (domain == null || domain.indexOf('.') < 0) { return (null); }
    if (domain != null && !domain.startsWith('.')) { domain = '.' + domain; }
    return (domain);
}

function linux_getProxy()
{
    console.info1('Checking Proxies [LINUX]');

    // Check Environment Variabels
    if(require('fs').existsSync('/etc/environment'))
    {
        console.info1('Checking global environment settings');
	    var child = require('child_process').execFile('/bin/sh', ['sh']);
	    child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
	    child.stdin.write('cat /etc/environment | grep = | ' + "tr '\\n' '`' | awk -F'`' '");
	    child.stdin.write('{');
	    child.stdin.write('   host=""; port=""; username=""; password=""; ')
	    child.stdin.write('   for(i=1;i<NF;++i)');
	    child.stdin.write('   {');
	    child.stdin.write('      if($i~/^#/) { continue; }');
	    child.stdin.write('      split($i,tokens,"=");');
	    child.stdin.write('      if(tokens[1]=="HTTP_PROXY")');
	    child.stdin.write('      { ');
	    child.stdin.write('         proxy=substr($i,2+length(tokens[1]));');
	    child.stdin.write('         printf "http://%s", proxy;');
	    child.stdin.write('         break;');
	    child.stdin.write('      } ');
	    child.stdin.write('   }');
	    child.stdin.write("}'\nexit\n");
	    child.waitExit();
	    if (child.stdout.str.trim() != '')
	    {
	        console.info1(' => FOUND: ' + child.stdout.str.trim());
	        return (child.stdout.str.trim());
	    }
    }

    // Check profile.d
    if(require('fs').existsSync('/etc/profile.d/proxy_setup'))
    {
        console.info1('Checking profile.d');
	    var child = require('child_process').execFile('/bin/sh', ['sh']);
	    child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
	    child.stdin.write("cat /etc/profile.d/proxy_setup | awk '" + '{ split($2, tok, "="); if(tok[1]=="http_proxy") { print tok[2]; }}\'\nexit\n');
	    child.waitExit();
	    console.info1(' => FOUND: ' + child.stdout.str.trim().split('\n')[0]);
	    return (child.stdout.str.trim().split('\n')[0]);
    }

    // check apt proxy setting fro /etc/apt/apt.conf.d/proxy.conf
    if (require('fs').existsSync('/etc/apt/apt.conf.d/proxy.conf'))
    {
        console.info1('Checking apt package manager settings [proxy.conf]');
        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stderr.on('data', function (c) { console.log(c.toString()); });
        child.stdin.write("cat /etc/apt/apt.conf.d/proxy.conf | tr '\\n' '`' | awk -F'`' '");
        child.stdin.write('{');
        child.stdin.write('   for(n=1;n<NF;++n) ');
        child.stdin.write('   {');
        child.stdin.write('      if($n~/^#/) { continue; }')
        child.stdin.write('      if($n~/^Acquire::http::proxy /)');
        child.stdin.write('      {');
        child.stdin.write('         split($n, dummy, "Acquire::http::proxy ");');
        child.stdin.write('         print substr(dummy[2],2,length(dummy[2])-3);');
        child.stdin.write('         break;');
        child.stdin.write('      }');
        child.stdin.write('   }');
        child.stdin.write("}'\nexit\n");
        child.waitExit();
        if (child.stdout.str.trim() != "")
        {
            console.info1(' => FOUND: ' + child.stdout.str.trim());
            return (child.stdout.str.trim());
        }
    }

    // check apt proxy setting from /etc/apt/apt/apt.conf
    if (require('fs').existsSync('/etc/apt/apt.conf'))
    {
        console.info1('Checking apt package manager settings [apt.conf]');
        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stderr.on('data', function (c) { console.log(c.toString()); });
        child.stdin.write("cat /etc/apt/apt.conf | tr '\\n' '`' | awk -F'`' '");
        child.stdin.write('{');
        child.stdin.write('   for(n=1;n<NF;++n) ');
        child.stdin.write('   {');
        child.stdin.write('      if($n~/^#/) { continue; }')
        child.stdin.write('      if($n~/^Acquire::http::proxy /)');
        child.stdin.write('      {');
        child.stdin.write('         split($n, dummy, "Acquire::http::proxy ");');
        child.stdin.write('         print substr(dummy[2],2,length(dummy[2])-3);');
        child.stdin.write('         break;');
        child.stdin.write('      }');
        child.stdin.write('   }');
        child.stdin.write("}'\nexit\n");
        child.waitExit();
        if (child.stdout.str.trim() != "")
        {
            console.info1(' => FOUND: ' + child.stdout.str.trim());
            return (child.stdout.str.trim());
        }
    }


    // check yum proxy setting
    if (require('fs').existsSync('/etc/yum.conf'))
    {
        console.info1('Checking yum package manager settings');
        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stderr.on('data', function (c) { console.log(c.toString()); });
        child.stdin.write('cat /etc/yum.conf | grep "proxy" | ' + "tr '\\n' '`' | awk -F'`' '");
        child.stdin.write('{');
        child.stdin.write('   host=""; port=""; username=""; password="";');
        child.stdin.write('   for(n=1;n<NF;++n)');
        child.stdin.write('   {');
        child.stdin.write('      if($n~/^#/) { continue; }');
        child.stdin.write('      split($n,tokens,"=");');
        child.stdin.write('      if(tokens[1]=="proxy")');
        child.stdin.write('      {');
        child.stdin.write('         split(tokens[2],dummy,"://");');
        child.stdin.write('         split(dummy[2],url,":");');
        child.stdin.write('         host = url[1];');
        child.stdin.write('         port = url[2]; if(port=="") { port = "8080"; }');
        child.stdin.write('      }');
        child.stdin.write('      if(tokens[1]=="proxy_username") { username = tokens[2]; }');
        child.stdin.write('      if(tokens[1]=="proxy_password") { password = tokens[2]; }');
        child.stdin.write('   }');
        child.stdin.write('   if(host!="" && port!="")');
        child.stdin.write('   {');
        child.stdin.write('      if(username!="" && password!="")');
        child.stdin.write('      {');
        child.stdin.write('         printf "http://%s:%s@%s:%s", username, password, host, port; ');
        child.stdin.write('      }');
        child.stdin.write('      else');
        child.stdin.write('      {');
        child.stdin.write('         printf "http://%s:%s", host, port; ');
        child.stdin.write('      }');
        child.stdin.write('   }');
        child.stdin.write("}'\nexit\n");
        child.waitExit();
        if (child.stdout.str.trim() != "")
        {
            console.info1(' => FOUND: ' + child.stdout.str.trim());
            return (child.stdout.str.trim());
        }
    }

    // openSUSE proxy setting
    if (require('fs').existsSync('/etc/sysconfig/proxy'))
    {
        console.info1('Checking sysconfig settings');
        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stderr.on('data', function (c) { });
        child.stdin.write('cat /etc/sysconfig/proxy /root/.curlrc | grep = | ' + "tr '\\n' '`' | awk -F'`' '");
        child.stdin.write('{');
        child.stdin.write('   proxy=""; enabled=""; username=""; password=""; ')
        child.stdin.write('   for(i=1;i<NF;++i)');
        child.stdin.write('   {');
        child.stdin.write('      if($i~/^#/) { continue; }');
        child.stdin.write('      split($i,tokens,"=");');
        child.stdin.write('      if(tokens[1]=="PROXY_ENABLED")');
        child.stdin.write('      {');
        child.stdin.write('         split(tokens[2],dummy,"\\"");');
        child.stdin.write('         enabled = dummy[2];');
        child.stdin.write('      }');
        child.stdin.write('      if(tokens[1]=="HTTP_PROXY")');
        child.stdin.write('      { ');
        child.stdin.write('         split(tokens[2],dummy,"\\"");');
        child.stdin.write('         proxy = dummy[2];');
        child.stdin.write('      } ');
        child.stdin.write('      if(tokens[1]~/^proxy-user/)');
        child.stdin.write('      {');
        child.stdin.write('         cred = substr($i,1+index($i,"="));');
        child.stdin.write('         cred = substr(cred, index(cred, "\\""));');
        child.stdin.write('         if(cred~/^"/) { cred = substr(cred,2,length(cred)-2); }');
        child.stdin.write('         username=substr(cred,0,index(cred,":")-1);');
        child.stdin.write('         password=substr(cred,1+index(cred,":"));');
        child.stdin.write('      }');
        child.stdin.write('   }');
        child.stdin.write('   if(enabled=="yes" && proxy!="") ');
        child.stdin.write('   {');
        child.stdin.write('      if(username=="" || password=="") ');
        child.stdin.write('      {');
        child.stdin.write('         print proxy;');
        child.stdin.write('      }');
        child.stdin.write('      else ');
        child.stdin.write('      {');
        child.stdin.write('         split(proxy,dummy, "://");');
        child.stdin.write('         printf "%s://%s:%s@%s", dummy[1], username, password, dummy[2];');
        child.stdin.write('      }');
        child.stdin.write('   }');
        child.stdin.write("}'\nexit\n");
        child.waitExit();

        if (child.stdout.str.trim() != '')
        {
            console.info1(' => FOUND: ' + child.stdout.str.trim());
            return (child.stdout.str.trim());
        }
    }

    if(require('fs').existsSync('/etc/login.conf'))
    {
        console.info1('Checking login.conf settings');
        var child = require('child_process').execFile('/bin/sh', ['sh']);
        child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
        child.stdin.write("cat /etc/login.conf | tr '\\n' '`' | awk -F'`' '");
        child.stdin.write('{');
        child.stdin.write('   printf "{";');
        child.stdin.write('   group=""; first=1; firstprop=0;')
        child.stdin.write('   for(i=1;i<NF;++i) ');
        child.stdin.write('   {');
        child.stdin.write('       a=split($i,tok,":"); ');
        child.stdin.write('       if(split(tok[1],dummy,"#")==1 && split(tok[1],dummy2," ")==1)');
        child.stdin.write('       { ');
        child.stdin.write('           if(group != "") { printf "}"; }');
        child.stdin.write('           group = tok[1]; firstprop=1;');
        child.stdin.write('           printf "%s\\"%s\\": {", (first==0?",":""), tok[1];');
        child.stdin.write('           first=0;');
        child.stdin.write('       }');
        child.stdin.write('       else ');
        child.stdin.write('       {');
        child.stdin.write('          if(group != "" && split($i,dummy3,"\\\\")>1 && split($i, dummy4, "#")==1)');
        child.stdin.write('          {');
        child.stdin.write('              if(split($i,key1,"=")==1)');
        child.stdin.write('              {');
        child.stdin.write('                  split($i,key2,":");');
        child.stdin.write('                  if(key2[2]!="\\\\")');
        child.stdin.write('                  {');
        child.stdin.write('                     printf "%s\\"%s\\": null",(firstprop==0?",":""),key2[2];');
        child.stdin.write('                     firstprop=0;')
        child.stdin.write('                  }');
        child.stdin.write('              }');
        child.stdin.write('              else');
        child.stdin.write('              {');
        child.stdin.write('                 tmp = substr($i,2+length(key1[1]));');
        child.stdin.write('                 split(tmp,dummy,"\\\\");');
        child.stdin.write('                 tmp=substr(tmp,0,length(tmp)-2);');
        child.stdin.write('                 split(key1[1],keyname,":");');
        child.stdin.write('                 printf "%s\\"%s\\": \\"%s\\"", (firstprop==0?",":""), keyname[2], tmp;');
        child.stdin.write('                 firstprop=0;');
        child.stdin.write('              }');
        child.stdin.write('          }');
        child.stdin.write('       }');
        child.stdin.write('   }');
        child.stdin.write('   if(group!="") { printf "}"; }')
        child.stdin.write('   printf "}";');
        child.stdin.write("}'");
        child.stdin.write('\nexit\n');
        child.waitExit();
        if (child.stdout.str.trim() != '')
        {
            var config = null;
            try
            {
                config = JSON.parse(child.stdout.str);
            }
            catch (e)
            {
            }

            if(config)
            {
                // check root
                if(config.root && config.root.setenv)
                {
                    var i, tokens;
                    var items = config.root.setenv.split(',');
                    for(i=0;i<items.length;++i)
                    {
                        tokens = items[i].split('=');
                        if(tokens[0] == 'https_proxy' || tokens[0] == 'http_proxy')
                        {
                            console.info1(' => FOUND: ' + tokens[1].trim());
                            return (tokens[1].trim());
                        }
                    }
                }

                // check default
                if (config.default && config.default.setenv)
                {
                    var i, tokens;
                    var items = config.default.setenv.split(',');
                    for (i = 0; i < items.length; ++i)
                    {
                        tokens = items[i].split('=');
                        if (tokens[0] == 'https_proxy' || tokens[0] == 'http_proxy')
                        {
                            console.info1(' => FOUND: ' + tokens[1].trim());
                            return (tokens[1].trim());
                        }
                    }
                }
            }
        }
    }

    // Check gsettings
    if (require('fs').existsSync('/usr/bin/gsettings'))
    {
        // Start by seeing if we are running as user
        var checkId = require('user-sessions').Self();
        if (checkId == 0)
        {
            var checkLoggedInUser = true;

            // Running as root, so check which user installed us
            try
            {
                if ((require('MeshAgent').getStartupOptions().installedByUser) != null)
                {
                    checkId = require('MeshAgent').getStartupOptions().installedByUser;
                    checkLoggedInUser = false;
                }
            }
            catch (e)
            {
            }

            if (checkLoggedInUser)
            {
                try
                {
                    // Don't know who installed us, so see if anyone is logged in
                    checkId = require('user-sessions').consoleUid();
                }
                catch (e)
                {
                    // Unable to determine which user to probe
                    checkId = 0;
                }
            }
        }

        console.info1('Checking gsettings with UID: ' + checkId);
        var setting = require('linux-gnome-helpers').getProxySettings(checkId);
        if (setting.mode == 'manual')
        {
            if (setting.authEnabled)
            {
                console.info1(' => FOUND: http://' + setting.username + ':' + setting.password + '@' + setting.host + ':' + setting.port);

                return ('http://' + setting.username + ':' + setting.password + '@' + setting.host + ':' + setting.port);
            }
            else
            {
                console.info1(' => FOUND: http://' + setting.host + ':' + setting.port);
                return ('http://' + setting.host + ':' + setting.port);
            }
        }
    }
    console.info1('NO PROXIES settings detected');
    throw ('No proxies');
}
function posix_proxyCheck(uid, checkAddr)
{
    var g;
    var x = process.env['no_proxy'] ? process.env['no_proxy'].split(',') : [];
    var t;

    if (require('linux-gnome-helpers').available && (g = require('linux-gnome-helpers').getProxySettings(uid)).mode != 'none')
    {
        x = g.exceptions;
    }

    for(var i in x)
    {
        if (x[i] == checkAddr) { return (true); }               // Direct Match
        if (checkAddr.endsWith('.' + x[i])) { return (true); }  // Subdomain Match
        if ((v = x[i].split('/')).length == 2)
        {
            try
            {
                if(require('ip-address').Address4.fromString(v[0]).mask(parseInt(v[1])) == require('ip-address').Address4.fromString(checkAddr).mask(parseInt(v[1])))
                {
                    return(true);
                }
            }
            catch (ex)
            {
            }
        }
    }
    return (false);
}

function windows_getUserRegistryKey()
{
    var i;
    if ((i = require('user-sessions').getProcessOwnerName(process.pid)).tsid == 0)
    {
        // We are a service, so we should check the user that installed the Mesh Agent
        try
        {
            key = require('win-registry').QueryKey(require('win-registry').HKEY.LocalMachine, 'SYSTEM\\CurrentControlSet\\Services\\Mesh Agent', '_InstalledBy');
        }
        catch (xx)
        {
            // This info isn't available, so let's try to use the currently logged in user
            try
            {
                key = require('win-registry').usernameToUserKey(require('user-sessions').getUsername(require('user-sessions').consoleUid()));
            }
            catch (xxx)
            {
                // No users are logged in, so as a last resort, let's try the last logged in user.
                var entries = require('win-registry').QueryKey(require('win-registry').HKEY.Users);
                for (i in entries.subkeys)
                {
                    if (entries.subkeys[i].split('-').length > 5 && !entries.subkeys[i].endsWith('_Classes'))
                    {
                        key = entries.subkeys[i];
                        break;
                    }
                }
            }
        }
    }
    else
    {
        // We are a logged in user
        key = require('win-registry').usernameToUserKey(i.name);
    }
    if (!key) { throw ('Could not determine which user proxy setting to query'); }
    return (key);
}
function windows_proxyCheck(key, checkAddr)
{
    if (!key) { key = windows_getUserRegistryKey(); }

    var proxyOverride = require('win-registry').QueryKey(require('win-registry').HKEY.Users, key + '\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings', 'ProxyOverride').split(';');
    for(var i in proxyOverride)
    {
        proxyOverride[i] = proxyOverride[i].trim();
        if ((checkAddr == '127.0.0.1' || checkAddr == '::1') && proxyOverride[i] == '<local>') { return (true); }
        if (checkAddr == proxyOverride[i]) { return (true); } // Exact Match
        if (proxyOverride[i].startsWith('*.') && checkAddr.endsWith(proxyOverride[i].substring(1))) { return (true); }
        if (proxyOverride[i].endsWith('.*') && checkAddr.startsWith(proxyOverride[i].substring(0, proxyOverride[i].length - 1))) { return (true); }
    }
    return (false);
}

function macos_getProxy()
{
    var child = require('child_process').execFile('/bin/sh', ['sh']);
    child.stdout.str = ''; child.stdout.on('data', function (c) { this.str += c.toString(); });
    child.stderr.str = ''; child.stderr.on('data', function (c) { this.str += c.toString(); });
    child.stdin.write("scutil --proxy | tr '\\n' '`' | awk -F'`' '");
    child.stdin.write('{');
    child.stdin.write('   pstart=0;')
    child.stdin.write('   for(i=1;i<NF;++i)');
    child.stdin.write("   {");
    child.stdin.write('      if(split($i,dummy,"ExceptionsList ")>1)');
    child.stdin.write("      {");
    child.stdin.write('          printf "{ \\"exceptions\\": [";');
    child.stdin.write('          ++i;');
    child.stdin.write('          fstart=1; pstart=1;');
    child.stdin.write('          for(;i<NF;++i)');
    child.stdin.write('          {');
    child.stdin.write('             if(split($i,dummy,"}")>1) { break; } ');
    child.stdin.write('             split($i, val, " : ");');
    child.stdin.write('             printf "%s\\"%s\\"", (fstart==0?",":""), val[2];');
    child.stdin.write('             fstart=0;');
    child.stdin.write('          }');
    child.stdin.write('          printf "]";');
    child.stdin.write('          continue;');
    child.stdin.write("      }");
    child.stdin.write('      else');
    child.stdin.write('      {');
    child.stdin.write('         if(pstart==1 && split($i,dummy,"}")==1)');
    child.stdin.write('         {');
    child.stdin.write('            split($i,tok," : ");');
    child.stdin.write('            split(tok[1],key," ");');
    child.stdin.write('            printf ",\\"%s\\": \\"%s\\"", key[1], tok[2];');
    child.stdin.write('         }');
    child.stdin.write('      }')
    child.stdin.write("   }");
    child.stdin.write('   printf "}";');
    child.stdin.write("}'\nexit\n");
    child.waitExit();
    if(child.stdout.str != '')
    {
        try
        {
            var p = JSON.parse(child.stdout.str);
            if(p.HTTPEnable == "1")
            {
                return('http://' + p.HTTPProxy + ':' + p.HTTPPort);
            }
        }
        catch(e)
        {
            console.log(e);
        }
    }
    throw ('No Proxies');
}

function windows_getProxy()
{
    var isroot = false;
    var key, value;

    key = windows_getUserRegistryKey();
    try
    {
        if (require('win-registry').QueryKey(require('win-registry').HKEY.Users, key + '\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings', 'ProxyEnable') == 1)
        {
            // Proxy is enabled
            return (require('win-registry').QueryKey(require('win-registry').HKEY.Users, key + '\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings', 'ProxyServer'));
        }
    }
    catch(e)
    {
        throw ('No proxies');
    }
}

function auto_proxy_helper(target)
{
    // The first thing we need to do, is disable any existing proxy settings, otherwise we won't be able to find the autoconfig script
    require('global-tunnel').end();

    var promise = require('promise');

    var ret = new promise(promise.defaultInit);
    if (!this.enabled) { ret.resolve(null); return (ret); }

    var wpadip = resolve('wpad' + (this.domain != null ? this.domain : ''));

    if (wpadip.length == 0)
    {
        ret.resolve(null);
        return (ret);
    }

    ret.target = target;
    ret.r = require('http').get('http://' + wpadip[0] + '/wpad.dat');
    ret.r.p = ret;
    ret.r.on('response', function (img)
    {
        if (img.statusCode == 200 && img.headers['Content-Type'] == 'application/x-ns-proxy-autoconfig')
        {
            img.wpad = '';
            this.i = img;
            this.i.p = this.p;
            img.on('data', function (c)
            {
                this.wpad += c.toString();
            });
            img.on('end', function ()
            {
                var z = require('PAC').Create(this.wpad);
                this.p.resolve(z(this.p.target));
            });
        }
        else
        {
            this.p.resolve(null);
        }
    });
    ret.r.on('error', function () { this.p.resolve(null); });
    return (ret);
}


switch (process.platform)
{
    case 'linux':
    case 'freebsd':
        module.exports = { ignoreProxy: posix_proxyCheck, getProxy: linux_getProxy };
        break;
    case 'win32':
        module.exports = { ignoreProxy: windows_proxyCheck, getProxy: windows_getProxy };
        break;
    case 'darwin':
        module.exports = { getProxy: macos_getProxy };
        break;
}
module.exports.autoHelper = auto_proxy_helper;
module.exports.domain = getAutoProxyDomain();

Object.defineProperty(module.exports, 'auto',
    {
        get: function ()
        {
            if (this.enabled)
            {
                var result = resolve('wpad' + (this.domain != null ? this.domain : ''));
                return (result.length > 0);
            }
            else
            {
                return (false);
            }
        }
    });

Object.defineProperty(module.exports, 'enabled',
    {
        get: function ()
        {
            var domain = null;
            try
            {
                domain = _MSH().autoproxy;
            }
            catch (e) { }
            if (domain == null)
            {
                domain = process.argv.getParameter('autoproxy');
            }
            if (domain != null)
            {
                if (domain.indexOf('.') >= 0) { domain = 1; }
            }
            return (domain == 1 || domain == '"1"');
        }
    })
