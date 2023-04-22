/*   
Copyright 2006 - 2019 Intel Corporation

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

// When compiled in 32bit, this module uses Windows CAPI which is compatible with Windows XP.
// When compiled in 64bit, this module uses Windows CNG (Crypto Next Gen) that is compatible and will use TPM modules.

#if !defined(_NOCAPI) && defined(WIN32)

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <wincrypt.h>

extern "C"
{
#include "wincrypto.h"
#include "../microstack/ILibParsers.h"
#ifdef _WIN64
HCRYPTPROV wincrypto_hProv = NULL;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
LPWSTR wincrypto_CngProviders[3] = { L"Microsoft Platform Crypto Provider", MS_KEY_STORAGE_PROVIDER, NULL };
LPWSTR wincrypto_containername = L"MeshAgentContainerNG";
LPCTSTR wincrypto_subject = "CN=MeshNodeCertificateNG";
#else
NCRYPT_PROV_HANDLE wincrypto_hProv = NULL;
LPWSTR wincrypto_containername = L"MeshAgentContainer";
LPCTSTR wincrypto_subject = "CN=MeshNodeCertificate";
LPCWSTR wincrypto_CapiProviders[3] = { L"Intel IPT Cryptographic Provider", NULL, NULL };
DWORD wincrypto_CapiProvidersTypes[3] = { PROV_RSA_AES, PROV_RSA_AES, 0xFFFFFFFF };
#endif
HANDLE wincrypto_hCertStore = NULL;
PCCERT_CONTEXT wincrypto_certCtx = NULL;
#define MY_ENCODING_TYPE (PKCS_7_ASN_ENCODING | X509_ASN_ENCODING)

#define wincrypto_TlsServerOid  "1.3.6.1.5.5.7.3.1"				// TLS Server certificate
#define wincrypto_TlsClientOid	"1.3.6.1.5.5.7.3.2"				// TLS Client certificate
#define wincrypto_AmtConsoleOid	"2.16.840.1.113741.1.2.1"		// Intel AMT Remote Console
#define wincrypto_AmtAgentOid	"2.16.840.1.113741.1.2.2"		// Intel AMT Remote Agent
#define wincrypto_AmtSetupOid	"2.16.840.1.113741.1.2.3"		// Intel AMT Local Console
const char* wincrypto_ServerOids[1] = { wincrypto_TlsServerOid };
const char* wincrypto_ClientOids[4] = { wincrypto_TlsClientOid, wincrypto_AmtConsoleOid, wincrypto_AmtAgentOid, wincrypto_AmtSetupOid };

#ifndef RSA2048BIT_KEY
#define RSA2048BIT_KEY 0x08000000
#endif

#ifndef RSA3072BIT_KEY
#define RSA3072BIT_KEY 0x0C000000
#endif

void __fastcall wincrypto_setregistry(LPWSTR name, LPWSTR value)
{
	HKEY hKey;
#ifdef _WINSERVICE
	// If running as a Windows Service, save the key in LOCAL_MACHINE
	if(RegCreateKeyW(HKEY_LOCAL_MACHINE, L"Software\\Open Source\\MeshAgent2", &hKey) == ERROR_SUCCESS)
#else
	// If running in Console mode, save the key in CURRENT_USER
	if(RegCreateKeyW(HKEY_CURRENT_USER, L"Software\\Open Source\\MeshAgent2", &hKey) == ERROR_SUCCESS)
#endif
	{
		RegSetValueExW(hKey, name, 0, REG_SZ, (BYTE*)value, (DWORD)(wcslen(value) * 2));
		RegCloseKey(hKey);
	}
}

void __fastcall wincrypto_setregistryA(char* name, char* value)
{
	HKEY hKey;
#ifdef _WINSERVICE
	// If running as a Windows Service, save the key in LOCAL_MACHINE
	if(RegCreateKeyW(HKEY_LOCAL_MACHINE, L"Software\\Open Source\\MeshAgent2", &hKey) == ERROR_SUCCESS)
#else
	// If running in Console mode, save the key in CURRENT_USER
	if(RegCreateKeyW(HKEY_CURRENT_USER, L"Software\\Open Source\\MeshAgent2", &hKey) == ERROR_SUCCESS)
#endif
	{
		RegSetValueExA(hKey, name, 0, REG_SZ, (BYTE*)value, (DWORD)strlen(value));
		RegCloseKey(hKey);
	}
}

int __fastcall wincrypto_getregistry(LPCWSTR name, char** value)
{
	HKEY hKey;
	DWORD len = 0;
#ifdef _WINSERVICE
	// If running as a Windows Service, open the key in LOCAL_MACHINE
	if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Open Source\\MeshAgent2", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS ) { *value = NULL; return 0;}
#else
	// If running in Console mode, save the key in CURRENT_USER
	if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Open Source\\MeshAgent2", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS ) { *value = NULL; return 0;}
#endif
	if (RegQueryValueExW(hKey, name, NULL, NULL, NULL, &len ) != ERROR_SUCCESS || len == 0) { *value = NULL; RegCloseKey(hKey); return 0;}
	if ((*value = (char*)malloc(len)) == NULL) ILIBCRITICALEXIT(254);
	if (RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)(*value), &len ) != ERROR_SUCCESS || len == 0) { free(*value); *value = NULL; RegCloseKey(hKey); return 0;}
	RegCloseKey(hKey);
	return len;
}

int __fastcall wincrypto_getregistryA(char* name, char** value)
{
	HKEY hKey;
	DWORD len = 0;
#ifdef _WINSERVICE
	// If running as a Windows Service, open the key in LOCAL_MACHINE
	if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Open Source\\MeshAgent2", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS ) { *value = NULL; return 0;}
#else
	// If running in Console mode, save the key in CURRENT_USER
	if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Open Source\\MeshAgent2", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS ) { *value = NULL; return 0;}
#endif
	if (RegQueryValueExA(hKey, name, NULL, NULL, NULL, &len ) != ERROR_SUCCESS || len == 0) { *value = NULL; RegCloseKey(hKey); return 0;}
	if ((*value = (char*)malloc(len)) == NULL) ILIBCRITICALEXIT(254);
	if (RegQueryValueExA(hKey, name, NULL, NULL, (LPBYTE)(*value), &len ) != ERROR_SUCCESS || len == 0) { free(*value); *value = NULL; RegCloseKey(hKey); return 0;}
	RegCloseKey(hKey);
	return len;
}

int __fastcall wincrypto_isopen()
{
	return (wincrypto_hProv != NULL && wincrypto_hCertStore != NULL && wincrypto_certCtx != NULL);
}

void __fastcall wincrypto_close()
{
	if (wincrypto_certCtx != NULL) { CertFreeCertificateContext(wincrypto_certCtx); wincrypto_certCtx = NULL; }
#ifdef _WIN64
	if (wincrypto_hProv != NULL) { NCryptFreeObject(wincrypto_hProv); wincrypto_hProv = NULL; }
#else
	if (wincrypto_hProv != NULL) { CryptReleaseContext(wincrypto_hProv, 0); wincrypto_hProv = NULL; }
#endif
	if (wincrypto_hCertStore != NULL) { CertCloseStore(wincrypto_hCertStore, 0); wincrypto_hCertStore = NULL; }
}

int __fastcall wincrypto_open(int newcert)
{
#ifdef _WIN64
	DWORD KeyLength = 3072;
	NCRYPT_KEY_HANDLE hKeyNode = NULL;
#else
	CRYPT_KEY_PROV_INFO ckp;
	HCRYPTKEY hKeyNode = NULL;
	DWORD providerType = 0;
	DWORD len;
#endif
	LPWSTR providerName = NULL;
	char* providerNameA = NULL;
	DWORD r = 0;
	DWORD hKeyNodeSpec = 0;
	BOOL hFreeKeyNode = FALSE;
	BOOL status = FALSE;
	PBYTE subjectEncoded = NULL;
	DWORD subjectEncodedSize;
	CRYPT_KEY_PROV_INFO kpi;
#ifdef _CONSOLE
	DWORD machineflag = 0;
#else
	DWORD machineflag = CRYPT_MACHINE_KEYSET;
#endif

	SYSTEMTIME st1;
	SYSTEMTIME st2;
	CERT_EXTENSIONS exts;
	CERT_NAME_BLOB sib;
	CRYPT_ALGORITHM_IDENTIFIER sa;
	
	CERT_EXTENSION certExtension[2];
	BYTE* pbPolicyInfo2 = NULL;
	CERT_BASIC_CONSTRAINTS2_INFO BasicConstraints;
	BYTE* pbBasicConstraints = NULL;
	CRYPT_BIT_BLOB keyusage2;
	DWORD pkSize = 0;
	
	ZeroMemory(&exts, sizeof(exts));
	wincrypto_close();

#ifdef _WIN64
	// Open the best CNG possible
	while (providerName == NULL && wincrypto_CngProviders[r] != NULL) {
		providerName = wincrypto_CngProviders[r];
		NCryptOpenStorageProvider(&wincrypto_hProv, providerName, 0);
		if (wincrypto_hProv == NULL) providerName = NULL;
		r++;
	}
	if (wincrypto_hProv == NULL) goto error;
#else
	// Open the best CSP possible with an existing Mesh container
	status = FALSE;
	while (status == FALSE && wincrypto_CapiProvidersTypes[r] != 0xFFFFFFFF) {
		providerName = (LPWSTR)wincrypto_CapiProviders[r];
		providerType = wincrypto_CapiProvidersTypes[r];
		if (newcert) { status = CryptAcquireContextW(&wincrypto_hProv, wincrypto_containername, wincrypto_CapiProviders[r], wincrypto_CapiProvidersTypes[r], machineflag | CRYPT_DELETEKEYSET); status = FALSE; }
		else status = CryptAcquireContextW(&wincrypto_hProv, wincrypto_containername, wincrypto_CapiProviders[r], wincrypto_CapiProvidersTypes[r], machineflag);
		r++;
	}

	// If a container does not already exist, create one with the best provider
	if (status == FALSE)
	{
		r = 0;
		while (status == FALSE && wincrypto_CapiProvidersTypes[r] != 0xFFFFFFFF) {
			providerName = (LPWSTR)wincrypto_CapiProviders[r];
			providerType = wincrypto_CapiProvidersTypes[r];
			status = CryptAcquireContextW(&wincrypto_hProv, wincrypto_containername, wincrypto_CapiProviders[r], wincrypto_CapiProvidersTypes[r], machineflag | CRYPT_NEWKEYSET);
			r++;
		}
	}
	if (!status) goto error;
#endif
	
	// Create cert subject string in format csp understands
	if (!CertStrToName(X509_ASN_ENCODING, wincrypto_subject, CERT_X500_NAME_STR, NULL, NULL, &subjectEncodedSize, NULL)) goto error;
	if ((subjectEncoded = (PBYTE)malloc(subjectEncodedSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CertStrToName(X509_ASN_ENCODING, wincrypto_subject, CERT_X500_NAME_STR, NULL, subjectEncoded, &subjectEncodedSize, NULL)) goto error;
	sib.cbData = subjectEncodedSize;
	sib.pbData = subjectEncoded;
	 
#ifdef _CONSOLE
	wincrypto_hCertStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, wincrypto_hProv, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG, L"MY"); // CERT_STORE_NO_CRYPT_RELEASE_FLAG
	if (!wincrypto_hCertStore) goto error;
#else
	wincrypto_hCertStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, wincrypto_hProv, CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, L"MY"); // CERT_STORE_NO_CRYPT_RELEASE_FLAG // CERT_SYSTEM_STORE_LOCAL_MACHINE
	if (!wincrypto_hCertStore) goto error;
#endif

	// Look for cert and if exists, delete it
	wincrypto_certCtx = CertFindCertificateInStore(wincrypto_hCertStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_NAME, &sib, NULL );

	// Check if we can get the private key
	if (wincrypto_certCtx != NULL)
	{
#ifdef _WIN64
		if (!CryptAcquireCertificatePrivateKey(wincrypto_certCtx, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG, NULL, &hKeyNode, &hKeyNodeSpec, &hFreeKeyNode)) { newcert = 1; }
		if (hKeyNodeSpec != CERT_NCRYPT_KEY_SPEC) { newcert = 1; } // If this private key is not CNG, don't use it.
		if (hFreeKeyNode && hKeyNode != NULL) { if (hKeyNodeSpec == CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(hKeyNode); else CryptReleaseContext(hKeyNode, 0); }
#else
		if (!CryptAcquireCertificatePrivateKey(wincrypto_certCtx, CRYPT_ACQUIRE_SILENT_FLAG, NULL, &hKeyNode, &hKeyNodeSpec, &hFreeKeyNode)) { newcert = 1; }
		if (hFreeKeyNode && hKeyNode != NULL) { CryptReleaseContext(hKeyNode, 0); }
#endif
	}

	// Check if have a certificate already, or need to create a new one
	if (wincrypto_certCtx != NULL && newcert == 0) goto end;
	if (wincrypto_certCtx) { status = CertDeleteCertificateFromStore(wincrypto_certCtx); if (!status) goto error; wincrypto_certCtx = NULL; }

	// Generate node RSA key-pair
#ifdef _WIN64
#ifdef _CONSOLE
    if (FAILED(status = NCryptCreatePersistedKey(wincrypto_hProv, &hKeyNode, BCRYPT_RSA_ALGORITHM, wincrypto_containername, 0, NCRYPT_OVERWRITE_KEY_FLAG))) goto error;
#else
    if (FAILED(status = NCryptCreatePersistedKey(wincrypto_hProv, &hKeyNode, BCRYPT_RSA_ALGORITHM, wincrypto_containername, 0, NCRYPT_MACHINE_KEY_FLAG | NCRYPT_OVERWRITE_KEY_FLAG))) goto error;
#endif
	if (FAILED(status = NCryptSetProperty(hKeyNode, NCRYPT_LENGTH_PROPERTY, (PBYTE)&KeyLength, 4, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG))) {
		KeyLength = 2048; // If 3072 is not supported, go down to 2048.
		if (FAILED(status = NCryptSetProperty(hKeyNode, NCRYPT_LENGTH_PROPERTY, (PBYTE)&KeyLength, 4, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG))) { goto error; }
	}
	if (FAILED(status = NCryptFinalizeKey(hKeyNode, NCRYPT_SILENT_FLAG))) { goto error; } // Ask for silent create, this will fail if not admin.

	// Create self signed cert 
	ZeroMemory(&kpi, sizeof(kpi));
	kpi.pwszContainerName = wincrypto_containername;
	kpi.pwszProvName = providerName;
	kpi.dwProvType = 0;
	kpi.dwFlags = 0;
	kpi.dwKeySpec = 0;
#else
	if (!CryptGenKey(wincrypto_hProv, AT_KEYEXCHANGE, RSA3072BIT_KEY, &hKeyNode)) goto error;

	// Create self signed cert 
	ZeroMemory(&kpi, sizeof(kpi));
	kpi.pwszContainerName = wincrypto_containername;
	kpi.pwszProvName = providerName;
	kpi.dwProvType = providerType;
	kpi.dwKeySpec = AT_KEYEXCHANGE;
#ifdef _CONSOLE
	ckp.dwFlags = CERT_SET_KEY_CONTEXT_PROP_ID;
#else
	ckp.dwFlags = CERT_SET_KEY_CONTEXT_PROP_ID | CRYPT_MACHINE_KEYSET;
#endif
#endif

	ZeroMemory(&certExtension, sizeof(certExtension));

	// Set key usage for root certificate
	BYTE ByteData = CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_OFFLINE_CRL_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE;
	ZeroMemory(&keyusage2, sizeof(keyusage2));
	keyusage2.cbData = 1;
	keyusage2.pbData = &ByteData;

	// Encode key usage
	if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, (LPVOID)&keyusage2, NULL, &pkSize)) goto end;
	if ((pbPolicyInfo2 = (BYTE*)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, (LPVOID)&keyusage2, pbPolicyInfo2, &pkSize)) goto end;
	certExtension[0].pszObjId = szOID_KEY_USAGE;
	certExtension[0].fCritical = FALSE;
	certExtension[0].Value.cbData = pkSize;
	certExtension[0].Value.pbData = pbPolicyInfo2;

	// Encode Basic Constraints
	// --> Subject Type=CA
	// --> Path Length Constraint=None
     
	ZeroMemory(&BasicConstraints, sizeof(BasicConstraints));
    BasicConstraints.fCA = TRUE;
    BasicConstraints.fPathLenConstraint = FALSE;
    BasicConstraints.dwPathLenConstraint = 0;

	// Get Basic Constraints blob size*/
	if (!CryptEncodeObject(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, (LPVOID)&BasicConstraints, NULL, &pkSize)) goto end;
	if ((pbBasicConstraints = (BYTE*)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptEncodeObject(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, (LPVOID)&BasicConstraints, pbBasicConstraints, &pkSize)) goto end;

	// Set Basic Constraints extension
	certExtension[1].pszObjId = szOID_BASIC_CONSTRAINTS2;
	certExtension[1].fCritical = FALSE;
	certExtension[1].Value.cbData = pkSize;
	certExtension[1].Value.pbData = pbBasicConstraints;

	exts.cExtension = 2;
	exts.rgExtension = certExtension;

	// Setup certificate expiration
	GetSystemTime(&st1);
	GetSystemTime(&st2);
	st1.wYear -= 1;		// Starts a year ago
	st2.wYear += 30;	// Expires 30 years from now
	
	ZeroMemory(&sa, sizeof(sa));
	sa.pszObjId = szOID_RSA_SHA384RSA; // Using SHA384

#ifdef _WIN64
	wincrypto_certCtx = CertCreateSelfSignCertificate(NULL, &sib, 0, &kpi, NULL, NULL, NULL, NULL);
	wincrypto_certCtx = CertCreateSelfSignCertificate(NULL, &sib, 0, &kpi, &sa, &st1, &st2, &exts);
#else
	wincrypto_certCtx = CertCreateSelfSignCertificate(wincrypto_hProv, &sib, 0, &kpi, &sa, &st1, &st2, &exts);
#endif
	if (!wincrypto_certCtx) { goto error; }

	// Note this is a different context to certCtx, this ctx is the in-store ctx
	status = CertAddCertificateContextToStore(wincrypto_hCertStore, wincrypto_certCtx, CERT_STORE_ADD_REPLACE_EXISTING, &wincrypto_certCtx);
	if (!status || wincrypto_certCtx == NULL) goto error;

#ifndef _WIN64
	// Associate private key with cert in key store
	ZeroMemory(&ckp, sizeof(ckp));
	ckp.pwszContainerName = wincrypto_containername;
	ckp.pwszProvName = providerName;
	ckp.dwProvType = providerType;
	ckp.dwKeySpec = AT_KEYEXCHANGE;
#ifdef _CONSOLE
	ckp.dwFlags = 0;
#else
	ckp.dwFlags = CRYPT_MACHINE_KEYSET;
#endif
	status = CertSetCertificateContextProperty(wincrypto_certCtx, CERT_KEY_PROV_INFO_PROP_ID, 0, &ckp);
	if (!status) goto error;
#endif

// Get the selected provider name and save it in the registry
#ifdef _WIN64
	if (providerName != NULL) wincrypto_setregistry(L"KeyStore", providerName);
#else
	if (!CryptGetProvParam(wincrypto_hProv, PP_NAME, NULL, &len, 0)) goto error;
	providerNameA = (char*)malloc(len);
	if (!CryptGetProvParam(wincrypto_hProv, PP_NAME, (BYTE*)providerNameA, &len, 0)) goto error;
	wincrypto_setregistryA("KeyStore", providerNameA);
	free(providerNameA);
#endif

	goto end;

error:
	// Clean up
#ifdef _WIN64
	if (hKeyNode != NULL) NCryptFreeObject(hKeyNode);
#else
	if (hKeyNode != NULL) CryptDestroyKey(hKeyNode);
#endif
	wincrypto_close();
	return 1;

end:
	if (subjectEncoded != NULL) free(subjectEncoded);
	return 0;
}

#ifdef _WIN64
void __fastcall wincrypto_random(int length, char* result)
{
	BCRYPT_ALG_HANDLE hAlg = NULL;
	if(!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, NULL, 0))) goto Cleanup;
	if(!NT_SUCCESS(BCryptGenRandom(hAlg, (PUCHAR)result, length, BCRYPT_RNG_USE_ENTROPY_IN_BUFFER))) goto Cleanup;
Cleanup:
	if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
}

// Perform cryptographic hash
int __fastcall wincrypto_hash(LPCWSTR alg, char* data, int datalen, char* result, DWORD resultlen)
{
	BCRYPT_ALG_HANDLE hAlg = NULL;
	BCRYPT_HASH_HANDLE hHash = NULL;
	DWORD cbData = 0, cbHashObject = 0;
	PBYTE pbHashObject = NULL;
	int r = 0;

	if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, alg, NULL, 0))) goto Cleanup;
	if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0))) goto Cleanup;
	if ((pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject)) == NULL) goto Cleanup;
	if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, NULL, 0, 0))) goto Cleanup;
	if (!NT_SUCCESS(BCryptHashData(hHash, (PBYTE)data, datalen, 0))) goto Cleanup;
	if (!NT_SUCCESS(BCryptFinishHash(hHash, (PUCHAR)result, resultlen, 0))) goto Cleanup;
	r = resultlen;
Cleanup:
	if (hHash) BCryptDestroyHash(hHash);
	if (pbHashObject) HeapFree(GetProcessHeap(), 0, pbHashObject);
	if (hAlg) BCryptCloseAlgorithmProvider(hAlg,0);
	return r;
}

int __fastcall wincrypto_md5(char* data, int datalen, char* result) { return wincrypto_hash(BCRYPT_MD5_ALGORITHM, data, datalen, result, 16); }
int __fastcall wincrypto_sha256(char* data, int datalen, char* result) { return wincrypto_hash(BCRYPT_SHA256_ALGORITHM, data, datalen, result, 32); }
int __fastcall wincrypto_sha385(char* data, int datalen, char* result) { return wincrypto_hash(BCRYPT_SHA384_ALGORITHM, data, datalen, result, 32); }
#else
void __fastcall wincrypto_random(int length, char* result)
{
	// Check that we have open context
	if (wincrypto_hProv == NULL || !CryptGenRandom(wincrypto_hProv, length, (BYTE*)result)) ILIBCRITICALEXIT(252);
}

int  __fastcall wincrypto_hash(ALG_ID calg, char* data, int datalen, char* result, int resultlen)
{
	HCRYPTHASH hHash = NULL;
	DWORD hlen = resultlen;
	int r = 0;

	// Add hash of the certificate to start of data
	if (wincrypto_hProv == NULL || CryptCreateHash(wincrypto_hProv, calg, 0, 0, &hHash) == 0) return r;
	if (CryptHashData(hHash, (BYTE*)data, datalen, 0) == 0) goto end;
	if (!CryptGetHashParam(hHash, HP_HASHVAL, (BYTE*)result, &hlen, 0)) goto end;
	r = resultlen;
end:
	if (hHash) CryptDestroyHash(hHash);
	return r;
}

int __fastcall wincrypto_md5(char* data, int datalen, char* result) { return wincrypto_hash(CALG_MD5, data, datalen, result, 16); }
int __fastcall wincrypto_sha256(char* data, int datalen, char* result) { return wincrypto_hash(CALG_SHA_256, data, datalen, result, 32); }
int __fastcall wincrypto_sha384(char* data, int datalen, char* result) { return wincrypto_hash(CALG_SHA_384, data, datalen, result, 32); }
#endif

// Sign the data with the Mesh Agent certificate and return a PKCS7 result.
int __fastcall wincrypto_sign(char* data, int len, char** signature)
{
	int signatureLen = 0;
	DWORD cbSignedBlob;
	HCRYPTMSG hMsg = NULL;
	DWORD HashAlgSize;
	CRYPT_ALGORITHM_IDENTIFIER HashAlgorithm;
	CMSG_SIGNER_ENCODE_INFO SignerEncodeInfo;
	CERT_BLOB SignerCertBlob;
	CERT_BLOB SignerCertBlobArray[1];
	CMSG_SIGNER_ENCODE_INFO SignerEncodeInfoArray[1];
	CMSG_SIGNED_ENCODE_INFO SignedMsgEncodeInfo;
#ifdef _WIN64
	int r = 0;
	NCRYPT_KEY_HANDLE hKeyNode = NULL;
	DWORD hKeyNodeSpec = 0;
	BOOL hFreeKeyNode = FALSE;
#endif

	// Check that we have open context
	if (wincrypto_hProv == NULL || wincrypto_hCertStore == NULL || wincrypto_certCtx == NULL) return 0;

	// Initialize the algorithm identifier structure.
	HashAlgSize = sizeof(HashAlgorithm);
	memset(&HashAlgorithm, 0, HashAlgSize);     // Initialize to zero
	HashAlgorithm.pszObjId = szOID_RSA_SHA384RSA; // Using SHA384
	
	// Initialize the CMSG_SIGNER_ENCODE_INFO structure.
	memset(&SignerEncodeInfo, 0, sizeof(CMSG_SIGNER_ENCODE_INFO));
	SignerEncodeInfo.cbSize = sizeof(CMSG_SIGNER_ENCODE_INFO);
#ifdef _WIN64
	if (!CryptAcquireCertificatePrivateKey(wincrypto_certCtx, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG, NULL, &hKeyNode, &hKeyNodeSpec, &hFreeKeyNode)) { r = 10; goto end; }
	SignerEncodeInfo.hNCryptKey = hKeyNode;
#else
	SignerEncodeInfo.hCryptProv = wincrypto_hProv;
#endif
	SignerEncodeInfo.pCertInfo = wincrypto_certCtx->pCertInfo;
	SignerEncodeInfo.dwKeySpec = AT_KEYEXCHANGE;
	SignerEncodeInfo.HashAlgorithm = HashAlgorithm;
	SignerEncodeInfo.pvHashAuxInfo = NULL;

	// Create an array of one.
	SignerEncodeInfoArray[0] = SignerEncodeInfo;

	// Initialize the CMSG_SIGNED_ENCODE_INFO structure.
	SignerCertBlob.cbData = wincrypto_certCtx->cbCertEncoded;
	SignerCertBlob.pbData = wincrypto_certCtx->pbCertEncoded;

	// Initialize the array of one CertBlob.
	SignerCertBlobArray[0] = SignerCertBlob;
	memset(&SignedMsgEncodeInfo, 0, sizeof(CMSG_SIGNED_ENCODE_INFO));
	SignedMsgEncodeInfo.cbSize = sizeof(CMSG_SIGNED_ENCODE_INFO);
	SignedMsgEncodeInfo.cSigners = 1;
	SignedMsgEncodeInfo.rgSigners = SignerEncodeInfoArray;
	SignedMsgEncodeInfo.cCertEncoded = 1;
	SignedMsgEncodeInfo.rgCertEncoded = SignerCertBlobArray;
	SignedMsgEncodeInfo.rgCrlEncoded = NULL;

	// Get the size of the encoded, signed message BLOB.
	cbSignedBlob = CryptMsgCalculateEncodedLength(MY_ENCODING_TYPE, 0, CMSG_SIGNED, &SignedMsgEncodeInfo, NULL, len);
	if (cbSignedBlob == 0) goto end;

	// Allocate memory for the encoded BLOB.
	if ((*signature = (char*)malloc(cbSignedBlob)) == NULL) ILIBCRITICALEXIT(254);

	// Open a message to encode.
	hMsg = CryptMsgOpenToEncode(MY_ENCODING_TYPE, 0, CMSG_SIGNED, &SignedMsgEncodeInfo, NULL, NULL);

	// Update the message with the data.
	if (!CryptMsgUpdate(hMsg, (BYTE*)data, len, TRUE)) goto end;

	// Get the resulting message.
	if (!CryptMsgGetParam(hMsg, CMSG_CONTENT_PARAM, 0, (BYTE*)*signature, &cbSignedBlob)) goto end;
	signatureLen = cbSignedBlob;

end:
	// Clean up
	if (hMsg) CryptMsgClose(hMsg);
	if (signatureLen == 0 && *signature != NULL) { free(*signature); *signature = NULL; }
#ifdef _WIN64
	if (hFreeKeyNode && hKeyNode != NULL) { if (hKeyNodeSpec == CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(hKeyNode); else CryptReleaseContext(hKeyNode, 0); }
#endif
	return signatureLen;
}

struct wincrypto_stream {
	char* buf;
	int buflen;
	int ptr;
};

static BOOL WINAPI wincrypto_CmsgStreamOutputCallback(IN const void *pvArg, IN BYTE *pbData, IN DWORD cbData, IN BOOL fFinal)
{
	struct wincrypto_stream* StreamArg = (struct wincrypto_stream*)pvArg;
	UNREFERENCED_PARAMETER(fFinal);

	if ((int)(StreamArg->ptr + cbData) > (int)StreamArg->buflen) return FALSE;
	memcpy(StreamArg->buf + StreamArg->ptr, pbData, cbData);
	StreamArg->ptr += cbData;
    return TRUE;
}

// Decrypt the PKCS7 block and return the content.
int __fastcall wincrypto_decrypt(char* encdata, int encdatalen, char** data)
{
	int datalen = 0;
	HCRYPTMSG hMsg = NULL;
    CMSG_CTRL_DECRYPT_PARA decryptPara = { sizeof(CMSG_CTRL_DECRYPT_PARA) };
    BOOL flagHandle = FALSE;
    CMSG_STREAM_INFO StreamInfo;
	struct wincrypto_stream StreamArg;

	// Check that we have open context
	if (wincrypto_hProv == NULL || wincrypto_hCertStore == NULL || wincrypto_certCtx == NULL) return 0;

	// Perform setup
	if ((StreamArg.buf = (char*)malloc(encdatalen)) == NULL) ILIBCRITICALEXIT(254);
	StreamArg.buflen = encdatalen;
	StreamArg.ptr = 0;
    StreamInfo.cbContent= CMSG_INDEFINITE_LENGTH;
    StreamInfo.pfnStreamOutput = wincrypto_CmsgStreamOutputCallback;
    StreamInfo.pvArg = &StreamArg;
	if ((hMsg = CryptMsgOpenToDecode(MY_ENCODING_TYPE, 0, 0, NULL, NULL, &StreamInfo)) == NULL) goto end;
	if (!CryptMsgUpdate(hMsg, (BYTE*)encdata, encdatalen, TRUE)) goto end;

	// Setup the certificate
    if (!CryptAcquireCertificatePrivateKey(wincrypto_certCtx, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG, NULL, &decryptPara.hCryptProv, &decryptPara.dwKeySpec, &flagHandle)) goto end;
    decryptPara.dwRecipientIndex = 0;

	// Perform the decrypt
    if (!CryptMsgControl(hMsg, 0, CMSG_CTRL_DECRYPT, &decryptPara)) goto end;
	datalen = StreamArg.ptr;
	*data = StreamArg.buf;
	StreamArg.buf = NULL;

end:
	// Clean up
	if (hMsg) CryptMsgClose(hMsg);
	if (StreamArg.buf != NULL) { free(StreamArg.buf); }
	return datalen;
}

// Get the X509 certificate including the public key (Direct reference, no need to free this).
int __fastcall wincrypto_getcert(char** data)
{
	if (wincrypto_certCtx == NULL) { *data = NULL; return 0; }
	*data = (char*)wincrypto_certCtx->pbCertEncoded;
	return (int)wincrypto_certCtx->cbCertEncoded;
}

// Create an X509, RSA 3027bit certificate with the MeshAgent certificate as signing root.
int __fastcall wincrypto_mkCert(wchar_t* subject, int certtype, wchar_t* password, char** data)
{
#ifdef _WIN64
	NCRYPT_KEY_HANDLE hKeyNode = NULL;
	DWORD hKeyNodeSpec = 0;
	BOOL hFreeKeyNode = FALSE;
	NCRYPT_KEY_HANDLE hNewKey = NULL;
	DWORD KeyLength = 2048;
	DWORD KeyPolicy = NCRYPT_ALLOW_EXPORT_FLAG;
#endif

	int len = 0;
	HCRYPTKEY hKey = NULL;
	PCCERT_CONTEXT certCtx = NULL;
	HCRYPTPROV hProv = NULL;
    CRYPT_KEY_PROV_INFO keyProviderInfo;

	// Issuer and cert subject names
	CERT_NAME_BLOB sib1;
	CERT_NAME_BLOB sib2;
	PBYTE subject1Encoded = NULL;
	DWORD subject1EncodedSize;
	PBYTE subject2Encoded = NULL;
	DWORD subject2EncodedSize;

	CRYPT_KEY_PROV_INFO kpi;
	BYTE* pbPolicyInfo = NULL;
	BYTE* pbPolicyInfo2 = NULL;

	CERT_EXTENSION certExtension[2];
	CERT_ENHKEY_USAGE keyusage;
	CRYPT_BIT_BLOB keyusage2;
	//CERT_BASIC_CONSTRAINTS2_INFO BasicConstraints;
	//BYTE* pbBasicConstraints = NULL;

	DWORD pkSize = 0;
    PCERT_PUBLIC_KEY_INFO pkInfo = NULL;

    CERT_INFO certInfo;
    SYSTEMTIME systemTime;
    FILETIME notBefore;
    FILETIME notAfter;
    BYTE serialNumber[8];
    DWORD certSize = 0xFFFFFFFF;
    BYTE *certData = NULL;

	HCERTSTORE hCertStore = NULL;
    PCCERT_CONTEXT certContext = NULL;
	CRYPT_DATA_BLOB pfxBlob;
    DWORD pfxExportFlags = EXPORT_PRIVATE_KEYS; // | REPORT_NO_PRIVATE_KEY | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY;

	// Check that we have open context
	if (wincrypto_hProv == NULL || wincrypto_hCertStore == NULL || wincrypto_certCtx == NULL) return 0;

	*data = NULL;
	ZeroMemory(&kpi, sizeof(kpi));
	ZeroMemory(&certExtension, sizeof(certExtension));

	// Generate node RSA key-pair
#ifdef _WIN64
	if (FAILED(NCryptOpenStorageProvider(&hProv, MS_KEY_STORAGE_PROVIDER, 0))) goto end;
#ifdef _CONSOLE
    if (FAILED(NCryptCreatePersistedKey(hProv, &hNewKey, BCRYPT_RSA_ALGORITHM, L"MeshDummy", AT_KEYEXCHANGE, NCRYPT_OVERWRITE_KEY_FLAG))) goto end;
#else
    if (FAILED(NCryptCreatePersistedKey(hProv, &hNewKey, BCRYPT_RSA_ALGORITHM, L"MeshDummy", AT_KEYEXCHANGE, NCRYPT_MACHINE_KEY_FLAG | NCRYPT_OVERWRITE_KEY_FLAG))) goto end;
#endif
    if (FAILED(NCryptSetProperty(hNewKey, NCRYPT_LENGTH_PROPERTY, (PBYTE)&KeyLength, 4, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG))) goto end;
    if (FAILED(NCryptSetProperty(hNewKey, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&KeyPolicy, 4, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG))) goto end;
    if (FAILED(NCryptFinalizeKey(hNewKey, NCRYPT_SILENT_FLAG))) goto end;
	if (!CryptExportPublicKeyInfo(hNewKey, AT_KEYEXCHANGE, X509_ASN_ENCODING, NULL, &pkSize)) goto end;
	if ((pkInfo = (PCERT_PUBLIC_KEY_INFO)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptExportPublicKeyInfo(hNewKey, AT_KEYEXCHANGE, X509_ASN_ENCODING, pkInfo, &pkSize)) goto end;
#else
    // Create a dummy container
#ifdef _CONSOLE
	if (!CryptAcquireContextW(&hProv, L"MeshDummy", NULL, PROV_RSA_AES, 0) && !CryptAcquireContextW(&hProv, L"MeshDummy", NULL, PROV_RSA_AES, CRYPT_NEWKEYSET)) goto end;
#else
	if (!CryptAcquireContextW(&hProv, L"MeshDummy", NULL, PROV_RSA_AES, CRYPT_MACHINE_KEYSET) && !CryptAcquireContextW(&hProv, L"MeshDummy", NULL, PROV_RSA_AES, CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET)) goto end;
#endif

    // Generate 3072-bit RSA keypair and export the public key information
	if (!CryptGenKey(hProv, AT_KEYEXCHANGE, CRYPT_EXPORTABLE | RSA3072BIT_KEY, &hKey)) goto end;
	if (!CryptExportPublicKeyInfo(hProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, NULL, &pkSize)) goto end;
	if ((pkInfo = (PCERT_PUBLIC_KEY_INFO)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptExportPublicKeyInfo(hProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, pkInfo, &pkSize)) goto end;
#endif

	// Create cert issuer string in format the CSP understands
	if (!CertStrToName(X509_ASN_ENCODING, wincrypto_subject, CERT_X500_NAME_STR, NULL, NULL, &subject1EncodedSize, NULL)) goto end;
	if ((subject1Encoded = (PBYTE)malloc(subject1EncodedSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CertStrToName(X509_ASN_ENCODING, wincrypto_subject, CERT_X500_NAME_STR, NULL, subject1Encoded, &subject1EncodedSize, NULL)) goto end;
	sib1.cbData = subject1EncodedSize;
	sib1.pbData = subject1Encoded;

	// Create cert subject string in format the CSP understands
	if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, NULL, NULL, &subject2EncodedSize, NULL)) goto end;
	if ((subject2Encoded = (PBYTE)malloc(subject2EncodedSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, NULL, subject2Encoded, &subject2EncodedSize, NULL)) goto end;
	sib2.cbData = subject2EncodedSize;
	sib2.pbData = subject2Encoded;

    // Setup the certificate validity period
    ZeroMemory(&certInfo, sizeof(certInfo));
    GetSystemTime(&systemTime);
    systemTime.wYear -= 1;
    SystemTimeToFileTime(&systemTime, &notBefore);
    systemTime.wYear += 30;
    SystemTimeToFileTime(&systemTime, &notAfter);

    // Generate a serial number.
	wincrypto_random(8, (char*)serialNumber);

	// Setup all of the certificate information
	certInfo.dwVersion = CERT_V3;
    certInfo.SerialNumber.cbData = 8;
    certInfo.SerialNumber.pbData = serialNumber;
    certInfo.SignatureAlgorithm.pszObjId = szOID_RSA_SHA384RSA; // Using SHA384
    certInfo.Issuer = sib1;
    certInfo.NotBefore = notBefore;
    certInfo.NotAfter = notAfter;
    certInfo.Subject = sib2;
    certInfo.SubjectPublicKeyInfo = *pkInfo;

	// Setup key usage
	if (certtype == 1)
	{
		// TLS Server certificate
		keyusage.cUsageIdentifier = 1;
		keyusage.rgpszUsageIdentifier = (LPSTR*)wincrypto_ServerOids;
	}
	else if (certtype == 2)
	{
		// TLS Client certificate
		keyusage.cUsageIdentifier = 4;
		keyusage.rgpszUsageIdentifier = (LPSTR*)wincrypto_ClientOids;
	}
	else
	{
		// Root certificate
		keyusage.cUsageIdentifier = 0;
		keyusage.rgpszUsageIdentifier = NULL;
	}

	// If we have key usages, add them to the certInfo here
	if (keyusage.cUsageIdentifier != 0)
	{
		// Set key usage. Only use (CERT_DIGITAL_SIGNATURE_KEY_USAGE| CERT_KEY_ENCIPHERMENT_KEY_USAGE | CERT_KEY_AGREEMENT_KEY_USAGE) for TLS Server certificate, other usages will cause FireFox to error on the cert without an ignore button.
		BYTE ByteData = CERT_DIGITAL_SIGNATURE_KEY_USAGE| CERT_KEY_ENCIPHERMENT_KEY_USAGE | CERT_KEY_AGREEMENT_KEY_USAGE; // | CERT_DATA_ENCIPHERMENT_KEY_USAGE | CERT_NON_REPUDIATION_KEY_USAGE | CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE;
		ZeroMemory(&keyusage2, sizeof(keyusage2));
		keyusage2.cbData = 1;
		keyusage2.pbData = &ByteData;
      
		// Encode key usage
		if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, (LPVOID)&keyusage2, NULL, &pkSize)) goto end;
		if ((pbPolicyInfo2 = (BYTE*)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
		if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, (LPVOID)&keyusage2, pbPolicyInfo2, &pkSize)) goto end;
		certExtension[0].pszObjId = szOID_KEY_USAGE;
		certExtension[0].fCritical = FALSE;
		certExtension[0].Value.cbData = pkSize;
		certExtension[0].Value.pbData = pbPolicyInfo2;

		// Encode enhanced key usage
		if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, (LPVOID)&keyusage, NULL, &pkSize)) goto end;
		if ((pbPolicyInfo = (BYTE*)malloc(pkSize)) == NULL) ILIBCRITICALEXIT(254);
		if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, (LPVOID)&keyusage, pbPolicyInfo, &pkSize)) goto end;
		certExtension[1].pszObjId = szOID_ENHANCED_KEY_USAGE;
		certExtension[1].fCritical = FALSE;
		certExtension[1].Value.cbData = pkSize;
		certExtension[1].Value.pbData = pbPolicyInfo;

		certInfo.cExtension = 2;
		certInfo.rgExtension = certExtension;
	}

	// Sign the certificate with the MeshAgent private key
#ifdef _WIN64
	if (!CryptAcquireCertificatePrivateKey(wincrypto_certCtx, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG, NULL, &hKeyNode, &hKeyNodeSpec, &hFreeKeyNode)) goto end;
	if (!CryptSignAndEncodeCertificate(hKeyNode, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, (LPVOID)&certInfo, &(certInfo.SignatureAlgorithm), NULL, NULL, &certSize)) goto end;
	if ((certData = (BYTE*)malloc(certSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptSignAndEncodeCertificate(hKeyNode, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, (LPVOID)&certInfo, &(certInfo.SignatureAlgorithm), NULL, certData, &certSize)) goto end;
#else
	if (!CryptSignAndEncodeCertificate(wincrypto_hProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, (LPVOID)&certInfo, &(certInfo.SignatureAlgorithm), NULL, NULL, &certSize)) goto end;
	if ((certData = (BYTE*)malloc(certSize)) == NULL) ILIBCRITICALEXIT(254);
	if (!CryptSignAndEncodeCertificate(wincrypto_hProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, (LPVOID)&certInfo, &(certInfo.SignatureAlgorithm), NULL, certData, &certSize)) goto end;
#endif

    // Open a new temporary store.
    if ((hCertStore = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, NULL, CERT_STORE_CREATE_NEW_FLAG, NULL)) == NULL) goto end;

    // Add to temporary store so we can use the PFX functions to export a store + private keys in PFX format.
    if (!CertAddEncodedCertificateToStore(hCertStore, X509_ASN_ENCODING, certData, certSize, CERT_STORE_ADD_NEW, &certContext)) goto end;

#ifdef _WIN64
    // Link keypair to certificate (without this the keypair gets "lost" on export).
    ZeroMemory(&keyProviderInfo, sizeof(keyProviderInfo));
    keyProviderInfo.pwszContainerName = L"MeshDummy";
    keyProviderInfo.pwszProvName = MS_KEY_STORAGE_PROVIDER;
    keyProviderInfo.dwProvType = 0;
#ifdef _CONSOLE
	keyProviderInfo.dwFlags = 0;
#else
	keyProviderInfo.dwFlags = CRYPT_MACHINE_KEYSET;
#endif
    keyProviderInfo.dwKeySpec = 0;
    if (!CertSetCertificateContextProperty(certContext, CERT_KEY_PROV_INFO_PROP_ID, 0, (LPVOID)&keyProviderInfo)) goto end;
#else
    // Link keypair to certificate (without this the keypair gets "lost" on export).
    ZeroMemory(&keyProviderInfo, sizeof(keyProviderInfo));
    keyProviderInfo.pwszContainerName = L"MeshDummy";
    keyProviderInfo.pwszProvName = NULL;
    keyProviderInfo.dwProvType = PROV_RSA_AES;
#ifdef _CONSOLE
	keyProviderInfo.dwFlags = 0;
#else
	keyProviderInfo.dwFlags = CRYPT_MACHINE_KEYSET;
#endif
    keyProviderInfo.dwKeySpec = AT_KEYEXCHANGE;
    if (!CertSetCertificateContextProperty(certContext, CERT_KEY_PROV_INFO_PROP_ID, 0, (LPVOID)&keyProviderInfo)) goto end;
#endif

    // Calculate size required.
    ZeroMemory(&pfxBlob, sizeof(pfxBlob));
    if (!PFXExportCertStore(hCertStore, &pfxBlob, password, pfxExportFlags)) goto end;

    // Export to PFX
	if ((pfxBlob.pbData = (BYTE*)malloc(pfxBlob.cbData)) == NULL) ILIBCRITICALEXIT(254);
    if (!PFXExportCertStore(hCertStore, &pfxBlob, password, pfxExportFlags)) goto end;
	*data = (char*)pfxBlob.pbData;
	len = pfxBlob.cbData;

end:
	// Clean up everything
#ifdef _WIN64
	if (hKey != NULL) NCryptFreeObject(hKey);
	if (hProv != NULL) NCryptFreeObject(hProv);
	#ifdef _WIN64
	if (hFreeKeyNode && hKeyNode != NULL) { if (hKeyNodeSpec == CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(hKeyNode); else CryptReleaseContext(hKeyNode, 0); }
	#endif
#else
	if (hProv != NULL) CryptReleaseContext(hProv, 0);
#endif
	if (pkInfo != NULL) free(pkInfo);
	if (hKey != NULL) CryptDestroyKey(hKey);
	if (subject1Encoded != NULL) free(subject1Encoded);
	if (subject2Encoded != NULL) free(subject2Encoded);
	if (certCtx != NULL) CertFreeCertificateContext(certCtx);
	if (hCertStore != NULL) CertCloseStore(hCertStore, 0);
	if (pbPolicyInfo != NULL) free(pbPolicyInfo);
	if (pbPolicyInfo2 != NULL) free(pbPolicyInfo2);

	return len;
}

}

#endif
