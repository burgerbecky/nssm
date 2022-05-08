/***************************************

	Main entry and common subroutines

***************************************/

#ifndef __NSSM_H__
#define __NSSM_H__

#include <stdint.h>

extern void nssm_exit(int iStatus);
extern int str_equiv(const wchar_t* pInput1, const wchar_t* pInput2);
extern int str_number(
	const wchar_t* pString, uint32_t* pNumber, wchar_t** ppEnd);
extern int str_number(const wchar_t* pString, uint32_t* pNumber);
extern int quote(
	const wchar_t* pUnquoted, wchar_t* pBuffer, uintptr_t uBufferLength);
extern void strip_basename(wchar_t* pBuffer);
extern int usage(int iResult);
extern int num_cpus(void);
extern const wchar_t* nssm_unquoted_imagepath(void);
extern const wchar_t* nssm_imagepath(void);
extern const wchar_t* nssm_exe(void);
extern int _tmain(int iArgc, wchar_t** argv);

#endif
