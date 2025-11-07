#include "db.hpp"
#include <mysql/mysql.h>
#include <iostream>
#include <vector>

static std::string esc(MYSQL* m, const std::string& in){
    std::string out; out.resize(in.size()*2 + 1);
    unsigned long len = mysql_real_escape_string(m, out.data(), in.data(), in.size());
    out.resize(len);
    return out;
}

Database::~Database(){
    if (mysql_) {
        mysql_close((MYSQL*)mysql_);
        mysql_ = nullptr;
    }
}

bool Database::connect(const std::string& host, unsigned port,
                       const std::string& user, const std::string& pass,
                       const std::string& dbname){
    MYSQL* m = mysql_init(nullptr);
    if (!m){ std::cerr<<"mysql_init failed\n"; return false; }
    if (!mysql_real_connect(m, host.c_str(), user.c_str(), pass.c_str(), dbname.c_str(), port, nullptr, 0)){
        std::cerr<<"mysql_connect: "<< mysql_error(m) <<"\n";
        mysql_close(m); return false;
    }
    mysql_ = m;
    return true;
}

bool Database::ensure_table(){
    const char* q = "CREATE TABLE IF NOT EXISTS kv (k VARCHAR(255) PRIMARY KEY, v MEDIUMBLOB)";
    if (mysql_query((MYSQL*)mysql_, q) != 0){
        std::cerr<<"ensure_table: "<< mysql_error((MYSQL*)mysql_) <<"\n";
        return false;
    }
    return true;
}

bool Database::upsert(const std::string& key, const std::string& val){
    MYSQL* m = (MYSQL*)mysql_;
    std::string k = esc(m, key);
    std::string v = esc(m, val);
    std::string q = "INSERT INTO kv (k,v) VALUES ('"+k+"','"+v+"') "
                    "ON DUPLICATE KEY UPDATE v=VALUES(v)";
    if (mysql_query(m, q.c_str()) != 0){
        std::cerr<<"upsert: "<< mysql_error(m) <<"\n"; return false;
    }
    return true;
}

bool Database::get(const std::string& key, std::string& out, bool& found){
    MYSQL* m = (MYSQL*)mysql_;
    std::string k = esc(m, key);
    std::string q = "SELECT v FROM kv WHERE k='"+k+"'";
    if (mysql_query(m, q.c_str()) != 0){
        std::cerr<<"get: "<< mysql_error(m) <<"\n"; return false;
    }
    MYSQL_RES* res = mysql_store_result(m);
    if (!res){ std::cerr<<"get store_result: "<< mysql_error(m) <<"\n"; return false; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row){
        found = false;
        mysql_free_result(res);
        return true;
    }
    unsigned long* lens = mysql_fetch_lengths(res);
    out.assign(row[0], lens[0]);
    found = true;
    mysql_free_result(res);
    return true;
}

bool Database::del(const std::string& key, bool& existed){
    MYSQL* m = (MYSQL*)mysql_;
    std::string k = esc(m, key);
    std::string q = "DELETE FROM kv WHERE k='"+k+"'";
    if (mysql_query(m, q.c_str()) != 0){
        std::cerr<<"del: "<< mysql_error(m) <<"\n"; return false;
    }
    my_ulonglong aff = mysql_affected_rows(m);
    existed = (aff > 0);
    return true;
}
