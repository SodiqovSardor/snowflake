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
#include <unordered_map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <poll.h>
#include <fcntl.h>

namespace fs = std::filesystem;

static volatile sig_atomic_t g_running = 1;
extern "C" void handleSig(int) {
    g_running = 0;
}

// ─── config ────────────────────────────────────────────────────

struct Config {
    std::string path;           // file or directory to share
    bool serve = false;
    bool once  = false;
    bool lock  = false;
    bool hide  = false;
    int  pin   = -1;            // generated PIN
    int  port  = 8080;          // public HTTP port
    std::string host;           // relay host (empty = standalone)
    int  relayPort = 9000;      // relay control port
    bool help = false;
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
    while (g_running) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 1000); // 1s timeout – lets signal break through
        if (ret <= 0) {
            if (!g_running) return buf;
            continue;
        }
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return buf;
        buf.append(tmp, static_cast<size_t>(n));
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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
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

static const std::string SNO = "\xe2\x9d\x84"; // \u2744 (terminal, for non-UI)

static const std::string SNO_SVG = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><path d='M12 2v20M3 12h18M5.64 5.64l12.72 12.72M18.36 5.64l-12.72 12.72'/></svg>";

static const std::string ICON_DOC = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><path d='M4 4a2 2 0 0 1 2-2h8l6 6v12a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4z'/><path d='M14 2v6h6'/></svg>";
static const std::string ICON_IMG = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='8.5' cy='8.5' r='1.5'/><path d='M21 15l-5-5L5 21'/></svg>";
static const std::string ICON_VID = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='2' y='6' width='15' height='12' rx='2'/><path d='M17 10l4-2.5v9L17 14'/></svg>";
static const std::string ICON_AUD = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><path d='M9 18V5l12-2v13'/><circle cx='6' cy='18' r='3'/><circle cx='18' cy='16' r='3'/></svg>";
static const std::string ICON_ZIP = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='2' y='3' width='20' height='4' rx='1'/><path d='M4 7v11a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7'/><path d='M10 12h4'/></svg>";
static const std::string ICON_BIN = "<svg viewBox='0 0 24 24' width='1em' height='1em' fill='none' stroke='currentColor' stroke-width='1.5' style='display:inline-block;vertical-align:-.15em'><rect x='4' y='2' width='6' height='6' rx='1'/><rect x='14' y='2' width='6' height='6' rx='1'/><rect x='4' y='16' width='6' height='6' rx='1'/><rect x='14' y='16' width='6' height='6' rx='1'/></svg>";


int generatePin() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    return 1000 + (std::rand() % 9000);
}

std::string pinStr(int pin) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", pin);
    return buf;
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
body{background:#0d1117;color:#c9d1d9;font-family:ui-monospace,'SF Mono','Cascadia Code','Fira Code','Consolas',monospace;padding:2rem;min-height:100vh}
.top{display:flex;align-items:center;gap:.75rem;margin-bottom:2rem;padding-bottom:1.25rem;border-bottom:1px solid #21262d}
.logo{display:inline-block;font-size:1.6rem;animation:spin 4s linear infinite;user-select:none}
@keyframes spin{100%{transform:rotate(360deg)}}
.title{font-size:1.3rem;color:#f0f6fc;font-weight:600;letter-spacing:-.03em}
.badge{font-size:.65rem;padding:.15rem .55rem;border-radius:3px;text-transform:uppercase;letter-spacing:.06em}
.badge-once{background:#3a1a1a;color:#ff7b72;border:1px solid #5c1a1a}
.badge-lock{background:#1a2a3a;color:#58a6ff;border:1px solid #1a3a5c}
.status{display:flex;align-items:center;gap:.45rem;margin-left:auto;font-size:.75rem;color:#8b949e}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#3fb950;animation:pulse 2s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.28}}
table{width:100%;border-collapse:collapse}
th,td{padding:.65rem .75rem;text-align:left;border-bottom:1px solid #21262d}
th{color:#8b949e;font-weight:400;font-size:.68rem;text-transform:uppercase;letter-spacing:.08em;border-bottom-color:#30363d}
td{font-size:.85rem;color:#c9d1d9;word-break:break-all}
td.icon{width:2rem;font-size:1.1rem;text-align:center;word-break:normal}
td.name{max-width:60vw}
td.size{color:#8b949e;font-size:.8rem;white-space:nowrap;width:6rem}
td.action{width:7rem;text-align:right;word-break:normal}
.btn{display:inline-block;background:#21262d;color:#58a6ff;padding:.32rem .8rem;border-radius:6px;border:1px solid #30363d;font-size:.75rem;font-family:inherit;transition:background .15s,border-color .15s;cursor:pointer;text-decoration:none}
.btn:hover{background:#1a233c;border-color:#58a6ff;text-decoration:none}
.empty{color:#484f58;font-style:italic;padding:3rem 0;font-size:.88rem;text-align:center}
.foot{margin-top:2rem;padding-top:1.25rem;border-top:1px solid #21262d;color:#30363d;font-size:.65rem}
.pin-wrap{display:flex;align-items:center;justify-content:center;min-height:60vh}
.pin-card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:2.5rem 2.8rem;max-width:400px;width:100%;text-align:center}
.pin-card .logo{font-size:2.5rem;margin-bottom:.75rem}
.pin-card h2{font-size:1.3rem;color:#f0f6fc;margin-bottom:.4rem}
.pin-card p{color:#8b949e;font-size:.82rem;margin-bottom:1.5rem}
.pin-input{background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:.7rem;font-size:1.6rem;letter-spacing:.6em;text-align:center;width:100%;border-radius:6px;margin-bottom:1rem;font-family:inherit;outline:none;transition:border-color .2s}
.pin-input:focus{border-color:#58a6ff}
.pin-btn{background:#238636;color:#fff;border:none;padding:.65rem 0;width:100%;border-radius:6px;cursor:pointer;font-size:.9rem;font-family:inherit;transition:background .15s}
.pin-btn:hover{background:#2ea043}
.pin-err{color:#f85149;font-size:.78rem;margin-top:.75rem}
@media(max-width:600px){body{padding:1rem}.top{flex-wrap:wrap;gap:.5rem}td,th{padding:.5rem .55rem}td.name{max-width:50vw}td.size{width:4rem;font-size:.72rem}td.action{width:5rem}.pin-card{padding:1.5rem;margin:1rem}}
</style>
</head>
<body>
)RAW";

static const std::string PAGE_BOTTOM = R"RAW(
<p class="foot">snowflake &mdash; zero-dependency file sharing</p>
</body>
</html>
)RAW";

std::string topBar(const std::string& title, bool onceMode, bool lockMode, const std::string& status) {
    std::string h;
    h += "<div class=\"top\"><span class=\"logo\">" + SNO + "</span>";
    h += "<span class=\"title\">" + title + "</span>";
    if (onceMode) h += "<span class=\"badge badge-once\">once</span>";
    if (lockMode) h += "<span class=\"badge badge-lock\">lock</span>";
    h += "<span class=\"status\"><span class=\"dot\"></span><span>" + status + "</span></span></div>\n";
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
    p += topBar("snowflake", false, false, "locked");
    p += "<div class=\"pin-wrap\"><div class=\"pin-card\">";
    p += "<span class=\"logo\">" + SNO + "</span>";
    p += "<h2>snowflake</h2><p>Enter PIN to access shared files</p>";
    p += "<form method=\"get\" action=\"/\">";
    p += "<input class=\"pin-input\" type=\"text\" name=\"pin\" inputmode=\"numeric\" pattern=\"[0-9]{4}\" maxlength=\"4\" placeholder=\"\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b\" autofocus>";
    p += "<button class=\"pin-btn\" type=\"submit\">Unlock</button></form>";
    if (!error.empty()) p += "<p class=\"pin-err\">" + error + "</p>";
    p += "</div></div>" + PAGE_BOTTOM;
    return p;
}

std::string fileRows(const fs::path& dir) {
    std::error_code ec;
    std::string rows;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto name = e.path().filename().string();
        rows += "<tr><td class=\"icon\">" + fileIcon(name) + "</td>";
        rows += "<td class=\"name\">" + htmlEscape(name) + "</td>";
        rows += "<td class=\"size\">" + formatSize(e.file_size(ec)) + "</td>";
        rows += "<td class=\"action\"><a class=\"btn\" href=\"/download/" + name + "\">Get File</a></td></tr>\n";
    }
    return rows;
}

std::string generateFileListHTML(const fs::path& dir, bool onceMode, bool lockMode) {
    std::string p;
    p += PAGE_TOP;
    p += topBar("snowflake", onceMode, lockMode, "connected");
    std::error_code ec;
    bool hasFiles = false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) { hasFiles = true; break; }
    }
    if (!hasFiles) {
        p += "<p class=\"empty\">no files in this directory</p>";
    } else {
        p += "<table><thead><tr><th></th><th>name</th><th>size</th><th></th></tr></thead><tbody>\n";
        p += fileRows(dir);
        p += "</tbody></table>\n";
    }
    p += PAGE_BOTTOM;
    return p;
}

std::string generateSingleFileHTML(const fs::path& file, bool onceMode, bool lockMode) {
    auto name = file.filename().string();
    auto sz = formatSize(fs::file_size(file));
    std::string p;
    p += PAGE_TOP;
    p += topBar("snowflake \xe2\x80\x94 single file", onceMode, lockMode, "connected");
    p += "<table><thead><tr><th></th><th>name</th><th>size</th><th></th></tr></thead><tbody>\n";
    p += "<tr><td class=\"icon\">" + fileIcon(name) + "</td>";
    p += "<td class=\"name\">" + htmlEscape(name) + "</td>";
    p += "<td class=\"size\">" + sz + "</td>";
    p += "<td class=\"action\"><a class=\"btn\" href=\"/download/" + name + "\">Get File</a></td></tr>\n";
    p += "</tbody></table>\n";
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

// ─── PIN check ─────────────────────────────────────────────────

bool checkPin(const std::string& query, int correctPin) {
    if (query.empty()) return false;
    auto pos = query.find("pin=");
    if (pos == std::string::npos) return false;
    return std::atoi(query.c_str() + pos + 4) == correctPin;
}

// ─── arg parsing ───────────────────────────────────────────────

Config parseArgs(int argc, char* argv[]) {
    Config c;
    if (argc < 3) { c.help = true; return c; }
    if (std::string(argv[1]) != "send") { c.help = true; return c; }
    c.path = argv[2];
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "serve")     c.serve = true;
        else if (a == "once")      c.once  = true;
        else if (a == "lock")      c.lock  = true;
        else if (a == "hide")      c.hide  = true;
        else if (a == "--host" && i+1 < argc) c.host = argv[++i];
        else if (a == "--port" && i+1 < argc) c.port = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help")  c.help = true;
    }
    if (!c.serve && (c.once || c.lock || c.hide)) c.serve = true;
    return c;
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
              << "  -h, --help     This help\n\n"
              << "Examples:\n"
              << "  " << prog << " send . serve                 # standalone HTTP server\n"
              << "  " << prog << " send file.mp4 serve once     # single file, melt after dl\n"
              << "  " << prog << " send /docs serve lock hide   # quiet, PIN-protected\n"
              << "  " << prog << " send . serve --host relay.io # relay mode\n";
}

// ─── log macros ────────────────────────────────────────────────

#define LOG(cfg, msg)   do { if (!(cfg).hide) { std::cout << msg; } } while(0)
#define LOGN(cfg, msg)  do { if (!(cfg).hide) { std::cout << msg << std::endl; } } while(0)

// ─── response helpers ──────────────────────────────────────────

void send200(int fd, const std::string& body, const std::string& contentType) {
    sendStr(fd, "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
}

void sendShort(int fd, int code, const std::string& status) {
    sendStr(fd, "HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

void streamFile(int fd, const fs::path& fpath, const std::string& fname) {
    std::error_code ec;
    auto sz = fs::file_size(fpath, ec);
    sendStr(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"" + fname + "\"\r\n"
        "Content-Length: " + std::to_string(sz) + "\r\nConnection: close\r\n\r\n");
    std::ifstream file(fpath, std::ios::binary);
    if (file.is_open()) {
        char buf[4096];
        while (g_running && (file.read(buf, sizeof(buf)) || file.gcount() > 0)) {
            sendAll(fd, buf, static_cast<size_t>(file.gcount()));
            if (file.eof()) break;
        }
    }
}

// ─── request handler ──────────────────────────────────────────

void handleRequest(int fd, Config& cfg, const fs::path& servePath,
                   bool isSingleFile, const std::string& htmlPage) {
    auto raw = recvUntil(fd, "\r\n\r\n");
    if (raw.empty()) return;
    auto req = parseReq(raw);
    if (!req.ok) return;

    LOG(cfg, "  \xe2\x86\x92 " << req.method << " " << req.path);
    if (!req.query.empty()) LOG(cfg, "?" << req.query);
    if (!cfg.hide) std::cout << std::endl;

    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        if (cfg.lock && !checkPin(req.query, cfg.pin)) {
            send200(fd, generatePinPage(cfg.pin, req.query), "text/html; charset=utf-8");
        } else {
            send200(fd, htmlPage, "text/html; charset=utf-8");
        }
    }
    else if (req.method == "GET" && req.path.rfind("/download/", 0) == 0) {
        std::string fname = req.path.substr(10);

        if (cfg.lock && !checkPin(req.query, cfg.pin)) {
            LOGN(cfg, "  [warn] blocked (no PIN)");
            sendShort(fd, 403, "403 Forbidden");
            return;
        }
        if (fname.empty() || fname.find("..") != std::string::npos ||
            fname.find('/') != std::string::npos || fname.find('\\') != std::string::npos) {
            LOGN(cfg, "  [warn] traversal blocked: " << fname);
            sendShort(fd, 403, "403 Forbidden");
            return;
        }

        fs::path fpath = isSingleFile ? servePath : (servePath / fname);
        std::error_code ec;
        if (!fs::is_regular_file(fpath, ec)) {
            LOGN(cfg, "  [warn] not found: " << fname);
            sendShort(fd, 404, "404 Not Found");
            return;
        }

        LOG(cfg, "  streaming " << fname << " (" << formatSize(fs::file_size(fpath, ec)) << ")");
        streamFile(fd, fpath, fname);

        if (cfg.once) {
            LOGN(cfg, "\n  once mode: download complete, melting...");
            g_running = 0;
        }
    }
    else {
        sendShort(fd, 404, "404 Not Found");
    }
}

// ─── standalone server ─────────────────────────────────────────

int runStandalone(Config& cfg, const fs::path& servePath) {
    bool isFile = fs::is_regular_file(servePath);
    std::string htmlPage;
    if (isFile) htmlPage = generateSingleFileHTML(servePath, cfg.once, cfg.lock);
    else        htmlPage = generateFileListHTML(servePath, cfg.once, cfg.lock);

    int srv = listenOn(cfg.port);
    if (srv < 0) {
        std::cerr << "  [err] cannot bind port " << cfg.port << "\n";
        return 1;
    }

    LOGN(cfg, "  http://0.0.0.0:" << cfg.port << "\n");

    // non-blocking accept – signal handler sets g_running=0, loop exits
    int flags = fcntl(srv, F_GETFL, 0);
    if (flags >= 0) fcntl(srv, F_SETFL, flags | O_NONBLOCK);

    while (g_running) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = accept(srv, (struct sockaddr*)&cli, &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); // 100ms – check g_running regularly
                continue;
            }
            continue;
        }
        handleRequest(fd, cfg, servePath, isFile, htmlPage);
        close(fd);
    }
    close(srv);
    if (cfg.once) std::cout << "  \xe2\x9d\x84 melted.\n";
    return 0;
}

// ─── relay mode ────────────────────────────────────────────────

int runRelay(Config& cfg, const fs::path& servePath) {
    bool isFile = fs::is_regular_file(servePath);
    std::string htmlPage;
    if (isFile) htmlPage = generateSingleFileHTML(servePath, cfg.once, cfg.lock);
    else        htmlPage = generateFileListHTML(servePath, cfg.once, cfg.lock);

    int fd = connectTo(cfg.host.c_str(), cfg.relayPort);
    if (fd < 0) {
        std::cerr << "  [err] connect to relay failed\n";
        return 1;
    }

    LOGN(cfg, "  relay: " << cfg.host << ":" << cfg.relayPort << "\n");

    while (g_running) {
        auto raw = recvUntil(fd, "\r\n\r\n");
        if (raw.empty()) {
            if (g_running) LOGN(cfg, "  [info] relay closed");
            break;
        }
        auto req = parseReq(raw);
        if (!req.ok) continue;

        LOG(cfg, "  \xe2\x86\x92 " << req.method << " " << req.path);
        if (!req.query.empty()) LOG(cfg, "?" << req.query);
        if (!cfg.hide) std::cout << std::endl;

        if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
            if (cfg.lock && !checkPin(req.query, cfg.pin)) {
                auto page = generatePinPage(cfg.pin, req.query);
                send200(fd, page, "text/html; charset=utf-8");
            } else {
                send200(fd, htmlPage, "text/html; charset=utf-8");
            }
        }
        else if (req.method == "GET" && req.path.rfind("/download/", 0) == 0) {
            std::string fname = req.path.substr(10);
            if (cfg.lock && !checkPin(req.query, cfg.pin)) {
                LOGN(cfg, "  [warn] blocked (no PIN)");
                sendShort(fd, 403, "403 Forbidden");
                continue;
            }
            if (fname.empty() || fname.find("..") != std::string::npos ||
                fname.find('/') != std::string::npos || fname.find('\\') != std::string::npos) {
                LOGN(cfg, "  [warn] traversal blocked: " << fname);
                sendShort(fd, 403, "403 Forbidden");
                continue;
            }
            fs::path fpath = isFile ? servePath : (servePath / fname);
            std::error_code ec;
            if (!fs::is_regular_file(fpath, ec)) {
                LOGN(cfg, "  [warn] not found: " << fname);
                sendShort(fd, 404, "404 Not Found");
                continue;
            }
            LOG(cfg, "  streaming " << fname << " (" << formatSize(fs::file_size(fpath, ec)) << ")");
            streamFile(fd, fpath, fname);
            if (cfg.once) {
                LOGN(cfg, "\n  once mode: melting...");
                g_running = 0;
                break;
            }
        }
        else {
            sendShort(fd, 404, "404 Not Found");
        }
    }
    close(fd);
    if (cfg.once) std::cout << "  \xe2\x9d\x84 melted.\n";
    else LOGN(cfg, "  disconnected.");
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
    if (cfg.help) { printHelp(argv[0]); return 0; }
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
        std::cout << "\n  " << SNO << " snowflake\n"
                  << "  [lock] PIN: " << pinStr(cfg.pin) << std::endl << std::endl;
    }

    bool isFile = fs::is_regular_file(servePath);
    LOGN(cfg, "  " << SNO << " snowflake");
    if (isFile) LOGN(cfg, "  sharing: " << servePath.filename().string());
    else        LOGN(cfg, "  serving: " << servePath.string());
    if (cfg.once) LOGN(cfg, "  mode: ephemeral");
    if (cfg.lock && !cfg.hide) LOGN(cfg, "  mode: locked");
    if (!cfg.hide) LOGN(cfg, "");

    if (cfg.host.empty()) {
        return runStandalone(cfg, servePath);
    } else {
        return runRelay(cfg, servePath);
    }
}
