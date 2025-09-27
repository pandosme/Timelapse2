#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>

#include "vdo-stream.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "ACAP.h"
#include "cJSON.h"
#include "timelapse.h"
#include "recordings.h"
#include "sunevents.h"

#define APP_PACKAGE "timelapse2"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

void MAIN_Timelapse_Trigger(cJSON* profile) {
	char* json = cJSON_PrintUnformatted(profile);
	if (json) {
		LOG_TRACE("%s: %s\n", __func__, json);
		free(json);
	}

	// Check if profile has conditions
	const char* conditions = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "conditions"));
	LOG_TRACE("%s: D2D= %d S2S= %d Conditions= %s\n", 
              __func__, SunEvents_Between_Dawn_Dusk(), SunEvents_Between_Sunrise_Sunset(), conditions ? conditions : "None");

	if (conditions) {
		if (strcmp(conditions, "dawn_dusk") == 0 && SunEvents_Between_Dawn_Dusk() == 0 ) {
			LOG_TRACE("%s: Condition 'dawn_dusk' not met\n", __func__);
			return;
		}
		if (strcmp(conditions, "sunrise-sunset") == 0 && SunEvents_Between_Sunrise_Sunset() == 0 ) {
			LOG_TRACE("%s: Condition 'sunrise_sunset' not met\n", __func__);
			return;
		}
	}

	// All conditions met or no conditions, capture the recording
	LOG_TRACE("%s: All conditions met, capturing recording\n", __func__);
	Recordings_Capture(profile);
}


void Settings_Updated_Callback(const char* service, cJSON* data) {
    char* json = cJSON_PrintUnformatted(data);
    LOG_TRACE("%s: Service=%s Data=%s\n", __func__, service, json);
    free(json);
}

static void
HTTP_Endpoint_Reset(const ACAP_HTTP_Response response, 
                              const ACAP_HTTP_Request request) {
    const char* base_path = "/var/spool/storage/SD_DISK/timelapse2";
    DIR* dir = opendir(base_path);
    if (!dir) {
        LOG_WARN("%s: Cannot open directory %s\n", __func__, base_path);
        ACAP_HTTP_Respond_Text(response, "OK");
    }

	LOG("Resetting everything\n");

    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Delete directory contents first
                DIR* subdir = opendir(path);
                if (subdir) {
                    struct dirent* subentry;
                    while ((subentry = readdir(subdir))) {
                        if (strcmp(subentry->d_name, ".") == 0 || 
                            strcmp(subentry->d_name, "..") == 0)
                            continue;

                        char subpath[256];
                        snprintf(subpath, sizeof(subpath), "%s/%s", 
                                path, subentry->d_name);
                        unlink(subpath);
                    }
                    closedir(subdir);
                }
                rmdir(path);
            } else {
                unlink(path);
            }
        }
    }
    closedir(dir);

	Timelapse_Reset();
	Recordings_Reset();
	LOG("Everythin reset\n");
    ACAP_HTTP_Respond_Text(response, "OK");
}

static GMainLoop *main_loop = NULL;

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

static gboolean delayed_Init(gpointer user_data) {
    const char* timelapse_dir = "/var/spool/storage/SD_DISK/timelapse2";
    struct stat st;
    int dir_ok = 0;  // Track overall directory status
	LOG("SD Card will be initializing\n");

	LOG("Check SD Card\n");
    // 1. Check directory existence
    if (stat(timelapse_dir, &st) == -1) {
        if (errno == ENOENT) {
            // Directory doesn't exist - attempt creation
            LOG("Creating directory: %s", timelapse_dir);
            if (mkdir(timelapse_dir, 0755) == -1) {
				char error_message[256];
				sprintf(error_message, "Creation failed: %s (%s)", timelapse_dir, strerror(errno) );
                LOG_WARN("%s",error_message);
				ACAP_STATUS_SetBool("sdcard","present",0);
				ACAP_STATUS_SetString("sdcard","message",error_message);
            } else {
                LOG("Directory created successfully");
                dir_ok = 1;  // Tentatively mark OK until access check
				ACAP_STATUS_SetBool("sdcard","present",1);
				ACAP_STATUS_SetString("sdcard","message","");
            }
        } else {
            LOG_WARN("Directory check failed: %s (%s)", timelapse_dir, strerror(errno));
        }
    } else {
        dir_ok = 1;  // Exists, check access next
		ACAP_STATUS_SetBool("sdcard","present",1);
		ACAP_STATUS_SetString("sdcard","message","");
    }

    // 2. Verify directory accessibility
    if (dir_ok) {
        if (access(timelapse_dir, R_OK | W_OK | X_OK) == -1) {
			char error_message[256];
			sprintf(error_message, "Directory inaccessible: %s (%s)", timelapse_dir, strerror(errno) );
            LOG_WARN("%s",error_message);
			ACAP_STATUS_SetBool("sdcard","present",0);
			ACAP_STATUS_SetString("sdcard","message",error_message);
            dir_ok = 0;
        } else {
            LOG("Directory verified: %s", timelapse_dir);
			ACAP_STATUS_SetBool("sdcard","present",1);
			ACAP_STATUS_SetString("sdcard","message","");
        }
    }

    // 3. Update status and handle services
    if (dir_ok) {
        LOG("Timelapse settings OK\n");
        Timelapse_Init(MAIN_Timelapse_Trigger);
        LOG("Timelapse recording OK\n");
        Recordings_Init();
        LOG("Sun events OK\n");
        SunEvents_Init();
    } else {
        LOG_WARN("Cannot initialize services - directory unavailable");
    }

    LOG("SD Card OK\n");

    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
    LOG("------ Starting %s ------\n",APP_PACKAGE);
    ACAP_STATUS_SetString("app", "status", "The application is starting");


    ACAP(APP_PACKAGE, Settings_Updated_Callback);

	//Last resort for a corrupt file system on SD Card
    ACAP_HTTP_Node("reset", HTTP_Endpoint_Reset);

	LOG("SD Card will be initialized in 10 seconds\n");
	ACAP_STATUS_SetBool("sdcard","present",0);
	ACAP_STATUS_SetString("sdcard","message","Intializaing SD Card");
	g_timeout_add_seconds(6, delayed_Init, NULL);
	
    // Create and run the main loop
	main_loop = g_main_loop_new(NULL, FALSE);
	GMainContext *context = g_main_loop_get_context(main_loop);

    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed");
	}
	
    g_main_loop_run(main_loop);
	LOG("------ Exit %s ------\n",APP_PACKAGE);
    ACAP_Cleanup();
    closelog();
    return 0;
}
