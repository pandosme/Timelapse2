#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ACAP.h"
#include "cJSON.h"
#include "timelapse.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define TIMELAPSE_PATH "/var/spool/storage/SD_DISK/timelapse2/timelapse.json"

static cJSON *TimelapseProfiles = NULL;
static Timelapse_Callback Timelapse_ServiceCallBack = 0;

typedef struct {
    GSource* timer_source;
    cJSON* profile;
    int interval;
} TimelapseTimer;

static GHashTable* timelapse_timers = NULL;

static gboolean
Timer_Callback(gpointer user_data) {
    TimelapseTimer* timer = (TimelapseTimer*)user_data;
    if (timer && timer->profile && Timelapse_ServiceCallBack) {
        Timelapse_ServiceCallBack(timer->profile);
    }
    return G_SOURCE_CONTINUE;
}

// Add this helper function
static void Cleanup_Timer(const char* id) {
    if (!timelapse_timers) return;

    TimelapseTimer* timer = g_hash_table_lookup(timelapse_timers, id);
    if (timer) {
        LOG_TRACE("%s: Removing timer for profile %s\n", __func__, id);
        if (timer->timer_source) {
            g_source_destroy(timer->timer_source);
            g_source_unref(timer->timer_source);
        }
        g_hash_table_remove(timelapse_timers, id);
    }
}

// Enhanced null-check, consistent id handling, and logging added
static void Setup_Timer(cJSON* profile) {
    const char* id = cJSON_GetObjectItem(profile, "id")->valuestring;
    cJSON* timer_obj = cJSON_GetObjectItem(profile, "timer");

    if (!timer_obj || timer_obj->type != cJSON_Number) {
        LOG_WARN("Setup_Timer: Skipping, no valid timer for profile %s\n", id);
        return;
    }

    TimelapseTimer* timer = g_hash_table_lookup(timelapse_timers, id);
    if (timer) {
        if (timer->timer_source) {
            g_source_destroy(timer->timer_source);
            g_source_unref(timer->timer_source);
        }
    }
    timer = g_new0(TimelapseTimer, 1);
    g_hash_table_insert(timelapse_timers, g_strdup(id), timer);

    timer->profile = profile;
    timer->interval = timer_obj->valueint;
    timer->timer_source = g_timeout_source_new_seconds(timer->interval);
    g_source_set_callback(timer->timer_source, Timer_Callback, timer, NULL);
    g_source_attach(timer->timer_source, NULL);
	LOG("%s: Started repeating timer for %s every %d seconds\n", __func__, id, timer->interval);
}

void
Timelapse_Event_Callback(cJSON *event, void* jsonProfile) {

	//Only capture on triggers and stateful true
	cJSON* props = event->child;
	while(props) {
		if( props->type == cJSON_False )
			return;
		props = props->next;
	}

	char *json = cJSON_PrintUnformatted(event);
	if( json ) {
		LOG_TRACE("%s: %s",__func__,json);
		free(json);
	}

	if(Timelapse_ServiceCallBack)
		Timelapse_ServiceCallBack((cJSON*)jsonProfile);
}


static int Ensure_Directory_Exists(const char* id) {
    char path[256];
    snprintf(path, sizeof(path), "/var/spool/storage/SD_DISK/timelapse2/%s", id);
    LOG_TRACE("%s: Checking directory %s\n", __func__, path);
    
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        LOG_TRACE("%s: Creating directory %s\n", __func__, path);
        if (mkdir(path, 0755) != 0) {
            LOG_WARN("%s: Failed to create directory %s: %s\n", __func__, path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int DeleteDirectory(const char* path) {
	LOG_TRACE("%s: %s\n",__func__,path);

    struct dirent* entry;
    DIR* dir = opendir(path);


    if (!dir) {
        perror("opendir");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        struct stat statbuf;
        if (stat(fullPath, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                // Recursively delete subdirectory
                DeleteDirectory(fullPath);
            } else {
                // Delete file
                unlink(fullPath);
            }
        }
    }

    closedir(dir);

    // Remove the directory itself
    if (rmdir(path) != 0) {
        LOG_WARN("%s: Unable to remove directory %s", __func__,path);
        return -1;
    }

    return 0;
}

cJSON*
Timelapse_Find_Profile_By_Id( const char *id ) {
	if(!TimelapseProfiles)
		return 0;
	cJSON* profile = TimelapseProfiles->child;
	while(profile) {
		if( cJSON_GetObjectItem(profile,"id") && strcmp( cJSON_GetObjectItem(profile,"id")->valuestring, id ) == 0 )
			return profile;
		profile = profile->next;
	}
	LOG_TRACE("%s: No profile found with id %s\n",__func__,id);
	return 0;
}

cJSON*
Timelapse_Find_Profile_By_Name( const char *name ) {
	if(!TimelapseProfiles)
		return 0;
	cJSON* profile = TimelapseProfiles->child;
	while(profile) {
		if( cJSON_GetObjectItem(profile,"name") && strcmp( cJSON_GetObjectItem(profile,"name")->valuestring, name ) == 0 )
			return profile;
		profile = profile->next;
	}
	LOG_TRACE("%s: No profile found with name %s\n",__func__,name);
	return 0;
}

cJSON*
Timelapse_Find_Profile_By_Event_Name( const char *name ) {
	if(!TimelapseProfiles)
		return 0;
	cJSON* profile = TimelapseProfiles->child;
	while(profile) {
		cJSON* triggerEvent = cJSON_GetObjectItem(profile,"triggerEvent");
		if( triggerEvent && strcmp(cJSON_GetObjectItem(triggerEvent,name)->valuestring, name ) == 0 )
			return profile;
		profile = profile->next;
	}
	LOG_TRACE("%s: No profile found with event name %s\n",__func__,name);
	return 0;
}

int Timelapse_Remove_Profile_By_Id(const char* id) {
    if (!TimelapseProfiles)
        return 1;

    int index = 0;
    int removeIndex = -1;
    cJSON* remove = 0;
    cJSON* profile = TimelapseProfiles->child;
    
    while (profile) {
        if (cJSON_GetObjectItem(profile,"id") && 
            strcmp(cJSON_GetObjectItem(profile,"id")->valuestring, id) == 0) {
            remove = profile;
            removeIndex = index;
        }
        index++;
        profile = profile->next;
    }
    
    if (remove && removeIndex >= 0) {
        // Handle event subscription cleanup
        cJSON* triggerEvent = cJSON_GetObjectItem(remove, "triggerEvent");
        if (triggerEvent && triggerEvent->type == cJSON_Object) {
            int subscriptionId = cJSON_GetObjectItem(remove, "subscriptionId") ? 
                               cJSON_GetObjectItem(remove, "subscriptionId")->valueint : 0;
            if (subscriptionId) 
                ACAP_EVENTS_Unsubscribe(subscriptionId);
        }

        // Handle timer cleanup
        cJSON* timer = cJSON_GetObjectItem(remove, "timer");
        if (timer && timer->type == cJSON_Number) {
            Cleanup_Timer(id);
            LOG_TRACE("%s: Removing timer for profile %s\n", __func__, id);
        }

        cJSON_DeleteItemFromArray(TimelapseProfiles, removeIndex);
        LOG_TRACE("%s: Profile %s removed\n", __func__, id);
    } else {
        LOG_TRACE("%s: Profile %s not found\n", __func__, id);
    }
    return 1;
}

/* Add profile and subscribe to an event */
int
Timelapse_Activate_Profile( cJSON* profile ) {
	int subscriptionId	= 0;
	
	if( !profile ) {
		LOG_WARN("%s: profile is NULL\n",__func__);
		return 0;
	}
	char *json = cJSON_PrintUnformatted(profile);
	if( json ) {
		LOG_TRACE("%s: %s\n",__func__,json);
		free(json);
	}
	if(!TimelapseProfiles)
		TimelapseProfiles = cJSON_CreateArray();

	const char* profileId = cJSON_GetObjectItem(profile,"id")?cJSON_GetObjectItem(profile,"id")->valuestring:0;
	if( !profileId ) {
		LOG_WARN("%s: Invalid in in profile\n",__func__);
		return 0;
	}

	if(!cJSON_GetObjectItem(profile,"name") || !strlen(cJSON_GetObjectItem(profile,"name")->valuestring) ) {
		LOG_WARN("%s: Profile is missing name\n", __func__);
		return 0;
	}

	if(!cJSON_GetObjectItem(profile,"resolution") || !strlen(cJSON_GetObjectItem(profile,"resolution")->valuestring) ) {
		LOG_WARN("%s: Profile is missing resolution\n", __func__);
		return 0;
	}

	if(!cJSON_GetObjectItem(profile,"conditions") || !strlen(cJSON_GetObjectItem(profile,"conditions")->valuestring) ) {
		LOG_WARN("%s: Profile is missing conditions\n", __func__);
		return 0;
	}

	Timelapse_Remove_Profile_By_Id(profileId);

	if( cJSON_GetObjectItem( profile,"subscriptionId") )
		cJSON_DeleteItemFromObject(profile,"subscriptionId" );

	cJSON* triggerEvent = cJSON_GetObjectItem(profile,"triggerEvent");
	cJSON* timer = cJSON_GetObjectItem(profile,"timer");

	if( timer && timer->type == cJSON_Number ) {
		Setup_Timer(profile);
	}

	if( triggerEvent->type == cJSON_Object ) {
		subscriptionId = ACAP_EVENTS_Subscribe( triggerEvent, (void*)profile );
		if( !subscriptionId ) {
			LOG_WARN("%s: Unable to subscribe to event\n",__func__);
			return 0;
		}
		cJSON_AddNumberToObject(profile, "subscriptionId", subscriptionId);
	}
	cJSON_AddItemToArray(TimelapseProfiles,profile);
	return 1;
}

// Load profiles from JSON file
static int Timelapse_Load_Profiles() {
    LOG_TRACE("%s: Loading profiles from %s\n", __func__, TIMELAPSE_PATH);
    
    FILE *file = fopen(TIMELAPSE_PATH, "r");
    if (!file) {
        LOG_WARN("%s: File not found\n", __func__);
        TimelapseProfiles = cJSON_CreateArray();
        return Timelapse_Save_Profiles();
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    LOG_TRACE("%s: File size: %ld bytes\n", __func__, length);
    fseek(file, 0, SEEK_SET);

    char *data = (char *)malloc(length + 1);
    if (!data) {
        LOG_WARN("%s: Memory allocation failed\n", __func__);
        fclose(file);
        return -1;
    }

    size_t read = fread(data, 1, length, file);
    LOG_TRACE("%s: Read %zu bytes\n", __func__, read);
    fclose(file);

    data[length] = '\0';
    LOG_TRACE("%s: JSON content: %s\n", __func__, data);

	cJSON* fileList = cJSON_Parse(data);
    if (!fileList) {
		LOG_WARN("%s: Unable to parse profiles.json\n",__func__);
        return 0;
    }

    cJSON *profile;
    cJSON_ArrayForEach(profile, fileList) {
        cJSON *id_obj = cJSON_GetObjectItem(profile, "id");
        if (id_obj && id_obj->valuestring) {
            Ensure_Directory_Exists(id_obj->valuestring);
        }
		Timelapse_Activate_Profile(cJSON_Duplicate(profile, 1));
    }
	cJSON_Delete(fileList);
    free(data);
    return 1;
}

// Save profiles to JSON file
int Timelapse_Save_Profiles() {
	LOG_TRACE("%s: Saving profiles to %s\n", __func__, TIMELAPSE_PATH);	
    FILE *file = fopen(TIMELAPSE_PATH, "w");
    if (!file) {
        LOG_WARN("Error opening %s for writing\n", TIMELAPSE_PATH);
        return -1;
    }

    char *json_str = cJSON_Print(TimelapseProfiles);
    if (!json_str) {
        LOG_WARN("Error converting profiles to JSON string\n");
        fclose(file);
        return -1;
    }

    size_t written = fwrite(json_str, strlen(json_str), 1, file);
    free(json_str);
    fclose(file);

    if (written != 1) {
        LOG_WARN("Error writing to %s\n", TIMELAPSE_PATH);
        return -1;
    }

    return 0;
}

cJSON* Timelapse_Get_Profiles() {
    LOG_TRACE("%s: Getting profiles\n", __func__);
    
    // Add error checking
    if (!TimelapseProfiles) {
        LOG_WARN("%s: TimelapseProfiles is NULL\n", __func__);
        return cJSON_CreateArray();
    }
    return TimelapseProfiles;
}

static void HTTP_Endpoint_Timelpase(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {

    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        LOG_WARN("Invalid Request Method\n");
        ACAP_HTTP_Respond_Error(response, 400, "Invalid Request Method");
        return;
    }
    
    LOG_TRACE("%s: Method=%s\n", __func__, method);

    // Handle POST: Add a new timelapse profile
    if (strcmp(method, "POST") == 0) {
        const char* contentType = ACAP_HTTP_Get_Content_Type(request);
        if (!contentType || strcmp(contentType, "application/json") != 0) {
            ACAP_HTTP_Respond_Error(response, 415, "Unsupported Media Type - Use application/json");
            return;
        }

        if (!request->postData || request->postDataLength == 0) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing POST data");
            return;
        }
		LOG_TRACE("%s: %s",__func__,request->postData);
        cJSON* profile = cJSON_Parse(request->postData);
        if (!profile) {
            ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON data");
            return;
        }
		const char* id = cJSON_GetObjectItem(profile,"id")?cJSON_GetObjectItem(profile,"id")->valuestring:0;
		if( !id || !strlen(id) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing id");
			return;
		}
		const char* name = cJSON_GetObjectItem(profile,"name")?cJSON_GetObjectItem(profile,"name")->valuestring:0;
		if( !name || !strlen(name) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing name");
			return;
		}
		const char* resolution = cJSON_GetObjectItem(profile,"resolution")?cJSON_GetObjectItem(profile,"resolution")->valuestring:0;
		if( !resolution || !strlen(resolution) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing resolution");
			return;
		}
			
		if( !cJSON_GetObjectItem(profile,"fps" ) )
			cJSON_AddNumberToObject( profile,"fps", 10 );
		if( !cJSON_GetObjectItem(profile,"archived" ) )
			cJSON_AddNumberToObject( profile,"archived", 10 );
		
        // Add the new profile to the list
        if (!Timelapse_Activate_Profile(profile)) {
            cJSON_Delete(profile);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to add timelapse profile");
            return;
        }
		Ensure_Directory_Exists(id);
		Timelapse_Save_Profiles();
        ACAP_HTTP_Respond_Text(response, "Timelapse added successfully");
        return;
    }

    // Handle PUT: Update an existing timelapse profile by ID
    if (strcmp(method, "PUT") == 0) {
        const char* contentType = ACAP_HTTP_Get_Content_Type(request);
        if (!contentType || strcmp(contentType, "application/json") != 0) {
            ACAP_HTTP_Respond_Error(response, 415, "Unsupported Media Type - Use application/json");
            return;
        }

        if (!request->postData || request->postDataLength == 0) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing PUT data");
            return;
        }

        cJSON* profile = cJSON_Parse(request->postData);
        if (!profile) {
            ACAP_HTTP_Respond_Error(response, 400, "Parse error in JSON data");
            return;
        }

		const char* id = cJSON_GetObjectItem(profile,"id")?cJSON_GetObjectItem(profile,"id")->valuestring:0;
		if( !id || !strlen(id) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing id");
			return;
		}
		const char* name = cJSON_GetObjectItem(profile,"name")?cJSON_GetObjectItem(profile,"name")->valuestring:0;
		if( !name || !strlen(name) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing name");
			return;
		}
		const char* resolution = cJSON_GetObjectItem(profile,"resolution")?cJSON_GetObjectItem(profile,"resolution")->valuestring:0;
		if( !resolution || !strlen(resolution) ) {
            ACAP_HTTP_Respond_Error(response, 500, "Invalid profile. Missing resolution");
			return;
		}

		if( !cJSON_GetObjectItem(profile,"fps" ) )
			cJSON_AddNumberToObject( profile,"fps", 10 );
		if( !cJSON_GetObjectItem(profile,"archived" ) )
			cJSON_AddNumberToObject( profile,"archived", 10 );

        // Update the profile
        if (!Timelapse_Activate_Profile(profile)) {
            cJSON_Delete(profile);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to update timelapse profile");
            return;
        }
		
		Ensure_Directory_Exists(id);
		Timelapse_Save_Profiles();
        ACAP_HTTP_Respond_Text(response, "Timelapse updated successfully");
        return;
    }

    // Handle DELETE: Remove a timelapse profile by ID
	if (strcmp(method, "DELETE") == 0) {
		const char* id = ACAP_HTTP_Request_Param(request, "id");
		LOG_TRACE("%s: DELETE %s\n",__func__,id);
		if (!id) {
			LOG_WARN("%s: DELETE missing id parameter\n", __func__);
			ACAP_HTTP_Respond_Error(response, 400, "Missing 'id' parameter");
			return;
		}

		// Remove from profiles
		if (!Timelapse_Remove_Profile_By_Id(id)) {
			LOG_WARN("%s: DELETE failed on %s\n", __func__,id);
			ACAP_HTTP_Respond_Error(response, 500, "Failed to delete timelapse profile");
			return;
		}

		// Save changes
		Timelapse_Save_Profiles();

		// Respond success
		ACAP_HTTP_Respond_Text(response, "Timelapse deleted successfully");
		LOG_TRACE("%s: DELETE Success\n",__func__);
		return;
	}

    // Handle GET: Retrieve all timelapse profiles
    if (strcmp(method, "GET") == 0) {
        cJSON* profiles = Timelapse_Get_Profiles();
        if (!profiles) {
            LOG_WARN("%s: Failed to get profiles\n", __func__);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to retrieve timelapse profiles");
            return;
        }

        char *json_str = cJSON_PrintUnformatted(profiles);
        if (!json_str) {
            LOG_WARN("%s: Failed to serialize profiles\n", __func__);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to serialize profiles");
            return;
        }

        LOG_TRACE("%s: Response JSON: %s\n", __func__, json_str);
        ACAP_HTTP_Respond_JSON(response, profiles);
        free(json_str);
        return;
    }

    // If method is not supported
    ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
}

void Timelapse_Reset() {
	Timelapse_Load_Profiles();
	
    if (!timelapse_timers) return;

    GHashTableIter iter;
    gpointer key, value;
    
    g_hash_table_iter_init(&iter, timelapse_timers);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TimelapseTimer* timer = (TimelapseTimer*)value;
        if (timer && timer->timer_source) {
            g_source_destroy(timer->timer_source);
            g_source_unref(timer->timer_source);
        }
    }
    g_hash_table_destroy(timelapse_timers);
    timelapse_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

int
Timelapse_Init(Timelapse_Callback callback) {
    Timelapse_ServiceCallBack = callback;
    ACAP_EVENTS_Unsubscribe(0);
    
    // Initialize timer hash table
    timelapse_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    ACAP_HTTP_Node("timelapse", HTTP_Endpoint_Timelpase);
    ACAP_EVENTS_SetCallback(Timelapse_Event_Callback);
    return Timelapse_Load_Profiles();
}
