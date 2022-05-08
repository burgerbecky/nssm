/***************************************

	User account manager

***************************************/

#ifndef __ACCOUNT_H__
#define __ACCOUNT_H__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <ntsecapi.h>

extern int open_lsa_policy(LSA_HANDLE* pPolicy);
extern int username_sid(
	const wchar_t* pUsername, SID** ppSecurityID, LSA_HANDLE* pPolicy);
extern int username_sid(const wchar_t* pUsername, SID** ppSecurityID);
extern int canonicalise_username(const wchar_t* pUsername, wchar_t** ppCanon);
extern int username_equiv(const wchar_t* a, const wchar_t* b);
extern int is_localsystem(const wchar_t* pUsername);
extern wchar_t* virtual_account(const wchar_t* pServiceName);
extern int is_virtual_account(
	const wchar_t* pServiceName, const wchar_t* pUsername);
extern const wchar_t* well_known_sid(SID* pSecurityID);
extern const wchar_t* well_known_username(const wchar_t* pUsername);
extern int grant_logon_as_service(const wchar_t* pUsername);

#endif
