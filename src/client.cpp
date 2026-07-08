#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <csignal>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <ifaddrs.h>

namespace fs = std::filesystem;

static volatile sig_atomic_t g_running = 1;
extern "C" void handleSig(int) {
    g_running = 0;
}

// shared runtime state (thread-safe)
static std::mutex       g_logMtx;
static std::atomic<int> g_downloaded{0};
static std::atomic<int> g_pinFails{0};
static std::atomic<int> g_reqCount{0};
static std::mutex       g_logFileMtx;
static std::ofstream    g_logFile;

// ─── color ─────────────────────────────────────────────────────

static bool g_color = false; // enabled when stdout is a TTY

static const char* c_reset() { return g_color ? "\033[0m"  : ""; }
static const char* c_dim()   { return g_color ? "\033[2m"  : ""; }
static const char* c_bold()  { return g_color ? "\033[1m"  : ""; }
static const char* c_grn()   { return g_color ? "\033[32m" : ""; }
static const char* c_yel()   { return g_color ? "\033[33m" : ""; }
static const char* c_cyn()   { return g_color ? "\033[36m" : ""; }

// ─── config ────────────────────────────────────────────────────

struct Config {
    std::string path;           // file or directory to share
    bool serve = false;
    bool once  = false;
    bool lock  = false;
    bool hide  = false;
    bool upload = false;        // allow receiving files (POST /upload)
    int  pin   = -1;            // generated PIN
    int  port  = 8080;          // public HTTP port
    std::string host;           // relay host (empty = standalone)
    int  relayPort = 9000;      // relay control port
    bool help = false;
    bool version = false;
    bool install = false;       // self-install mode
    int  totalFiles = 0;        // total files in directory (once mode)
    int  expire = 0;            // max lifetime seconds (0 = forever)
    int  idle   = 0;            // idle timeout seconds (0 = none)
    std::string logFile;        // access log path
    std::string configFile;     // config file path
    std::string token;          // relay auth token
    std::string tlsCert;        // TLS cert (optional)
    std::string tlsKey;         // TLS key (optional)
    time_t startedAt = 0;
};

// ─── network helpers ───────────────────────────────────────────

void sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += n;
    }
}

void sendStr(int fd, const std::string& s) {
    sendAll(fd, s.c_str(), s.size());
}

std::string recvUntil(int fd, const std::string& delim) {
    std::string buf;
    buf.reserve(4096);
    char tmp[4096];
    int idle = 0;
    while (g_running) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) {
            if (!g_running) return buf;
            if (++idle > 30) return buf;
            continue;
        }
        idle = 0;
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return buf;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 65536) return buf;
        if (buf.find(delim) != std::string::npos) return buf;
    }
    return buf;
}

int connectTo(const char* host, int port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host, ps.c_str(), &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int ok = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ok < 0) { close(fd); return -1; }
    return fd;
}

int listenOn(int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // accept both IPv4 and IPv6 on the same socket (0 = dual-stack)
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 16) < 0) { close(fd); return -1; }
    return fd;
}

// ─── utilities ─────────────────────────────────────────────────

std::string formatSize(uint64_t bytes) {
    static const char* u[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    char buf[32];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

std::string htmlEscape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 16);
    for (char c : raw) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi || lo || s[i+1] != '0') {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += '%';
            }
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static const std::string SNO = "\xe2\x9d\x84"; // \u2744 (terminal, for non-UI)
static const std::string VERSION = "1.0.0";

#include "qrcodegen.hpp"
std::string qrSvg(const std::string& text, int quietZone = 4) {
    using namespace qrcodegen;
    try {
        QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::LOW);
        int sz = qr.getSize();
        int scale = 4;
        int dim = sz + quietZone * 2;
        std::ostringstream os;
        os << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " << (dim*scale) << " " << (dim*scale)
           << "' shape-rendering='crispEdges' width='" << (dim*scale) << "' height='" << (dim*scale) << "'>";
        os << "<rect width='100%' height='100%' fill='#ffffff'/>";
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                if (qr.getModule(x, y)) {
                    int px = (x + quietZone) * scale;
                    int py = (y + quietZone) * scale;
                    os << "<rect x='" << px << "' y='" << py << "' width='" << scale << "' height='" << scale << "' fill='#000000'/>";
                }
            }
        }
        os << "</svg>";
        return os.str();
    } catch (...) {
        return "";
    }
}

static const std::string ICON_DOC = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><path d='M4 4a2 2 0 0 1 2-2h8l6 6v12a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4z'/><path d='M14 2v6h6'/></svg>";
static const std::string ICON_IMG = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='8.5' cy='8.5' r='1.5'/><path d='M21 15l-5-5L5 21'/></svg>";
static const std::string ICON_VID = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='2' y='6' width='15' height='12' rx='2'/><path d='M17 10l4-2.5v9L17 14'/></svg>";
static const std::string ICON_AUD = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><path d='M9 18V5l12-2v13'/><circle cx='6' cy='18' r='3'/><circle cx='18' cy='16' r='3'/></svg>";
static const std::string ICON_ZIP = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='2' y='3' width='20' height='4' rx='1'/><path d='M4 7v11a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7'/><path d='M10 12h4'/></svg>";
static const std::string ICON_BIN = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='4' y='2' width='6' height='6' rx='1'/><rect x='14' y='2' width='6' height='6' rx='1'/><rect x='4' y='16' width='6' height='6' rx='1'/><rect x='14' y='16' width='6' height='6' rx='1'/></svg>";


int generatePin() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return dis(gen);
}

std::string pinStr(int pin) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", pin);
    return buf;
}

std::string getLocalIP() {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) < 0) return "";
    std::string ip;
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto sa = (struct sockaddr_in*)ifa->ifa_addr;
        char addr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, addr, sizeof(addr));
        if (std::string(addr) == "127.0.0.1") continue;
        ip = addr;
        break;
    }
    freeifaddrs(ifaddr);
    return ip;
}

// ─── security / helpers ────────────────────────────────────────

// true if `target` resolves to a path inside `base` (blocks symlink escapes)
bool isWithin(const fs::path& base, const fs::path& target) {
    std::error_code ec;
    fs::path b = fs::weakly_canonical(base, ec);
    if (ec) b = fs::absolute(base);
    fs::path t = fs::weakly_canonical(target, ec);
    if (ec) return false;
    auto rel = fs::relative(t, b, ec);
    if (ec) return false;
    if (rel.empty()) return true;
    return !rel.is_absolute() && rel.string().find("..") == std::string::npos;
}

bool pinBlocked() { return g_pinFails.load() >= 20; }

void accessLog(const std::string& method, const std::string& path, int status) {
    if (!g_logFile.is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::lock_guard<std::mutex> l(g_logFileMtx);
    g_logFile << ts << "  " << method << " " << path << "  " << status << "\n";
    g_logFile.flush();
}

// parse "Range: bytes=START-END" → {ok, start, count}. count=0 means "to end"
bool parseRange(const std::string& hdr, uint64_t fileSize, uint64_t& start, uint64_t& count) {
    if (hdr.rfind("bytes=", 0) != 0) return false;
    std::string r = hdr.substr(6);
    auto dash = r.find('-');
    if (dash == std::string::npos) return false;
    std::string a = r.substr(0, dash);
    std::string b = r.substr(dash + 1);
    uint64_t s = a.empty() ? 0 : std::strtoull(a.c_str(), nullptr, 10);
    uint64_t e = b.empty() ? (fileSize - 1) : std::strtoull(b.c_str(), nullptr, 10);
    if (s > e || s >= fileSize) return false;
    start = s;
    count = (e >= fileSize) ? (fileSize - s) : (e - s + 1);
    return true;
}

// ─── file type icons ───────────────────────────────────────────

std::string fileIcon(const std::string& name) {
    auto ext = name.rfind('.');
    if (ext == std::string::npos) return ICON_DOC;
    std::string e = name.substr(ext + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    if (e=="txt"||e=="md"||e=="js"||e=="ts"||e=="jsx"||e=="tsx"||e=="py"||e=="java"||
        e=="c"||e=="cpp"||e=="h"||e=="hpp"||e=="rs"||e=="go"||e=="rb"||e=="php"||
        e=="css"||e=="scss"||e=="html"||e=="xml"||e=="json"||e=="yaml"||e=="yml"||
        e=="toml"||e=="sh"||e=="bash"||e=="sql"||e=="cfg"||e=="conf"||e=="log"||e=="csv")
        return ICON_DOC;
    if (e=="jpg"||e=="jpeg"||e=="png"||e=="gif"||e=="webp"||e=="bmp"||e=="svg"||e=="ico")
        return ICON_IMG;
    if (e=="mp4"||e=="avi"||e=="mkv"||e=="mov"||e=="webm"||e=="wmv")
        return ICON_VID;
    if (e=="mp3"||e=="wav"||e=="flac"||e=="ogg"||e=="aac"||e=="opus")
        return ICON_AUD;
    if (e=="zip"||e=="tar"||e=="gz"||e=="bz2"||e=="xz"||e=="7z"||e=="rar"||e=="zst")
        return ICON_ZIP;
    if (e=="exe"||e=="dmg"||e=="AppImage"||e=="deb"||e=="rpm"||e=="apk"||e=="bin")
        return ICON_BIN;
    if (e=="pdf"||e=="doc"||e=="docx"||e=="xls"||e=="xlsx"||e=="ppt"||e=="pptx")
        return ICON_DOC;
    return ICON_DOC;
}

// ─── HTML shell ────────────────────────────────────────────────

static const std::string PAGE_TOP = R"RAW(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>snowflake</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0d1117;--fg:#c9d1d9;--border:#30363d;--muted:#8b949e;--accent:#58a6ff;--btn-bg:#21262d;--once-bg:#3a1a1a;--once-fg:#ff7b72;--lock-bg:#1a2a3a;--lock-fg:#58a6ff}
:root.light{--bg:#ffffff;--fg:#24292f;--border:#d0d7de;--muted:#656d76;--accent:#0969da;--btn-bg:#f3f4f6;--once-bg:#ffebe9;--once-fg:#cf222e;--lock-bg:#ddf4ff;--lock-fg:#0969da}
body{background:var(--bg);color:var(--fg);font-family:ui-monospace,'SF Mono','Cascadia Code','Fira Code','Consolas',monospace;display:flex;flex-direction:column;align-items:center;min-height:100vh;padding:2rem}
.hero{text-align:center;margin:3rem 0 2.5rem}
.logo{display:inline-block;font-size:3.5rem;animation:spin 4s linear infinite;user-select:none}
@keyframes spin{100%{transform:rotate(360deg)}}
.badges{margin-top:.75rem;display:flex;gap:.5rem;justify-content:center}
.badge{font-size:.65rem;padding:.15rem .55rem;border-radius:3px;text-transform:uppercase;letter-spacing:.06em}
.badge-once{background:var(--once-bg);color:var(--once-fg);border:1px solid var(--border)}
.badge-lock{background:var(--lock-bg);color:var(--lock-fg);border:1px solid var(--border)}
table{width:100%;border-collapse:collapse;max-width:800px}
th,td{padding:.6rem .7rem;text-align:left;border-bottom:1px solid var(--border)}
th{color:var(--muted);font-weight:400;font-size:.68rem;text-transform:uppercase;letter-spacing:.08em}
td{font-size:.85rem;word-break:break-all}
td.icon{width:2rem;font-size:1.1rem;text-align:center;word-break:normal}
td.name{max-width:50vw}
td.size{color:var(--muted);font-size:.8rem;white-space:nowrap;width:6rem}
td.action{width:7rem;text-align:right;word-break:normal}
.btn{display:inline-block;background:var(--btn-bg);color:var(--accent);padding:.32rem .8rem;border-radius:6px;border:1px solid var(--border);font-size:.75rem;font-family:inherit;cursor:pointer;text-decoration:none}
.btn:hover{border-color:var(--accent)}
.empty{color:var(--muted);font-style:italic;padding:3rem 0;font-size:.88rem;text-align:center}
.foot{margin-top:2rem;padding-top:1rem;border-top:1px solid var(--border);color:var(--muted);font-size:.65rem;opacity:.6;text-align:center}
.pin-wrap{display:flex;align-items:center;justify-content:center;min-height:50vh}
.pin-card{background:var(--btn-bg);border:1px solid var(--border);border-radius:12px;padding:2.5rem 2.8rem;max-width:380px;width:100%;text-align:center}
.pin-card .logo{font-size:2.8rem;margin-bottom:.6rem}
.pin-card h2{font-size:1.2rem;font-weight:600;margin-bottom:.3rem}
.pin-card p{color:var(--muted);font-size:.82rem;margin-bottom:1.5rem}
.pin-input{background:var(--bg);border:1px solid var(--border);color:var(--fg);padding:.7rem;font-size:1.6rem;letter-spacing:.6em;text-align:center;width:100%;border-radius:6px;margin-bottom:1rem;font-family:inherit;outline:none}
.pin-input:focus{border-color:var(--accent)}
.pin-btn{background:var(--accent);color:#fff;border:none;padding:.65rem 0;width:100%;border-radius:6px;cursor:pointer;font-size:.9rem;font-family:inherit;font-weight:500}
.pin-btn:hover{opacity:.9}
.pin-err{color:#f85149;font-size:.78rem;margin-top:.75rem}
@media(max-width:900px){body{padding:1.5rem}.logo{font-size:3rem}.hero{margin:2.5rem 0 2rem}td.name{max-width:40vw}td.size{width:5rem}}
@media(max-width:600px){body{padding:1rem}.logo{font-size:2.5rem}.hero{margin:2rem 0 1.5rem}th,td{padding:.5rem .5rem}td.name{max-width:35vw;font-size:.8rem}td.size{width:4rem;font-size:.72rem}td.action{width:5.5rem}.btn{font-size:.72rem;padding:.25rem .65rem}.pin-card{padding:2rem 1.5rem}.pin-card .logo{font-size:2.2rem}.pin-input{font-size:1.4rem;padding:.6rem}.pin-btn{padding:.55rem 0}}
@media(max-width:420px){body{padding:.75rem}.logo{font-size:2rem}.hero{margin:1.5rem 0 1rem}table{font-size:.78rem}th,td{padding:.4rem .35rem}td.icon{width:1.5rem;font-size:1rem}td.name{max-width:30vw;font-size:.78rem}td.size{width:3rem;font-size:.65rem}td.action{width:4.5rem}.btn{font-size:.65rem;padding:.2rem .5rem}.pin-card{padding:1.5rem;margin:0 .5rem}.pin-card .logo{font-size:2rem}.pin-card h2{font-size:1.1rem}.pin-input{font-size:1.3rem;padding:.5rem;letter-spacing:.4em}.pin-btn{padding:.5rem 0;font-size:.82rem}}
.upload{margin:1.8rem 0;max-width:800px;width:100%;text-align:center}
.upload label{display:inline-block;background:var(--btn-bg);border:1px solid var(--border);color:var(--accent);padding:.55rem 1.1rem;border-radius:6px;cursor:pointer;font-size:.8rem;font-family:inherit}
.upload label:hover{border-color:var(--accent)}
.upload input[type=file]{display:none}
.upload .hint{display:block;margin-top:.6rem;color:var(--muted);font-size:.7rem}
.qr{margin-top:1.6rem;display:flex;flex-direction:column;align-items:center;gap:.4rem}
.qr svg{background:#fff;padding:10px;border-radius:10px;width:160px;height:160px}
.qr .qr-cap{color:var(--muted);font-size:.7rem}
.bar{display:none;max-width:800px;width:100%;height:6px;background:var(--btn-bg);border-radius:4px;margin:1.2rem 0;overflow:hidden}
.fill{height:100%;width:0;background:var(--accent);transition:width .12s linear}
</style>
</head>
<body>
<button class="theme-toggle" onclick="var r=document.documentElement;r.classList.toggle('light');localStorage.setItem('t',r.classList.contains('light')?'l':'d')"><svg class="sun" viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg><svg class="moon" viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg></button>
<style>
.theme-toggle{position:fixed;top:1rem;right:1rem;background:var(--btn-bg);border:1px solid var(--border);border-radius:8px;width:2.2rem;height:2.2rem;cursor:pointer;display:flex;align-items:center;justify-content:center;z-index:10;color:var(--fg)}
.theme-toggle .sun{display:block}
.theme-toggle .moon{display:none}
:root.light .theme-toggle .sun{display:none}
:root.light .theme-toggle .moon{display:block}
@media(max-width:600px){.theme-toggle{top:.75rem;right:.75rem;width:2rem;height:2rem}.theme-toggle svg{width:16px;height:16px}}
@media(max-width:420px){.theme-toggle{top:.5rem;right:.5rem;width:1.8rem;height:1.8rem}.theme-toggle svg{width:14px;height:14px}}
</style>
)RAW";

static const std::string PAGE_BOTTOM = R"RAW(
<p class="foot">snowflake</p>
<script>(function(){var r=document.documentElement;if(localStorage.getItem('t')=='l')r.classList.add('light')})()</script>
<script>
function dl(btn){var url=btn.href;var bar=document.getElementById('bar');var fill=document.getElementById('fill');if(bar)bar.style.display='block';
fetch(url).then(function(r){var total=+r.headers.get('Content-Length');var reader=r.body.getReader();var rec=0;var chunks=[];
function pump(){return reader.read().then(function(res){if(res.done){var blob=new Blob(chunks);var a=document.createElement('a');a.href=URL.createObjectURL(blob);var nm=url.split('/').pop().split('?')[0];a.download=decodeURIComponent(nm);a.click();if(bar)bar.style.display='none';if(fill)fill.style.width='0%';return;}chunks.push(res.value);rec+=res.value.length;if(total&&fill)fill.style.width=(100*rec/total).toFixed(1)+'%';return pump();});}
return pump();});return false;}
function up(input){var f=input.files[0];if(!f)return;var url='/upload?name='+encodeURIComponent(f.name);var bar=document.getElementById('bar');var fill=document.getElementById('fill');if(bar)bar.style.display='block';
fetch(url,{method:'POST',body:f}).then(function(r){if(bar)bar.style.display='none';if(fill)fill.style.width='0%';if(r.ok){location.reload();}else{alert('upload failed: '+r.status);}});}
</script>
</body>
</html>
)RAW";

std::string heroSection(bool onceMode, bool lockMode) {
    std::string h = "<div class=\"hero\"><span class=\"logo\">" + SNO + "</span>";
    if (onceMode || lockMode) {
        h += "<div class=\"badges\">";
        if (onceMode) h += "<span class=\"badge badge-once\">once</span>";
        if (lockMode) h += "<span class=\"badge badge-lock\">lock</span>";
        h += "</div>";
    }
    h += "</div>\n";
    return h;
}

// ─── HTML generation ───────────────────────────────────────────

std::string generatePinPage(int correctPin, const std::string& query) {
    std::string error;
    if (!query.empty()) {
        auto pos = query.find("pin=");
        if (pos != std::string::npos) {
            int given = std::atoi(query.c_str() + pos + 4);
            if (given != correctPin) error = "Incorrect PIN. Try again.";
        }
    }
    std::string p;
    p += PAGE_TOP;
    p += "<div class=\"pin-wrap\"><div class=\"pin-card\">";
    p += "<div class=\"hero\"><span class=\"logo\">" + SNO + "</span></div>";
    p += "<h2>snowflake</h2><p>Enter PIN to access shared files</p>";
    p += "<form method=\"get\" action=\"/\">";
    p += "<input class=\"pin-input\" type=\"text\" name=\"pin\" inputmode=\"numeric\" pattern=\"[0-9]{4}\" maxlength=\"4\" placeholder=\"\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b\" autofocus>";
    p += "<button class=\"pin-btn\" type=\"submit\">Unlock</button></form>";
    if (!error.empty()) p += "<p class=\"pin-err\">" + error + "</p>";
    p += "</div></div>" + PAGE_BOTTOM;
    return p;
}

std::string fileRows(const fs::path& dir, const std::string& pinQuery) {
    std::error_code ec;
    std::string rows;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto name = e.path().filename().string();
        rows += "<tr><td class=\"icon\">" + fileIcon(name) + "</td>";
        rows += "<td class=\"name\">" + htmlEscape(name) + "</td>";
        rows += "<td class=\"size\">" + formatSize(e.file_size(ec)) + "</td>";
        rows += "<td class=\"action\"><a class=\"btn\" href=\"/download/" + urlEncode(name) + pinQuery + "\" onclick=\"return dl(this)\">Get File</a></td></tr>\n";
    }
    return rows;
}

std::string uploadSection() {
    return "<div class=\"upload\"><label for=\"up\">⬆ Upload a file</label>"
           "<input id=\"up\" type=\"file\" onchange=\"up(this)\">"
           "<span class=\"hint\">sent to this share via POST /upload</span></div>";
}

std::string qrSection(const std::string& url) {
    if (url.empty()) return "";
    std::string svg = qrSvg(url);
    if (svg.empty()) return "";
    return "<div class=\"qr\">" + svg + "<span class=\"qr-cap\">scan to open</span></div>";
}

std::string generateFileListHTML(const fs::path& dir, bool onceMode, bool lockMode,
                                 const std::string& pinQuery, bool allowUpload,
                                 const std::string& shareURL) {
    std::string p;
    p += PAGE_TOP;
    p += heroSection(onceMode, lockMode);
    std::error_code ec;
    bool hasFiles = false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) { hasFiles = true; break; }
    }
    if (!hasFiles && !allowUpload) {
        p += "<p class=\"empty\">no files in this directory</p>";
    } else {
        p += "<table><thead><tr><th></th><th>name</th><th>size</th><th></th></tr></thead><tbody>\n";
        p += fileRows(dir, pinQuery);
        p += "</tbody></table>\n";
    }
    if (allowUpload) p += uploadSection();
    p += qrSection(shareURL);
    p += "<div class=\"bar\" id=\"bar\"><div class=\"fill\" id=\"fill\"></div></div>";
    p += PAGE_BOTTOM;
    return p;
}

std::string generateSingleFileHTML(const fs::path& file, bool onceMode, bool lockMode,
                                   const std::string& pinQuery, bool allowUpload,
                                   const std::string& shareURL) {
    auto name = file.filename().string();
    auto sz = formatSize(fs::file_size(file));
    std::string p;
    p += PAGE_TOP;
    p += heroSection(onceMode, lockMode);
    p += "<table><thead><tr><th></th><th>name</th><th>size</th><th></th></tr></thead><tbody>\n";
    p += "<tr><td class=\"icon\">" + fileIcon(name) + "</td>";
    p += "<td class=\"name\">" + htmlEscape(name) + "</td>";
    p += "<td class=\"size\">" + sz + "</td>";
    p += "<td class=\"action\"><a class=\"btn\" href=\"/download/" + urlEncode(name) + pinQuery + "\" onclick=\"return dl(this)\">Get File</a></td></tr>\n";
    p += "</tbody></table>\n";
    if (allowUpload) p += uploadSection();
    p += qrSection(shareURL);
    p += "<div class=\"bar\" id=\"bar\"><div class=\"fill\" id=\"fill\"></div></div>";
    p += PAGE_BOTTOM;
    return p;
}

// ─── request parsing ───────────────────────────────────────────

struct Request {
    std::string method, path, version, query;
    bool ok = false;
};

Request parseReq(const std::string& raw) {
    Request r;
    auto nl = raw.find("\r\n");
    if (nl == std::string::npos) return r;
    std::string line = raw.substr(0, nl);
    std::istringstream ss(line);
    if (ss >> r.method >> r.path >> r.version) r.ok = true;
    auto qpos = r.path.find('?');
    if (qpos != std::string::npos) {
        r.query = r.path.substr(qpos + 1);
        r.path  = r.path.substr(0, qpos);
    }
    return r;
}

std::string getHeader(const std::string& head, const std::string& name) {
    std::string needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    size_t pos = 0;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        std::string line = head.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            std::string lk = key;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            if (lk == needle) return val;
        }
        if (nl == std::string::npos) break;
        pos = nl + 2;
    }
    return "";
}

// read exactly `len` bytes from fd (after the head has been consumed)
std::string readBody(int fd, size_t len) {
    std::string body;
    body.reserve(len);
    char tmp[4096];
    while (body.size() < len && g_running) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 1000) <= 0) { if (!g_running) break; continue; }
        ssize_t n = recv(fd, tmp, std::min(sizeof(tmp), len - body.size()), 0);
        if (n <= 0) break;
        body.append(tmp, static_cast<size_t>(n));
    }
    return body;
}

// ─── PIN check ─────────────────────────────────────────────────

bool checkPin(const std::string& query, int correctPin) {
    if (query.empty()) return false;
    auto pos = query.find("pin=");
    if (pos == std::string::npos) return false;
    int given = std::atoi(query.c_str() + pos + 4);
    if (given != correctPin) { g_pinFails++; return false; }
    return true;
}

// ─── arg parsing ───────────────────────────────────────────────

Config parseArgs(int argc, char* argv[]) {
    Config c;
    if (argc < 2) { c.help = true; return c; }
    std::string cmd = argv[1];
    if (cmd == "install") {
        c.install = true;
        return c;
    }
    if (cmd == "--version" || cmd == "-v") { c.version = true; return c; }
    if (argc < 3 || cmd != "send") { c.help = true; return c; }
    c.path = argv[2];
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "serve")     c.serve = true;
        else if (a == "once")      c.once  = true;
        else if (a == "lock")      c.lock  = true;
        else if (a == "hide")      c.hide  = true;
        else if (a == "upload")    c.upload = true;
        else if (a == "--host" && i+1 < argc) c.host = argv[++i];
        else if (a == "--port" && i+1 < argc) c.port = std::atoi(argv[++i]);
        else if (a == "--relay-port" && i+1 < argc) c.relayPort = std::atoi(argv[++i]);
        else if (a == "--token" && i+1 < argc) c.token = argv[++i];
        else if (a == "--expire" && i+1 < argc) c.expire = std::atoi(argv[++i]);
        else if (a == "--idle" && i+1 < argc) c.idle = std::atoi(argv[++i]);
        else if (a == "--log" && i+1 < argc) c.logFile = argv[++i];
        else if (a == "--config" && i+1 < argc) c.configFile = argv[++i];
        else if (a == "--tls-cert" && i+1 < argc) c.tlsCert = argv[++i];
        else if (a == "--tls-key" && i+1 < argc) c.tlsKey = argv[++i];
        else if (a == "-h" || a == "--help")  c.help = true;
        else if (a == "-v" || a == "--version") c.version = true;
    }
    if (!c.serve && (c.once || c.lock || c.hide)) c.serve = true;
    return c;
}

void applyExternalConfig(Config& c) {
    // config file: simple key=value, # comments
    if (!c.configFile.empty()) {
        std::ifstream f(c.configFile);
        std::string line;
        while (std::getline(f, line)) {
            line.erase(0, line.find_first_not_of(" \t"));
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            k.erase(k.find_last_not_of(" \t") + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t") + 1);
            if (k == "port"    && c.port == 8080) c.port = std::atoi(v.c_str());
            if (k == "host"    && c.host.empty()) c.host = v;
            if (k == "relay-port" && c.relayPort == 9000) c.relayPort = std::atoi(v.c_str());
            if (k == "token"   && c.token.empty()) c.token = v;
            if (k == "expire"  && c.expire == 0) c.expire = std::atoi(v.c_str());
            if (k == "idle"    && c.idle == 0) c.idle = std::atoi(v.c_str());
            if (k == "log"     && c.logFile.empty()) c.logFile = v;
            if (k == "tls-cert"&& c.tlsCert.empty()) c.tlsCert = v;
            if (k == "tls-key" && c.tlsKey.empty()) c.tlsKey = v;
        }
    }
    // env vars (same precedence as config file)
    auto env = [](const char* n) -> std::string {
        const char* v = std::getenv(n); return v ? v : "";
    };
    if (!env("SNOWFLAKE_PORT").empty()    && c.port == 8080) c.port = std::atoi(env("SNOWFLAKE_PORT").c_str());
    if (!env("SNOWFLAKE_HOST").empty()    && c.host.empty()) c.host = env("SNOWFLAKE_HOST");
    if (!env("SNOWFLAKE_TOKEN").empty()   && c.token.empty()) c.token = env("SNOWFLAKE_TOKEN");
    if (!env("SNOWFLAKE_EXPIRE").empty()  && c.expire == 0) c.expire = std::atoi(env("SNOWFLAKE_EXPIRE").c_str());
    if (!env("SNOWFLAKE_IDLE").empty()    && c.idle == 0) c.idle = std::atoi(env("SNOWFLAKE_IDLE").c_str());
    if (!env("SNOWFLAKE_LOG").empty()     && c.logFile.empty()) c.logFile = env("SNOWFLAKE_LOG");
}

void printHelp(const char* prog) {
    std::cout << SNO << " snowflake \xe2\x80\x94 zero-dependency file sharing\n\n"
              << "Usage: " << prog << " send <PATH> [modifiers] [options]\n\n"
              << "Required:\n"
              << "  send <PATH>   file or directory to share\n\n"
              << "Modifiers (combinable):\n"
              << "  serve    Start HTTP server or connect to relay\n"
              << "  once     Melt after first file download\n"
              << "  lock     PIN-protect the web interface\n"
              << "  hide     Run silently\n\n"
              << "Options:\n"
              << "  --host <addr>  Relay address (omit for standalone server)\n"
              << "  --port <num>   HTTP port (default: 8080)\n"
               << "  -h, --help     This help\n"
               << "  -v, --version  Show version\n\n"
              << "Install:\n"
              << "  install    Copy snowflake to ~/.local/bin/ and add to PATH\n\n"
              << "Examples:\n"
              << "  " << prog << " install                     # install to PATH\n"
              << "  " << prog << " send . serve                 # standalone HTTP server\n"
              << "  " << prog << " send file.mp4 serve once     # single file, melt after dl\n"
              << "  " << prog << " send /docs serve lock hide   # quiet, PIN-protected\n"
              << "  " << prog << " send . serve --host relay.io # relay mode\n";
}

// ─── log macros ────────────────────────────────────────────────

#define LOG(cfg, msg)   do { if (!(cfg).hide) { std::lock_guard<std::mutex> _l(g_logMtx); std::cout << msg; } } while(0)
#define LOGN(cfg, msg)  do { if (!(cfg).hide) { std::lock_guard<std::mutex> _l(g_logMtx); std::cout << msg << std::endl; } } while(0)

// ─── response helpers ──────────────────────────────────────────

void send200(int fd, const std::string& body, const std::string& contentType) {
    sendStr(fd, "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
}

void sendShort(int fd, int code, const std::string& status) {
    sendStr(fd, "HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

std::string escapeFilename(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\r' || c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

void streamFile(int fd, const fs::path& fpath, const std::string& fname,
                 uint64_t off = 0, uint64_t count = 0, bool isRange = false) {
    std::error_code ec;
    auto sz = fs::file_size(fpath, ec);
    std::string status = isRange ? "206 Partial Content" : "200 OK";
    std::string clen   = isRange ? std::to_string(count) : std::to_string(sz);
    std::string rangeHdr = isRange
        ? ("Content-Range: bytes " + std::to_string(off) + "-"
           + std::to_string(off + count - 1) + "/" + std::to_string(sz) + "\r\n")
        : "";
    sendStr(fd, "HTTP/1.1 " + status + "\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"" + escapeFilename(fname) + "\"\r\n"
        + rangeHdr +
        "Content-Length: " + clen + "\r\nConnection: close\r\n\r\n");
    std::ifstream file(fpath, std::ios::binary);
    if (file.is_open()) {
        if (off) file.seekg(static_cast<std::streamoff>(off));
        uint64_t remaining = isRange ? count : sz;
        char buf[4096];
        while (g_running && remaining > 0) {
            uint64_t toRead = std::min<uint64_t>(sizeof(buf), remaining);
            file.read(buf, static_cast<std::streamsize>(toRead));
            std::streamsize got = file.gcount();
            if (got <= 0) break;
            sendAll(fd, buf, static_cast<size_t>(got));
            remaining -= static_cast<uint64_t>(got);
            if (file.eof()) break;
        }
    }
}

// ─── request handler ──────────────────────────────────────────

std::string queryParam(const std::string& query, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = query.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = query.find('&', pos);
    return query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

void handleRequest(int fd, Config& cfg, const fs::path& servePath,
                   bool isSingleFile, const std::string& htmlPage,
                   const std::string& shareURL) {
    auto raw = recvUntil(fd, "\r\n\r\n");
    if (raw.empty()) return;
    auto req = parseReq(raw);
    if (!req.ok) return;

    LOG(cfg, c_dim() << "  ❯ " << c_reset() << c_grn() << req.method << c_reset() << " " << req.path);
    if (!req.query.empty()) LOG(cfg, c_dim() << "?" << req.query << c_reset());
    if (!cfg.hide) std::cout << std::endl;

    bool locked = cfg.lock && !checkPin(req.query, cfg.pin);
    if (cfg.lock && pinBlocked()) { sendShort(fd, 429, "429 Too Many Requests"); accessLog(req.method, req.path, 429); return; }

    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        if (locked) {
            send200(fd, generatePinPage(cfg.pin, req.query), "text/html; charset=utf-8");
            accessLog(req.method, req.path, 200);
        } else {
            std::string page = htmlPage;
            if (cfg.lock) {
                std::string pinQuery = "?pin=" + pinStr(cfg.pin);
                if (isSingleFile) page = generateSingleFileHTML(servePath, cfg.once, cfg.lock, pinQuery, cfg.upload, shareURL);
                else              page = generateFileListHTML(servePath, cfg.once, cfg.lock, pinQuery, cfg.upload, shareURL);
            } else {
                if (isSingleFile) page = generateSingleFileHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
                else              page = generateFileListHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
            }
            send200(fd, page, "text/html; charset=utf-8");
            accessLog(req.method, req.path, 200);
        }
    }
    else if (req.method == "GET" && req.path.rfind("/download/", 0) == 0) {
        std::string fname = urlDecode(req.path.substr(10));

        if (locked) {
            LOGN(cfg, "  [warn] blocked (no PIN)");
            sendShort(fd, 403, "403 Forbidden");
            accessLog(req.method, req.path, 403);
            return;
        }
        if (fname.empty() || fname.find("..") != std::string::npos ||
            fname.find('/') != std::string::npos || fname.find('\\') != std::string::npos) {
            LOGN(cfg, "  [warn] traversal blocked: " << fname);
            sendShort(fd, 403, "403 Forbidden");
            accessLog(req.method, req.path, 403);
            return;
        }

        fs::path fpath = isSingleFile ? servePath : (servePath / fname);
        std::error_code ec;
        if (!fs::is_regular_file(fpath, ec) || !isWithin(servePath, fpath)) {
            LOGN(cfg, "  [warn] not found / blocked: " << fname);
            sendShort(fd, 404, "404 Not Found");
            accessLog(req.method, req.path, 404);
            return;
        }

        // range support (resume)
        uint64_t off = 0, count = 0;
        bool isRange = false;
        std::string rangeHdr = getHeader(raw, "Range");
        if (!rangeHdr.empty()) {
            uint64_t sz = fs::file_size(fpath, ec);
            if (parseRange(rangeHdr, sz, off, count)) isRange = true;
        }

        streamFile(fd, fpath, fname, off, count, isRange);
        accessLog(req.method, req.path, isRange ? 206 : 200);

        if (cfg.once) {
            int prev = g_downloaded.fetch_add(1) + 1;
            int need = isSingleFile ? 1 : cfg.totalFiles;
            if (prev >= need) {
                LOGN(cfg, c_bold() << c_cyn() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset());
                LOGN(cfg, c_yel() << "  once mode: download complete, melting..." << c_reset());
                g_running = 0;
            } else {
                LOGN(cfg, c_dim() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset()
                          << "  " << c_yel() << "once " << prev << "/" << need << c_reset());
            }
        } else {
            LOGN(cfg, c_dim() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset());
        }
    }
    else if (req.method == "POST" && req.path.rfind("/upload", 0) == 0) {
        if (!cfg.upload) {
            sendShort(fd, 403, "403 Forbidden");
            accessLog(req.method, req.path, 403);
            return;
        }
        std::string name = queryParam(req.query, "name");
        if (name.empty() || name.find("..") != std::string::npos ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            sendShort(fd, 400, "400 Bad Request");
            accessLog(req.method, req.path, 400);
            return;
        }
        std::string clenHdr = getHeader(raw, "Content-Length");
        size_t clen = clenHdr.empty() ? 0 : (size_t)std::strtoull(clenHdr.c_str(), nullptr, 10);
        if (clen == 0 || clen > (1024ULL * 1024 * 1024 * 4)) { // cap 4 GB
            sendShort(fd, 413, "413 Payload Too Large");
            accessLog(req.method, req.path, 413);
            return;
        }
        std::string body = readBody(fd, clen);
        fs::path dest = isSingleFile ? servePath.parent_path() / name : (servePath / name);
        std::ofstream out(dest, std::ios::binary);
        out.write(body.data(), (std::streamsize)body.size());
        LOGN(cfg, c_dim() << "  ↑ " << c_reset() << "uploaded " << name << c_dim() << " (" << formatSize(body.size()) << ")" << c_reset());
        sendShort(fd, 200, "200 OK");
        accessLog(req.method, req.path, 200);
    }
    else {
        sendShort(fd, 404, "404 Not Found");
        accessLog(req.method, req.path, 404);
    }
}

// ─── standalone server ─────────────────────────────────────────

int runStandalone(Config& cfg, const fs::path& servePath) {
    bool isFile = fs::is_regular_file(servePath);
    auto localIP = getLocalIP();
    std::string shareURL = !localIP.empty()
        ? ("http://" + localIP + ":" + std::to_string(cfg.port) + "/")
        : ("http://localhost:" + std::to_string(cfg.port) + "/");

    std::string htmlPage;
    if (isFile) htmlPage = generateSingleFileHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
    else        htmlPage = generateFileListHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);

    if (!cfg.logFile.empty()) g_logFile.open(cfg.logFile, std::ios::app);
    g_downloaded.store(0);

    int srv = listenOn(cfg.port);
    if (srv < 0) {
        std::cerr << "  [err] cannot bind port " << cfg.port << "\n";
        return 1;
    }

    LOGN(cfg, c_dim() << "  URLs" << c_reset());
    LOGN(cfg, "    " << c_cyn() << "http://127.0.0.1:" << cfg.port << c_reset());
    LOGN(cfg, "    " << c_cyn() << "http://localhost:" << cfg.port << c_reset());
    if (!localIP.empty()) LOGN(cfg, "    " << c_cyn() << "http://" << localIP << ":" << cfg.port << c_reset());
    if (cfg.upload) LOGN(cfg, c_dim() << "  upload enabled (POST /upload)" << c_reset());
    if (cfg.expire > 0) LOGN(cfg, c_dim() << "  expires in " << cfg.expire << "s" << c_reset());
    if (cfg.idle   > 0) LOGN(cfg, c_dim() << "  idle limit " << cfg.idle << "s" << c_reset());
    if (!cfg.hide) std::cout << std::endl;

    // expire / idle watchdog
    cfg.startedAt = std::time(nullptr);
    std::atomic<time_t> lastActivity{std::time(nullptr)};
    std::thread timer;
    if (cfg.expire > 0 || cfg.idle > 0) {
        timer = std::thread([&]() {
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                time_t now = std::time(nullptr);
                if (cfg.expire > 0 && (now - cfg.startedAt) >= cfg.expire) {
                    LOGN(cfg, c_yel() << "  expired after " << cfg.expire << "s, melting..." << c_reset());
                    g_running = 0; break;
                }
                if (cfg.idle > 0 && (now - lastActivity.load()) >= cfg.idle) {
                    LOGN(cfg, c_yel() << "  idle " << cfg.idle << "s, melting..." << c_reset());
                    g_running = 0; break;
                }
            }
        });
    }

    int flags = fcntl(srv, F_GETFL, 0);
    if (flags >= 0) fcntl(srv, F_SETFL, flags | O_NONBLOCK);

    std::vector<std::thread> workers;
    while (g_running) {
        struct sockaddr_storage cli;
        socklen_t len = sizeof(cli);
        int fd = accept(srv, (struct sockaddr*)&cli, &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(50000); continue; }
            if (errno == EINTR) continue;
            continue;
        }
        lastActivity.store(std::time(nullptr));
        workers.emplace_back(handleRequest, fd, std::ref(cfg),
                             std::ref(servePath), isFile, std::ref(htmlPage),
                             std::ref(shareURL));
    }
    for (auto& t : workers) if (t.joinable()) t.join();
    close(srv);
    if (timer.joinable()) timer.join();
    if (cfg.once && !cfg.hide) std::cout << c_bold() << c_cyn() << "  " << SNO << " melted." << c_reset() << std::endl;
    return 0;
}

// ─── relay mode ────────────────────────────────────────────────

int runRelay(Config& cfg, const fs::path& servePath) {
    bool isFile = fs::is_regular_file(servePath);
    std::string shareURL = "http://" + cfg.host + ":8080/";
    std::string htmlPage;
    if (isFile) htmlPage = generateSingleFileHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
    else        htmlPage = generateFileListHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);

    if (!cfg.logFile.empty()) g_logFile.open(cfg.logFile, std::ios::app);
    g_downloaded.store(0);

    int fd = connectTo(cfg.host.c_str(), cfg.relayPort);
    if (fd < 0) {
        std::cerr << "  [err] connect to relay failed\n";
        return 1;
    }

    // relay auth (relay enforces token if RELAY_TOKEN is set)
    if (!cfg.token.empty()) {
        sendStr(fd, "AUTH " + cfg.token + "\n");
    }

    LOGN(cfg, "  " << c_dim() << "relay " << c_reset() << cfg.host << ":" << cfg.relayPort);
    LOGN(cfg, "  " << c_dim() << "open  " << c_reset() << c_cyn() << "http://" << cfg.host << ":8080" << c_reset());
    if (cfg.upload) LOGN(cfg, c_dim() << "  upload enabled (POST /upload)" << c_reset());

    while (g_running) {
        auto raw = recvUntil(fd, "\r\n\r\n");
        if (raw.empty()) {
            if (g_running) LOGN(cfg, "  [info] relay closed");
            break;
        }
        auto req = parseReq(raw);
        if (!req.ok) continue;

        LOG(cfg, c_dim() << "  ❯ " << c_reset() << c_grn() << req.method << c_reset() << " " << req.path);
        if (!req.query.empty()) LOG(cfg, c_dim() << "?" << req.query << c_reset());
        if (!cfg.hide) std::cout << std::endl;

        bool locked = cfg.lock && !checkPin(req.query, cfg.pin);
        if (cfg.lock && pinBlocked()) { sendShort(fd, 429, "429 Too Many Requests"); accessLog(req.method, req.path, 429); continue; }

        if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
            if (locked) {
                auto page = generatePinPage(cfg.pin, req.query);
                send200(fd, page, "text/html; charset=utf-8");
                accessLog(req.method, req.path, 200);
            } else {
                std::string page = htmlPage;
                if (cfg.lock) {
                    std::string pinQuery = "?pin=" + pinStr(cfg.pin);
                    if (isFile) page = generateSingleFileHTML(servePath, cfg.once, cfg.lock, pinQuery, cfg.upload, shareURL);
                    else        page = generateFileListHTML(servePath, cfg.once, cfg.lock, pinQuery, cfg.upload, shareURL);
                } else {
                    if (isFile) page = generateSingleFileHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
                    else        page = generateFileListHTML(servePath, cfg.once, cfg.lock, "", cfg.upload, shareURL);
                }
                send200(fd, page, "text/html; charset=utf-8");
                accessLog(req.method, req.path, 200);
            }
        }
        else if (req.method == "GET" && req.path.rfind("/download/", 0) == 0) {
            std::string fname = urlDecode(req.path.substr(10));
            if (locked) {
                LOGN(cfg, "  [warn] blocked (no PIN)");
                sendShort(fd, 403, "403 Forbidden");
                accessLog(req.method, req.path, 403);
                continue;
            }
            if (fname.empty() || fname.find("..") != std::string::npos ||
                fname.find('/') != std::string::npos || fname.find('\\') != std::string::npos) {
                LOGN(cfg, "  [warn] traversal blocked: " << fname);
                sendShort(fd, 403, "403 Forbidden");
                accessLog(req.method, req.path, 403);
                continue;
            }
            fs::path fpath = isFile ? servePath : (servePath / fname);
            std::error_code ec;
            if (!fs::is_regular_file(fpath, ec) || !isWithin(servePath, fpath)) {
                LOGN(cfg, "  [warn] not found / blocked: " << fname);
                sendShort(fd, 404, "404 Not Found");
                accessLog(req.method, req.path, 404);
                continue;
            }
            uint64_t off = 0, count = 0;
            bool isRange = false;
            std::string rangeHdr = getHeader(raw, "Range");
            if (!rangeHdr.empty()) {
                uint64_t sz = fs::file_size(fpath, ec);
                if (parseRange(rangeHdr, sz, off, count)) isRange = true;
            }
            streamFile(fd, fpath, fname, off, count, isRange);
            accessLog(req.method, req.path, isRange ? 206 : 200);
            if (cfg.once) {
                int prev = g_downloaded.fetch_add(1) + 1;
                int need = isFile ? 1 : cfg.totalFiles;
                if (prev >= need) {
                    LOGN(cfg, c_bold() << c_cyn() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset());
                    LOGN(cfg, c_yel() << "  once mode: download complete, melting..." << c_reset());
                    g_running = 0;
                    break;
                } else {
                    LOGN(cfg, c_dim() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset()
                              << "  " << c_yel() << "once " << prev << "/" << need << c_reset());
                }
            } else {
                LOGN(cfg, c_dim() << "  ✓ " << c_reset() << fname << c_dim() << " (" << formatSize(fs::file_size(fpath, ec)) << ")" << c_reset());
            }
        }
        else if (req.method == "POST" && req.path.rfind("/upload", 0) == 0) {
            if (!cfg.upload) { sendShort(fd, 403, "403 Forbidden"); accessLog(req.method, req.path, 403); continue; }
            std::string name = queryParam(req.query, "name");
            if (name.empty() || name.find("..") != std::string::npos ||
                name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                sendShort(fd, 400, "400 Bad Request"); accessLog(req.method, req.path, 400); continue;
            }
            std::string clenHdr = getHeader(raw, "Content-Length");
            size_t clen = clenHdr.empty() ? 0 : (size_t)std::strtoull(clenHdr.c_str(), nullptr, 10);
            if (clen == 0 || clen > (1024ULL * 1024 * 1024 * 4)) {
                sendShort(fd, 413, "413 Payload Too Large"); accessLog(req.method, req.path, 413); continue;
            }
            std::string body = readBody(fd, clen);
            fs::path dest = isFile ? servePath.parent_path() / name : (servePath / name);
            std::ofstream out(dest, std::ios::binary);
            out.write(body.data(), (std::streamsize)body.size());
            LOGN(cfg, c_dim() << "  ↑ " << c_reset() << "uploaded " << name << c_dim() << " (" << formatSize(body.size()) << ")" << c_reset());
            sendShort(fd, 200, "200 OK");
            accessLog(req.method, req.path, 200);
        }
        else {
            sendShort(fd, 404, "404 Not Found");
            accessLog(req.method, req.path, 404);
        }
    }
    close(fd);
    if (cfg.once) std::cout << c_bold() << c_cyn() << "  " << SNO << " melted." << c_reset() << std::endl;
    else LOGN(cfg, c_dim() << "  disconnected." << c_reset());
    return 0;
}

// ─── main ──────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    struct sigaction sa{};
    sa.sa_handler = handleSig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // do NOT restart syscalls – lets poll()/accept() return EINTR
    if (sigaction(SIGINT, &sa, nullptr) < 0)
        std::cerr << "[warn] sigaction SIGINT failed\n";
    if (sigaction(SIGTERM, &sa, nullptr) < 0)
        std::cerr << "[warn] sigaction SIGTERM failed\n";

    auto cfg = parseArgs(argc, argv);
    applyExternalConfig(cfg);
    g_color = isatty(STDOUT_FILENO) || isatty(STDERR_FILENO);
    if (cfg.version) { std::cout << "sonnet\n"; return 0; }
    if (cfg.help) { printHelp(argv[0]); return 0; }

    // ── self-install ──
    if (cfg.install) {
        fs::path self = fs::read_symlink("/proc/self/exe");
        std::vector<fs::path> candidates = {
            fs::path(getenv("HOME") ? getenv("HOME") : "") / ".local" / "bin",
            fs::path("/usr/local/bin")
        };
        fs::path dest;
        for (auto& d : candidates) {
            std::error_code ec;
            if (fs::is_directory(d, ec)) { dest = d; break; }
        }
        if (dest.empty()) {
            dest = candidates[0];
            fs::create_directories(dest);
        }
        fs::path target = dest / "snowflake";
        std::error_code ec;
        fs::copy_file(self, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "  [err] install failed: " << ec.message() << "\n";
            return 1;
        }
        fs::permissions(target, fs::perms::owner_all | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec, ec);
        std::cout << "  " << SNO << " installed to " << target.string() << "\n";

        // Check if in PATH
        std::string pathEnv = getenv("PATH") ? getenv("PATH") : "";
        std::string destStr = dest.string();
        if (pathEnv.find(destStr) == std::string::npos) {
            std::cout << "  add to PATH: export PATH=\"" << destStr << ":$PATH\"\n";
            auto home = getenv("HOME");
            if (home) {
                std::string rcFile = std::string(home) + "/.bashrc";
                std::cout << "  or run: echo 'export PATH=\"" << destStr << ":$PATH\"' >> " << rcFile << "\n";
            }
        } else {
            std::cout << "  " << destStr << " is already in PATH\n";
        }
        return 0;
    }

    if (!cfg.serve) { printHelp(argv[0]); return 1; }

    fs::path servePath;
    try { servePath = fs::absolute(cfg.path); }
    catch (...) { std::cerr << "  [err] invalid path\n"; return 1; }
    if (!fs::exists(servePath)) {
        std::cerr << "  [err] path not found: " << cfg.path << "\n";
        return 1;
    }

    if (cfg.lock) {
        cfg.pin = generatePin();
        if (cfg.hide) {
            // stay silent: persist PIN to a file the user can read
            std::string rt = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp";
            fs::path pinPath = fs::path(rt) / ("snowflake-pin-" + std::to_string(getpid()) + ".txt");
            std::ofstream pf(pinPath);
            if (pf) pf << pinStr(cfg.pin) << "\n";
        } else {
            std::cerr << "\n  " << SNO << " snowflake\n"
                      << "  " << c_dim() << "PIN" << c_reset() << "  "
                      << c_bold() << c_yel() << pinStr(cfg.pin) << c_reset() << std::endl << std::endl;
        }
    }

    bool isFile = fs::is_regular_file(servePath);

    LOG(cfg, c_bold() << c_cyn() << "  " << SNO << " snowflake" << c_reset()
            << c_dim() << "  zero-dependency file sharing" << c_reset() << "\n");
    LOGN(cfg, c_dim() << "  ───────────────────────────────────────" << c_reset());

    if (isFile) LOGN(cfg, "  " << c_dim() << "share " << c_reset() << servePath.filename().string());
    else        LOGN(cfg, "  " << c_dim() << "share " << c_reset() << servePath.string());

    std::string modes;
    if (cfg.once) {
        if (!isFile) {
            int cnt = 0;
            for (auto& e : fs::directory_iterator(servePath))
                if (e.is_regular_file()) cnt++;
            cfg.totalFiles = cnt;
            modes += "ephemeral (" + std::to_string(cnt) + " files)";
        } else {
            modes += "ephemeral";
        }
    }
    if (cfg.lock) {
        if (!modes.empty()) modes += std::string(c_dim()) + " · " + c_reset();
        modes += "locked";
    }
    if (!modes.empty()) LOGN(cfg, "  " << c_dim() << "modes " << c_reset() << modes);
    LOGN(cfg, "");

    if (cfg.host.empty()) {
        return runStandalone(cfg, servePath);
    } else {
        return runRelay(cfg, servePath);
    }
}
