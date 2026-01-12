// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// This file is under development and it is subject to change

#ifndef IOTHUB_DEVICETWIN_H
#define IOTHUB_DEVICETWIN_H

#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/map.h"
#include <time.h>
#include "iothub_service_client_auth.h"

#include "umock_c/umock_c_prod.h"

#ifdef __cplusplus
extern "C"
{
#else
#endif

#define IOTHUB_DEVICE_TWIN_RESULT_VALUES     \
    IOTHUB_DEVICE_TWIN_OK,                   \
    IOTHUB_DEVICE_TWIN_INVALID_ARG,          \
    IOTHUB_DEVICE_TWIN_ERROR,                \
    IOTHUB_DEVICE_TWIN_HTTPAPI_ERROR         \

MU_DEFINE_ENUM_WITHOUT_INVALID(IOTHUB_DEVICE_TWIN_RESULT, IOTHUB_DEVICE_TWIN_RESULT_VALUES);

/** @brief Handle to hide struct and use it in consequent APIs
*/
typedef struct IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_TAG* IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE;


/** @brief    Creates a IoT Hub Service Client DeviceTwin handle for use it in consequent APIs.
*
* @param    serviceClientHandle    Service client handle.
*
* @return    A non-NULL @c IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE value that is used when
*             invoking other functions for IoT Hub DeviceTwin and @c NULL on failure.
*/
MOCKABLE_FUNCTION(, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, IoTHubDeviceTwin_Create, IOTHUB_SERVICE_CLIENT_AUTH_HANDLE, serviceClientHandle);

/** @brief    Disposes of resources allocated by the IoT Hub IoTHubDeviceTwin_Create.
*
* @param    serviceClientDeviceTwinHandle    The handle created by a call to the create function.
*/
MOCKABLE_FUNCTION(, void,  IoTHubDeviceTwin_Destroy, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle);

/** @brief    Retrieves the given device's twin info.
*
* @details   This function retrieves the FULL Device Twin JSON document from Azure IoT Hub using the 
*            REST API endpoint: GET /twins/{deviceId}
*            
*            The returned JSON contains ALL Device Twin data including:
*            - deviceId, etag, version, status, statusReason, connectionState
*            - tags (metadata set by backend)
*            - properties.desired (desired state set by backend)
*            - properties.reported (reported state sent by device)
*            - All metadata fields ($metadata, $version) for desired and reported properties
*
*            For retrieving only PARTIAL twin data (e.g., only desired or reported properties), 
*            use IoTHubDeviceTwin_QueryTwin() instead.
*
* @param    serviceClientDeviceTwinHandle    The handle created by a call to the create function.
* @param    deviceId                        The device name (id) to retrieve twin info for.
*
* @return    A non-NULL char* containing device twin info upon success or NULL upon failure.
*/
MOCKABLE_FUNCTION(, char*,  IoTHubDeviceTwin_GetTwin, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle, const char*, deviceId);

/** @brief    Updates (partial update) the given device's twin info.
*
* @param    serviceClientDeviceTwinHandle    The handle created by a call to the create function.
* @param    deviceId                        The device name (id) to update the twin info for.
* @param    deviceTwinJson                  DeviceTwin JSon string containing the info (tags, desired properties) to update.
*                                           All well-known read-only members are ignored.
*                                           Properties provided with value of null are removed from twin's document.
*
* @return    A non-NULL char* containing updated device twin info upon success or NULL upon failure.
*/
MOCKABLE_FUNCTION(, char*,  IoTHubDeviceTwin_UpdateTwin, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle, const char*, deviceId, const char*, deviceTwinJson);

/** @brief  Retrieves the given module's twin info.
*
* @param    serviceClientDeviceTwinHandle   The handle created by a call to the create function.
* @param    deviceId                        The device name (id) containing the module to retrieve the twin info for.
* @param    moduleId                        The module name (id) to retrieve twin info for.
*
* @return   A non-NULL char* containing module twin info upon success or NULL upon failure.
*/
MOCKABLE_FUNCTION(, char*,  IoTHubDeviceTwin_GetModuleTwin, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle, const char*, deviceId, const char*, moduleId);

/** @brief  Updates (partial update) the given module's twin info.
*
* @param    serviceClientDeviceTwinHandle   The handle created by a call to the create function.
* @param    deviceId                        The device name (id) containing the module to update.
* @param    moduleId                        The module name (id) to update the twin info for.
* @param    moduleTwinJson                  ModuleTwin JSon string containing the info (tags, desired properties) to update.
*                                           All well-known read-only members are ignored.
*                                           Properties provided with value of null are removed from twin's document.
*
* @return   A non-NULL char* containing updated module twin info upon success or NULL upon failure.
*/
MOCKABLE_FUNCTION(, char*,  IoTHubDeviceTwin_UpdateModuleTwin, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle, const char*, deviceId, const char*, moduleId, const char*, moduleTwinJson);

/** @brief    Retrieves PARTIAL device twin data using Azure IoT Hub Query API.
*
* @details   This function allows you to retrieve ONLY specific parts of the Device Twin JSON
*            instead of the full twin document. It uses the Azure IoT Hub Query API:
*            POST https://{iothub-hostname}/devices/query?api-version=2020-09-30
*            
*            COMPARISON WITH IoTHubDeviceTwin_GetTwin():
*            -------------------------------------------
*            IoTHubDeviceTwin_GetTwin():
*              - Uses: GET /twins/{deviceId}
*              - Returns: FULL twin (all tags, desired, reported, metadata)
*              - Use when: You need complete twin information
*            
*            IoTHubDeviceTwin_QueryTwin():
*              - Uses: POST /devices/query
*              - Returns: ONLY the properties you specify in the SQL query
*              - Use when: You need only desired, only reported, or specific fields
*            
*            EXAMPLE QUERIES FOR PARTIAL RETRIEVAL:
*            ---------------------------------------
*            
*            1. Get ONLY desired properties (no reported, no tags, no metadata):
*               "SELECT properties.desired FROM devices WHERE deviceId = 'myDevice'"
*            
*            2. Get ONLY reported properties:
*               "SELECT properties.reported FROM devices WHERE deviceId = 'myDevice'"
*            
*            3. Get ONLY specific field from desired properties:
*               "SELECT properties.desired.telemetryInterval FROM devices WHERE deviceId = 'myDevice'"
*            
*            4. Get specific fields from both desired and reported:
*               "SELECT properties.desired.telemetryInterval, properties.reported.connectivity FROM devices WHERE deviceId = 'myDevice'"
*            
*            5. Get only desired properties WITHOUT metadata:
*               "SELECT properties.desired FROM devices WHERE deviceId = 'myDevice'"
*               (Note: metadata like $version is still included, but you parse only what you need)
*            
*            RESPONSE FORMAT:
*            ----------------
*            The Query API returns a JSON array. For a single device query, the result will be:
*            [{"properties": {"desired": {...}}}]  or  [{"properties": {"reported": {...}}}]
*            
*            You can parse this array to extract only the data you requested, significantly
*            reducing payload size compared to the full twin document.
*
* @param    serviceClientDeviceTwinHandle    The handle created by a call to the create function.
* @param    sqlQuery                        SQL-like query string to select specific twin properties.
*                                           Use Azure IoT Hub query syntax.
*
* @return    A non-NULL char* containing query results as JSON array upon success or NULL upon failure.
*            The caller is responsible for freeing the returned string.
*/
MOCKABLE_FUNCTION(, char*,  IoTHubDeviceTwin_QueryTwin, IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE, serviceClientDeviceTwinHandle, const char*, sqlQuery);

#ifdef __cplusplus
}
#endif

#endif // IOTHUB_DEVICETWIN_H
