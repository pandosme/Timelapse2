#ifndef _recordings_h_
#define _recordings_h_

#include "cJSON.h"

int Recordings_Init(void);
int Recordings_Capture(cJSON* profile);
int Recordings_Clear(const char* profileId);
cJSON* Recordings_Get_List(void);
cJSON* Recordings_Get_Metadata(const char* profileId);

#endif
