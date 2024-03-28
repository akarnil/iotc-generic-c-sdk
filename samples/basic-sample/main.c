/* SPDX-License-Identifier: MIT
 * Copyright (C) 2020-2024 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotcl.h"
#include "iotconnect.h"

#include "app_config.h"
#include "cJSON.h"


#include <time.h>
#include <sys/stat.h>


// windows compatibility
#if defined(_WIN32) || defined(_WIN64)
#define F_OK 0
#include <Windows.h>
#include <io.h>
#define sleep       Sleep
#define access      _access_s
#else
#include <unistd.h>
#endif

#define APP_VERSION "00.01.00"
#define STRINGS_ARE_EQUAL 0
#define FREE(x) if ((x)) { free(x); (x) = NULL; }

typedef struct telemetry_attribute
{
    char* name;
    int name_len;

    char* path;
    int path_len;

    bool read_ascii;
    time_t last_accessed;

} telemetry_attribute_t;



static void on_connection_status(IotConnectMqttStatus status) {
    // Add your own status handling
    switch (status) {
        case IOTC_CS_MQTT_CONNECTED:
            printf("IoTConnect Client Connected notification.\n");
            break;
        case IOTC_CS_MQTT_DISCONNECTED:
            printf("IoTConnect Client Disconnected notification.\n");
            break;
        case IOTC_CS_MQTT_DELIVERED:
            printf("IoTConnect Client message delivered.\n");
            break;
        case IOTC_CS_MQTT_SEND_FAILED:
            printf("IoTConnect Client message send failed!\n");
            break;
        default:
            printf("IoTConnect Client ERROR notification\n");
            break;
    }
}

static void on_command(IotclC2dEventData data) {
    const char *command = iotcl_c2d_get_command(data);
    const char *ack_id = iotcl_c2d_get_ack_id(data);
    if (command) {
        printf("Command %s received with %s ACK ID\n", command, ack_id ? ack_id : "no");
        // could be a command without acknowledgement, so ackID can be null
        if (ack_id) {
            iotcl_mqtt_send_cmd_ack(ack_id, IOTCL_C2D_EVT_CMD_FAILED, "Not implemented");
        }
    } else {
        // could be a command without acknowledgement, so ackID can be null
        printf("Failed to parse command\n");
        if (ack_id) {
            iotcl_mqtt_send_cmd_ack(ack_id, IOTCL_C2D_EVT_CMD_FAILED, "Internal error");
        }
    }
}

static bool is_app_version_same_as_ota(const char *version) {
    return strcmp(APP_VERSION, version) == 0;
}

static bool app_needs_ota_update(const char *version) {
    return strcmp(APP_VERSION, version) < 0;
}

// This sample OTA handling only checks the version and verifies if the firmware needs an update but does not download.
static void on_ota(IotclC2dEventData data) {
    const char *message = NULL;
    const char *url = iotcl_c2d_get_ota_url(data, 0);
    const char *ack_id = iotcl_c2d_get_ack_id(data);
    bool success = false;
    if (NULL != url) {
        printf("Download URL is: %s\n", url);
        const char *version = iotcl_c2d_get_ota_sw_version(data);
        if (is_app_version_same_as_ota(version)) {
            printf("OTA request for same version %s. Sending success\n", version);
            success = true;
            message = "Version is matching";
        } else if (app_needs_ota_update(version)) {
            printf("OTA update is required for version %s.\n", version);
            success = false;
            message = "Not implemented";
        } else {
            printf("Device firmware version %s is newer than OTA version %s. Sending failure\n", APP_VERSION,
                   version);
            // Not sure what to do here. The app version is better than OTA version.
            // Probably a development version, so return failure?
            // The user should decide here.
            success = false;
            message = "Device firmware version is newer";
        }
    }

    iotcl_mqtt_send_ota_ack(ack_id, (success ? IOTCL_C2D_EVT_OTA_SUCCESS : IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED), message);
}

static void publish_telemetry(int number_of_attributes, telemetry_attribute_t* telemetry) {
    IotclMessageHandle msg = iotcl_telemetry_create();

    for (int i = 0; i < number_of_attributes; i++)
    {
        if (access(telemetry[i].path, F_OK) != 0 )
        {
            printf("failed to access input telemetry path - %s ; Skipping\n", telemetry[i].path);
            continue;
        }
        struct stat file_stat;
        if (stat(telemetry[i].path, &file_stat) == -1)
        {
            fprintf(stderr, "Error: %s\n", strerror(telemetry[i].path));
            continue;
        }

        time_t modified_time = file_stat.st_mtime;
        if (modified_time <= telemetry[i].last_accessed)
        {
            printf("telemetry not updated since last send - %s ; Skipping\n", telemetry[i].path);
            continue;
        }

        FILE* fp = fopen(telemetry[i].path, "r");
        char* buffer = NULL;
        size_t len;
        ssize_t bytes_read = getdelim( &buffer, &len, '\0', fp);
        
        fclose(fp);

        iotcl_telemetry_set_string(msg, telemetry[i].name, APP_VERSION);

        telemetry[i].last_accessed = modified_time;

    }

    // STRING template field type
    iotcl_telemetry_set_string(msg, "version", APP_VERSION);

    // INTEGER template field type
    int random_int =  (int) ((double) rand() / RAND_MAX * 10.0); // ger an integer from 0 to 9 first
    iotcl_telemetry_set_number(msg, "random_int", (double) random_int);

    // DECIMAL template field type
    iotcl_telemetry_set_number(msg, "random_decimal", (double) rand() / RAND_MAX);

    // BOOLEAN template field type
    iotcl_telemetry_set_bool(msg, "random_boolean", ((double) rand() / RAND_MAX) > 0.5 ? true: false);

    // OBJECT template field type with two nested DECIMAL values
    iotcl_telemetry_set_number(msg, "coordinate.x", (double) rand() / RAND_MAX * 10.0);
    iotcl_telemetry_set_number(msg, "coordinate.y", (double) rand() / RAND_MAX * 10.0);

    iotcl_mqtt_send_telemetry(msg, false);
    iotcl_telemetry_destroy(msg);
}


static bool string_ends_with(const char * needle, const char* haystack)
{
    const char *str_end = haystack + strlen(haystack) -  strlen(needle);
    return (strncmp(str_end, needle, strlen(needle) ) == 0);
}

int parse_raw_json_to_string(char* output, const char * const raw_json_str, char* key)
{
    const cJSON *value = NULL;
    cJSON *json = cJSON_Parse(raw_json_str);
    if (json == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        cJSON_Delete(json);
        return EXIT_FAILURE; 
    }

    value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(value) && (value->valuestring != NULL))
    {
        strncpy(output,value->valuestring, strlen(value->valuestring));
        
        cJSON_Delete(json);
        return EXIT_SUCCESS;
    }

    cJSON_Delete(json);
    return EXIT_FAILURE;
}

int parse_json_to_string(char* output, cJSON* json, char* key)
{
    const cJSON *value = NULL;
    value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(value) && (value->valuestring != NULL))
    {
        strncpy(output,value->valuestring, strlen(value->valuestring));
        
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}

char device_id[256] = {};
char company_id[256] = {};
char environment[256] = {};
char iotc_server_cert_path[4096] = {};
char sdk_id[256] = {};

char connection_type_str[256] = {};
IotConnectConnectionType connection_type = 0; 

char auth_type[256] = {};
char commands_list_path[4096] = {};




int main(int argc, char *argv[]) {

    IotConnectClientConfig config;
    iotconnect_sdk_init_config(&config);

    telemetry_attribute_t* telemetry = NULL; 
    

    // need to pull the json path from argv
    char json_path[4096] = "";

    // if (argc == 2) {};

    char json[] = "/home/akarnil/work/iotc-generic-c-sdk/samples/basic-sample/config.json";
    strncpy(json_path,json,strlen(json));

    if (access(json_path, F_OK) != 0)
    {
        printf("failed to access input json file - %s ; Aborting\n", json_path);
        return EXIT_FAILURE;
    }

    FILE* fd = fopen(json_path, "r");
    if (!fd)
    {
        printf("File failed to open - %s", json_path);
        return EXIT_FAILURE;
    }
    fseek(fd, 0l, SEEK_END);
    long file_len = ftell(fd);

    if (file_len <= 0)
    {
        printf("failed calculating file length: %ld. Aborting\n", file_len);
        return EXIT_FAILURE;
    }
    rewind(fd);

    char* json_str = (char*)calloc(file_len+1, sizeof(char));
    if (!json_str)
    {
        printf("failed to calloc. Aborting\n");
        json_str = NULL;
        return EXIT_FAILURE;
    }
    
    for (int i = 0; i < file_len; i++)
    {
        json_str[i] = fgetc(fd);
    }
    fclose(fd);

    cJSON* json_parser = NULL;
    json_parser = cJSON_Parse(json_str);
    if (!json_parser)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        cJSON_Delete(json_parser);
        return EXIT_FAILURE;
    }

    int parsing_result = 0;
    parsing_result += parse_raw_json_to_string(device_id,json_str,"duid");
    parsing_result += parse_raw_json_to_string(company_id,json_str,"cpid");
    parsing_result += parse_raw_json_to_string(environment,json_str,"env");
    parsing_result += parse_raw_json_to_string(iotc_server_cert_path,json_str,"iotc_server_cert");
    parsing_result += parse_raw_json_to_string(sdk_id,json_str,"sdk_id");
    parsing_result += parse_raw_json_to_string(connection_type_str,json_str,"connection_type");

    if (parsing_result != 0)
    {
        return EXIT_FAILURE;
    }

    cJSON* auth_parser = cJSON_GetObjectItemCaseSensitive(json_parser, "auth");
    parsing_result += parse_json_to_string(auth_type,auth_parser,"auth_type");

    if (strcmp(connection_type_str, "IOTC_CT_AWS") == STRINGS_ARE_EQUAL){
        connection_type = IOTC_CT_AWS;
    }
    else if (strcmp(connection_type_str, "IOTC_CT_AZURE") == STRINGS_ARE_EQUAL){
        connection_type = IOTC_CT_AZURE;
    }

    if (strcmp(auth_type, "IOTC_AT_X509") == STRINGS_ARE_EQUAL){
        config.auth_info.type= IOTC_AT_X509;
        char client_key[256] = {};
        char client_cert[256] = {};

        cJSON* params_parser = cJSON_GetObjectItemCaseSensitive(auth_parser, "params");
        parsing_result += parse_json_to_string(client_key, params_parser, "client_key");
        parsing_result += parse_json_to_string(client_cert, params_parser, "client_cert");
        cJSON_free(params_parser);

        config.auth_info.data.cert_info.device_cert = client_cert;
        config.auth_info.data.cert_info.device_key = client_key;

    } else if (strcmp(auth_type, "IOTC_AT_SYMMETRIC_KEY") == STRINGS_ARE_EQUAL){
        config.auth_info.type= IOTC_AT_SYMMETRIC_KEY;
    } else if (strcmp(auth_type, "IOTC_AT_TPM") == STRINGS_ARE_EQUAL) {
        // config.auth_info.type= IOTC_AT_TPM;
    } else if (strcmp(auth_type, "IOTC_AT_TOKEN") == STRINGS_ARE_EQUAL) {
        // config.auth_info.type= IOTC_AT_TOKEN;
    } else {
        printf("unsupported auth type. Aborting\r\n");
        return EXIT_FAILURE;
    }

    cJSON* device_parser = cJSON_GetObjectItemCaseSensitive(json_parser, "device");
    parsing_result += parse_json_to_string(commands_list_path,device_parser,"commands_list_path");


    cJSON* attribute = NULL;
    cJSON* attributes_parser = cJSON_GetObjectItemCaseSensitive(device_parser, "attributes");
    int number_of_attributes = 0;
    cJSON_ArrayForEach(attribute, attributes_parser)
    {
        number_of_attributes++;
    }
    telemetry = (telemetry_attribute_t*)calloc(number_of_attributes, sizeof(telemetry_attribute_t)); 
    telemetry_attribute_t* telem_ptr = telemetry;
    cJSON_ArrayForEach(attribute, attributes_parser)
    {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(attribute, "name");
        telem_ptr->name_len = strlen(name->valuestring);
        telem_ptr->name = calloc(telem_ptr->name_len, sizeof(char));
        strncpy(telem_ptr->name, name->valuestring, telem_ptr->name_len);
        
        cJSON *path = cJSON_GetObjectItemCaseSensitive(attribute, "private_data");
        telem_ptr->path_len = strlen(path->valuestring);
        telem_ptr->path = calloc(telem_ptr->path_len, sizeof(char));
        strncpy(telem_ptr->path, path->valuestring, telem_ptr->path_len);
        
        cJSON *read_type = cJSON_GetObjectItemCaseSensitive(attribute, "private_data_type");
        if (strncmp(read_type->valuestring, "ascii", strlen("ascii")) == STRINGS_ARE_EQUAL)
        {
            telem_ptr->read_ascii = true;
        }

        telem_ptr++;
    }

    printf("%s\n", commands_list_path);
    cJSON_free(attributes_parser);
    cJSON_free(device_parser);
    cJSON_free(auth_parser);
    cJSON_free(json_parser);



    printf("%s\n", device_id);
    free(json_str);




    // char *trust_store;

    (void) argc;
    (void) argv;

// #ifdef IOTCONNECT_MQTT_SERVER_CA_CERT
//     trust_store = IOTCONNECT_CA_CERT_PATH
// #else
//     if (IOTCONNECT_CONNECTION_TYPE == IOTC_CT_AWS) {
//         trust_store = IOTCONNECT_MQTT_SERVER_CA_CERT_DEFAULT_AWS;
//     } else {
//         trust_store = IOTCONNECT_MQTT_SERVER_CA_CERT_DEFAULT_AZURE;
//     }
// #endif

//     if (access(trust_store, F_OK) != 0) {
//         printf("Unable to access the MQTT CA certificate. "
//                "Please change directory so that %s can be accessed from the application or update IOTCONNECT_CERT_PATH",
//                trust_store);
//         return -1;
//     }

//     if (IOTCONNECT_AUTH_TYPE == IOTC_AT_X509) {
//         if (access(IOTCONNECT_DEVICE_CERT, F_OK) != 0
//                 ) {
//             printf("Unable to access device identity certificate. "
//                    "Please change directory so that %s can be accessed from the application or update IOTCONNECT_DEVICE_CERT",
//                    IOTCONNECT_DEVICE_CERT);
//             return -1;
//         }
//         if (access(IOTCONNECT_DEVICE_PRIVATE_KEY, F_OK) != 0
//                 ) {
//             printf("Unable to access device identity private key. "
//                    "Please change directory so that %s can be accessed from the application or update IOTCONNECT_DEVICE_PRIVATE_KEY",
//                    IOTCONNECT_DEVICE_PRIVATE_KEY);
//             return -1;
//         }
//     }

    config.cpid = company_id;
    config.env = environment;
    config.duid = device_id;
    config.connection_type = connection_type;
    // config.auth_info.type = IOTCONNECT_AUTH_TYPE;
    config.auth_info.trust_store = iotc_server_cert_path;
    config.verbose = true;


    // if (config.auth_info.type == IOTC_AT_X509) {
    //     config.auth_info.data.cert_info.device_cert = IOTCONNECT_DEVICE_CERT;
    //     config.auth_info.data.cert_info.device_key = IOTCONNECT_DEVICE_PRIVATE_KEY;
    // } else if (config.auth_info.type == IOTC_AT_SYMMETRIC_KEY){
    //     config.auth_info.data.symmetric_key = IOTCONNECT_SYMMETRIC_KEY;
    // } else {
    //     // none of the above
    //     printf("Unknown IotConnectAuthType\n");
    //     return -1;
    // }


    config.status_cb = on_connection_status;
    config.ota_cb = on_ota;
    config.cmd_cb = on_command;

    // initialize random seed for the telemetry test
    srand((unsigned int) time(NULL));

    // run a dozen connect/send/disconnect cycles with each cycle being about a minute
    int ret = iotconnect_sdk_init(&config);
    if (0 != ret) {
        printf("iotconnect_sdk_init() exited with error code %d\n", ret);
        return ret;
    }

    ret = iotconnect_sdk_connect();
    if (0 != ret) {
        printf("iotconnect_sdk_init() exited with error code %d\n", ret);
        return ret;
    }

    while (iotconnect_sdk_is_connected())
    {
        publish_telemetry(number_of_attributes, telemetry);
        sleep(5);
    }
    
    iotconnect_sdk_disconnect();
    iotconnect_sdk_deinit();

    printf("Basic sample demo is complete. Exiting.\n");


    // free attributes
    for (int i = 0; i < number_of_attributes; i++)
    {
        printf("%s %s\n", telemetry[i].name, telemetry[i].path);
        free(telemetry[i].name);
        free(telemetry[i].path);
    }
    free(telemetry);

    return 0;
}
