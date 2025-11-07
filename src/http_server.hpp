#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <netinet/in.h>

#include "lru_cache.hpp"

class Database;

class HTTPServer {
public:
    HTTPServer(const std::string& addr, uint16_t port, size_t cache_size,
               Database* db, size_t worker_count = std::thread::hardware_concurrency());

    ~HTTPServer();

    bool start();
    void stop();

private:
    // networking
    int listen_fd_ = -1;
    std::string addr_;
    uint16_t port_;

    // thread pool
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    std::thread accept_thread_;

    std::queue<int> connq_;
    std::mutex qmu_;
    std::condition_variable qcv_;

    // components
    LRUCache cache_;
    Database* db_;

    // internals
    void acceptLoop();
    void workerLoop();
    void handleClient(int fd);

    // helpers
    static bool setReuseAddr(int fd);
    static bool setNonBlock(int fd, bool nb);
    static bool readN(int fd, char* buf, size_t n);
    static bool readLine(int fd, std::string& line);
    static bool writeAll(int fd, const std::string& s);
    static void closeFd(int fd);

    // request handlers
    void handleGET(const std::string& path, int fd);
    void handleDELETE(const std::string& path, int fd);
    void handlePOST(const std::string& path, int fd, const std::string& body);
};
