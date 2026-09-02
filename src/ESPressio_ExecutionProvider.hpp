#pragma once

#include <cstdint>
#include <limits>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ESPressio_Execution.hpp>

namespace ESPressio::ESP32Platform {

class ExecutionProvider final : public System::Execution::IExecutionProvider {
private:
    static TaskHandle_t Native(System::Execution::ExecutionHandle handle) noexcept {
        return reinterpret_cast<TaskHandle_t>(handle);
    }

    static System::Execution::ExecutionHandle Portable(TaskHandle_t handle) noexcept {
        return reinterpret_cast<System::Execution::ExecutionHandle>(handle);
    }

public:
    System::Execution::ExecutionCreationResult Create(
        System::Execution::ExecutionEntry entry,
        void* context,
        const System::Execution::ExecutionConfiguration& configuration
    ) override {
        using namespace System;
        using namespace System::Execution;

        if (entry == nullptr || configuration.StackSizeBytes == 0) {
            return {PlatformResult::Failed(PlatformStatus::InvalidArgument), InvalidExecutionHandle};
        }

        if (
            configuration.Affinity.IsSpecific() &&
            static_cast<uint32_t>(configuration.Affinity.Processor) >= ProcessorCount()
        ) {
            return {PlatformResult::Failed(PlatformStatus::InvalidArgument), InvalidExecutionHandle};
        }

        TaskHandle_t handle = nullptr;
        BaseType_t created = pdFAIL;

        if (configuration.Affinity.IsSpecific()) {
            created = xTaskCreatePinnedToCore(
                entry,
                configuration.Name != nullptr ? configuration.Name : "ESPressio",
                static_cast<uint32_t>(configuration.StackSizeBytes),
                context,
                static_cast<UBaseType_t>(configuration.Priority),
                &handle,
                configuration.Affinity.Processor
            );
        } else {
            created = xTaskCreate(
                entry,
                configuration.Name != nullptr ? configuration.Name : "ESPressio",
                static_cast<uint32_t>(configuration.StackSizeBytes),
                context,
                static_cast<UBaseType_t>(configuration.Priority),
                &handle
            );
        }

        if (created != pdPASS || handle == nullptr) {
            return {PlatformResult::Failed(PlatformStatus::OutOfMemory), InvalidExecutionHandle};
        }

        return {PlatformResult::Succeeded(), Portable(handle)};
    }

    System::PlatformResult Destroy(System::Execution::ExecutionHandle handle) override {
        vTaskDelete(Native(handle));
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Suspend(System::Execution::ExecutionHandle handle) override {
        vTaskSuspend(Native(handle));
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Resume(System::Execution::ExecutionHandle handle) override {
        if (handle == System::Execution::InvalidExecutionHandle) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        vTaskResume(Native(handle));
        return System::PlatformResult::Succeeded();
    }

    System::Execution::ExecutionHandle Current() const noexcept override {
        return Portable(xTaskGetCurrentTaskHandle());
    }

    uint32_t MinimumFreeStackBytes(System::Execution::ExecutionHandle handle) const noexcept override {
        return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(Native(handle)));
    }

    uint32_t ProcessorCount() const noexcept override {
#if defined(portNUM_PROCESSORS)
        return portNUM_PROCESSORS > 0 ? static_cast<uint32_t>(portNUM_PROCESSORS) : 1U;
#elif defined(configNUMBER_OF_CORES)
        return configNUMBER_OF_CORES > 0 ? static_cast<uint32_t>(configNUMBER_OF_CORES) : 1U;
#else
        return 1U;
#endif
    }

    void SleepMilliseconds(uint32_t milliseconds) override {
        TickType_t ticks = pdMS_TO_TICKS(milliseconds);
        if (milliseconds != 0 && ticks == 0) ticks = 1;
        vTaskDelay(ticks);
    }

    void Yield() override {
        taskYIELD();
    }

    bool SupportsProcessorAffinity() const noexcept override {
        return ProcessorCount() > 1;
    }
};

inline ExecutionProvider& GetExecutionProvider() noexcept {
    static ExecutionProvider provider;
    return provider;
}

inline void InstallExecutionProvider() noexcept {
    System::Execution::SetProvider(&GetExecutionProvider());
}

} // namespace ESPressio::ESP32Platform
