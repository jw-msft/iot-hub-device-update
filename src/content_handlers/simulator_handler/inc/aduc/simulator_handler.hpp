/**
 * @file simulator_handler.hpp
 * @brief Defines SimulatorHandlerImpl.
 *
 * @copyright Copyright (c) Microsoft Corp.
 */
#ifndef ADUC_SIMULATOR_HANDLER_HPP
#define ADUC_SIMULATOR_HANDLER_HPP

#include "aduc/content_handler.hpp"
#include "aduc/content_handler_factory.hpp"
#include "aduc/simulator_handler.hpp"
#include <aduc/result.h>
#include <memory>
#include <string>

EXTERN_C_BEGIN

/**
 * @brief Instantiates an Update Content Handler simulator.
 * @return A pointer to an instantiated Update Content Handler object.
 */
ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel);

EXTERN_C_END

/**
 * @class SimulatorHandlerImpl
 * @brief The simulator handler implementation.
 */
class SimulatorHandlerImpl : public ContentHandler
{
public:
    static ContentHandler* CreateContentHandler();

    // Delete copy ctor, copy assignment, move ctor and move assignment operators.
    SimulatorHandlerImpl(const SimulatorHandlerImpl&) = delete;
    SimulatorHandlerImpl& operator=(const SimulatorHandlerImpl&) = delete;
    SimulatorHandlerImpl(SimulatorHandlerImpl&&) = delete;
    SimulatorHandlerImpl& operator=(SimulatorHandlerImpl&&) = delete;

    ~SimulatorHandlerImpl() override = default;

    ADUC_Result Download(const ADUC_WorkflowData* workflowData) override;
    ADUC_Result Install(const ADUC_WorkflowData* workflowData) override;
    ADUC_Result Apply(const ADUC_WorkflowData* workflowData) override;
    ADUC_Result Cancel(const ADUC_WorkflowData* workflowData) override;
    ADUC_Result IsInstalled(const ADUC_WorkflowData* workflowData) override;

    void SetIsInstalled(bool isInstalled);

private:
    // Private constructor, must call CreateContentHandler factory method.
    SimulatorHandlerImpl()
    {
    }

    bool _isInstalled{ false };
};

#endif // ADUC_SIMULATOR_HANDLER_HPP