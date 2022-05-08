/***************************************

	User account manager

***************************************/

#include "account.h"
#include "constants.h"
#include "event.h"
#include "imports.h"
#include "memorymanager.h"
#include "messages.h"
#include "nssm.h"
#include "utf8.h"

#include <wchar.h>

#include <sddl.h>
#include <strsafe.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ERROR_SUCCESS
#endif

/* Open Policy object. */
int open_lsa_policy(LSA_HANDLE* pPolicy)
{
	LSA_OBJECT_ATTRIBUTES attributes;
	ZeroMemory(&attributes, sizeof(attributes));

	NTSTATUS status = LsaOpenPolicy(0, &attributes, POLICY_ALL_ACCESS, pPolicy);
	if (status != STATUS_SUCCESS) {
		print_message(stderr, NSSM_MESSAGE_LSAOPENPOLICY_FAILED,
			error_string(LsaNtStatusToWinError(status)));
		return 1;
	}

	return 0;
}

/* Look up SID for an account. */
int username_sid(
	const wchar_t* pUsername, SID** ppSecurityID, LSA_HANDLE* pPolicy)
{
	LSA_HANDLE handle;
	if (!pPolicy) {
		pPolicy = &handle;
		if (open_lsa_policy(pPolicy)) {
			return 1;
		}
	}

	/*
	  LsaLookupNames() can't look up .\username but can look up
	  %COMPUTERNAME%\username.  ChangeServiceConfig() writes .\username to the
	  registry when %COMPUTERNAME%\username is a passed as a parameter.  We
	  need to preserve .\username when calling ChangeServiceConfig() without
	  changing the username, but expand to %COMPUTERNAME%\username when calling
	  LsaLookupNames().
	*/
	wchar_t* expanded;
	unsigned long expandedlen;
	if (_wcsnicmp(L".\\", pUsername, 2)) {
		expandedlen = (unsigned long)(wcslen(pUsername) + 1) * sizeof(wchar_t);
		expanded = (wchar_t*)heap_alloc(expandedlen);
		if (!expanded) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"expanded",
				L"username_sid");
			if (pPolicy == &handle)
				LsaClose(handle);
			return 2;
		}
		memmove(expanded, pUsername, expandedlen);
	} else {
		wchar_t computername[MAX_COMPUTERNAME_LENGTH + 1];
		expandedlen = RTL_NUMBER_OF(computername);
		GetComputerNameW(computername, &expandedlen);
		expandedlen += (unsigned long)wcslen(pUsername);

		expanded = (wchar_t*)heap_alloc(expandedlen * sizeof(wchar_t));
		if (!expanded) {
			print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"expanded",
				L"username_sid");
			if (pPolicy == &handle)
				LsaClose(handle);
			return 2;
		}
		StringCchPrintfW(
			expanded, expandedlen, L"%s\\%s", computername, pUsername + 2);
	}

	LSA_UNICODE_STRING lsa_username;
	int ret = to_utf16(
		expanded, &lsa_username.Buffer, (uint32_t*)&lsa_username.Length);
	heap_free(expanded);
	if (ret) {
		if (pPolicy == &handle) {
			LsaClose(handle);
		}
		print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"LSA_UNICODE_STRING",
			L"username_sid()");
		return 4;
	}
	lsa_username.Length *= sizeof(wchar_t);
	lsa_username.MaximumLength =
		static_cast<USHORT>(lsa_username.Length + sizeof(wchar_t));

	LSA_REFERENCED_DOMAIN_LIST* translated_domains;
	LSA_TRANSLATED_SID* translated_sid;
	NTSTATUS status = LsaLookupNames(
		*pPolicy, 1, &lsa_username, &translated_domains, &translated_sid);
	heap_free(lsa_username.Buffer);
	if (pPolicy == &handle) {
		LsaClose(handle);
	}
	if (status != STATUS_SUCCESS) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_sid);
		print_message(stderr, NSSM_MESSAGE_LSALOOKUPNAMES_FAILED, pUsername,
			error_string(LsaNtStatusToWinError(status)));
		return 5;
	}

	if (translated_sid->Use != SidTypeUser &&
		translated_sid->Use != SidTypeWellKnownGroup) {
		if (translated_sid->Use != SidTypeUnknown ||
			_wcsnicmp(g_NSSMVirtualServiceAccountDomainSlash, pUsername,
				wcslen(g_NSSMVirtualServiceAccountDomain) + 1)) {
			LsaFreeMemory(translated_domains);
			LsaFreeMemory(translated_sid);
			print_message(stderr, NSSM_GUI_INVALID_USERNAME, pUsername);
			return 6;
		}
	}

	LSA_TRUST_INFORMATION* trust =
		&translated_domains->Domains[translated_sid->DomainIndex];
	if (!trust || !IsValidSid(trust->Sid)) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_sid);
		print_message(stderr, NSSM_GUI_INVALID_USERNAME, pUsername);
		return 7;
	}

	/* GetSidSubAuthority*() return pointers! */
	unsigned char* n = GetSidSubAuthorityCount(trust->Sid);

	/* Convert translated SID to SID. */
	*ppSecurityID = static_cast<SID*>(
		heap_calloc(GetSidLengthRequired(static_cast<UCHAR>(*n + 1))));
	if (!*ppSecurityID) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_sid);
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"SID", L"username_sid");
		return 8;
	}

	if (!InitializeSid(*ppSecurityID, GetSidIdentifierAuthority(trust->Sid),
			static_cast<BYTE>(*n + 1))) {
		DWORD error = GetLastError();
		heap_free(*ppSecurityID);
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_sid);
		print_message(stderr, NSSM_MESSAGE_INITIALIZESID_FAILED, pUsername,
			error_string(error));
		return 9;
	}

	for (unsigned char i = 0; i <= *n; i++) {
		DWORD* sub = GetSidSubAuthority(*ppSecurityID, i);
		if (i < *n) {
			*sub = *GetSidSubAuthority(trust->Sid, i);
		} else {
			*sub = translated_sid->RelativeId;
		}
	}

	ret = 0;
	if (translated_sid->Use == SidTypeWellKnownGroup &&
		!well_known_sid(*ppSecurityID)) {
		print_message(stderr, NSSM_GUI_INVALID_USERNAME, pUsername);
		ret = 10;
	}

	LsaFreeMemory(translated_domains);
	LsaFreeMemory(translated_sid);

	return ret;
}

int username_sid(const wchar_t* pUsername, SID** ppSecurityID)
{
	return username_sid(pUsername, ppSecurityID, 0);
}

int canonicalise_username(const wchar_t* pUsername, wchar_t** ppCanon)
{
	LSA_HANDLE policy;
	if (open_lsa_policy(&policy)) {
		return 1;
	}

	SID* sid;
	if (username_sid(pUsername, &sid, &policy)) {
		return 2;
	}
	PSID sids = {sid};

	LSA_REFERENCED_DOMAIN_LIST* translated_domains;
	LSA_TRANSLATED_NAME* translated_name;
	NTSTATUS status =
		LsaLookupSids(policy, 1, &sids, &translated_domains, &translated_name);
	if (status != STATUS_SUCCESS) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_name);
		print_message(stderr, NSSM_MESSAGE_LSALOOKUPSIDS_FAILED,
			error_string(LsaNtStatusToWinError(status)));
		return 3;
	}

	LSA_TRUST_INFORMATION* trust =
		&translated_domains->Domains[translated_name->DomainIndex];
	LSA_UNICODE_STRING lsa_canon;
	lsa_canon.Length = static_cast<USHORT>(
		translated_name->Name.Length + trust->Name.Length + sizeof(wchar_t));
	lsa_canon.MaximumLength =
		static_cast<USHORT>(lsa_canon.Length + sizeof(wchar_t));
	lsa_canon.Buffer =
		static_cast<wchar_t*>(heap_calloc(lsa_canon.MaximumLength));
	if (!lsa_canon.Buffer) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_name);
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"lsa_canon", L"username_sid");
		return 9;
	}

	/* Buffer is wchar_t but Length is in bytes. */
	memmove(lsa_canon.Buffer, trust->Name.Buffer, trust->Name.Length);
	memmove(lsa_canon.Buffer + trust->Name.Length, L"\\", sizeof(wchar_t));
	memmove(lsa_canon.Buffer + trust->Name.Length + sizeof(wchar_t),
		translated_name->Name.Buffer, translated_name->Name.Length);

	uint32_t canonlen;
	if (from_utf16(lsa_canon.Buffer, ppCanon, &canonlen)) {
		LsaFreeMemory(translated_domains);
		LsaFreeMemory(translated_name);
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"canon", L"username_sid");
		return 10;
	}
	heap_free(lsa_canon.Buffer);

	LsaFreeMemory(translated_domains);
	LsaFreeMemory(translated_name);

	return 0;
}

/* Do two usernames map to the same SID? */
int username_equiv(const wchar_t* a, const wchar_t* b)
{
	SID *sid_a, *sid_b;
	if (username_sid(a, &sid_a)) {
		return 0;
	}

	if (username_sid(b, &sid_b)) {
		FreeSid(sid_a);
		return 0;
	}

	int ret = 0;
	if (EqualSid(sid_a, sid_b)) {
		ret = 1;
	}

	FreeSid(sid_a);
	FreeSid(sid_b);

	return ret;
}

/* Does the username represent the LocalSystem account? */
int is_localsystem(const wchar_t* pUsername)
{
	if (str_equiv(pUsername, g_NSSMLocalSystemAccount)) {
		return 1;
	}
	if (!g_Imports.IsWellKnownSid) {
		return 0;
	}

	SID* sid;
	if (username_sid(pUsername, &sid)) {
		return 0;
	}

	int ret = 0;
	if (g_Imports.IsWellKnownSid(sid, WinLocalSystemSid)) {
		ret = 1;
	}

	FreeSid(sid);

	return ret;
}

/* Build the virtual account name. */
wchar_t* virtual_account(const wchar_t* pServiceName)
{
	uintptr_t len =
		wcslen(g_NSSMVirtualServiceAccountDomain) + wcslen(pServiceName) + 2;
	wchar_t* pName = static_cast<wchar_t*>(heap_alloc(len * sizeof(wchar_t)));
	if (!pName) {
		print_message(
			stderr, NSSM_MESSAGE_OUT_OF_MEMORY, L"name", L"virtual_account");
		return 0;
	}

	StringCchPrintfW(
		pName, len, L"%s\\%s", g_NSSMVirtualServiceAccountDomain, pServiceName);
	return pName;
}

/* Does the username represent a virtual account for the service? */
int is_virtual_account(const wchar_t* pServiceName, const wchar_t* pUsername)
{
	if (!g_Imports.IsWellKnownSid) {
		return 0;
	}
	if (!pServiceName) {
		return 0;
	}
	if (!pUsername) {
		return 0;
	}

	wchar_t* pCanon = virtual_account(pServiceName);
	int ret = str_equiv(pCanon, pUsername);
	heap_free(pCanon);
	return ret;
}

/*
  Get well-known alias for LocalSystem and friends.
  Returns a pointer to a static string.  DO NOT try to free it.
*/
const wchar_t* well_known_sid(SID* pSecurityID)
{
	if (!g_Imports.IsWellKnownSid) {
		return NULL;
	}
	if (g_Imports.IsWellKnownSid(pSecurityID, WinLocalSystemSid)) {
		return g_NSSMLocalSystemAccount;
	}
	if (g_Imports.IsWellKnownSid(pSecurityID, WinLocalServiceSid)) {
		return g_NSSMLocalServiceAccount;
	}
	if (g_Imports.IsWellKnownSid(pSecurityID, WinNetworkServiceSid)) {
		return g_NSSMNetworkServiceAccount;
	}
	return NULL;
}

const wchar_t* well_known_username(const wchar_t* pUsername)
{
	if (!pUsername) {
		return g_NSSMLocalSystemAccount;
	}
	if (str_equiv(pUsername, g_NSSMLocalSystemAccount)) {
		return g_NSSMLocalSystemAccount;
	}
	SID* pSecurityID;
	if (username_sid(pUsername, &pSecurityID)) {
		return NULL;
	}

	const wchar_t* pWellKnown = well_known_sid(pSecurityID);
	FreeSid(pSecurityID);

	return pWellKnown;
}

int grant_logon_as_service(const wchar_t* pUsername)
{
	if (!pUsername) {
		return 0;
	}

	/* Open Policy object. */
	LSA_OBJECT_ATTRIBUTES attributes;
	ZeroMemory(&attributes, sizeof(attributes));

	LSA_HANDLE policy;
	NTSTATUS status;

	if (open_lsa_policy(&policy)) {
		return 1;
	}

	/* Look up SID for the account. */
	SID* sid;
	if (username_sid(pUsername, &sid, &policy)) {
		LsaClose(policy);
		return 2;
	}

	/*
	  Shouldn't happen because it should have been checked before callling this
	  function.
	*/
	if (well_known_sid(sid)) {
		LsaClose(policy);
		return 3;
	}

	/* Check if the SID has the "Log on as a service" right. */
	LSA_UNICODE_STRING lsa_right;
	lsa_right.Buffer = const_cast<wchar_t*>(g_NSSMLogonAsServiceRight);
	lsa_right.Length =
		static_cast<USHORT>(wcslen(lsa_right.Buffer) * sizeof(wchar_t));
	lsa_right.MaximumLength =
		static_cast<USHORT>(lsa_right.Length + sizeof(wchar_t));

	LSA_UNICODE_STRING* rights;
	ULONG count = static_cast<ULONG>(~0);
	status = LsaEnumerateAccountRights(policy, sid, &rights, &count);
	if (status != STATUS_SUCCESS) {
		/*
		  If the account has no rights set LsaEnumerateAccountRights() will
		  return STATUS_OBJECT_NAME_NOT_FOUND and set count to 0.
		*/
		unsigned long error = LsaNtStatusToWinError(status);
		if (error != ERROR_FILE_NOT_FOUND) {
			FreeSid(sid);
			LsaClose(policy);
			print_message(stderr, NSSM_MESSAGE_LSAENUMERATEACCOUNTRIGHTS_FAILED,
				pUsername, error_string(error));
			return 4;
		}
	}

	for (unsigned long i = 0; i < count; i++) {
		if (rights[i].Length != lsa_right.Length)
			continue;
		if (_wcsnicmp(
				rights[i].Buffer, lsa_right.Buffer, lsa_right.MaximumLength))
			continue;
		/* The SID has the right. */
		FreeSid(sid);
		LsaFreeMemory(rights);
		LsaClose(policy);
		return 0;
	}
	LsaFreeMemory(rights);

	/* Add the right. */
	status = LsaAddAccountRights(policy, sid, &lsa_right, 1);
	FreeSid(sid);
	LsaClose(policy);
	if (status != STATUS_SUCCESS) {
		print_message(stderr, NSSM_MESSAGE_LSAADDACCOUNTRIGHTS_FAILED,
			error_string(LsaNtStatusToWinError(status)));
		return 5;
	}

	print_message(stdout, NSSM_MESSAGE_GRANTED_LOGON_AS_SERVICE, pUsername);
	return 0;
}
