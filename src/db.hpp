#pragma once
#include <string>

class Database {
public:
    Database() = default;
    ~Database();

    bool connect(const std::string& host, unsigned port,
                 const std::string& user, const std::string& pass,
                 const std::string& dbname);

    bool ensure_table();

    bool upsert(const std::string& key, const std::string& val);
    bool get(const std::string& key, std::string& out, bool& found);
    bool del(const std::string& key, bool& existed);

private:
    void* mysql_ = nullptr; // MYSQL*
};
