/***************************************

	Event logging

***************************************/

#ifndef __EVENT_H__
#define __EVENT_H__

#include <stdint.h>
#include <stdio.h>

struct HWND__;

extern void setup_event(void);
extern void unsetup_event(void);
extern wchar_t* error_string(uint32_t uErrorCode);
extern wchar_t* message_string(uint32_t uMessageCode);
extern void log_event(uint16_t uMessageType, uint32_t uMessageID, ...);
extern void print_message(FILE* fp, uint32_t uMessageID, ...);
extern int popup_message(
	HWND__* hWindow, uint32_t uMessageType, uint32_t uMessageID, ...);

#endif
