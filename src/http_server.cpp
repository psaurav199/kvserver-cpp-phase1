#include "http_server.hpp"
#include "db.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <regex>

HTTPServer::HTTPServer(const std::string& addr, uint16_t port, size_t cache_size, Database* db, size_t worker_count)
: addr_(addr), port_(port), cache_(cache_size), db_(db) {
    if (worker_count == 0) worker_count = 4;
    workers_.reserve(worker_count);
}

HTTPServer::~HTTPServer() {
    stop();
}

bool HTTPServer::setReuseAddr(int fd){
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool HTTPServer::setNonBlock(int fd, bool nb){
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0;
}

bool HTTPServer::start(){
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("socket"); return false; }
    setReuseAddr(listen_fd_);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port_);
    if (addr_.empty()) addr_ = "0.0.0.0";
    if (inet_pton(AF_INET, addr_.c_str(), &sa.sin_addr) != 1) { std::cerr<<"bad addr\n"; return false; }
    if (bind(listen_fd_, (sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return false; }
    if (listen(listen_fd_, 1024) < 0) { perror("listen"); return false; }

    running_.store(true);
    // workers
    for (size_t i=0;i<workers_.capacity();++i){
        workers_.emplace_back(&HTTPServer::workerLoop, this);
    }
    // acceptor
    accept_thread_ = std::thread(&HTTPServer::acceptLoop, this);

    std::cout << "[server] listening on " << addr_ << ":" << port_ << std::endl;
    return true;
}

void HTTPServer::stop(){
    if (!running_.exchange(false)) return;
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    if (accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard<std::mutex> lk(qmu_);
        // wake workers
        while(!connq_.empty()){
            closeFd(connq_.front());
            connq_.pop();
        }
    }
    qcv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void HTTPServer::acceptLoop(){
    while (running_.load()){
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            perror("accept");
            continue;
        }
        // enqueue
        {
            std::lock_guard<std::mutex> lk(qmu_);
            connq_.push(fd);
        }
        qcv_.notify_one();
    }
}

void HTTPServer::workerLoop(){
    while (running_.load()){
        int fd = -1;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            qcv_.wait(lk, [&]{ return !running_.load() || !connq_.empty(); });
            if (!running_.load() && connq_.empty()) return;
            fd = connq_.front(); connq_.pop();
        }
        if (fd >= 0) handleClient(fd);
    }
}

bool HTTPServer::readLine(int fd, std::string& line){
    line.clear();
    char c;
    while (true){
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\r') continue;
        if (c == '\n') break;
        line.push_back(c);
    }
    return true;
}

bool HTTPServer::readN(int fd, char* buf, size_t n){
    size_t got = 0;
    while (got < n){
        ssize_t r = ::recv(fd, buf+got, n-got, 0);
        if (r <= 0) return false;
        got += size_t(r);
    }
    return true;
}

bool HTTPServer::writeAll(int fd, const std::string& s){
    size_t sent = 0;
    while (sent < s.size()){
        ssize_t n = ::send(fd, s.data()+sent, s.size()-sent, 0);
        if (n <= 0) return false;
        sent += size_t(n);
    }
    return true;
}

void HTTPServer::closeFd(int fd){ ::shutdown(fd, SHUT_RDWR); ::close(fd); }

void HTTPServer::handleClient(int fd){
    // very small HTTP parser: request line + headers
    std::string line;
    if (!readLine(fd, line)) { closeFd(fd); return; }
    std::istringstream rl(line);
    std::string method, path, version;
    rl >> method >> path >> version;
    if (method.empty() || path.empty()){
        closeFd(fd); return;
    }

    // headers (we only care about Content-Length)
    size_t content_len = 0;
    while (true){
        if (!readLine(fd, line)) { closeFd(fd); return; }
        if (line.empty()) break;
        auto pos = line.find(':');
        if (pos != std::string::npos){
            std::string key = line.substr(0,pos);
            std::string val = line.substr(pos+1);
            // trim
            auto ltrim=[&](std::string& s){ s.erase(0, s.find_first_not_of(" \t")); };
            auto rtrim=[&](std::string& s){ s.erase(s.find_last_not_of(" \t")+1); };
            ltrim(val); rtrim(val);
            for (auto& c: key) c = ::tolower(c);
            if (key == "content-length") content_len = std::stoul(val);
        }
    }

    std::string body;
    if (method == "POST" && content_len > 0){
        body.resize(content_len);
        if (!readN(fd, body.data(), content_len)) { closeFd(fd); return; }
    }

    if (method == "GET") {
        handleGET(path, fd);
    } else if (method == "DELETE") {
        handleDELETE(path, fd);
    } else if (method == "POST") {
        handlePOST(path, fd, body);
    } else {
        writeAll(fd, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length:0\r\n\r\n");
    }
    closeFd(fd);
}

static std::string http_json(int status, const std::string& payload){
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << payload.size() << "\r\n\r\n"
      << payload;
    return o.str();
}

static std::string http_empty(int status){
    return "HTTP/1.1 " + std::to_string(status) + "\r\nContent-Length:0\r\n\r\n";
}

void HTTPServer::handleGET(const std::string& path, int fd){
    if (path.rfind("/kv/", 0) != 0){
        writeAll(fd, http_empty(404)); return;
    }
    std::string key = path.substr(4);
    if (key.empty()){ writeAll(fd, http_empty(404)); return; }

    std::string val;
    if (cache_.get(key, val)){
        writeAll(fd, http_json(200, std::string("{\"value\":\"") + val + "\"}"));
        return;
    }
    std::string dbval;
    bool ok = false;
    if (!db_->get(key, dbval, ok)){
        writeAll(fd, http_empty(500)); return;
    }
    if (!ok){
        writeAll(fd, http_empty(404)); return;
    }
    cache_.set(key, dbval);
    writeAll(fd, http_json(200, std::string("{\"value\":\"") + dbval + "\"}"));
}

void HTTPServer::handleDELETE(const std::string& path, int fd){
    if (path.rfind("/kv/", 0) != 0){
        writeAll(fd, http_empty(404)); return;
    }
    std::string key = path.substr(4);
    if (key.empty()){ writeAll(fd, http_empty(404)); return; }

    bool existed = false;
    if (!db_->del(key, existed)){
        writeAll(fd, http_empty(500)); return;
    }
    if (!existed){
        writeAll(fd, http_empty(404)); return;
    }
    cache_.erase(key);
    writeAll(fd, http_empty(204));
}

static bool extract_json_key_value(const std::string& body, std::string& key, std::string& val){
    // naive extraction of "key":"...","value":"..." (strings without escaped quotes)
    std::regex re(R"raw("key"\s*:\s*"([^"]+)"\s*,\s*"value"\s*:\s*"([^"]*)")raw");
    std::smatch m;
    if (std::regex_search(body, m, re) && m.size()==3){
        key = m[1].str();
        val = m[2].str();
        return true;
    }
    // also allow {"value":"...","key":"..."}
    std::regex re2(R"raw("value"\s*:\s*"([^"]*)"\s*,\s*"key"\s*:\s*"([^"]+)")raw");
    if (std::regex_search(body, m, re2) && m.size()==3){
        val = m[1].str();
        key = m[2].str();
        return true;
    }
    return false;
}

void HTTPServer::handlePOST(const std::string& path, int fd, const std::string& body){
    if (path != "/kv"){
        writeAll(fd, http_empty(404)); return;
    }
    std::string key, val;
    if (!extract_json_key_value(body, key, val) || key.empty()){
        writeAll(fd, http_empty(400)); return;
    }
    cache_.set(key, val);
    if (!db_->upsert(key, val)){
        writeAll(fd, http_empty(500)); return;
    }
    writeAll(fd, http_empty(200));
}
