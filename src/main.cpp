#include "http_server.hpp"
#include "db.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sig(int){ g_stop = 1; }

static std::string getenv_or(const char* k, const char* d){
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d);
}
static int getenv_or_int(const char* k, int d){
    const char* v = std::getenv(k);
    if (!v) return d;
    try { return std::stoi(v); } catch (...) { return d; }
}

int main(){
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    std::string addr = getenv_or("ADDR","0.0.0.0");
    int port = getenv_or_int("PORT", 8080);
    int cache_size = getenv_or_int("CACHE_SIZE", 100);

    std::string dbhost = getenv_or("MYSQL_HOST","127.0.0.1");
    int dbport = getenv_or_int("MYSQL_PORT", 3306);
    std::string dbuser = getenv_or("MYSQL_USER","root");
    std::string dbpass = getenv_or("MYSQL_PASS","root");
    std::string dbname = getenv_or("MYSQL_DB","kv");

    Database db;
    if (!db.connect(dbhost, dbport, dbuser, dbpass, dbname)){
        std::cerr<<"[db] connect failed\n"; return 1;
    }
    if (!db.ensure_table()){
        std::cerr<<"[db] ensure_table failed\n"; return 1;
    }

    HTTPServer srv(addr, (uint16_t)port, (size_t)cache_size, &db);
    if (!srv.start()) return 1;

    while (!g_stop) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    srv.stop();
    std::cerr<<"[server] bye\n";
    return 0;
}
