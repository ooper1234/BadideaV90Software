#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace v92 {

class WintunAdapter {
public:
    WintunAdapter() = default;
    ~WintunAdapter();
    WintunAdapter(const WintunAdapter&) = delete;
    WintunAdapter& operator=(const WintunAdapter&) = delete;

    bool open(const std::wstring& name = L"v92isp",
              const std::string& address = "10.77.0.1",
              uint8_t prefix_length = 24);
    void close();

    bool send_packet(const uint8_t* data, size_t len);
    bool send_packet(const std::vector<uint8_t>& p) { return send_packet(p.data(), p.size()); }
    bool receive_packet(std::vector<uint8_t>& out);

    HANDLE read_event() const { return read_event_; }
    bool is_open() const { return session_ != nullptr; }
    const std::string& last_error() const { return last_error_; }

private:
    using AdapterHandle = void*;
    using SessionHandle = void*;

    HMODULE dll_ = nullptr;
    AdapterHandle adapter_ = nullptr;
    SessionHandle session_ = nullptr;
    HANDLE read_event_ = nullptr;
    std::string last_error_;

    using CreateAdapterFn = AdapterHandle (WINAPI*)(const WCHAR*, const WCHAR*, const GUID*);
    using OpenAdapterFn = AdapterHandle (WINAPI*)(const WCHAR*);
    using CloseAdapterFn = void (WINAPI*)(AdapterHandle);
    using GetAdapterLuidFn = void (WINAPI*)(AdapterHandle, void*);
    using StartSessionFn = SessionHandle (WINAPI*)(AdapterHandle, DWORD);
    using EndSessionFn = void (WINAPI*)(SessionHandle);
    using GetReadWaitEventFn = HANDLE (WINAPI*)(SessionHandle);
    using ReceivePacketFn = BYTE* (WINAPI*)(SessionHandle, DWORD*);
    using ReleaseReceivePacketFn = void (WINAPI*)(SessionHandle, const BYTE*);
    using AllocateSendPacketFn = BYTE* (WINAPI*)(SessionHandle, DWORD);
    using SendPacketFn = void (WINAPI*)(SessionHandle, const BYTE*);

    CreateAdapterFn create_adapter_ = nullptr;
    OpenAdapterFn open_adapter_ = nullptr;
    CloseAdapterFn close_adapter_ = nullptr;
    GetAdapterLuidFn get_adapter_luid_ = nullptr;
    StartSessionFn start_session_ = nullptr;
    EndSessionFn end_session_ = nullptr;
    GetReadWaitEventFn get_read_wait_event_ = nullptr;
    ReceivePacketFn receive_packet_ = nullptr;
    ReleaseReceivePacketFn release_receive_packet_ = nullptr;
    AllocateSendPacketFn allocate_send_packet_ = nullptr;
    SendPacketFn send_packet_ = nullptr;

    bool load_api();
    bool configure_ipv4(const std::string& address, uint8_t prefix_length);
    void set_win32_error(const char* what, DWORD code = GetLastError());
};

} // namespace v92

#endif // _WIN32
