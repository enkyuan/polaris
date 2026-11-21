#pragma once

#include "model.hpp"

namespace shop {

class UserWriter {
public:
    UserWriter(fs::path path, const Catalog& catalog);
    ~UserWriter();
    UserWriter(const UserWriter&) = delete;
    UserWriter& operator=(const UserWriter&) = delete;
    void append(const UserDay& row);
    void close(bool write_empty = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void write_campaigns(const fs::path& path, const Catalog& catalog, const CampaignTotals& totals);

}  // namespace shop
