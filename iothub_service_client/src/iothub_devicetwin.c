// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <ctype.h>
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/string_tokenizer.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/httpapiex.h"
#include "azure_c_shared_utility/httpapiexsas.h"
#include "azure_c_shared_utility/azure_base64.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_c_shared_utility/connection_string_parser.h"

#include "parson.h"
#include "iothub_devicetwin.h"
#include "iothub_sc_version.h"

#define IOTHUB_TWIN_REQUEST_MODE_VALUES    \
    IOTHUB_TWIN_REQUEST_GET,               \
    IOTHUB_TWIN_REQUEST_UPDATE,            \
    IOTHUB_TWIN_REQUEST_REPLACE_TAGS,      \
    IOTHUB_TWIN_REQUEST_REPLACE_DESIRED,   \
    IOTHUB_TWIN_REQUEST_UPDATE_DESIRED

MU_DEFINE_ENUM(IOTHUB_TWIN_REQUEST_MODE, IOTHUB_TWIN_REQUEST_MODE_VALUES);
MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(IOTHUB_DEVICE_TWIN_RESULT, IOTHUB_DEVICE_TWIN_RESULT_VALUES);

#define  HTTP_HEADER_KEY_AUTHORIZATION  "Authorization"
#define  HTTP_HEADER_VAL_AUTHORIZATION  " "
#define  HTTP_HEADER_KEY_REQUEST_ID  "Request-Id"
#define  HTTP_HEADER_KEY_USER_AGENT  "User-Agent"
#define  HTTP_HEADER_VAL_USER_AGENT  IOTHUB_SERVICE_CLIENT_TYPE_PREFIX IOTHUB_SERVICE_CLIENT_BACKSLASH IOTHUB_SERVICE_CLIENT_VERSION
#define  HTTP_HEADER_KEY_ACCEPT  "Accept"
#define  HTTP_HEADER_VAL_ACCEPT  "application/json"
#define  HTTP_HEADER_KEY_CONTENT_TYPE  "Content-Type"
#define  HTTP_HEADER_VAL_CONTENT_TYPE  "application/json; charset=utf-8"
#define  HTTP_HEADER_KEY_IFMATCH  "If-Match"
#define  HTTP_HEADER_VAL_IFMATCH  "*"
#define UID_LENGTH 37

/*
 * ============================================================================
 * AZURE IOT HUB REST API ENDPOINTS FOR DEVICE TWIN OPERATIONS
 * ============================================================================
 * 
 * This SDK uses the Azure IoT Hub REST API version 2020-09-30 for Device Twin operations.
 * 
 * CURRENT DEFAULT REST API REQUESTS USED TO DOWNLOAD DEVICE TWIN:
 * ----------------------------------------------------------------
 * 
 * 1. GET FULL DEVICE TWIN (used by IoTHubDeviceTwin_GetTwin):
 *    Request:  GET https://{iothub-hostname}/twins/{deviceId}?api-version=2020-09-30
 *    Returns:  COMPLETE Device Twin JSON document including:
 *              - deviceId, etag, version, status, connectionState, lastActivityTime
 *              - tags (all tag metadata set by backend)
 *              - properties.desired (all desired properties with $metadata and $version)
 *              - properties.reported (all reported properties with $metadata and $version)
 *    
 *    Example Full Response:
 *    {
 *      "deviceId": "myDevice",
 *      "etag": "AAAAAAAAAAE=",
 *      "version": 5,
 *      "tags": { "location": "building1" },
 *      "properties": {
 *        "desired": { 
 *          "telemetryInterval": 60,
 *          "$metadata": {...}, 
 *          "$version": 3 
 *        },
 *        "reported": { 
 *          "connectivity": "wifi",
 *          "$metadata": {...}, 
 *          "$version": 2 
 *        }
 *      }
 *    }
 * 
 * 2. GET FULL MODULE TWIN (used by IoTHubDeviceTwin_GetModuleTwin):
 *    Request:  GET https://{iothub-hostname}/twins/{deviceId}/modules/{moduleId}?api-version=2020-09-30
 *    Returns:  Complete Module Twin JSON (similar structure to device twin)
 * 
 * 3. UPDATE DEVICE TWIN (used by IoTHubDeviceTwin_UpdateTwin):
 *    Request:  PATCH https://{iothub-hostname}/twins/{deviceId}?api-version=2020-09-30
 *    Body:     Partial JSON with only properties to update
 *    Returns:  Updated full Device Twin JSON
 * 
 * LIMITATION OF DEFAULT API:
 * --------------------------
 * The default GET /twins/{deviceId} endpoint ALWAYS returns the COMPLETE twin document.
 * There is NO way to request only "desired" or only "reported" properties using this endpoint.
 * This can result in:
 *   - Large JSON payloads when you only need specific properties
 *   - Unnecessary network bandwidth consumption
 *   - Slower parsing when processing large twin documents
 * 
 * NEW PARTIAL RETRIEVAL CAPABILITY (Added in this update):
 * ---------------------------------------------------------
 * 
 * To retrieve ONLY PARTIAL twin data (e.g., only desired properties, only reported properties,
 * or specific fields within them), this SDK now provides the IoTHubDeviceTwin_QueryTwin() function
 * which uses the Azure IoT Hub QUERY API:
 * 
 * 4. QUERY FOR PARTIAL DEVICE TWIN (NEW - IoTHubDeviceTwin_QueryTwin):
 *    Request:  POST https://{iothub-hostname}/devices/query?api-version=2020-09-30
 *    Body:     JSON with SQL-like query to select specific properties
 *    Returns:  Array of JSON objects containing ONLY the requested fields
 *    
 *    Example Queries:
 *    
 *    a) Get ONLY desired properties:
 *       Query: "SELECT properties.desired FROM devices WHERE deviceId = 'myDevice'"
 *       Returns: [{"properties": {"desired": {"telemetryInterval": 60, "$version": 3}}}]
 *    
 *    b) Get ONLY reported properties:
 *       Query: "SELECT properties.reported FROM devices WHERE deviceId = 'myDevice'"
 *       Returns: [{"properties": {"reported": {"connectivity": "wifi", "$version": 2}}}]
 *    
 *    c) Get ONLY specific fields from desired properties:
 *       Query: "SELECT properties.desired.telemetryInterval FROM devices WHERE deviceId = 'myDevice'"
 *       Returns: [{"properties": {"desired": {"telemetryInterval": 60}}}]
 *    
 *    d) Get both desired and reported (but no tags or metadata):
 *       Query: "SELECT properties.desired, properties.reported FROM devices WHERE deviceId = 'myDevice'"
 *       Returns: [{"properties": {"desired": {...}, "reported": {...}}}]
 * 
 * BENEFITS OF PARTIAL RETRIEVAL:
 * -------------------------------
 * - Reduced network bandwidth (only download what you need)
 * - Faster JSON parsing (smaller documents)
 * - More efficient when working with large twin documents
 * - Ability to filter multiple devices and get specific properties in one call
 * 
 * ============================================================================
 */

static const char* URL_API_VERSION = "?api-version=2020-09-30";

static const char* RELATIVE_PATH_FMT_TWIN = "/twins/%s%s";
static const char* RELATIVE_PATH_FMT_TWIN_MODULE = "/twins/%s/modules/%s%s";
static const char* RELATIVE_PATH_FMT_QUERY = "/devices/query%s";  // NEW: Query API endpoint for partial twin retrieval


/** @brief Structure to store IoTHub authentication information
*/
typedef struct IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_TAG
{
    char* hostname;
    char* sharedAccessKey;
    char* keyName;
} IOTHUB_SERVICE_CLIENT_DEVICE_TWIN;

static const char* generateGuid(void)
{
    char* result;

    if ((result = malloc(UID_LENGTH)) != NULL)
    {
        result[0] = '\0';
        if (UniqueId_Generate(result, UID_LENGTH) != UNIQUEID_OK)
        {
            free((void*)result);
            result = NULL;
        }
    }
    return (const char*)result;
}

static STRING_HANDLE createRelativePath(IOTHUB_TWIN_REQUEST_MODE iotHubTwinRequestMode, const char* deviceId, const char* moduleId)
{
    //IOTHUB_TWIN_REQUEST_GET               GET      {iot hub}/twins/{device id}                     // Get device twin
    //IOTHUB_TWIN_REQUEST_UPDATE            PATCH    {iot hub}/twins/{device id}                     // Partally update device twin
    //IOTHUB_TWIN_REQUEST_REPLACE_TAGS      PUT      {iot hub}/twins/{device id}/tags                // Replace update tags
    //IOTHUB_TWIN_REQUEST_REPLACE_DESIRED   PUT      {iot hub}/twins/{device id}/properties/desired  // Replace update desired properties
    //IOTHUB_TWIN_REQUEST_UPDATE_DESIRED    PATCH    {iot hub}/twins/{device id}/properties/desired  // Partially update desired properties

    STRING_HANDLE result;

    if ((iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_GET) || (iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_UPDATE))
    {
        if (moduleId == NULL)
        {
            result = STRING_construct_sprintf(RELATIVE_PATH_FMT_TWIN, deviceId, URL_API_VERSION);
        }
        else
        {
            result = STRING_construct_sprintf(RELATIVE_PATH_FMT_TWIN_MODULE, deviceId, moduleId, URL_API_VERSION);
        }
    }
    else
    {
        result = NULL;
    }
    return result;
}

static HTTP_HEADERS_HANDLE createHttpHeader(IOTHUB_TWIN_REQUEST_MODE iotHubTwinRequestMode)
{
    HTTP_HEADERS_HANDLE httpHeader;
    const char* guid = NULL;

    if ((httpHeader = HTTPHeaders_Alloc()) == NULL)
    {
        LogError("HTTPHeaders_Alloc failed");
    }
    else if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, HTTP_HEADER_KEY_AUTHORIZATION, HTTP_HEADER_VAL_AUTHORIZATION) != HTTP_HEADERS_OK)
    {
        LogError("HTTPHeaders_AddHeaderNameValuePair failed for Authorization header");
        HTTPHeaders_Free(httpHeader);
        httpHeader = NULL;
    }
    else if ((guid = generateGuid()) == NULL)
    {
        LogError("GUID creation failed");
        HTTPHeaders_Free(httpHeader);
        httpHeader = NULL;
    }
    else if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, HTTP_HEADER_KEY_REQUEST_ID, guid) != HTTP_HEADERS_OK)
    {
        LogError("HTTPHeaders_AddHeaderNameValuePair failed for RequestId header");
        HTTPHeaders_Free(httpHeader);
        httpHeader = NULL;
    }
    else if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, HTTP_HEADER_KEY_USER_AGENT, HTTP_HEADER_VAL_USER_AGENT) != HTTP_HEADERS_OK)
    {
        LogError("HTTPHeaders_AddHeaderNameValuePair failed for User-Agent header");
        HTTPHeaders_Free(httpHeader);
        httpHeader = NULL;
    }
    else if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, HTTP_HEADER_KEY_CONTENT_TYPE, HTTP_HEADER_VAL_CONTENT_TYPE) != HTTP_HEADERS_OK)
    {
        LogError("HTTPHeaders_AddHeaderNameValuePair failed for Content-Type header");
        HTTPHeaders_Free(httpHeader);
        httpHeader = NULL;
    }
    else if (iotHubTwinRequestMode != IOTHUB_TWIN_REQUEST_GET)
    {
        if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, HTTP_HEADER_KEY_IFMATCH, HTTP_HEADER_VAL_IFMATCH) != HTTP_HEADERS_OK)
        {
            LogError("HTTPHeaders_AddHeaderNameValuePair failed for If-Match header");
            HTTPHeaders_Free(httpHeader);
            httpHeader = NULL;
        }
    }
    free((void*)guid);

    return httpHeader;
}

static IOTHUB_DEVICE_TWIN_RESULT sendHttpRequestTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, IOTHUB_TWIN_REQUEST_MODE iotHubTwinRequestMode, const char* deviceName, const char* moduleId, BUFFER_HANDLE deviceJsonBuffer, BUFFER_HANDLE responseBuffer)
{
    IOTHUB_DEVICE_TWIN_RESULT result;

    STRING_HANDLE uriResource = NULL;
    STRING_HANDLE accessKey = NULL;
    STRING_HANDLE keyName = NULL;
    HTTPAPIEX_SAS_HANDLE httpExApiSasHandle;
    HTTPAPIEX_HANDLE httpExApiHandle;
    HTTP_HEADERS_HANDLE httpHeader;

    if ((uriResource = STRING_construct(serviceClientDeviceTwinHandle->hostname)) == NULL)
    {
        LogError("STRING_construct failed for uriResource");
        result = IOTHUB_DEVICE_TWIN_ERROR;
    }
    else if ((accessKey = STRING_construct(serviceClientDeviceTwinHandle->sharedAccessKey)) == NULL)
    {
        LogError("STRING_construct failed for accessKey");
        STRING_delete(uriResource);
        result = IOTHUB_DEVICE_TWIN_ERROR;
    }
    else if ((keyName = STRING_construct(serviceClientDeviceTwinHandle->keyName)) == NULL)
    {
        LogError("STRING_construct failed for keyName");
        STRING_delete(accessKey);
        STRING_delete(uriResource);
        result = IOTHUB_DEVICE_TWIN_ERROR;
    }
    else if ((httpHeader = createHttpHeader(iotHubTwinRequestMode)) == NULL)
    {
        LogError("HttpHeader creation failed");
        STRING_delete(keyName);
        STRING_delete(accessKey);
        STRING_delete(uriResource);
        result = IOTHUB_DEVICE_TWIN_ERROR;
    }
    else if ((httpExApiSasHandle = HTTPAPIEX_SAS_Create(accessKey, uriResource, keyName)) == NULL)
    {
        LogError("HTTPAPIEX_SAS_Create failed");
        HTTPHeaders_Free(httpHeader);
        STRING_delete(keyName);
        STRING_delete(accessKey);
        STRING_delete(uriResource);
        result = IOTHUB_DEVICE_TWIN_HTTPAPI_ERROR;
    }
    else if ((httpExApiHandle = HTTPAPIEX_Create(serviceClientDeviceTwinHandle->hostname)) == NULL)
    {
        LogError("HTTPAPIEX_Create failed");
        HTTPAPIEX_SAS_Destroy(httpExApiSasHandle);
        HTTPHeaders_Free(httpHeader);
        STRING_delete(keyName);
        STRING_delete(accessKey);
        STRING_delete(uriResource);
        result = IOTHUB_DEVICE_TWIN_HTTPAPI_ERROR;
    }
    else
    {
        HTTPAPI_REQUEST_TYPE httpApiRequestType = HTTPAPI_REQUEST_GET;
        STRING_HANDLE relativePath;
        unsigned int statusCode = 0;
        unsigned char is_error = 0;

        //IOTHUB_TWIN_REQUEST_GET               GET      {iot hub}/twins/{device id}                     // Get device twin
        //IOTHUB_TWIN_REQUEST_UPDATE            PATCH    {iot hub}/twins/{device id}                     // Partally update device twin
        //IOTHUB_TWIN_REQUEST_REPLACE_TAGS      PUT      {iot hub}/twins/{device id}/tags                // Replace update tags
        //IOTHUB_TWIN_REQUEST_REPLACE_DESIRED   PUT      {iot hub}/twins/{device id}/properties/desired  // Replace update desired properties
        //IOTHUB_TWIN_REQUEST_UPDATE_DESIRED    PATCH    {iot hub}/twins/{device id}/properties/desired  // Partially update desired properties

        if ((iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_REPLACE_TAGS) || (iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_REPLACE_DESIRED))
        {
            httpApiRequestType = HTTPAPI_REQUEST_PUT;
        }
        else if ((iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_UPDATE) || (iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_UPDATE_DESIRED))
        {
            httpApiRequestType = HTTPAPI_REQUEST_PATCH;
        }
        else if (iotHubTwinRequestMode == IOTHUB_TWIN_REQUEST_GET)
        {
            httpApiRequestType = HTTPAPI_REQUEST_GET;
        }
        else
        {
            is_error = 1;
        }

        if (is_error)
        {
            LogError("Invalid request type");
            result = IOTHUB_DEVICE_TWIN_HTTPAPI_ERROR;
        }
        else
        {
            if ((relativePath = createRelativePath(iotHubTwinRequestMode, deviceName, moduleId)) == NULL)
            {
                LogError("Failure creating relative path");
                result = IOTHUB_DEVICE_TWIN_ERROR;
            }
            else if (HTTPAPIEX_SAS_ExecuteRequest(httpExApiSasHandle, httpExApiHandle, httpApiRequestType, STRING_c_str(relativePath), httpHeader, deviceJsonBuffer, &statusCode, NULL, responseBuffer) != HTTPAPIEX_OK)
            {
                LogError("HTTPAPIEX_SAS_ExecuteRequest failed");
                STRING_delete(relativePath);
                result = IOTHUB_DEVICE_TWIN_HTTPAPI_ERROR;
            }
            else
            {
                STRING_delete(relativePath);
                if (statusCode == 200)
                {
                    result = IOTHUB_DEVICE_TWIN_OK;
                }
                else
                {
                    LogError("Http Failure status code %d.", statusCode);
                    result = IOTHUB_DEVICE_TWIN_ERROR;
                }
            }
        }
        HTTPAPIEX_Destroy(httpExApiHandle);
        HTTPAPIEX_SAS_Destroy(httpExApiSasHandle);
        HTTPHeaders_Free(httpHeader);
        STRING_delete(keyName);
        STRING_delete(accessKey);
        STRING_delete(uriResource);
    }
    return result;
}

static void free_devicetwin_handle(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN* deviceTwin)
{
    free(deviceTwin->hostname);
    free(deviceTwin->sharedAccessKey);
    free(deviceTwin->keyName);
    free(deviceTwin);
}

IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE IoTHubDeviceTwin_Create(IOTHUB_SERVICE_CLIENT_AUTH_HANDLE serviceClientHandle)
{
    IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE result;

    if (serviceClientHandle == NULL)
    {
        LogError("serviceClientHandle input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        IOTHUB_SERVICE_CLIENT_AUTH* serviceClientAuth = (IOTHUB_SERVICE_CLIENT_AUTH*)serviceClientHandle;

        if (serviceClientAuth->hostname == NULL)
        {
            LogError("authInfo->hostName input parameter cannot be NULL");
            result = NULL;
        }
        else if (serviceClientAuth->iothubName == NULL)
        {
            LogError("authInfo->iothubName input parameter cannot be NULL");
            result = NULL;
        }
        else if (serviceClientAuth->iothubSuffix == NULL)
        {
            LogError("authInfo->iothubSuffix input parameter cannot be NULL");
            result = NULL;
        }
        else if (serviceClientAuth->keyName == NULL)
        {
            LogError("authInfo->keyName input parameter cannot be NULL");
            result = NULL;
        }
        else if (serviceClientAuth->sharedAccessKey == NULL)
        {
            LogError("authInfo->sharedAccessKey input parameter cannot be NULL");
            result = NULL;
        }
        else
        {
            result = malloc(sizeof(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN));
            if (result == NULL)
            {
                LogError("Malloc failed for IOTHUB_SERVICE_CLIENT_DEVICE_TWIN");
            }
            else
            {
                memset(result, 0, sizeof(*result));

                if (mallocAndStrcpy_s(&result->hostname, serviceClientAuth->hostname) != 0)
                {
                    LogError("mallocAndStrcpy_s failed for hostName");
                    free_devicetwin_handle(result);
                    result = NULL;
                }
                else if (mallocAndStrcpy_s(&result->sharedAccessKey, serviceClientAuth->sharedAccessKey) != 0)
                {
                    LogError("mallocAndStrcpy_s failed for sharedAccessKey");
                    free_devicetwin_handle(result);
                    result = NULL;
                }
                else if (mallocAndStrcpy_s(&result->keyName, serviceClientAuth->keyName) != 0)
                {
                    LogError("mallocAndStrcpy_s failed for keyName");
                    free_devicetwin_handle(result);
                    result = NULL;
                }
            }
        }
    }
    return result;
}

void IoTHubDeviceTwin_Destroy(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle)
{
    if (serviceClientDeviceTwinHandle != NULL)
    {
        free_devicetwin_handle((IOTHUB_SERVICE_CLIENT_DEVICE_TWIN*)serviceClientDeviceTwinHandle);
    }
}

static int malloc_and_copy_uchar(char** strDestination, BUFFER_HANDLE strSource)
{
    int result;
    if ((strDestination == NULL) || (strSource == NULL))
    {
        /* If strDestination or strSource is a NULL pointer[...] function return line number where error is spotted */
        LogError("invalid parameter strDestination or strSource");
        result = MU_FAILURE;
    }
    else
    {
        size_t buffer_size = BUFFER_length(strSource);
        char *temp = malloc(buffer_size + 1);
        if (temp == NULL)
        {
            LogError("failed to malloc");
            result = MU_FAILURE;
        }
        else
        {
            *strDestination = memcpy(temp, BUFFER_u_char(strSource), buffer_size);
            temp[buffer_size] = '\0';
            result = 0;
        }
    }
    return result;
}

char* IoTHubDeviceTwin_GetDeviceOrModuleTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId, const char* moduleId)
{
    char* result;

    if ((serviceClientDeviceTwinHandle == NULL) || (deviceId == NULL))
    {
        LogError("Input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        BUFFER_HANDLE responseBuffer;

        if ((responseBuffer = BUFFER_new()) == NULL)
        {
            LogError("BUFFER_new failed for responseBuffer");
            result = NULL;
        }
        else if (sendHttpRequestTwin(serviceClientDeviceTwinHandle, IOTHUB_TWIN_REQUEST_GET, deviceId, moduleId, NULL, responseBuffer) != IOTHUB_DEVICE_TWIN_OK)
        {
            LogError("Failure sending HTTP request for create device");
            BUFFER_delete(responseBuffer);
            result = NULL;
        }
        else
        {
            if (malloc_and_copy_uchar(&result, responseBuffer) != 0)
            {
                LogError("failed to copy response");
                result = NULL;
            }
            BUFFER_delete(responseBuffer);
        }
    }
    return result;

}

char* IoTHubDeviceTwin_GetTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId)
{
    return IoTHubDeviceTwin_GetDeviceOrModuleTwin(serviceClientDeviceTwinHandle, deviceId, NULL);
}

char* IoTHubDeviceTwin_GetModuleTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId, const char* moduleId)
{
    char* result;
    if ((serviceClientDeviceTwinHandle == NULL) || (deviceId == NULL) || (moduleId == NULL))
    {
        LogError("Input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        result = IoTHubDeviceTwin_GetDeviceOrModuleTwin(serviceClientDeviceTwinHandle, deviceId, moduleId);
    }

    return result;
}

char* IoTHubDeviceTwin_UpdateDeviceOrModuleTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId, const char* moduleId, const char* deviceTwinJson)
{
    char* result;

    if ((serviceClientDeviceTwinHandle == NULL) || (deviceId == NULL) || (deviceTwinJson == NULL))
    {
        LogError("Input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        BUFFER_HANDLE updateJson;
        BUFFER_HANDLE responseBuffer;

        if ((updateJson = BUFFER_create((const unsigned char*)deviceTwinJson, strlen(deviceTwinJson))) == NULL)
        {
            LogError("BUFFER_create failed for deviceTwinJson");
            result = NULL;
        }
        else if ((responseBuffer = BUFFER_new()) == NULL)
        {
            LogError("BUFFER_new failed for responseBuffer");
            BUFFER_delete(updateJson);
            result = NULL;
        }
        else if (sendHttpRequestTwin(serviceClientDeviceTwinHandle, IOTHUB_TWIN_REQUEST_UPDATE, deviceId, moduleId, updateJson, responseBuffer) != IOTHUB_DEVICE_TWIN_OK)
        {
            LogError("Failure sending HTTP request for twin update");
            BUFFER_delete(responseBuffer);
            BUFFER_delete(updateJson);
            result = NULL;
        }
        else
        {
            if (malloc_and_copy_uchar(&result, responseBuffer) != 0)
            {
                LogError("failed to copy response");
                result = NULL;
            }
            BUFFER_delete(responseBuffer);
            BUFFER_delete(updateJson);
        }
    }
    return result;

}

char* IoTHubDeviceTwin_UpdateTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId, const char* deviceTwinJson)
{
    return IoTHubDeviceTwin_UpdateDeviceOrModuleTwin(serviceClientDeviceTwinHandle, deviceId, NULL, deviceTwinJson);
}

char* IoTHubDeviceTwin_UpdateModuleTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* deviceId, const char* moduleId, const char* moduleTwinJson)
{
    char* result;
    if ((serviceClientDeviceTwinHandle == NULL) || (deviceId == NULL) || (moduleId == NULL) || (moduleTwinJson == NULL))
    {
        LogError("Input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        result = IoTHubDeviceTwin_UpdateDeviceOrModuleTwin(serviceClientDeviceTwinHandle, deviceId, moduleId, moduleTwinJson);
    }

    return result;
}

/*
 * NEW FUNCTION: IoTHubDeviceTwin_QueryTwin
 * ----------------------------------------
 * This function implements PARTIAL Device Twin retrieval using the Azure IoT Hub Query API.
 * 
 * WHAT IT DOES:
 * - Sends a POST request to: https://{iothub-hostname}/devices/query?api-version=2020-09-30
 * - Uses SQL-like query syntax to select ONLY the twin properties you need
 * - Returns a JSON array with only the requested fields (not the full twin document)
 * 
 * WHY USE THIS INSTEAD OF IoTHubDeviceTwin_GetTwin():
 * - IoTHubDeviceTwin_GetTwin() uses GET /twins/{deviceId} which ALWAYS returns the FULL twin
 * - This function uses POST /devices/query which returns ONLY what you ask for
 * - Benefits: Reduced bandwidth, faster parsing, more efficient for large twins
 * 
 * EXAMPLE USAGE SCENARIOS:
 * 
 * Scenario 1: Download ONLY desired properties (no reported, no tags):
 *   char* query = "SELECT properties.desired FROM devices WHERE deviceId = 'device1'";
 *   char* result = IoTHubDeviceTwin_QueryTwin(handle, query);
 *   // Result: [{"properties": {"desired": {"telemetryInterval": 60, "$version": 3}}}]
 * 
 * Scenario 2: Download ONLY reported properties:
 *   char* query = "SELECT properties.reported FROM devices WHERE deviceId = 'device1'";
 *   char* result = IoTHubDeviceTwin_QueryTwin(handle, query);
 *   // Result: [{"properties": {"reported": {"connectivity": "wifi", "$version": 2}}}]
 * 
 * Scenario 3: Download ONLY specific field from desired properties:
 *   char* query = "SELECT properties.desired.telemetryInterval FROM devices WHERE deviceId = 'device1'";
 *   char* result = IoTHubDeviceTwin_QueryTwin(handle, query);
 *   // Result: [{"properties": {"desired": {"telemetryInterval": 60}}}]
 * 
 * Scenario 4: Download partial from BOTH desired AND reported:
 *   char* query = "SELECT properties.desired.interval, properties.reported.status FROM devices WHERE deviceId = 'device1'";
 *   char* result = IoTHubDeviceTwin_QueryTwin(handle, query);
 *   // Result: [{"properties": {"desired": {"interval": 60}, "reported": {"status": "ok"}}}]
 */
char* IoTHubDeviceTwin_QueryTwin(IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle, const char* sqlQuery)
{
    char* result;

    if ((serviceClientDeviceTwinHandle == NULL) || (sqlQuery == NULL))
    {
        LogError("Input parameter cannot be NULL");
        result = NULL;
    }
    else
    {
        BUFFER_HANDLE queryBuffer;
        BUFFER_HANDLE responseBuffer;
        STRING_HANDLE uriResource = NULL;
        STRING_HANDLE accessKey = NULL;
        STRING_HANDLE keyName = NULL;
        STRING_HANDLE relativePath = NULL;
        HTTPAPIEX_SAS_HANDLE httpExApiSasHandle;
        HTTPAPIEX_HANDLE httpExApiHandle;
        HTTP_HEADERS_HANDLE httpHeader;

        // Construct the query JSON payload: {"query": "SELECT ..."}
        JSON_Value* root_value = json_value_init_object();
        JSON_Object* root_object = json_value_get_object(root_value);
        
        if (root_object == NULL)
        {
            LogError("Failed to create JSON object for query");
            json_value_free(root_value);
            result = NULL;
        }
        else if (json_object_set_string(root_object, "query", sqlQuery) != JSONSuccess)
        {
            LogError("Failed to set query string in JSON");
            json_value_free(root_value);
            result = NULL;
        }
        else
        {
            char* queryJsonString = json_serialize_to_string(root_value);
            json_value_free(root_value);

            if (queryJsonString == NULL)
            {
                LogError("Failed to serialize query JSON");
                result = NULL;
            }
            else if ((queryBuffer = BUFFER_create((const unsigned char*)queryJsonString, strlen(queryJsonString))) == NULL)
            {
                LogError("BUFFER_create failed for queryBuffer");
                json_free_serialized_string(queryJsonString);
                result = NULL;
            }
            else if ((responseBuffer = BUFFER_new()) == NULL)
            {
                LogError("BUFFER_new failed for responseBuffer");
                BUFFER_delete(queryBuffer);
                json_free_serialized_string(queryJsonString);
                result = NULL;
            }
            else
            {
                // Setup HTTP connection similar to sendHttpRequestTwin
                if ((uriResource = STRING_construct(serviceClientDeviceTwinHandle->hostname)) == NULL)
                {
                    LogError("STRING_construct failed for uriResource");
                    result = NULL;
                }
                else if ((accessKey = STRING_construct(serviceClientDeviceTwinHandle->sharedAccessKey)) == NULL)
                {
                    LogError("STRING_construct failed for accessKey");
                    STRING_delete(uriResource);
                    result = NULL;
                }
                else if ((keyName = STRING_construct(serviceClientDeviceTwinHandle->keyName)) == NULL)
                {
                    LogError("STRING_construct failed for keyName");
                    STRING_delete(accessKey);
                    STRING_delete(uriResource);
                    result = NULL;
                }
                else if ((httpHeader = createHttpHeader(IOTHUB_TWIN_REQUEST_UPDATE)) == NULL)  // Use UPDATE mode to get proper headers
                {
                    LogError("HttpHeader creation failed");
                    STRING_delete(keyName);
                    STRING_delete(accessKey);
                    STRING_delete(uriResource);
                    result = NULL;
                }
                else if ((httpExApiSasHandle = HTTPAPIEX_SAS_Create(accessKey, uriResource, keyName)) == NULL)
                {
                    LogError("HTTPAPIEX_SAS_Create failed");
                    HTTPHeaders_Free(httpHeader);
                    STRING_delete(keyName);
                    STRING_delete(accessKey);
                    STRING_delete(uriResource);
                    result = NULL;
                }
                else if ((httpExApiHandle = HTTPAPIEX_Create(serviceClientDeviceTwinHandle->hostname)) == NULL)
                {
                    LogError("HTTPAPIEX_Create failed");
                    HTTPAPIEX_SAS_Destroy(httpExApiSasHandle);
                    HTTPHeaders_Free(httpHeader);
                    STRING_delete(keyName);
                    STRING_delete(accessKey);
                    STRING_delete(uriResource);
                    result = NULL;
                }
                else
                {
                    unsigned int statusCode = 0;
                    
                    // Create the query endpoint path: /devices/query?api-version=2020-09-30
                    if ((relativePath = STRING_construct_sprintf(RELATIVE_PATH_FMT_QUERY, URL_API_VERSION)) == NULL)
                    {
                        LogError("Failure creating relative path for query");
                        result = NULL;
                    }
                    // Execute POST request to the query endpoint
                    else if (HTTPAPIEX_SAS_ExecuteRequest(httpExApiSasHandle, httpExApiHandle, HTTPAPI_REQUEST_POST, STRING_c_str(relativePath), httpHeader, queryBuffer, &statusCode, NULL, responseBuffer) != HTTPAPIEX_OK)
                    {
                        LogError("HTTPAPIEX_SAS_ExecuteRequest failed for query");
                        STRING_delete(relativePath);
                        result = NULL;
                    }
                    else
                    {
                        STRING_delete(relativePath);
                        if (statusCode == 200)
                        {
                            // Copy the response (JSON array) to the result
                            if (malloc_and_copy_uchar(&result, responseBuffer) != 0)
                            {
                                LogError("failed to copy query response");
                                result = NULL;
                            }
                        }
                        else
                        {
                            LogError("Http Failure status code %d for query request.", statusCode);
                            result = NULL;
                        }
                    }

                    HTTPAPIEX_Destroy(httpExApiHandle);
                    HTTPAPIEX_SAS_Destroy(httpExApiSasHandle);
                    HTTPHeaders_Free(httpHeader);
                    STRING_delete(keyName);
                    STRING_delete(accessKey);
                    STRING_delete(uriResource);
                }

                BUFFER_delete(responseBuffer);
                BUFFER_delete(queryBuffer);
                json_free_serialized_string(queryJsonString);
            }
        }
    }
    
    return result;
}