#pragma once

#include <cstddef>

#include <ESPressio_MeshCapacityProfile.hpp>
#include <ESPressio_MeshV1ProtectedFrame.hpp>

namespace ESPressio::ESP32Platform {

/// <summary>ESP32 internal-memory Mesh v1 capacity profile.</summary>
/// <remarks>
/// The profile is a template because task stacks and non-framework application/composition storage must be supplied by
/// the shipping firmware build; ESPressio-ESP32 cannot truthfully infer them. Retained Mesh/Radio byte limits themselves
/// are fixed here and cross-checked against the RadioTransport compiled into the same whole-device accounting unit.
/// </remarks>
template<std::size_t MeshTaskStackBytes, std::size_t OtherApplicationAndCompositionBytes>
using InternalMemoryMeshCapacityProfile = Mesh::MeshPlatformCapacityProfile<
    0x45533332U, // ES32
    4096U,       // one complete Radio logical transfer for each inbound Mesh ownership slot
    512U,        // includes the largest hop-wrapped v1 handshake packet
    3584U,       // complete layered protected Node frame remains below the 4096-byte Radio ceiling
    4U,
    4096U,
    MeshTaskStackBytes,
    OtherApplicationAndCompositionBytes
>;

static_assert(
    Mesh::MeshV1ProtectedFrameCodec::HopPacketBytes(
        Mesh::MeshV1SecurityHandshakeCodec::ResponderPacketBytes) <= 512U,
    "The ESP32 control slot must retain the largest hop-wrapped Mesh v1 handshake."
);
static_assert(
    Mesh::MeshV1ProtectedFrameCodec::HopPacketBytes(
        Mesh::MeshV1ProtectedFrameCodec::EndToEndPacketBytes(3584U)) <= 4096U,
    "The ESP32 bounded-owned application payload must fit one layered Radio logical transfer."
);

} // namespace ESPressio::ESP32Platform
