// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <boost/container/flat_map.hpp>
#include "common/common_types.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/result.h"
#include "core/memory.h"

namespace Kernel {
class ServerSession;

// TODO(Subv): Move these declarations out of here
static const int kCommandHeaderOffset = 0x80; ///< Offset into command buffer of header

/**
 * Returns a pointer to the command buffer in the current thread's TLS
 * TODO(Subv): This is not entirely correct, the command buffer should be copied from
 * the thread's TLS to an intermediate buffer in kernel memory, and then copied again to
 * the service handler process' memory.
 * @param offset Optional offset into command buffer
 * @return Pointer to command buffer
 */
inline u32* GetCommandBuffer(const int offset = 0) {
    return (u32*)Memory::GetPointer(GetCurrentThread()->GetTLSAddress() + kCommandHeaderOffset +
                                    offset);
}
}

// TODO(Subv): Move this namespace out of here
namespace IPC {

enum DescriptorType : u32 {
    // Buffer related desciptors types (mask : 0x0F)
    StaticBuffer = 0x02,
    PXIBuffer = 0x04,
    MappedBuffer = 0x08,
    // Handle related descriptors types (mask : 0x30, but need to check for buffer related
    // descriptors first )
    CopyHandle = 0x00,
    MoveHandle = 0x10,
    CallingPid = 0x20,
};

/**
 * @brief Creates a command header to be used for IPC
 * @param command_id            ID of the command to create a header for.
 * @param normal_params         Size of the normal parameters in words. Up to 63.
 * @param translate_params_size Size of the translate parameters in words. Up to 63.
 * @return The created IPC header.
 *
 * Normal parameters are sent directly to the process while the translate parameters might go
 * through modifications and checks by the kernel.
 * The translate parameters are described by headers generated with the IPC::*Desc functions.
 *
 * @note While #normal_params is equivalent to the number of normal parameters,
 * #translate_params_size includes the size occupied by the translate parameters headers.
 */
constexpr u32 MakeHeader(u16 command_id, unsigned int normal_params,
                         unsigned int translate_params_size) {
    return (u32(command_id) << 16) | ((u32(normal_params) & 0x3F) << 6) |
           (u32(translate_params_size) & 0x3F);
}

union Header {
    u32 raw;
    BitField<0, 6, u32> translate_params_size;
    BitField<6, 6, u32> normal_params;
    BitField<16, 16, u32> command_id;
};

inline Header ParseHeader(u32 header) {
    return {header};
}

constexpr u32 MoveHandleDesc(u32 num_handles = 1) {
    return MoveHandle | ((num_handles - 1) << 26);
}

constexpr u32 CopyHandleDesc(u32 num_handles = 1) {
    return CopyHandle | ((num_handles - 1) << 26);
}

constexpr u32 CallingPidDesc() {
    return CallingPid;
}

constexpr bool isHandleDescriptor(u32 descriptor) {
    return (descriptor & 0xF) == 0x0;
}

constexpr u32 HandleNumberFromDesc(u32 handle_descriptor) {
    return (handle_descriptor >> 26) + 1;
}

constexpr u32 StaticBufferDesc(u32 size, u8 buffer_id) {
    return StaticBuffer | (size << 14) | ((buffer_id & 0xF) << 10);
}

union StaticBufferDescInfo {
    u32 raw;
    BitField<10, 4, u32> buffer_id;
    BitField<14, 18, u32> size;
};

inline StaticBufferDescInfo ParseStaticBufferDesc(const u32 desc) {
    return {desc};
}

/**
 * @brief Creates a header describing a buffer to be sent over PXI.
 * @param size         Size of the buffer. Max 0x00FFFFFF.
 * @param buffer_id    The Id of the buffer. Max 0xF.
 * @param is_read_only true if the buffer is read-only. If false, the buffer is considered to have
 * read-write access.
 * @return The created PXI buffer header.
 *
 * The next value is a phys-address of a table located in the BASE memregion.
 */
inline u32 PXIBufferDesc(u32 size, unsigned buffer_id, bool is_read_only) {
    u32 type = PXIBuffer;
    if (is_read_only)
        type |= 0x2;
    return type | (size << 8) | ((buffer_id & 0xF) << 4);
}

enum MappedBufferPermissions {
    R = 1,
    W = 2,
    RW = R | W,
};

constexpr u32 MappedBufferDesc(u32 size, MappedBufferPermissions perms) {
    return MappedBuffer | (size << 4) | (u32(perms) << 1);
}

union MappedBufferDescInfo {
    u32 raw;
    BitField<4, 28, u32> size;
    BitField<1, 2, MappedBufferPermissions> perms;
};

inline MappedBufferDescInfo ParseMappedBufferDesc(const u32 desc) {
    return{ desc };
}

inline DescriptorType GetDescriptorType(u32 descriptor) {
    // Note: Those checks must be done in this order
    if (isHandleDescriptor(descriptor))
        return (DescriptorType)(descriptor & 0x30);

    // handle the fact that the following descriptors can have rights
    if (descriptor & MappedBuffer)
        return MappedBuffer;

    if (descriptor & PXIBuffer)
        return PXIBuffer;

    return StaticBuffer;
}

} // namespace IPC

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace Service

namespace Service {

static const int kMaxPortSize = 8; ///< Maximum size of a port name (8 characters)
static const u32 DefaultMaxSessions = 10; ///< Arbitrary default number of maximum connections to an HLE port

/**
 * Interface implemented by HLE Session handlers.
 * This can be provided to a ServerSession in order to hook into several relevant events (such as a new connection or a SyncRequest)
 * so they can be implemented in the emulator.
 */
class SessionRequestHandler {
public:
    /**
     * Dispatches and handles a sync request from the emulated application.
     * @param server_session The ServerSession that was triggered for this sync request,
     * it should be used to differentiate which client (As in ClientSession) we're answering to.
     * @returns ResultCode the result code of the translate operation.
     */
    ResultCode HandleSyncRequest(Kernel::SharedPtr<Kernel::ServerSession> server_session);

protected:
    /**
     * Handles a sync request from the emulated application and writes the response to the command buffer.
     * TODO(Subv): Use a wrapper structure to hold all the information relevant to
     * this request (ServerSession, Originator thread, Translated command buffer, etc).
     */
    virtual void HandleSyncRequestImpl(Kernel::SharedPtr<Kernel::ServerSession> server_session) = 0;

private:
    /**
     * Performs command buffer translation for this request.
     * The command buffer from the ServerSession thread's TLS is copied into a
     * buffer and all descriptors in the buffer are processed.
     * TODO(Subv): Implement this function, currently we do not support multiple processes running at once,
     * but once that is implemented we'll need to properly translate all descriptors in the command buffer.
     */
    ResultCode TranslateRequest(Kernel::SharedPtr<Kernel::ServerSession> server_session);
};

/**
 * Framework for implementing HLE service handlers which dispatch incoming SyncRequests based on a table mapping header ids to handler functions.
 */
class Interface : public SessionRequestHandler {
public:
    std::string GetName() const {
        return GetPortName();
    }

    virtual void SetVersion(u32 raw_version) {
        version.raw = raw_version;
    }
    virtual ~Interface() {}

    /**
     * Gets the maximum allowed number of sessions that can be connected to this port at the same time.
     * It should be overwritten by each service implementation for more fine-grained control.
     * @returns The maximum number of connections allowed.
     */
    virtual u32 GetMaxSessions() const { return DefaultMaxSessions; }

    typedef void (*Function)(Interface*);

    struct FunctionInfo {
        u32 id;
        Function func;
        const char* name;
    };

    /**
     * Gets the string name used by CTROS for a service
     * @return Port name of service
     */
    virtual std::string GetPortName() const {
        return "[UNKNOWN SERVICE PORT]";
    }

protected:
    void HandleSyncRequestImpl(Kernel::SharedPtr<Kernel::ServerSession> server_session) override;

    /**
     * Registers the functions in the service
     */
    template <size_t N>
    inline void Register(const FunctionInfo (&functions)[N]) {
        Register(functions, N);
    }

    void Register(const FunctionInfo* functions, size_t n);

    union {
        u32 raw;
        BitField<0, 8, u32> major;
        BitField<8, 8, u32> minor;
        BitField<16, 8, u32> build;
        BitField<24, 8, u32> revision;
    } version = {};

private:
    boost::container::flat_map<u32, FunctionInfo> m_functions;
};

/// Initialize ServiceManager
void Init();

/// Shutdown ServiceManager
void Shutdown();

/// Map of named ports managed by the kernel, which can be retrieved using the ConnectToPort SVC.
extern std::unordered_map<std::string, Kernel::SharedPtr<Kernel::ClientPort>> g_kernel_named_ports;
/// Map of services registered with the "srv:" service, retrieved using GetServiceHandle.
extern std::unordered_map<std::string, Kernel::SharedPtr<Kernel::ClientPort>> g_srv_services;

/// Adds a service to the services table
void AddService(Interface* interface_);

} // namespace
