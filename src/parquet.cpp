#include "parquet.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <type_traits>

namespace shop {
namespace {

// This writer implements the flat, uncompressed subset of the Apache Parquet
// format used here. Schemas and metadata use the Thrift compact protocol.
constexpr int type_i32 = 1;
constexpr int type_i64 = 2;
constexpr int type_bytes = 6;
constexpr int converted_utf8 = 0;
constexpr int converted_date = 6;
constexpr int plain = 0;
constexpr int rle = 3;
constexpr std::size_t row_group_size = 32'768;

struct Column {
    std::string_view name;
    int physical;
    int converted = -1;
    bool nullable = false;
};

constexpr std::array user_columns{
    Column{"user_id", type_bytes, converted_utf8},
    Column{"day", type_i32, converted_date},
    Column{"sessions", type_i64}, Column{"events", type_i64}, Column{"revenue_cents", type_i64},
    Column{"top_category", type_bytes, converted_utf8, true}
};
constexpr std::array campaign_columns{
    Column{"campaign_id", type_bytes, converted_utf8}, Column{"channel", type_bytes, converted_utf8},
    Column{"day", type_i32, converted_date},
    Column{"sessions", type_i64}, Column{"purchasing_sessions", type_i64},
    Column{"revenue_cents", type_i64}, Column{"assisted_sessions", type_i64}
};

void vint(std::string& buffer, std::uint64_t value) {
    while (value >= 128) {
        buffer.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    buffer.push_back(static_cast<char>(value));
}

template<typename T> requires std::is_integral_v<T>
void little(std::string& buffer, T value) {
    static_assert(std::endian::native == std::endian::little);
    buffer.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void plain_string(std::string& page, std::string_view value) {
    if (value.size() > INT32_MAX) throw std::runtime_error("Parquet string exceeds page size limit");
    little(page, static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) page.append(value);
}

enum Compact : unsigned char { i32 = 5, i64 = 6, binary = 8, list = 9, structure = 12 };

class Thrift {
public:
    explicit Thrift(std::string& bytes) : bytes_(bytes) {}
    void integer(int id, std::int64_t value, Compact type = i32) {
        field(id, type);
        number(value);
    }
    void string(int id, std::string_view value) {
        field(id, binary);
        text(value);
    }
    void array(int id, std::size_t count, Compact kind) {
        field(id, list);
        if (count < 15) byte((count << 4) | kind);
        else {
            byte(0xf0 | kind);
            vint(bytes_, count);
        }
    }
    template<typename F>
    void object(int id, F&& write) {
        field(id, structure);
        object(std::forward<F>(write));
    }
    template<typename F>
    void object(F&& write) {
        Thrift nested(bytes_);
        write(nested);
        nested.stop();
    }
    void number(std::int64_t x) {
        vint(bytes_, (static_cast<std::uint64_t>(x) << 1) ^ (x < 0 ? UINT64_MAX : 0));
    }
    void text(std::string_view x) {
        vint(bytes_, x.size());
        bytes_.append(x);
    }
    void stop() { byte(0); }
private:
    void byte(unsigned x) { bytes_.push_back(static_cast<char>(x)); }
    void field(int id, Compact type) {
        const auto delta = id - last_;
        if (delta > 0 && delta <= 15) byte((delta << 4) | type);
        else {
            byte(type);
            number(id);
        }
        last_ = id;
    }
    std::string& bytes_;
    int last_ = 0;
};

struct Chunk {
    std::int64_t offset, size;
};
struct Group {
    std::array<Chunk, campaign_columns.size()> chunks{};
    std::int64_t count, size;
};

class File {
public:
    File(const fs::path& path, std::span<const Column> columns)
        : out_(path, std::ios::binary), columns_(columns) {
        out_.exceptions(std::ios::failbit | std::ios::badbit);
        write("PAR1");
    }

    template<typename F>
    void add_group(std::size_t count, F&& write_column) {
        if (!count) return;
        Group group{{}, static_cast<std::int64_t>(count), 0};
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            page_.clear();
            write_column(i, page_);
            if (page_.size() > INT32_MAX) throw std::runtime_error("Parquet page exceeds size limit");
            header_.clear();
            Thrift page_header(header_);
            page_header.integer(1, 0); // DATA_PAGE
            page_header.integer(2, page_.size());
            page_header.integer(3, page_.size());
            page_header.object(5, [count](Thrift& page) {
                page.integer(1, count);
                page.integer(2, plain);
                page.integer(3, rle);
                page.integer(4, rle);
            });
            page_header.stop();
            const auto size = static_cast<std::int64_t>(header_.size() + page_.size());
            group.chunks[i] = {offset_, size};
            group.size += size;
            write(header_);
            write(page_);
        }
        groups_.push_back(group);
        count_ += group.count;
    }

    void close() {
        std::string footer;
        Thrift file(footer);
        file.integer(1, 1);
        file.array(2, columns_.size() + 1, structure);
        file.object([&](Thrift& root) {
            root.string(4, "schema");
            root.integer(5, columns_.size());
        });
        for (const auto& col : columns_) {
            file.object([&](Thrift& schema) {
                schema.integer(1, col.physical);
                schema.integer(3, col.nullable ? 1 : 0);
                schema.string(4, col.name);
                if (col.converted >= 0) {
                    schema.integer(6, col.converted);
                    // LogicalType union field 1 is STRING and field 6 is DATE.
                    schema.object(10, [&](Thrift& logical) {
                        logical.object(col.converted == converted_date ? 6 : 1, [](Thrift&) {});
                    });
                }
            });
        }
        file.integer(3, count_, i64);
        file.array(4, groups_.size(), structure);
        for (const auto& group : groups_) {
            file.object([&](Thrift& row_group) {
                row_group.array(1, columns_.size(), structure);
                for (std::size_t i = 0; i < columns_.size(); ++i) {
                    const auto& col = columns_[i];
                    const auto& chunk = group.chunks[i];
                    row_group.object([&](Thrift& data) {
                        data.integer(2, 0, i64); // Deprecated offset of external metadata.
                        data.object(3, [&](Thrift& meta) {
                            meta.integer(1, col.physical);
                            meta.array(2, col.nullable ? 2 : 1, i32);
                            meta.number(plain);
                            if (col.nullable) meta.number(rle);
                            meta.array(3, 1, binary);
                            meta.text(col.name);
                            meta.integer(4, 0); // UNCOMPRESSED
                            meta.integer(5, group.count, i64);
                            meta.integer(6, chunk.size, i64);
                            meta.integer(7, chunk.size, i64);
                            meta.integer(9, chunk.offset, i64);
                        });
                    });
                }
                row_group.integer(2, group.size, i64);
                row_group.integer(3, group.count, i64);
            });
        }
        file.string(6, "polaris version 1");
        file.stop();
        const auto size = static_cast<std::uint32_t>(footer.size());
        little(footer, size);
        footer.append("PAR1");
        write(footer);
        out_.close();
    }
private:
    void write(std::string_view bytes) {
        out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        offset_ += static_cast<std::int64_t>(bytes.size());
    }
    std::ofstream out_;
    std::span<const Column> columns_;
    std::vector<Group> groups_;
    std::string page_, header_;
    std::int64_t offset_{}, count_{};
};

void category_page(std::string& page, const Catalog& catalog, std::span<const UserDay> rows) {
    // One-bit definition levels in a single hybrid bit-packed run. Data pages
    // v1 prefix the level section with its four-byte little-endian length.
    const auto groups = (rows.size() + 7) / 8;
    little(page, std::uint32_t{0});
    vint(page, (groups << 1) | 1);
    const auto level_base = page.size();
    page.resize(level_base + groups, '\0');
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].category == none) continue;
        page[level_base + i / 8] |= static_cast<char>(1U << (i % 8));
    }
    const auto bytes = static_cast<std::uint32_t>(page.size() - 4);
    std::memcpy(page.data(), &bytes, sizeof(bytes));
    for (const auto& row : rows) {
        if (row.category != none) plain_string(page, catalog.categories[row.category]);
    }
}

}  // namespace

struct UserWriter::Impl {
    fs::path path;
    const Catalog& catalog;
    std::unique_ptr<File> file;
    std::vector<UserDay> rows;

    void flush() {
        if (!file) file = std::make_unique<File>(path, user_columns);
        file->add_group(rows.size(), [&](auto column, std::string& page) {
            switch (column) {
            case 0:
                for (const auto& row : rows) {
                    char buffer[9];
                    plain_string(page, key_view(row.person, buffer));
                }
                break;
            case 1: for (const auto& row : rows) little(page, row.day); break;
            case 2: for (const auto& row : rows) little(page, row.sessions); break;
            case 3: for (const auto& row : rows) little(page, row.events); break;
            case 4: for (const auto& row : rows) little(page, row.revenue); break;
            case 5: category_page(page, catalog, rows); break;
            }
        });
        rows.clear();
    }
};

UserWriter::UserWriter(fs::path path, const Catalog& catalog)
    : impl_(std::make_unique<Impl>(Impl{std::move(path), catalog, {}, {}})) {
    impl_->rows.reserve(row_group_size);
}
UserWriter::~UserWriter() = default;

void UserWriter::append(const UserDay& row) {
    impl_->rows.push_back(row);
    if (impl_->rows.size() == row_group_size) impl_->flush();
}

void UserWriter::close(bool write_empty) {
    if (!impl_->rows.empty() || (write_empty && !impl_->file)) impl_->flush();
    if (impl_->file) impl_->file->close();
}

void write_campaigns(const fs::path& path, const Catalog& catalog, const CampaignTotals& totals) {
    struct Row {
        std::uint32_t campaign;
        std::int32_t day;
        const CampaignDay* info;
    };
    std::vector<Row> rows;
    const auto& values = totals.values();
    for (std::size_t campaign = 0; campaign < catalog.campaigns.size(); ++campaign) {
        for (std::size_t day = 0; day < totals.day_count(); ++day) {
            const auto& info = values[campaign * totals.day_count() + day];
            if (info.sessions || info.assists)
                rows.push_back({static_cast<std::uint32_t>(campaign),
                                totals.first_day() + static_cast<std::int32_t>(day), &info});
        }
    }
    File file(path, campaign_columns);
    for (std::size_t begin = 0; begin < rows.size(); begin += row_group_size) {
        const auto batch = std::span(rows).subspan(begin, std::min(row_group_size, rows.size() - begin));
        file.add_group(batch.size(), [&](auto column, std::string& page) {
            switch (column) {
            case 0: for (const auto& r : batch) plain_string(page, catalog.campaigns[r.campaign].name); break;
            case 1: for (const auto& r : batch) plain_string(page, catalog.campaigns[r.campaign].channel); break;
            case 2: for (const auto& r : batch) little(page, r.day); break;
            case 3: for (const auto& r : batch) little(page, r.info->sessions); break;
            case 4: for (const auto& r : batch) little(page, r.info->purchases); break;
            case 5: for (const auto& r : batch) little(page, r.info->revenue); break;
            case 6: for (const auto& r : batch) little(page, r.info->assists); break;
            }
        });
    }
    file.close();
}

}  // namespace shop
