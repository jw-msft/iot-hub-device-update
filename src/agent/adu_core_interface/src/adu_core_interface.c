/**
 * @file adu_core_interface.c
 * @brief Methods to communicate with "urn:azureiot:AzureDeviceUpdateCore:1" interface.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

#include "aduc/adu_core_interface.h"
#include "aduc/adu_core_export_helpers.h" // ADUC_SetUpdateStateWithResult
#include "aduc/agent_orchestration.h"
#include "aduc/agent_workflow.h"
#include "aduc/c_utils.h"
#include "aduc/client_handle_helper.h"
#include "aduc/hash_utils.h"
#include "aduc/logging.h"
#include "aduc/string_c_utils.h"
#include "aduc/types/update_content.h"
#include "aduc/workflow_data_utils.h"
#include "aduc/workflow_persistence_utils.h"
#include "aduc/workflow_utils.h"

#include "startup_msg_helper.h"

#include <azure_c_shared_utility/strings.h> // STRING_*
#include <iothub_client_version.h>
#include <parson.h>
#include <pnp_protocol.h>

// Name of an Device Update Agent component that this device implements.
static const char g_aduPnPComponentName[] = "deviceUpdate";

// Name of properties that Device Update Agent component supports.

// This is the device-to-cloud property.
// An agent communicates its state and other data to ADU Management service by reporting this property to IoTHub.
static const char g_aduPnPComponentClientPropertyName[] = "agent";

// This is the cloud-to-device property.
// ADU Management send an 'Update Action' to this device by setting this property on IoTHub.
static const char g_aduPnPComponentOrchestratorPropertyName[] = "service";

/**
 * @brief Handle for Device Update Agent component to communication to service.
 */
ADUC_ClientHandle g_iotHubClientHandleForADUComponent;

void ClientReportedStateCallback(int statusCode, void* context)
{
    UNREFERENCED_PARAMETER(context);

    if (statusCode < 200 || statusCode >= 300)
    {
        Log_Error(
            "Failed to report ADU agent's state, error: %d, %s",
            statusCode,
            MU_ENUM_TO_STRING(IOTHUB_CLIENT_RESULT, statusCode));
    }
}

/**
 * @brief Gets the client handle send report function.
 *
 * @param workflowData The workflow data.
 * @return ClientHandleSnedReportFunc The function for sending the client report.
 */
static ClientHandleSendReportFunc
ADUC_WorkflowData_GetClientHandleSendReportFunc(const ADUC_WorkflowData* workflowData)
{
    ClientHandleSendReportFunc fn = (ClientHandleSendReportFunc)ClientHandle_SendReportedState;

#ifdef ADUC_BUILD_UNIT_TESTS
    ADUC_TestOverride_Hooks* hooks = workflowData->TestOverrides;
    if (hooks && hooks->ClientHandle_SendReportedStateFunc_TestOverride)
    {
        fn = (ClientHandleSendReportFunc)(hooks->ClientHandle_SendReportedStateFunc_TestOverride);
    }
#endif

    return fn;
}

/**
 * @brief Reports the client json via PnP so it ends up in the reported section of the twin.
 *
 * @param json_value The json value to be reported.
 * @param workflowData The workflow data.
 * @return _Bool true if call succeeded.
 */
static _Bool ReportClientJsonProperty(const char* json_value, ADUC_WorkflowData* workflowData)
{
    _Bool success = false;

    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportClientJsonProperty called with invalid IoTHub Device Client handle! Can't report!");
        return false;
    }

    IOTHUB_CLIENT_RESULT iothubClientResult;
    STRING_HANDLE jsonToSend =
        PnP_CreateReportedProperty(g_aduPnPComponentName, g_aduPnPComponentClientPropertyName, json_value);

    if (jsonToSend == NULL)
    {
        Log_Error("Unable to create Reported property for ADU client.");
        goto done;
    }

    const char* jsonToSendStr = STRING_c_str(jsonToSend);
    size_t jsonToSendStrLen = strlen(jsonToSendStr);

    Log_Debug("Reporting agent state:\n%s", jsonToSendStr);

    ClientHandleSendReportFunc clientHandle_SendReportedState_Func =
        ADUC_WorkflowData_GetClientHandleSendReportFunc(workflowData);

    iothubClientResult = (IOTHUB_CLIENT_RESULT)clientHandle_SendReportedState_Func(
        g_iotHubClientHandleForADUComponent,
        (const unsigned char*)jsonToSendStr,
        jsonToSendStrLen,
        ClientReportedStateCallback,
        NULL);

    if (iothubClientResult != IOTHUB_CLIENT_OK)
    {
        Log_Error(
            "Unable to report state, %s, error: %d, %s",
            json_value,
            iothubClientResult,
            MU_ENUM_TO_STRING(IOTHUB_CLIENT_RESULT, iothubClientResult));
        goto done;
    }

    success = true;

done:
    STRING_delete(jsonToSend);

    return success;
}

/**
 * @brief Reports values to the cloud which do not change throughout ADUs execution
 * @details the current expectation is to report these values after the successful
 * connection of the AzureDeviceUpdateCoreInterface
 * @param workflowData the workflow data.
 * @returns true when the report is sent and false when reporting fails.
 */
_Bool ReportStartupMsg(ADUC_WorkflowData* workflowData)
{
    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportStartupMsg called before registration! Can't report!");
        return false;
    }

    _Bool success = false;

    char* jsonString = NULL;
    char* manufacturer = NULL;
    char* model = NULL;

    JSON_Value* startupMsgValue = json_value_init_object();

    if (startupMsgValue == NULL)
    {
        goto done;
    }

    JSON_Object* startupMsgObj = json_value_get_object(startupMsgValue);

    if (startupMsgObj == NULL)
    {
        goto done;
    }

    if (!StartupMsg_AddDeviceProperties(startupMsgObj))
    {
        Log_Error("Could not add Device Properties to the startup message");
        goto done;
    }

    if (!StartupMsg_AddCompatPropertyNames(startupMsgObj))
    {
        Log_Error("Could not add compatPropertyNames to the startup message");
        goto done;
    }

    jsonString = json_serialize_to_string(startupMsgValue);

    if (jsonString == NULL)
    {
        Log_Error("Serializing JSON to string failed!");
        goto done;
    }

    ReportClientJsonProperty(jsonString, workflowData);

    success = true;
done:
    free(model);
    free(manufacturer);
    json_value_free(startupMsgValue);
    json_free_serialized_string(jsonString);

    return success;
}

//
// AzureDeviceUpdateCoreInterface methods
//

_Bool AzureDeviceUpdateCoreInterface_Create(void** context, int argc, char** argv)
{
    _Bool succeeded = false;

    ADUC_WorkflowData* workflowData = calloc(1, sizeof(ADUC_WorkflowData));
    if (workflowData == NULL)
    {
        goto done;
    }

    Log_Info("ADUC agent started. Using IoT Hub Client SDK %s", IoTHubClient_GetVersionString());

    if (!ADUC_WorkflowData_Init(workflowData, argc, argv))
    {
        Log_Error("Workflow data initialization failed");
        goto done;
    }

    succeeded = true;

done:

    if (!succeeded)
    {
        ADUC_WorkflowData_Uninit(workflowData);
        free(workflowData);
        workflowData = NULL;
    }

    // Set out parameter.
    *context = workflowData;

    return succeeded;
}

void AzureDeviceUpdateCoreInterface_Connected(void* componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)componentContext;

    if (workflowData->WorkflowHandle == NULL)
    {
        // Only perform startup logic here, if no workflows has been created.
        ADUC_Workflow_HandleStartupWorkflowData(workflowData);
    }

    if (!ReportStartupMsg(workflowData))
    {
        Log_Warn("ReportStartupMsg failed");
    }
}

void AzureDeviceUpdateCoreInterface_DoWork(void* componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)componentContext;
    ADUC_WorkflowData_DoWork(workflowData);
}

void AzureDeviceUpdateCoreInterface_Destroy(void** componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)(*componentContext);

    Log_Info("ADUC agent stopping");

    ADUC_WorkflowData_Uninit(workflowData);
    free(workflowData);

    *componentContext = NULL;
}

void OrchestratorUpdateCallback(
    ADUC_ClientHandle clientHandle, JSON_Value* propertyValue, int propertyVersion, void* context)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)context;
    STRING_HANDLE jsonToSend = NULL;

    // Reads out the json string so we can Log Out what we've got.
    // The value will be parsed and handled in ADUC_Workflow_HandlePropertyUpdate.
    char* jsonString = json_serialize_to_string(propertyValue);
    if (jsonString == NULL)
    {
        Log_Error(
            "OrchestratorUpdateCallback failed to convert property JSON value to string, property version (%d)",
            propertyVersion);
        goto done;
    }

    // To reduce TWIN size, remove UpdateManifestSignature and fileUrls before ACK.
    char* ackString = NULL;
    JSON_Object* signatureObj = json_value_get_object(propertyValue);
    if (signatureObj != NULL)
    {
        json_object_set_null(signatureObj, "updateManifestSignature");
        json_object_set_null(signatureObj, "fileUrls");
        ackString = json_serialize_to_string(propertyValue);
    }

    Log_Debug("Update Action info string (%s), property version (%d)", ackString, propertyVersion);

    ADUC_Workflow_HandlePropertyUpdate(workflowData, (const unsigned char*)jsonString);
    free(jsonString);
    jsonString = ackString;

    // ACK the request.
    jsonToSend = PnP_CreateReportedPropertyWithStatus(
        g_aduPnPComponentName,
        g_aduPnPComponentOrchestratorPropertyName,
        jsonString,
        PNP_STATUS_SUCCESS,
        "", // Description for this acknowledgement.
        propertyVersion);

    if (jsonToSend == NULL)
    {
        Log_Error("Unable to build reported property ACK response.");
        goto done;
    }

    const char* jsonToSendStr = STRING_c_str(jsonToSend);
    size_t jsonToSendStrLen = strlen(jsonToSendStr);
    IOTHUB_CLIENT_RESULT iothubClientResult = ClientHandle_SendReportedState(
        clientHandle, (const unsigned char*)jsonToSendStr, jsonToSendStrLen, NULL, NULL);

    if (iothubClientResult != IOTHUB_CLIENT_OK)
    {
        Log_Error(
            "Unable to send acknowledgement of property to IoT Hub for component=%s, error=%d",
            g_aduPnPComponentName,
            iothubClientResult);
        goto done;
    }

done:
    STRING_delete(jsonToSend);

    free(jsonString);

    Log_Info("OrchestratorPropertyUpdateCallback ended");
}

void AzureDeviceUpdateCoreInterface_PropertyUpdateCallback(
    ADUC_ClientHandle clientHandle, const char* propertyName, JSON_Value* propertyValue, int version, void* context)
{
    if (strcmp(propertyName, g_aduPnPComponentOrchestratorPropertyName) == 0)
    {
        OrchestratorUpdateCallback(clientHandle, propertyValue, version, context);
    }
    else
    {
        Log_Info("Unsupported property. (%s)", propertyName);
    }
}

//
// Reporting
//
static JSON_Status _json_object_set_update_result(
    JSON_Object* object, int32_t resultCode, int32_t extendedResultCode, const char* resultDetails)
{
    JSON_Status status = json_object_set_number(object, ADUCITF_FIELDNAME_RESULTCODE, resultCode);
    if (status != JSONSuccess)
    {
        Log_Error("Could not set value for field: %s", ADUCITF_FIELDNAME_RESULTCODE);
        goto done;
    }

    status = json_object_set_number(object, ADUCITF_FIELDNAME_EXTENDEDRESULTCODE, extendedResultCode);
    if (status != JSONSuccess)
    {
        Log_Error("Could not set value for field: %s", ADUCITF_FIELDNAME_EXTENDEDRESULTCODE);
        goto done;
    }

    if (resultDetails != NULL)
    {
        status = json_object_set_string(object, ADUCITF_FIELDNAME_RESULTDETAILS, resultDetails);
        if (status != JSONSuccess)
        {
            Log_Error("Could not set value for field: %s", ADUCITF_FIELDNAME_RESULTDETAILS);
        }
    }
    else
    {
        status = json_object_set_null(object, ADUCITF_FIELDNAME_RESULTDETAILS);
        if (status != JSONSuccess)
        {
            Log_Error("Could not set field %s to 'null'", ADUCITF_FIELDNAME_RESULTDETAILS);
        }
    }

done:
    return status;
}

/**
 * @brief Sets workflow properties on the workflow json value.
 *
 * @param[in,out] workflowValue The workflow json value to set properties on.
 * @param[in] updateAction The updateAction for the action field.
 * @param[in] workflowId The workflow id of the update deployment.
 * @param[in] retryTimestamp optional. The retry timestamp that's present for service-initiated retries.
 * @return true if all properties were set successfully; false, otherwise.
 */
static _Bool set_workflow_properties(
    JSON_Value* workflowValue, ADUCITF_UpdateAction updateAction, const char* workflowId, const char* retryTimestamp)
{
    _Bool succeeded = false;

    JSON_Object* workflowObject = json_value_get_object(workflowValue);
    if (json_object_set_number(workflowObject, ADUCITF_FIELDNAME_ACTION, updateAction) != JSONSuccess)
    {
        Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_ACTION);
        goto done;
    }

    if (json_object_set_string(workflowObject, ADUCITF_FIELDNAME_ID, workflowId) != JSONSuccess)
    {
        Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_ID);
        goto done;
    }

    if (!IsNullOrEmpty(retryTimestamp))
    {
        if (json_object_set_string(workflowObject, ADUCITF_FIELDNAME_RETRYTIMESTAMP, retryTimestamp) != JSONSuccess)
        {
            Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_RETRYTIMESTAMP);
            goto done;
        }
    }

    succeeded = true;

done:

    return succeeded;
}

/**
 * @brief Updates the lastInstallResult resultCode and extendedResultCode in the client reporting json.
 *
 * @param rootValue The root json value object for the client report.
 * @param result The ADUC result from which to update the result codes.
 * @return JSON_Status The json status result.
 */
static JSON_Status UpdateLastInstallResult(JSON_Value* rootValue, const ADUC_Result* result)
{
    JSON_Status jsonStatus = JSONFailure;
    JSON_Object* rootObject = json_value_get_object(rootValue);
    if (rootValue == NULL)
    {
        goto done;
    }

    JSON_Object* lastInstallResultObject = json_object_get_object(rootObject, ADUCITF_FIELDNAME_LASTINSTALLRESULT);
    if (lastInstallResultObject == NULL)
    {
        goto done;
    }

    jsonStatus = json_object_set_number(lastInstallResultObject, ADUCITF_FIELDNAME_RESULTCODE, result->ResultCode);
    if (jsonStatus != JSONSuccess)
    {
        goto done;
    }

    jsonStatus = json_object_set_number(
        lastInstallResultObject, ADUCITF_FIELDNAME_EXTENDEDRESULTCODE, result->ExtendedResultCode);
    if (jsonStatus != JSONSuccess)
    {
        goto done;
    }

    jsonStatus = JSONSuccess;

done:
    return jsonStatus;
}

/**
 * @brief Get the Reporting Json Value object
 *
 * @param workflowData The workflow data.
 * @param updateState The workflow state machine state.
 * @param result The pointer to the result. If NULL, then the result will be retrieved from the opaque handle object in the workflow data.
 * @param installedUpdateId The installed Update ID string.
 * @return JSON_Value* The resultant json value object.
 */
JSON_Value* GetReportingJsonValue(
    ADUC_WorkflowData* workflowData,
    ADUCITF_State updateState,
    const ADUC_Result* result,
    const char* installedUpdateId)
{
    JSON_Value* resultValue = NULL;

    //
    // Get result from current workflow if exists.
    // (Note: on startup, update workflow is not started, unless there is an existing Update Action in the twin.)
    //
    // If not, try to use specified 'result' param.
    //
    // If there's no result details, we'll report only 'updateState'.
    //
    ADUC_Result rootResult;
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;

    if (result != NULL)
    {
        rootResult = *result;
    }
    else
    {
        rootResult = workflow_get_result(handle);
    }

    JSON_Value* rootValue = json_value_init_object();
    JSON_Object* rootObject = json_value_get_object(rootValue);
    int stepsCount = workflow_get_children_count(handle);

    //
    // Prepare 'lastInstallResult', 'stepResults' data.
    //
    // Example schema:
    //
    // {
    //     "state" : ###,
    //     "workflow": {
    //         "action": 3,
    //         "id": "..."
    //     },
    //     "installedUpdateId" : "...",
    //
    //     "lastInstallResult" : {
    //         "resultCode" : ####,
    //         "extendedResultCode" : ####,
    //         "resultDetails" : "...",
    //         "stepResults" : {
    //             "step_0" : {
    //                 "resultCode" : ####,
    //                 "extendedResultCode" : ####,
    //                 "resultDetails" : "..."
    //             },
    //             ...
    //             "step_N" : {
    //                 "resultCode" : ####,
    //                 "extendedResultCode" : ####,
    //                 "resultDetails" : "..."
    //             }
    //         }
    //     }
    // }

    JSON_Value* lastInstallResultValue = json_value_init_object();
    JSON_Object* lastInstallResultObject = json_object(lastInstallResultValue);

    JSON_Value* stepResultsValue = json_value_init_object();
    JSON_Object* stepResultsObject = json_object(stepResultsValue);

    JSON_Value* workflowValue = json_value_init_object();

    if (lastInstallResultValue == NULL || stepResultsValue == NULL || workflowValue == NULL)
    {
        Log_Error("Failed to init object for json value");
        goto done;
    }

    JSON_Status jsonStatus =
        json_object_set_value(rootObject, ADUCITF_FIELDNAME_LASTINSTALLRESULT, lastInstallResultValue);
    if (jsonStatus != JSONSuccess)
    {
        Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_LASTINSTALLRESULT);
        goto done;
    }

    lastInstallResultValue = NULL; // rootObject owns the value now.

    //
    // State
    //
    jsonStatus = json_object_set_number(rootObject, ADUCITF_FIELDNAME_STATE, updateState);
    if (jsonStatus != JSONSuccess)
    {
        Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_STATE);
        goto done;
    }

    //
    // Workflow
    //
    char* workflowId = workflow_get_id(handle);
    if (!IsNullOrEmpty(workflowId))
    {
        _Bool success = set_workflow_properties(
            workflowValue,
            ADUC_WorkflowData_GetCurrentAction(workflowData),
            workflowId,
            workflow_peek_retryTimestamp(handle));

        if (!success)
        {
            goto done;
        }

        if (json_object_set_value(rootObject, ADUCITF_FIELDNAME_WORKFLOW, workflowValue) != JSONSuccess)
        {
            Log_Error("Could not add JSON : %s", ADUCITF_FIELDNAME_WORKFLOW);
            goto done;
        }

        workflowValue = NULL; // rootObject owns the value now.
    }

    //
    // Install Update Id
    //
    if (installedUpdateId != NULL)
    {
        jsonStatus = json_object_set_string(rootObject, ADUCITF_FIELDNAME_INSTALLEDUPDATEID, installedUpdateId);
        if (jsonStatus != JSONSuccess)
        {
            Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_INSTALLEDUPDATEID);
            goto done;
        }
    }

    // If reporting 'downloadStarted' or 'ADUCITF_State_DeploymentInProgress' state, we must clear previous 'stepResults' map, if exists.
    if (updateState == ADUCITF_State_DownloadStarted || updateState == ADUCITF_State_DeploymentInProgress)
    {
        if (json_object_set_null(lastInstallResultObject, ADUCITF_FIELDNAME_STEPRESULTS) != JSONSuccess)
        {
            /* Note: continue the 'download' phase if we could not clear the previous results. */
            Log_Warn("Could not clear 'stepResults' property. The property may contains previous install results.");
        }
    }
    // Otherwise, we will only report 'stepResults' property if we have one or more step.
    else if (stepsCount > 0)
    {
        jsonStatus = json_object_set_value(lastInstallResultObject, ADUCITF_FIELDNAME_STEPRESULTS, stepResultsValue);
        if (jsonStatus != JSONSuccess)
        {
            Log_Error("Could not add JSON field: %s", ADUCITF_FIELDNAME_STEPRESULTS);
            goto done;
        }

        stepResultsValue = NULL; // rootObject owns the value now.
    }

    //
    // Report both state and result
    //

    // Set top-level update state and result.
    jsonStatus = _json_object_set_update_result(
        lastInstallResultObject,
        rootResult.ResultCode,
        rootResult.ExtendedResultCode,
        workflow_peek_result_details(handle));

    if (jsonStatus != JSONSuccess)
    {
        goto done;
    }

    // Report all steps result.
    if (updateState != ADUCITF_State_DownloadStarted)
    {
        stepsCount = workflow_get_children_count(handle);
        for (int i = 0; i < stepsCount; i++)
        {
            ADUC_WorkflowHandle childHandle = workflow_get_child(handle, i);
            ADUC_Result childResult;
            JSON_Value* childResultValue = NULL;
            JSON_Object* childResultObject = NULL;
            STRING_HANDLE childUpdateId = NULL;

            if (childHandle == NULL)
            {
                Log_Error("Could not get components #%d update result", i);
                continue;
            }

            childResult = workflow_get_result(childHandle);

            childResultValue = json_value_init_object();
            childResultObject = json_object(childResultValue);
            if (childResultValue == NULL)
            {
                Log_Error("Could not create components update result #%d", i);
                goto childDone;
            }

            // Note: IoTHub twin doesn't support some special characters in a map key (e.g. ':', '-').
            // Let's name the result using "step_" +  the array index.
            childUpdateId = STRING_construct_sprintf("step_%d", i);
            if (childUpdateId == NULL)
            {
                Log_Error("Could not create proper child update id result key.");
                goto childDone;
            }

            jsonStatus = json_object_set_value(stepResultsObject, STRING_c_str(childUpdateId), childResultValue);
            if (jsonStatus != JSONSuccess)
            {
                Log_Error("Could not add step #%d update result", i);
                goto childDone;
            }
            childResultValue = NULL; // stepResultsValue owns it now.

            jsonStatus = _json_object_set_update_result(
                childResultObject,
                childResult.ResultCode,
                childResult.ExtendedResultCode,
                workflow_peek_result_details(childHandle));

            if (jsonStatus != JSONSuccess)
            {
                goto childDone;
            }

        childDone:
            STRING_delete(childUpdateId);
            childUpdateId = NULL;
            json_value_free(childResultValue);
            childResultValue = NULL;
        }
    }

    resultValue = rootValue;
    rootValue = NULL;

done:
    json_value_free(rootValue);
    json_value_free(lastInstallResultValue);
    json_value_free(stepResultsValue);
    json_value_free(workflowValue);

    return resultValue;
}

/**
 * @brief Report state, and optionally result to service.
 *
 * @param workflowData A workflow data object.
 * @param updateState state to report.
 * @param result Result to report (optional, can be NULL).
 * @param installedUpdateId Installed update id (if update completed successfully).
 * @return true if succeeded.
 */
_Bool AzureDeviceUpdateCoreInterface_ReportStateAndResultAsync(
    ADUC_WorkflowData* workflowData,
    ADUCITF_State updateState,
    const ADUC_Result* result,
    const char* installedUpdateId)
{
    _Bool success = false;

    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportStateAsync called before registration! Can't report!");
        return false;
    }

    if (AgentOrchestration_ShouldNotReportToCloud(updateState))
    {
        Log_Debug("Skipping report of state '%s'", ADUCITF_StateToString(updateState));
        return true;
    }

    if (result == NULL && updateState == ADUCITF_State_DeploymentInProgress)
    {
        ADUC_Result resultForSet = { ADUC_Result_DeploymentInProgress_Success };
        workflow_set_result(workflowData->WorkflowHandle, resultForSet);
    }

    // We are reporting idle on startup when persistence state is set on the workflow data.
    // Use the persistence reporting json in that case; otherwise, generate the reporting
    // json value.
    char* jsonString = NULL;
    JSON_Value* rootValue = NULL;

    const WorkflowPersistenceState* persistenceState = workflowData->persistenceState;
    if (persistenceState)
    {
        rootValue = json_parse_string(persistenceState->ReportingJson);
        if (UpdateLastInstallResult(rootValue, result) != JSONSuccess)
        {
            Log_Error("Failed to update lastInstallResult");
            goto done;
        }
    }
    else
    {
        rootValue = GetReportingJsonValue(workflowData, updateState, result, installedUpdateId);
        if (rootValue == NULL)
        {
            Log_Error("Failed to get reporting json value");
            goto done;
        }
    }

    jsonString = json_serialize_to_string(rootValue);
    if (jsonString == NULL)
    {
        Log_Error("Serializing JSON to string failed");
        goto done;
    }

    if (!ReportClientJsonProperty(jsonString, workflowData))
    {
        goto done;
    }

    success = true;

done:
    json_free_serialized_string(jsonString);
    // Don't free the persistenceData as that will be done by the startup logic that owns it.

    return success;
}

/**
 * @brief Report Idle State and update ID to service.
 *
 * This method handles reporting values after a successful apply.
 * After a successful apply, we need to report State as Idle and
 * we need to also update the installedUpdateId property.
 * @param[in] workflowData The workflow data.
 * @param[in] updateId Id of and update installed on the device.
 * @returns true if reporting succeeded.
 */
_Bool AzureDeviceUpdateCoreInterface_ReportUpdateIdAndIdleAsync(ADUC_WorkflowData* workflowData, const char* updateId)
{
    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportUpdateIdAndIdleAsync called before registration! Can't report!");
        return false;
    }

    ADUC_Result result = { .ResultCode = ADUC_Result_Apply_Success, .ExtendedResultCode = 0 };

    return AzureDeviceUpdateCoreInterface_ReportStateAndResultAsync(
        workflowData, ADUCITF_State_Idle, &result, updateId);
}
