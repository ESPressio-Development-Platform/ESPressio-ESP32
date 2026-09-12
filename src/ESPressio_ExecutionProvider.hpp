#pragma once

#include <cstdint>
#include <limits>
#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <ESPressio_Execution.hpp>

namespace ESPressio::ESP32Platform {

/// <summary>ESP32/FreeRTOS implementation of the System execution-provider contract.</summary>
class ExecutionProvider final : public System::Execution::IExecutionProvider {
private:
    struct JoinableControl final {
        System::Execution::ExecutionEntry Entry{nullptr};
        void* Context{nullptr};
        TaskHandle_t Task{nullptr};
        SemaphoreHandle_t Completed{nullptr};
    };

    static TaskHandle_t Native(System::Execution::ExecutionHandle handle) noexcept {
        return reinterpret_cast<TaskHandle_t>(handle);
    }

    static System::Execution::ExecutionHandle Portable(TaskHandle_t handle) noexcept {
        return reinterpret_cast<System::Execution::ExecutionHandle>(handle);
    }

    static JoinableControl* Joinable(System::Execution::ExecutionHandle handle) noexcept {
        return reinterpret_cast<JoinableControl*>(handle);
    }

    static bool ConfigurationValid(const System::Execution::ExecutionConfiguration& configuration) noexcept {
        if (configuration.StackSizeBytes == 0) return false;
        if (configuration.Affinity.IsSpecific() &&
            static_cast<std::uint32_t>(configuration.Affinity.Processor) >= GetExecutionProviderProcessorCount())
            return false;
        return true;
    }

    static std::uint32_t GetExecutionProviderProcessorCount() noexcept {
#if defined(portNUM_PROCESSORS)
        return portNUM_PROCESSORS > 0 ? static_cast<std::uint32_t>(portNUM_PROCESSORS) : 1U;
#elif defined(configNUMBER_OF_CORES)
        return configNUMBER_OF_CORES > 0 ? static_cast<std::uint32_t>(configNUMBER_OF_CORES) : 1U;
#else
        return 1U;
#endif
    }

    static BaseType_t CreateNative(
        TaskFunction_t entry,
        void* context,
        const System::Execution::ExecutionConfiguration& configuration,
        TaskHandle_t* handle) noexcept {
        if (configuration.Affinity.IsSpecific()) {
            return xTaskCreatePinnedToCore(
                entry,
                configuration.Name != nullptr ? configuration.Name : "ESPressio",
                static_cast<std::uint32_t>(configuration.StackSizeBytes),
                context,
                static_cast<UBaseType_t>(configuration.Priority),
                handle,
                configuration.Affinity.Processor);
        }
        return xTaskCreate(
            entry,
            configuration.Name != nullptr ? configuration.Name : "ESPressio",
            static_cast<std::uint32_t>(configuration.StackSizeBytes),
            context,
            static_cast<UBaseType_t>(configuration.Priority),
            handle);
    }

    static void JoinableEntry(void* opaque) noexcept {
        auto* control = static_cast<JoinableControl*>(opaque);
        if (control == nullptr || control->Entry == nullptr || control->Completed == nullptr) {
            vTaskSuspend(nullptr);
            return;
        }

        // The caller-owned context remains reachable only while Entry executes. Once it returns,
        // publishing Completed is the synchronization boundary consumed by Join().
        control->Entry(control->Context);
        xSemaphoreGive(control->Completed);

        // A joinable FreeRTOS task must not return and must not self-delete: Join owns final release.
        // Join may delete us immediately after the give, including before this suspend executes.
        vTaskSuspend(nullptr);
    }

public:
    System::Execution::ExecutionCreationResult Create(
        System::Execution::ExecutionEntry entry,
        void* context,
        const System::Execution::ExecutionConfiguration& configuration
    ) override {
        using namespace System;
        using namespace System::Execution;

        if (entry == nullptr || !ConfigurationValid(configuration)) {
            return {PlatformResult::Failed(PlatformStatus::InvalidArgument), InvalidExecutionHandle};
        }

        TaskHandle_t handle = nullptr;
        const BaseType_t created = CreateNative(entry, context, configuration, &handle);
        if (created != pdPASS || handle == nullptr) {
            return {PlatformResult::Failed(PlatformStatus::OutOfMemory), InvalidExecutionHandle};
        }
        return {PlatformResult::Succeeded(), Portable(handle)};
    }

    System::Execution::ExecutionCreationResult CreateJoinable(
        System::Execution::ExecutionEntry entry,
        void* context,
        const System::Execution::ExecutionConfiguration& configuration
    ) override {
        using namespace System;
        using namespace System::Execution;

        if (entry == nullptr || !ConfigurationValid(configuration)) {
            return {PlatformResult::Failed(PlatformStatus::InvalidArgument), InvalidExecutionHandle};
        }

        auto* control = new (std::nothrow) JoinableControl{};
        if (control == nullptr) {
            return {PlatformResult::Failed(PlatformStatus::OutOfMemory), InvalidExecutionHandle};
        }
        control->Entry = entry;
        control->Context = context;
        control->Completed = xSemaphoreCreateBinary();
        if (control->Completed == nullptr) {
            delete control;
            return {PlatformResult::Failed(PlatformStatus::OutOfMemory), InvalidExecutionHandle};
        }

        const BaseType_t created = CreateNative(&JoinableEntry, control, configuration, &control->Task);
        if (created != pdPASS || control->Task == nullptr) {
            vSemaphoreDelete(control->Completed);
            delete control;
            return {PlatformResult::Failed(PlatformStatus::OutOfMemory), InvalidExecutionHandle};
        }

        return {
            PlatformResult::Succeeded(),
            reinterpret_cast<ExecutionHandle>(control)
        };
    }

    System::PlatformResult Join(System::Execution::ExecutionHandle handle) override {
        using namespace System;
        using namespace System::Execution;
        if (handle == InvalidExecutionHandle) {
            return PlatformResult::Failed(PlatformStatus::InvalidArgument);
        }
        auto* control = Joinable(handle);
        if (control == nullptr || control->Task == nullptr || control->Completed == nullptr ||
            xTaskGetCurrentTaskHandle() == control->Task) {
            return PlatformResult::Failed(PlatformStatus::InvalidArgument);
        }
        if (xSemaphoreTake(control->Completed, portMAX_DELAY) != pdTRUE) {
            return PlatformResult::Failed(PlatformStatus::Unavailable);
        }

        vTaskDelete(control->Task);
        vSemaphoreDelete(control->Completed);
        control->Task = nullptr;
        control->Completed = nullptr;
        delete control;
        return PlatformResult::Succeeded();
    }

    System::PlatformResult Destroy(System::Execution::ExecutionHandle handle) override {
        if (handle == System::Execution::InvalidExecutionHandle) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        vTaskDelete(Native(handle));
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Suspend(System::Execution::ExecutionHandle handle) override {
        if (handle == System::Execution::InvalidExecutionHandle) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
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

    std::uint32_t MinimumFreeStackBytes(System::Execution::ExecutionHandle handle) const noexcept override {
        return static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(Native(handle)));
    }

    std::uint32_t ProcessorCount() const noexcept override {
        return GetExecutionProviderProcessorCount();
    }

    void SleepMilliseconds(std::uint32_t milliseconds) override {
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
