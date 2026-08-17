#ifdef _WIN32

#include "wintun_adapter.hpp"

#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace v92 {

static std::string winerr(DWORD code) {
    char* text = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string s = n && text ? std::string(text, n) : ("Windows error " + std::to_string(code));
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

void WintunAdapter::set_win32_error(const char* what, DWORD code) {
    last_error_ = std::string(what) + ": " + winerr(code) + " (" + std::to_string(code) + ")";
}

WintunAdapter::~WintunAdapter() { close(); }

bool WintunAdapter::load_api() {
    if (dll_) return true;
    dll_ = LoadLibraryExW(L"wintun.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll_) dll_ = LoadLibraryW(L"wintun.dll");
    if (!dll_) {
        set_win32_error("LoadLibrary(wintun.dll)");
        return false;
    }
#define LOAD_FN(member, name) \
    member = reinterpret_cast<decltype(member)>(GetProcAddress(dll_, name)); \
    if (!member) { last_error_ = std::string("wintun.dll is missing export ") + name; return false; }
    LOAD_FN(create_adapter_, "WintunCreateAdapter");
    LOAD_FN(open_adapter_, "WintunOpenAdapter");
    LOAD_FN(close_adapter_, "WintunCloseAdapter");
    LOAD_FN(get_adapter_luid_, "WintunGetAdapterLUID");
    LOAD_FN(start_session_, "WintunStartSession");
    LOAD_FN(end_session_, "WintunEndSession");
    LOAD_FN(get_read_wait_event_, "WintunGetReadWaitEvent");
    LOAD_FN(receive_packet_, "WintunReceivePacket");
    LOAD_FN(release_receive_packet_, "WintunReleaseReceivePacket");
    LOAD_FN(allocate_send_packet_, "WintunAllocateSendPacket");
    LOAD_FN(send_packet_, "WintunSendPacket");
#undef LOAD_FN
    return true;
}

bool WintunAdapter::configure_ipv4(const std::string& address, uint8_t prefix_length) {
    if (!adapter_) return false;
    NET_LUID luid{};
    get_adapter_luid_(adapter_, &luid);

    // Reusing an adapter from an older v92isp build can leave its previous
    // private IPv4 (for example 10.77.0.1) attached. Remove stale IPv4
    // addresses before applying the current PPP/ICS subnet.
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR && table) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            auto& old = table->Table[i];
            if (old.InterfaceLuid.Value == luid.Value) {
                DeleteUnicastIpAddressEntry(&old);
            }
        }
        FreeMibTable(table);
    }

    MIB_UNICASTIPADDRESS_ROW row{};
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceLuid = luid;
    row.Address.Ipv4.sin_family = AF_INET;
    if (InetPtonA(AF_INET, address.c_str(), &row.Address.Ipv4.sin_addr) != 1) {
        last_error_ = "invalid Wintun IPv4 address: " + address;
        return false;
    }
    row.OnLinkPrefixLength = prefix_length;
    row.PrefixOrigin = IpPrefixOriginManual;
    row.SuffixOrigin = IpSuffixOriginManual;
    row.ValidLifetime = 0xffffffffu;
    row.PreferredLifetime = 0xffffffffu;
    row.SkipAsSource = FALSE;

    // The newly-created NDIS interface can take a moment to appear in the IP
    // helper tables. Retry ERROR_NOT_FOUND briefly rather than making startup flaky.
    for (int attempt = 0; attempt < 30; ++attempt) {
        DWORD r = CreateUnicastIpAddressEntry(&row);
        if (r == NO_ERROR || r == ERROR_OBJECT_ALREADY_EXISTS) return true;
        if (r != ERROR_NOT_FOUND) {
            set_win32_error("CreateUnicastIpAddressEntry", r);
            return false;
        }
        Sleep(100);
    }
    last_error_ = "Wintun adapter did not appear in the Windows IP stack";
    return false;
}

bool WintunAdapter::open(const std::wstring& name, const std::string& address, uint8_t prefix_length) {
    close();
    last_error_.clear();
    if (!load_api()) return false;

    adapter_ = open_adapter_(name.c_str());
    if (!adapter_) {
        SetLastError(ERROR_SUCCESS);
        adapter_ = create_adapter_(name.c_str(), L"v92isp", nullptr);
    }
    if (!adapter_) {
        set_win32_error("WintunCreateAdapter");
        close();
        return false;
    }

    if (!configure_ipv4(address, prefix_length)) {
        close();
        return false;
    }

    // 4 MiB is the capacity used by Wintun's reference example and easily
    // absorbs bursts from Windows while the dial-up side drains slowly.
    session_ = start_session_(adapter_, 0x400000);
    if (!session_) {
        set_win32_error("WintunStartSession");
        close();
        return false;
    }
    read_event_ = get_read_wait_event_(session_);
    if (!read_event_) {
        set_win32_error("WintunGetReadWaitEvent");
        close();
        return false;
    }
    return true;
}

void WintunAdapter::close() {
    read_event_ = nullptr;
    if (session_ && end_session_) end_session_(session_);
    session_ = nullptr;
    if (adapter_ && close_adapter_) close_adapter_(adapter_);
    adapter_ = nullptr;
    if (dll_) FreeLibrary(dll_);
    dll_ = nullptr;
    create_adapter_ = nullptr; open_adapter_ = nullptr; close_adapter_ = nullptr;
    get_adapter_luid_ = nullptr; start_session_ = nullptr; end_session_ = nullptr;
    get_read_wait_event_ = nullptr; receive_packet_ = nullptr; release_receive_packet_ = nullptr;
    allocate_send_packet_ = nullptr; send_packet_ = nullptr;
}

bool WintunAdapter::send_packet(const uint8_t* data, size_t len) {
    if (!session_ || !data || len == 0 || len > 65535) return false;
    BYTE* p = allocate_send_packet_(session_, static_cast<DWORD>(len));
    if (!p) {
        DWORD e = GetLastError();
        if (e != ERROR_BUFFER_OVERFLOW) set_win32_error("WintunAllocateSendPacket", e);
        return false;
    }
    std::memcpy(p, data, len);
    send_packet_(session_, p);
    return true;
}

bool WintunAdapter::receive_packet(std::vector<uint8_t>& out) {
    out.clear();
    if (!session_) return false;
    DWORD len = 0;
    BYTE* p = receive_packet_(session_, &len);
    if (!p) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_MORE_ITEMS) return false;
        if (e != ERROR_HANDLE_EOF) set_win32_error("WintunReceivePacket", e);
        return false;
    }
    out.assign(p, p + len);
    release_receive_packet_(session_, p);
    return true;
}

} // namespace v92

#endif // _WIN32
