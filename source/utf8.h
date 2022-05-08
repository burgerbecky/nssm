/***************************************

	UTF 8 translation functions

***************************************/

#ifndef __UTF8_H__
#define __UTF8_H__

#include <stdint.h>

extern void setup_utf8(void);
extern void unsetup_utf8(void);
extern int to_utf8(
	const wchar_t* pInput, char** ppOutput, uint32_t* pOutputLength);
extern int to_utf8(
	const char* pInput, char** ppOutput, uint32_t* pOutputLength);
extern int to_utf16(
	const char* pInput, wchar_t** ppOutput, uint32_t* pOutputLength);
extern int to_utf16(
	const wchar_t* pInput, wchar_t** ppOutput, uint32_t* pOutputLength);
extern int from_utf8(
	const char* pInput, wchar_t** ppOutput, uint32_t* pOutputLength);
extern int from_utf16(
	const wchar_t* pInput, wchar_t** ppOutput, uint32_t* pOutputLength);

#endif
