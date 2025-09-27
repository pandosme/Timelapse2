#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <syslog.h>
#include "vdo-stream.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "ACAP.h"
#include "cJSON.h"
#include "recordings.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

cJSON* Recordings_Container = 0;

static void 
save_recordings(void) {
	LOG_TRACE("%s:\n",__func__);

    char path[256];
    snprintf(path, sizeof(path), "/var/spool/storage/SD_DISK/timelapse2/recordings.json");
    char* json = cJSON_PrintUnformatted(Recordings_Container);
    if (!json) return;
    
    FILE* file = fopen(path, "w");
    if (file) {
        fwrite(json, strlen(json), 1, file);
        fclose(file);
    }
    free(json);
}

static cJSON*
load_recordings(void) {
	LOG_TRACE("%s:\n",__func__);

    char path[256];
    snprintf(path, sizeof(path), "/var/spool/storage/SD_DISK/timelapse2/recordings.json");
    FILE* file = fopen(path, "r");
    if (!file) {
		Recordings_Container = cJSON_CreateObject();
		save_recordings();
        return Recordings_Container;
    }
    
    // Read entire file
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* data = malloc(length + 1);
    if (!data) {
        fclose(file);
		Recordings_Container = cJSON_CreateObject();
		save_recordings();
        return Recordings_Container;
    }
    
    fread(data, 1, length, file);
    data[length] = '\0';
    fclose(file);
    
    Recordings_Container = cJSON_Parse(data);
	if( !Recordings_Container )
		Recordings_Container = cJSON_CreateObject();
    return Recordings_Container;
}


int 
Recordings_Capture(cJSON* profile) {
    if (!profile) return -1;

	LOG_TRACE("%s:\n",__func__);

    const char* profileId = cJSON_GetObjectItem(profile, "id")->valuestring;
    const char* resolution = cJSON_GetObjectItem(profile, "resolution")->valuestring;
    if (!profileId || !resolution) return -1;

    // Parse resolution string
    char* width_str = strdup(resolution);
    char* height_str = strchr(width_str, 'x');
    if (height_str) {
        *height_str = '\0';
        height_str++;
    }
    
    int width = width_str ? atoi(width_str) : 1920;
    int height = height_str ? atoi(height_str) : 1080;
    free(width_str);
    
    LOG_TRACE("%s: Image capture %dx%d\n", __func__, width, height);
    double timestamp = ACAP_DEVICE_Timestamp();

    // Ensure Recordings_Container is loaded
    if (!Recordings_Container) {
        load_recordings();
    }

    // Update recordings metadata
    cJSON* recording = cJSON_GetObjectItem(Recordings_Container, profileId);
    if (!recording) {
        recording = cJSON_CreateObject();
        cJSON_AddItemToObject(Recordings_Container, profileId, recording);
        cJSON_AddNumberToObject(recording, "images", 1);
        cJSON_AddNumberToObject(recording, "first", timestamp);
        cJSON_AddNumberToObject(recording, "last", timestamp);
    } else {
        cJSON_SetNumberValue(cJSON_GetObjectItem(recording, "last"), timestamp);
        cJSON_SetNumberValue(cJSON_GetObjectItem(recording, "images"), 
                           cJSON_GetObjectItem(recording, "images")->valueint + 1);
    }

	int sequence = cJSON_GetObjectItem(recording, "images")->valueint;

    
    save_recordings();

    // Create VDO settings for snapshot
    VdoMap* vdoSettings = vdo_map_new();
    vdo_map_set_uint32(vdoSettings, "format", VDO_FORMAT_JPEG);
    vdo_map_set_uint32(vdoSettings, "width", width);
    vdo_map_set_uint32(vdoSettings, "height", height);
    
    if (cJSON_GetObjectItem(profile, "overlay") && 
        cJSON_GetObjectItem(profile, "overlay")->type == cJSON_True) {
        vdo_map_set_string(vdoSettings, "overlays", "all,sync");
    }

    // Take snapshot
    GError* error = NULL;
    VdoBuffer* buffer = vdo_stream_snapshot(vdoSettings, &error);
    g_clear_object(&vdoSettings);

    if (error != NULL) {
        LOG_WARN("%s: Snapshot capture failed: %s\n", __func__, error->message);
        g_error_free(error);
        return -1;
    }

    // Save image to file
	char filename[256];
	snprintf(filename, sizeof(filename), "/var/spool/storage/SD_DISK/timelapse2/%s/image%05d.jpg", 
			 profileId, sequence);

    FILE* f = fopen(filename, "wb");
    if (!f) {
        LOG_WARN("%s: Failed to open file for writing: %s\n", __func__, filename);
        g_object_unref(buffer);
        return -1;
    }

    unsigned char* data = vdo_buffer_get_data(buffer);
    unsigned int size = vdo_frame_get_size(buffer);
    fwrite(data, 1, size, f);
    fclose(f);
    g_object_unref(buffer);

    return 0;
}

int
Recordings_Clear(const char* profileId) {
	LOG_TRACE("%s:\n",__func__);

    char path[256];
    snprintf(path, sizeof(path), "/var/spool/storage/SD_DISK/timelapse2/%s", profileId);
    
    // Delete all image files in the directory
    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
            unlink(filepath);
        }
        closedir(dir);
    }

    // Update recordings.json
    if (!Recordings_Container) {
        load_recordings();
    }

    cJSON* recording = cJSON_GetObjectItem(Recordings_Container, profileId);
    if (recording) {
        cJSON_SetNumberValue(cJSON_GetObjectItem(recording, "images"), 0);
        cJSON_SetNumberValue(cJSON_GetObjectItem(recording, "first"), 0);
        cJSON_SetNumberValue(cJSON_GetObjectItem(recording, "last"), 0);
        save_recordings();
    }
    return 0;
}

static void 
HTTP_Endpoint_Recordings(const ACAP_HTTP_Response response, 
                                     const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

	LOG_TRACE("%s: %s\n",__func__,method);
    
    if (strcmp(method, "GET") == 0) {
        if (!Recordings_Container) {
            load_recordings();
        }

        const char* profileId = ACAP_HTTP_Request_Param(request, "id");
        if (profileId) {
            cJSON* recording = cJSON_GetObjectItem(Recordings_Container, profileId);
            if (!recording) {
                ACAP_HTTP_Respond_Error(response, 404, "Recording not found");
                return;
            }
            ACAP_HTTP_Respond_JSON(response, recording);
        } else {
            ACAP_HTTP_Respond_JSON(response, Recordings_Container);
        }
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        const char* profileId = ACAP_HTTP_Request_Param(request, "id");
        if (!profileId) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing profile ID");
            return;
        }
        
        if (Recordings_Clear(profileId) != 0) {
            ACAP_HTTP_Respond_Error(response, 500, "Failed to clear recordings");
            return;
        }
        
        ACAP_HTTP_Respond_Text(response, "Recordings cleared");
        return;
    }

    ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
}

static void HTTP_Endpoint_Image(const ACAP_HTTP_Response response, 
                                const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

	LOG_TRACE("%s: %s\n",__func__,method);
    
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    const char* profileId = ACAP_HTTP_Request_Param(request, "id");
    const char* indexStr = ACAP_HTTP_Request_Param(request, "index");

    if (!profileId || !indexStr) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing parameters");
        return;
    }

    int index = atoi(indexStr);
    
    char filename[256];
    snprintf(filename, sizeof(filename), "/var/spool/storage/SD_DISK/timelapse2/%s/image%05d.jpg", 
             profileId, index);

    FILE* file = fopen(filename, "rb");
    if (!file) {
        ACAP_HTTP_Respond_Error(response, 404, "Image not found");
        return;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(fileSize);
    if (!buffer) {
        fclose(file);
        ACAP_HTTP_Respond_Error(response, 500, "Memory allocation failed");
        return;
    }

    fread(buffer, 1, fileSize, file);
    fclose(file);

    ACAP_HTTP_Respond_String(response, "status: 200 OK\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Type: image/jpeg\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Length: %ld\r\n", fileSize);
    ACAP_HTTP_Respond_String(response, "\r\n");

    int result = ACAP_HTTP_Respond_Data(response, fileSize, buffer);
    if (result != 1) {
        LOG_WARN("%s: Failed to send image data\n", __func__);
    }

    free(buffer);
}

static void
HTTP_Endpoint_Download(const ACAP_HTTP_Response response, 
                                const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    const char* profileId = ACAP_HTTP_Request_Param(request, "id");
    const char* indexStr = ACAP_HTTP_Request_Param(request, "index");

    if (!profileId || !indexStr) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing parameters");
        return;
    }

    int index = atoi(indexStr);
    
    char filename[256];
    snprintf(filename, sizeof(filename), "/var/spool/storage/SD_DISK/timelapse2/%s/image%05d.jpg", 
             profileId, index);

    FILE* file = fopen(filename, "rb");
    if (!file) {
        ACAP_HTTP_Respond_Error(response, 404, "Image not found");
        return;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(fileSize);
    if (!buffer) {
        fclose(file);
        ACAP_HTTP_Respond_Error(response, 500, "Memory allocation failed");
        return;
    }

    fread(buffer, 1, fileSize, file);
    fclose(file);

    char downloadName[64];
    snprintf(downloadName, sizeof(downloadName), "image%05d.jpg", index);

    ACAP_HTTP_Respond_String(response, "status: 200 OK\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Description: File Transfer\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Type: image/jpeg\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Disposition: attachment; filename=%s\r\n", downloadName);
    ACAP_HTTP_Respond_String(response, "Content-Transfer-Encoding: binary\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Length: %lu\r\n", fileSize);
    ACAP_HTTP_Respond_String(response, "\r\n");
    ACAP_HTTP_Respond_Data(response, fileSize, buffer);

    free(buffer);
}

int
Recordings_Init(void) {
	LOG_TRACE("%s:\n",__func__);
    cJSON* recordings = load_recordings();
    ACAP_HTTP_Node("recordings", HTTP_Endpoint_Recordings);
	ACAP_HTTP_Node("image", HTTP_Endpoint_Image);
	ACAP_HTTP_Node("download", HTTP_Endpoint_Image);
    return 0;
}
