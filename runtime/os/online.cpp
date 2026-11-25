// Online play on an MH3U Revival server (https://github.com/Matt-Wood-23/mh3u-revival), the community NEX server that
// Cemu (through the revival's patched build) and real Wii U consoles (through its Aroma plugin) already play on. The
// game keeps its own NEX client; this file gives it what a connected console would:
//   nsysnet  real sockets (Winsock). The NAT-check servers (nncs1/nncs2.app.nintendowifi.net) are looked up as the
//            revival server, which answers those checks itself.
//   nn_ac    connected, with this PC's address on the route to the server.
//   nn_act   a network account: a principal id kept in <data>/mh3u_account.txt, the revival's default Mii, and a NEX
//            token naming the server.
//   nn_fp    friends started and logged in, with no friends (there is no friends server). Network Mode waits for this
//            login, and every hall checks HasLoggedIn first.
// On when <data folder>/mh3u_server.txt names a server: "host" or "host:port" (port 1223 when left out), the same file
// the revival's Cemu reads. Without it only nn_fp is added (its login fails, so Network Mode shows the game's error
// instead of stopping on a missing import) and the rest stays as offline.cpp and nn_act.cpp make it.
// MH3U_NET_LOG=1 prints the token, lookups and socket errors.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "hle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace mh3u::os {
namespace {

void reg(const char* lib, const char* name, ppc::GuestFunction fn) {
    ppc::register_import((std::string(lib) + "." + name).c_str(), fn);
}

bool g_log = false;

// ---- settings ----------------------------------------------------------------------------------------------------

struct Server {
    std::string host;  // as written in mh3u_server.txt
    std::string ip;    // dotted IPv4 (the NEX token's host field holds 15 characters)
    uint16_t port = 1223;
};
Server g_server;
uint32_t g_pid = 0;     // principal id: the NEX user id the server knows this install by
std::string g_account;  // account id (16 characters at most)

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// the first line that isn't blank or a # comment: "host" or "host:port"
bool read_server(const std::filesystem::path& file, Server& out) {
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t colon = line.rfind(':');
        if (colon != std::string::npos && colon + 1 < line.size() &&
            line.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            const int port = std::atoi(line.c_str() + colon + 1);
            if (port > 0 && port < 65536) out.port = static_cast<uint16_t>(port);
            line = trim(line.substr(0, colon));
        }
        out.host = line;
        return !out.host.empty();
    }
    return false;
}

// one principal id per install, made on first use in the range the revival's launcher uses for Cemu
void load_identity(const std::filesystem::path& file) {
    {
        std::ifstream f(file);
        std::string line;
        while (std::getline(f, line))
            if (trim(line).rfind("pid=", 0) == 0) g_pid = static_cast<uint32_t>(std::strtoul(trim(line).c_str() + 4, nullptr, 16));
    }
    if (g_pid < 0x40000000 || g_pid > 0x6FFFFFFF) {
        std::random_device rd;
        g_pid = 0x40000000 + std::uniform_int_distribution<uint32_t>(0, 0x2FFFFFFF)(rd);
        std::ofstream f(file);
        char line[32];
        std::snprintf(line, sizeof line, "pid=%08x\n", g_pid);
        f << "# Cha-Cha's id on MH3U Revival servers (a random principal id, made once).\n" << line;
    }
    char id[24];
    std::snprintf(id, sizeof id, "ChaChaMH3U%04X", g_pid & 0xFFFF);
    g_account = id;
}

// the revival's default Mii (FFLStoreData), as its launcher writes into Cemu's account.dat
constexpr uint8_t kDefaultMii[0x60] = {
    0x01, 0x00, 0x01, 0x10, 0x00, 0x00, 0xd7, 0x3e, 0x03, 0x00, 0x34, 0x33, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x64, 0x00, 0x65, 0x00, 0x66, 0x00, 0x61, 0x00, 0x75, 0x00, 0x6c, 0x00, 0x74, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
    0x06, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01};

void put_wstr(uint8_t* b, uint32_t addr, const char* s, size_t max_chars) {  // UTF-16BE, NUL-terminated
    size_t i = 0;
    for (; s[i] && i + 1 < max_chars; ++i) ppc::st16(b, addr + 2 * static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    ppc::st16(b, addr + 2 * static_cast<uint32_t>(i), 0);
}

// ---- callbacks of async calls ------------------------------------------------------------------------------------
// On the console the reply to an async request comes back later, from IOSU, on another thread: the caller returns
// first and waits for the callback. One host thread with its own guest stack plays that part, as the alarm thread
// does for OSAlarm (coreinit_misc.cpp).

struct Callback {
    uint32_t fn, result, context;
    bool login;  // LoginAsync: logged in from now on (HasLoggedIn), set just before the callback runs
};
std::mutex g_cb_m;
std::condition_variable g_cb_cv;
std::deque<Callback> g_cb_queue;
uint32_t g_cb_stack_top = 0;
uint32_t g_cb_results = 0;  // 16 guest u32s: a callback gets a POINTER to its nn::Result (GHS passes classes by
                            // reference), as Cemu's nn_fp notes
std::atomic<bool> g_logged_in{false};

void callback_service() {
    for (;;) {
        Callback cb;
        {
            std::unique_lock l(g_cb_m);
            g_cb_cv.wait(l, [] { return !g_cb_queue.empty(); });
            cb = g_cb_queue.front();
            g_cb_queue.pop_front();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // after the caller has returned and waits
        if (cb.login && (cb.result & 0x80000000u) == 0) g_logged_in = true;
        static uint32_t n = 0;
        const uint32_t result = g_cb_results + 4 * (n++ % 16);
        ppc::st32(gbase(), result, cb.result);
        ppc::Context ctx{};
        ctx.r[1] = g_cb_stack_top - 0x20;
        ctx.r[3] = result;
        ctx.r[4] = cb.context;
        try {
            ppc::call_indirect(ctx, gbase(), cb.fn);
        } catch (const GuestExit& e) {
            std::fflush(stdout);
            std::_Exit(e.code);
        }
    }
}

void post(uint32_t fn, uint32_t result, uint32_t context, bool login = false) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(callback_service).detach(); });
    {
        std::lock_guard g(g_cb_m);
        g_cb_queue.push_back({fn, result, context, login});
    }
    g_cb_cv.notify_one();
}

// ---- nn_fp: friends --------------------------------------------------------------------------------------------

constexpr uint32_t kFpOk = 0x00C00000;    // nn::Result: success, module nn_fp (12)
constexpr uint32_t kFpFail = 0xE0C00000;  // fatal, module nn_fp: the login can't reach a friends server

bool g_online = false;  // mh3u_server.txt names a server
std::atomic<int> g_fp_init{0};
std::mutex g_fp_m;
std::array<uint8_t, 0x2C> g_game_mode{};      // GameMode from UpdateGameMode
std::array<uint8_t, 0x80> g_presence_text{};  // its message (UTF-16BE, 64 characters)
std::vector<uint32_t> g_blocked;              // AddBlackListAsync, for GetBlackList (this session)

void zero(uint8_t* b, uint32_t addr, uint32_t size) {
    if (addr && size) std::memset(b + addr, 0, size);
}

// async request: answer with the callback later, return success now
void fp_async(ppc::Context& c, uint32_t fn, uint32_t context, bool login = false) {
    if (fn) post(fn, g_online ? kFpOk : kFpFail, context, login);
    ret(c, kFpOk);
}

// Result GetXxxList(u32* principalIds, u32* count, u32 offset, u32 max)
void fp_list(ppc::Context& c, uint8_t* b, const std::vector<uint32_t>& list) {
    const uint32_t out = arg(c, 0), count = arg(c, 1), offset = arg(c, 2), max = arg(c, 3);
    uint32_t n = 0;
    for (size_t i = offset; i < list.size() && n < max; ++i, ++n)
        if (out) ppc::st32(b, out + 4 * n, list[i]);
    if (count) ppc::st32(b, count, n);
    ret(c, kFpOk);
}

void register_fp() {
    const auto ok = [](ppc::Context& c, uint8_t*) { ret(c, kFpOk); };
    reg("nn_fp", "Initialize__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) {
        ++g_fp_init;
        ret(c, kFpOk);
    });
    reg("nn_fp", "IsInitialized__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, g_fp_init > 0 ? 1 : 0); });
    reg("nn_fp", "Finalize__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) {
        if (g_fp_init > 0) --g_fp_init;
        ret(c, kFpOk);
    });
    // MH3U needs HasLoggedIn false until its LoginAsync has completed, or it never logs in (noted in Cemu's nn_fp)
    reg("nn_fp", "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv",
        [](ppc::Context& c, uint8_t*) { fp_async(c, arg(c, 0), arg(c, 1), true); });
    reg("nn_fp", "HasLoggedIn__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, g_logged_in ? 1 : 0); });
    reg("nn_fp", "Logout__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) {
        g_logged_in = false;
        ret(c, kFpOk);
    });
    reg("nn_fp", "RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv",
        [](ppc::Context& c, uint8_t*) { fp_async(c, arg(c, 0), arg(c, 1)); });
    // the room menu's "Add to blocked-user list": kept for GetBlackList; the callback must come or the game waits
    reg("nn_fp", "AddBlackListAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv", [](ppc::Context& c, uint8_t*) {
        {
            std::lock_guard g(g_fp_m);
            if (std::find(g_blocked.begin(), g_blocked.end(), arg(c, 0)) == g_blocked.end()) g_blocked.push_back(arg(c, 0));
        }
        fp_async(c, arg(c, 1), arg(c, 2));
    });
    reg("nn_fp", "AddFriendAsync__Q2_2nn2fpFPCcPFQ2_2nn6ResultPv_vPv",
        [](ppc::Context& c, uint8_t*) { fp_async(c, arg(c, 1), arg(c, 2)); });
    reg("nn_fp", "AddFriendAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv",
        [](ppc::Context& c, uint8_t*) { fp_async(c, arg(c, 1), arg(c, 2)); });
    // (u32 principalId, u8, const wchar_t*, u8, const wchar_t*, callback, context)
    reg("nn_fp", "AddFriendRequestAsync__Q2_2nn2fpFUiUcPCwT2T3PFQ2_2nn6ResultPv_vPv",
        [](ppc::Context& c, uint8_t*) { fp_async(c, arg(c, 5), arg(c, 6)); });
    reg("nn_fp", "AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi", ok);
    reg("nn_fp", "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv", ok);
    reg("nn_fp", "GetFriendList__Q2_2nn2fpFPUiT1UiT3", [](ppc::Context& c, uint8_t* b) { fp_list(c, b, {}); });
    reg("nn_fp", "GetBlackList__Q2_2nn2fpFPUiT1UiT3", [](ppc::Context& c, uint8_t* b) {
        std::vector<uint32_t> list;
        {
            std::lock_guard g(g_fp_m);
            list = g_blocked;
        }
        fp_list(c, b, list);
    });
    // per-friend data for a list of principal ids (out, ids, count): nothing is known about anyone
    reg("nn_fp", "GetBlackListAccountId__Q2_2nn2fpFPA17_cPCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 17 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetBlackListAdditionalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 8 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetFriendAccountId__Q2_2nn2fpFPA17_cPCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 17 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetFriendApprovalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 8 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 0x60 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 0x34 * arg(c, 2)), ret(c, kFpOk); });
    reg("nn_fp", "GetFriendProfile__Q2_2nn2fpFPQ3_2nn2fp7ProfilePCUiUi",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 4 * arg(c, 2)), ret(c, kFpOk); });
    // (wchar_t (*)[11] names, const u32* ids, u32 count, bool, u8* languages)
    reg("nn_fp", "GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc", [](ppc::Context& c, uint8_t* b) {
        zero(b, arg(c, 0), 22 * arg(c, 2));
        zero(b, arg(c, 4), arg(c, 2));
        ret(c, kFpOk);
    });
    reg("nn_fp", "IsJoinable__Q2_2nn2fpFPCQ3_2nn2fp14FriendPresenceUL", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    reg("nn_fp", "GetMyPrincipalId__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, g_online ? g_pid : 0); });
    reg("nn_fp", "GetMyAccountId__Q2_2nn2fpFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) {
            const std::string id = g_online ? g_account : std::string();
            std::memcpy(b + arg(c, 0), id.c_str(), id.size() + 1);
        }
        ret(c, kFpOk);
    });
    reg("nn_fp", "GetMyScreenName__Q2_2nn2fpFPw", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) put_wstr(b, arg(c, 0), "Cha-Cha", 11);
        ret(c, kFpOk);
    });
    reg("nn_fp", "GetMyMii__Q2_2nn2fpFP12FFLStoreData", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memcpy(b + arg(c, 0), kDefaultMii, sizeof kDefaultMii);
        ret(c, kFpOk);
    });
    reg("nn_fp", "GetMyProfile__Q2_2nn2fpFPQ3_2nn2fp7Profile",
        [](ppc::Context& c, uint8_t* b) { zero(b, arg(c, 0), 4), ret(c, kFpOk); });
    // MyPresence (0xB0): GameMode (0x2C), region, language, platform, pad, then a 0x80-byte message
    reg("nn_fp", "GetMyPresence__Q2_2nn2fpFPQ3_2nn2fp10MyPresence", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        if (out) {
            std::lock_guard g(g_fp_m);
            zero(b, out, 0xB0);
            std::memcpy(b + out, g_game_mode.data(), g_game_mode.size());
            std::memcpy(b + out + 0x30, g_presence_text.data(), g_presence_text.size());
        }
        ret(c, kFpOk);
    });
    reg("nn_fp", "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw", [](ppc::Context& c, uint8_t* b) {
        std::lock_guard g(g_fp_m);
        if (arg(c, 0)) std::memcpy(g_game_mode.data(), b + arg(c, 0), g_game_mode.size());
        g_presence_text.fill(0);
        if (arg(c, 1))
            for (uint32_t i = 0; i + 2 < g_presence_text.size(); i += 2) {
                const uint16_t ch = ppc::ld16(b, arg(c, 1) + i);
                if (!ch) break;
                ppc::st16(g_presence_text.data(), i, ch);
            }
        ret(c, kFpOk);
    });
    // ResultToErrorCode(nn::Result): the Result arrives by pointer
    reg("nn_fp", "ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result", [](ppc::Context& c, uint8_t* b) {
        ret(c, arg(c, 0) && (ppc::ld32(b, arg(c, 0)) & 0x80000000u) ? 1029999 : 0);
    });
}

#ifdef _WIN32

// ---- nn_act: the network account ---------------------------------------------------------------------------------

void register_act() {
    reg("nn_act", "GetPrincipalId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, g_pid); });
    reg("nn_act", "GetPrincipalIdEx__Q2_2nn3actFPUiUc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), g_pid);
        ret(c, 0);
    });
    reg("nn_act", "GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memcpy(b + arg(c, 0), g_account.c_str(), g_account.size() + 1);
        ret(c, 0);
    });
    // a friends (NFS) account: without one, games call nn_fp RegisterAccountAsync (Cemu's nn_act)
    reg("nn_act", "HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("nn_act", "GetNfsPasswordEx__Q2_2nn3actFPcUc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 0;
        ret(c, 0);
    });
    reg("nn_act", "GetHostServerSettings__Q2_2nn3actFP11ACTNnasTypeP10ACTNfsTypePUcUc", [](ppc::Context& c, uint8_t* b) {
        zero(b, arg(c, 0), 4);
        zero(b, arg(c, 1), 4);
        zero(b, arg(c, 2), 1);
        ret(c, 0);
    });
    reg("nn_act", "GetMii__Q2_2nn3actFP12FFLStoreData", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memcpy(b + arg(c, 0), kDefaultMii, sizeof kDefaultMii);
        ret(c, 0);
    });
    // ACTNexAuthenticationResult (0x25C): token[0x201], pad, nexPassword[0x41] @0x204, pad, host[0x10] @0x248,
    // u16 port @0x258. The revival server takes any token; the password is its NEX password (kerberos seed).
    reg("nn_act", "AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        if (out) {
            zero(b, out, 0x25C);
            std::memcpy(b + out, "mh3u_revival_token", sizeof "mh3u_revival_token");
            std::memcpy(b + out + 0x204, "mh3u_revival_pw", sizeof "mh3u_revival_pw");
            std::memcpy(b + out + 0x248, g_server.ip.c_str(), std::min<size_t>(g_server.ip.size(), 15));
            ppc::st16(b, out + 0x258, g_server.port);
        }
        if (g_log) std::printf("online: NEX token for server id %08X -> %s:%u\n", arg(c, 1), g_server.ip.c_str(), g_server.port);
        ret(c, 0);
    });
}

// ---- nn_ac: connected --------------------------------------------------------------------------------------------

constexpr uint32_t kAcOk = 0x00D00000;  // nn::Result: success, module nn_ac (13)
uint32_t g_local_ip = 0;              // this PC's IPv4 on the route to the server (host order)

uint32_t local_ip_towards(const std::string& ip, uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0x7F000001;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &to.sin_addr);
    uint32_t local = 0x7F000001;
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {  // UDP: picks a route, sends nothing
        sockaddr_in me{};
        int len = sizeof me;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) local = ntohl(me.sin_addr.s_addr);
    }
    ::closesocket(s);
    return local;
}

void register_ac() {
    reg("nn_ac", "Connect__Q2_2nn2acFv", [](ppc::Context& c, uint8_t*) { ret(c, 0); });  // 0, as Cemu (games test == 0)
    reg("nn_ac", "IsApplicationConnected__Q2_2nn2acFPb", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 1;
        ret(c, kAcOk);
    });
    reg("nn_ac", "GetLastErrorCode__Q2_2nn2acFPUi", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0);
        ret(c, kAcOk);
    });
    reg("nn_ac", "GetAssignedAddress__Q2_2nn2acFPUl", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), g_local_ip);
        ret(c, kAcOk);
    });
    reg("nn_ac", "GetAssignedSubnet__Q2_2nn2acFPUl", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0xFFFFFF00);
        ret(c, kAcOk);
    });
    reg("nn_ac", "GetMacAddress__Q2_2nn2acFPQ3_2nn2ac10MacAddress", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) {
            const uint8_t mac[6] = {0x02, 0x4D, 0x48, 0x33, static_cast<uint8_t>(g_pid >> 8), static_cast<uint8_t>(g_pid)};
            std::memcpy(b + arg(c, 0), mac, 6);
        }
        ret(c, kAcOk);
    });
}

// ---- nsysnet: sockets --------------------------------------------------------------------------------------------
// Handles 1..31 (select's fd_set is one 32-bit mask, bit n = handle n). Guest threads are host threads, so a blocking
// call simply blocks, as it does on the console. Errors: the console's socketlasterr numbers, per thread.

constexpr int kMaxSockets = 31;
constexpr uint32_t kSolSocket = 0xFFFFFFFF;  // SOL_SOCKET on the console (-1)
constexpr uint32_t kMsgPeek = 0x02, kMsgDontWait = 0x20;
enum : int32_t {
    kErrOk = 0, kErrWouldBlock = 6, kErrConnReset = 8, kErrNotConn = 9, kErrInval = 11, kErrShutdown = 15,
    kErrInProgress = 22, kErrAfNoSupport = 33, kErrNetDown = 50, kErrUnknown = 99999,
};

struct Sock {
    SOCKET s = INVALID_SOCKET;
    bool nonblocking = false;
    bool shut_rd = false;  // shutdown(SHUT_RD or SHUT_RDWR): receives return 0, blocked ones included (see sock_recv)
};
std::mutex g_sock_m;
std::array<Sock, kMaxSockets + 1> g_socks;  // [1..31]
thread_local int32_t t_err = kErrOk;

int32_t fail(int32_t err) {
    t_err = err;
    return -1;
}

int32_t wsa_error(bool connecting = false) {
    const int e = WSAGetLastError();
    switch (e) {
        case 0: return kErrOk;
        case WSAEWOULDBLOCK: return connecting ? kErrInProgress : kErrWouldBlock;
        case WSAEINPROGRESS: case WSAEALREADY: return kErrInProgress;
        case WSAECONNRESET: case WSAECONNABORTED: return kErrConnReset;
        case WSAENOTCONN: return kErrNotConn;
        case WSAESHUTDOWN: return kErrShutdown;
        case WSAEAFNOSUPPORT: return kErrAfNoSupport;
        case WSAEINVAL: case WSAENOTSOCK: case WSAEFAULT: return kErrInval;
        case WSAENETDOWN: case WSAENETUNREACH: case WSAEHOSTUNREACH: return kErrNetDown;
        default:
            if (g_log) std::printf("online: socket error %d\n", e);
            return kErrUnknown;
    }
}

SOCKET host_socket(uint32_t h, bool* nonblocking = nullptr, bool* shut_rd = nullptr) {
    std::lock_guard g(g_sock_m);
    if (h < 1 || h > kMaxSockets) return INVALID_SOCKET;
    if (nonblocking) *nonblocking = g_socks[h].nonblocking;
    if (shut_rd) *shut_rd = g_socks[h].shut_rd;
    return g_socks[h].s;
}

// A blocking receive waits here, in slices, so that a shutdown or close from another thread ends it. On the console
// (a BSD stack) shutdown wakes a thread blocked in recv, which then returns 0; on Windows the thread stays blocked.
// NEX leaving a room shuts its socket down and waits for its receive threads: without this it waits forever, the
// server drops the client and the game sits on "You have been disconnected".
enum class Wait { Readable, Shut, Closed, Error };
Wait wait_readable(uint32_t h, SOCKET s) {
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{0, 50000};
        const int r = ::select(0, &set, nullptr, nullptr, &tv);
        {
            std::lock_guard g(g_sock_m);
            if (g_socks[h].s != s) return Wait::Closed;
            if (g_socks[h].shut_rd) return Wait::Shut;
        }
        if (r > 0) return Wait::Readable;
        if (r < 0) return Wait::Error;
    }
}

// guest sockaddr_in: u16 family (BE), u16 port, u32 address (both already in network order), 8 zero bytes
bool to_host(uint8_t* b, uint32_t addr, sockaddr_in& out) {
    if (!addr) return false;
    out = {};
    out.sin_family = AF_INET;
    std::memcpy(&out.sin_port, b + addr + 2, 2);
    std::memcpy(&out.sin_addr, b + addr + 4, 4);
    return ppc::ld16(b, addr) == 2;
}

void to_guest(uint8_t* b, const sockaddr_in& in, uint32_t addr, uint32_t len_ptr) {
    if (addr) {
        ppc::st16(b, addr, 2);
        std::memcpy(b + addr + 2, &in.sin_port, 2);
        std::memcpy(b + addr + 4, &in.sin_addr, 4);
        std::memset(b + addr + 8, 0, 8);
    }
    if (len_ptr) ppc::st32(b, len_ptr, 16);
}

// MSG_DONTWAIT on a blocking socket: nothing to read now = EWOULDBLOCK
bool would_block(SOCKET s, bool for_write) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{0, 0};
    return ::select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv) == 0;
}

int host_flags(uint32_t flags) { return (flags & kMsgPeek) ? MSG_PEEK : 0; }

void sock_socket(ppc::Context& c, uint8_t*) {
    const uint32_t af = arg(c, 0), type = arg(c, 1), proto = arg(c, 2);
    if (af != 2) return ret(c, static_cast<uint32_t>(fail(kErrAfNoSupport)));
    SOCKET s = ::socket(AF_INET, static_cast<int>(type), static_cast<int>(proto));
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    if (type == 2) {  // UDP: no WSAECONNRESET from an ICMP "port unreachable" on the next receive
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
    }
    std::lock_guard g(g_sock_m);
    for (uint32_t h = 1; h <= kMaxSockets; ++h)
        if (g_socks[h].s == INVALID_SOCKET) {
            g_socks[h] = {s, false, false};
            t_err = kErrOk;
            return ret(c, h);
        }
    ::closesocket(s);
    ret(c, static_cast<uint32_t>(fail(kErrInval)));
}

void sock_close(ppc::Context& c, uint8_t*) {
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard g(g_sock_m);
        const uint32_t h = arg(c, 0);
        if (h >= 1 && h <= kMaxSockets) std::swap(s, g_socks[h].s);
    }
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    ::closesocket(s);
    t_err = kErrOk;
    ret(c, 0);
}

// shutdown(s, how): SHUT_RD 0, SHUT_WR 1, SHUT_RDWR 2, the same numbers as Winsock's SD_*
void sock_shutdown(ppc::Context& c, uint8_t*) {
    const uint32_t h = arg(c, 0), how = arg(c, 1);
    const SOCKET s = host_socket(h);
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    if (how == 0 || how == 2) {
        std::lock_guard g(g_sock_m);
        g_socks[h].shut_rd = true;  // a receive blocked in wait_readable sees it within 50 ms
    }
    // a UDP socket that was never connected answers WSAENOTCONN on the PC; BSD shuts it down all the same
    if (::shutdown(s, static_cast<int>(how)) != 0 && WSAGetLastError() != WSAENOTCONN)
        return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    t_err = kErrOk;
    ret(c, 0);
}

void sock_bind(ppc::Context& c, uint8_t* b) {
    const SOCKET s = host_socket(arg(c, 0));
    sockaddr_in a;
    if (s == INVALID_SOCKET || !to_host(b, arg(c, 1), a)) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    ret(c, ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 ? (t_err = kErrOk, 0u)
                                                                     : static_cast<uint32_t>(fail(wsa_error())));
}

void sock_connect(ppc::Context& c, uint8_t* b) {
    const SOCKET s = host_socket(arg(c, 0));
    sockaddr_in a;
    if (s == INVALID_SOCKET || !to_host(b, arg(c, 1), a)) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    ret(c, ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 ? (t_err = kErrOk, 0u)
                                                                        : static_cast<uint32_t>(fail(wsa_error(true))));
}

void sock_getsockname(ppc::Context& c, uint8_t* b) {
    const SOCKET s = host_socket(arg(c, 0));
    sockaddr_in a{};
    int len = sizeof a;
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    if (a.sin_addr.s_addr == 0 && g_local_ip) a.sin_addr.s_addr = htonl(g_local_ip);  // bound to any: the console reports its own
    to_guest(b, a, arg(c, 1), arg(c, 2));
    t_err = kErrOk;
    ret(c, 0);
}

// setsockopt(s, level, name, const void* value, int length); values are big-endian ints
void sock_setsockopt(ppc::Context& c, uint8_t* b) {
    const uint32_t h = arg(c, 0), level = arg(c, 1), name = arg(c, 2), val = arg(c, 3), len = arg(c, 4);
    const SOCKET s = host_socket(h);
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    const int v = (val && len >= 4) ? static_cast<int>(ppc::ld32(b, val)) : 0;
    int host_level = -1, host_name = 0;
    if (level == kSolSocket) {
        if (name == 0x1014 || name == 0x1015 || name == 0x1016) {  // SO_NBIO, SO_BIO, SO_NONBLOCK
            const bool nb = name == 0x1014 || (name == 0x1016 && v != 0);
            u_long mode = nb ? 1 : 0;
            ::ioctlsocket(s, FIONBIO, &mode);
            std::lock_guard g(g_sock_m);
            g_socks[h].nonblocking = nb;
            t_err = kErrOk;
            return ret(c, 0);
        }
        host_level = SOL_SOCKET;
        switch (name) {
            case 0x0004: host_name = SO_REUSEADDR; break;
            case 0x0020: host_name = SO_BROADCAST; break;
            case 0x1001: host_name = SO_SNDBUF; break;
            case 0x1002: host_name = SO_RCVBUF; break;
            default: host_level = -1; break;
        }
    } else if (level == 0 && (name == 3 || name == 4)) {  // IP_TOS, IP_TTL: same numbers
        host_level = IPPROTO_IP;
        host_name = static_cast<int>(name);
    } else if (level == 6 && name == 0x2004) {  // TCP_NODELAY
        host_level = IPPROTO_TCP;
        host_name = TCP_NODELAY;
    }
    if (host_level >= 0) ::setsockopt(s, host_level, host_name, reinterpret_cast<const char*>(&v), sizeof v);
    t_err = kErrOk;  // options the PC has no use for are accepted
    ret(c, 0);
}

void sock_send(ppc::Context& c, uint8_t* b, bool to) {
    bool nonblocking = false;
    const SOCKET s = host_socket(arg(c, 0), &nonblocking);
    const uint32_t buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    if ((flags & kMsgDontWait) && !nonblocking && would_block(s, true)) return ret(c, static_cast<uint32_t>(fail(kErrWouldBlock)));
    int n;
    if (to) {
        sockaddr_in a;
        if (!to_host(b, arg(c, 4), a)) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
        n = ::sendto(s, reinterpret_cast<const char*>(b + buf), static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        n = ::send(s, reinterpret_cast<const char*>(b + buf), static_cast<int>(len), 0);
    }
    if (n < 0) return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    t_err = kErrOk;
    ret(c, static_cast<uint32_t>(n));
}

void sock_recv(ppc::Context& c, uint8_t* b, bool from) {
    bool nonblocking = false, shut = false;
    const uint32_t h = arg(c, 0);
    const SOCKET s = host_socket(h, &nonblocking, &shut);
    const uint32_t buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail(kErrInval)));
    if (shut) return t_err = kErrOk, ret(c, 0);  // BSD: end of data
    if (nonblocking || (flags & kMsgDontWait)) {
        if (!nonblocking && would_block(s, false)) return ret(c, static_cast<uint32_t>(fail(kErrWouldBlock)));
    } else {
        switch (wait_readable(h, s)) {
            case Wait::Readable: break;
            case Wait::Shut: return t_err = kErrOk, ret(c, 0);
            case Wait::Closed: return ret(c, static_cast<uint32_t>(fail(kErrInval)));
            case Wait::Error: return ret(c, static_cast<uint32_t>(fail(wsa_error())));
        }
    }
    sockaddr_in a{};
    int alen = sizeof a;
    int n = from ? ::recvfrom(s, reinterpret_cast<char*>(b + buf), static_cast<int>(len), host_flags(flags),
                              reinterpret_cast<sockaddr*>(&a), &alen)
                 : ::recv(s, reinterpret_cast<char*>(b + buf), static_cast<int>(len), host_flags(flags));
    if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) n = static_cast<int>(len);  // a datagram cut to fit, as on POSIX
    if (n < 0) return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    if (from) to_guest(b, a, arg(c, 4), arg(c, 5));
    t_err = kErrOk;
    ret(c, static_cast<uint32_t>(n));
}

// select(nfds, u32* read, u32* write, u32* except, timeval* timeout): each set is one big-endian mask, bit n = handle n
void sock_select(ppc::Context& c, uint8_t* b) {
    const uint32_t nfds = std::min<uint32_t>(arg(c, 0), kMaxSockets + 1);
    const uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)};
    const uint32_t tv = arg(c, 4);
    uint32_t masks[3] = {};
    for (int i = 0; i < 3; ++i)
        if (sets[i]) masks[i] = ppc::ld32(b, sets[i]);
    if (!tv) {  // as Cemu, whose select MH3U plays with: no timeout returns at once
        for (int i = 0; i < 3; ++i)
            if (sets[i]) ppc::st32(b, sets[i], 0);
        return ret(c, 0);
    }
    timeval t{static_cast<long>(ppc::ld32(b, tv)), static_cast<long>(ppc::ld32(b, tv + 4))};
    fd_set host[3];
    SOCKET handles[kMaxSockets + 1] = {};
    bool any = false;
    {
        std::lock_guard g(g_sock_m);
        for (uint32_t h = 1; h < nfds; ++h) handles[h] = g_socks[h].s;
    }
    for (int i = 0; i < 3; ++i) {
        FD_ZERO(&host[i]);
        for (uint32_t h = 1; h < nfds; ++h)
            if ((masks[i] >> h) & 1 && handles[h] != INVALID_SOCKET) {
                FD_SET(handles[h], &host[i]);
                any = true;
            }
    }
    if (!any) {  // nothing to wait on: just the timeout
        std::this_thread::sleep_for(std::chrono::seconds(t.tv_sec) + std::chrono::microseconds(t.tv_usec));
        for (int i = 0; i < 3; ++i)
            if (sets[i]) ppc::st32(b, sets[i], 0);
        t_err = kErrOk;
        return ret(c, 0);
    }
    const int n = ::select(0, sets[0] ? &host[0] : nullptr, sets[1] ? &host[1] : nullptr, sets[2] ? &host[2] : nullptr, &t);
    if (n < 0) {
        for (int i = 0; i < 3; ++i)
            if (sets[i]) ppc::st32(b, sets[i], 0);
        return ret(c, static_cast<uint32_t>(fail(wsa_error())));
    }
    for (int i = 0; i < 3; ++i) {
        if (!sets[i]) continue;
        uint32_t out = 0;
        for (uint32_t h = 1; h < nfds; ++h)
            if ((masks[i] >> h) & 1 && handles[h] != INVALID_SOCKET && FD_ISSET(handles[h], &host[i])) out |= 1u << h;
        ppc::st32(b, sets[i], out);
    }
    t_err = kErrOk;
    ret(c, static_cast<uint32_t>(n));
}

// gethostbyname: the NAT-check servers are the revival server; anything else is looked up on the PC
uint32_t g_hostent = 0;  // guest hostent + its strings (one result at a time, as on the console)
void sock_gethostbyname(ppc::Context& c, uint8_t* b) {
    std::string name = guest_cstr(arg(c, 0));
    std::string lookup = name;
    if (name.rfind("nncs", 0) == 0 && name.find("nintendo") != std::string::npos) lookup = g_server.ip;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (::getaddrinfo(lookup.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (g_log) std::printf("online: gethostbyname(%s) failed\n", name.c_str());
        return ret(c, 0);
    }
    const in_addr ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    if (g_log) {
        char text[16];
        inet_ntop(AF_INET, &ip, text, sizeof text);
        std::printf("online: gethostbyname(%s) -> %s\n", name.c_str(), text);
    }
    // hostent {char* name; char** aliases; int addrtype; int length; char** addr_list} @0, aliases @0x14 (NULL),
    // addr_list @0x18 (address, NULL), address @0x20, name @0x24
    const uint32_t h = g_hostent;
    zero(b, h, 0x24);
    name.resize(std::min<size_t>(name.size(), 0xDB));
    std::memcpy(b + h + 0x24, name.c_str(), name.size() + 1);
    ppc::st32(b, h + 0x00, h + 0x24);
    ppc::st32(b, h + 0x04, h + 0x14);
    ppc::st32(b, h + 0x08, 2);
    ppc::st32(b, h + 0x0C, 4);
    ppc::st32(b, h + 0x10, h + 0x18);
    ppc::st32(b, h + 0x18, h + 0x20);
    std::memcpy(b + h + 0x20, &ip, 4);
    ret(c, h);
}

uint32_t g_ntoa = 0;
void register_sockets() {
    reg("nsysnet", "socket_lib_init", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    reg("nsysnet", "socket", sock_socket);
    reg("nsysnet", "socketclose", sock_close);
    reg("nsysnet", "shutdown", sock_shutdown);
    reg("nsysnet", "bind", sock_bind);
    reg("nsysnet", "connect", sock_connect);
    reg("nsysnet", "getsockname", sock_getsockname);
    reg("nsysnet", "setsockopt", sock_setsockopt);
    reg("nsysnet", "send", [](ppc::Context& c, uint8_t* b) { sock_send(c, b, false); });
    reg("nsysnet", "sendto", [](ppc::Context& c, uint8_t* b) { sock_send(c, b, true); });
    reg("nsysnet", "recv", [](ppc::Context& c, uint8_t* b) { sock_recv(c, b, false); });
    reg("nsysnet", "recvfrom", [](ppc::Context& c, uint8_t* b) { sock_recv(c, b, true); });
    reg("nsysnet", "select", sock_select);
    reg("nsysnet", "socketlasterr", [](ppc::Context& c, uint8_t*) { ret(c, static_cast<uint32_t>(t_err)); });
    reg("nsysnet", "gethostbyname", sock_gethostbyname);
    // inet_ntoa(struct in_addr): the struct arrives by pointer (GHS passes structs by reference)
    reg("nsysnet", "inet_ntoa", [](ppc::Context& c, uint8_t* b) {
        const uint32_t a = arg(c, 0) ? ppc::ld32(b, arg(c, 0)) : 0;
        std::snprintf(reinterpret_cast<char*>(b + g_ntoa), 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
        ret(c, g_ntoa);
    });
    // int inet_pton(int af, const char* text, void* out)
    reg("nsysnet", "inet_pton", [](ppc::Context& c, uint8_t* b) {
        in_addr a{};
        if (arg(c, 0) != 2 || !arg(c, 2) || ::inet_pton(AF_INET, guest_cstr(arg(c, 1)).c_str(), &a) != 1) return ret(c, 0);
        std::memcpy(b + arg(c, 2), &a, 4);
        ret(c, 1);
    });
}

#endif  // _WIN32

}  // namespace

void register_online(Runtime& r) {
    char* v = nullptr;
    size_t len = 0;
    g_log = _dupenv_s(&v, &len, "MH3U_NET_LOG") == 0 && v && std::string(v) == "1";
    std::free(v);
    g_cb_stack_top = r.mem->alloc_hle(0x10000, 16) + 0x10000;
    g_cb_results = r.mem->alloc_hle(16 * 4, 4);
    g_online = !r.data_dir.empty() && read_server(r.data_dir / "mh3u_server.txt", g_server);
#ifndef _WIN32
    g_online = false;  // sockets: Windows only so far
#endif
    register_fp();
    if (!g_online) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (::getaddrinfo(g_server.host.c_str(), nullptr, &hints, &res) == 0 && res) {
        char text[16];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, text, sizeof text);
        g_server.ip = text;
        ::freeaddrinfo(res);
    } else {
        g_server.ip = g_server.host.substr(0, 15);
        std::printf("online: can't look up %s\n", g_server.host.c_str());
    }
    load_identity(r.data_dir / "mh3u_account.txt");
    g_local_ip = local_ip_towards(g_server.ip, g_server.port);
    g_hostent = r.mem->alloc_hle(0x100, 16);
    g_ntoa = r.mem->alloc_hle(16, 4);
    register_act();
    register_ac();
    register_sockets();
    std::printf("online: MH3U Revival server %s (%s:%u), principal id %08X\n", g_server.host.c_str(), g_server.ip.c_str(),
                g_server.port, g_pid);
#endif
}

}  // namespace mh3u::os
