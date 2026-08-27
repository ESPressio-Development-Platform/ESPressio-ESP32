#pragma once

#include <cstdint>
#include <limits>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ESPressio_Execution.hpp>

namespace ESPressio::ESP32Platform {

class ESP32ExecutionProvider final : public System::Execution::IExecutionProvider {
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
            return {
                PlatformResult::Failed(PlatformStatus::InvalidArgument),
                InvalidExecutionHandle
            };
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
            return {
                PlatformResult::Failed(PlatformStatus::OutOfMemory),
                InvalidExecutionHandle
            };
        }

        return {
            PlatformResult::Succeeded(),
            Portable(handle)
        };
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

    uint32_t MinimumFreeStackBytes(
        System::Execution::ExecutionHandle handle
    ) const noexcept override {
        return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(Native(handle)));
    }

    void SleepMilliseconds(uint32_t milliseconds) override {
        TickType ticks = pdMS_TO_TICKS(milliseconds);
        if (milliseconds != 0 && ticks == 0) {
            ticks = 1;
        }
        vTaskDelay(ticks);
    }

    void Yield() override {
        taskYIELD();
    }

    bool SupportsProcessorAffinity() const noexcept override {
        return true;
    }
};

inline ESP32ExecutionProvider& ExecutionProvider() noexcept {
    static ESP32ExecutionProvider provider;
    return provider;
}

inline void InstallExecutionProvider() noexcept {
    System::Execution::SetProvider(&ExecutionProvider());
}

} // namespace ESPressio::ESP32Platform
