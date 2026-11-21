#include "model.hpp"

namespace shop {
namespace {

struct Component {
    Key name;
    std::uint32_t parent;
    std::uint32_t size = 1;
};

class Components {
public:
    explicit Components(std::size_t expected) : ids_(expected) { nodes_.reserve(expected); }

    void connect(Key a, Key b) {
        auto left = root(node(a));
        auto right = root(node(b));
        if (left == right) return;
        if (nodes_[left].size < nodes_[right].size) std::swap(left, right);
        nodes_[right].parent = left;
        nodes_[left].size += nodes_[right].size;
        if (KeyLess{}(nodes_[right].name, nodes_[left].name)) nodes_[left].name = nodes_[right].name;
    }

    KeyIndex index() {
        KeyIndex aliases(ids_.size() / 2);
        ids_.each([&](Key key, Key id) {
            const auto name = nodes_[root(static_cast<std::uint32_t>(id))].name;
            if (!KeyEqual{}(key, name)) aliases.insert(key, name);
        });
        return aliases;
    }
private:
    std::uint32_t node(Key name) {
        const auto id = static_cast<std::uint32_t>(nodes_.size());
        const auto [value, inserted] = ids_.insert(name, id);
        if (inserted) nodes_.push_back({name, id});
        return static_cast<std::uint32_t>(value);
    }

    std::uint32_t root(std::uint32_t at) {
        while (nodes_[at].parent != at) {
            nodes_[at].parent = nodes_[nodes_[at].parent].parent;
            at = nodes_[at].parent;
        }
        return at;
    }

    KeyIndex ids_;
    std::vector<Component> nodes_;
};

}  // namespace

Aliases read_links(const CsvFiles& files, Scanner scanner, Keys& keys) {
    std::size_t bytes = 0;
    for (const auto range : files.ranges) bytes += range.end - range.begin;
    Components components(bytes / 16);
    for (const auto range : files.ranges) {
        scan_csv<2>(range, scanner, [&](const auto& fields) {
            if (!fields[0].empty() && !fields[1].empty())
                components.connect(keys.pack(fields[0]), keys.pack(fields[1]));
        });
    }
    return Aliases(components.index());
}

Catalog::Catalog(const fs::path& data, Scanner scanner, Keys& keys)
    : products_file_(data / "products.csv"), campaigns_file_(data / "campaigns.csv") {
    std::vector<std::pair<Key, std::string_view>> product_categories;
    categories.push_back("unknown");
    scan_csv<2>(after_header(products_file_.bytes()), scanner, [&](const auto& fields) {
        if (fields[0].empty()) return;
        product_categories.emplace_back(keys.pack(fields[0]), fields[1]);
        categories.push_back(fields[1]);
    });
    std::sort(categories.begin(), categories.end());
    categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
    const auto category = [&](std::string_view name) {
        return static_cast<std::uint32_t>(
            std::lower_bound(categories.begin(), categories.end(), name) - categories.begin());
    };
    unknown = category("unknown");
    products = KeyIndex(product_categories.size());
    for (const auto& [id, name] : product_categories) products.insert(id, category(name));

    campaigns.push_back({"organic", "organic"});
    scan_csv<2>(after_header(campaigns_file_.bytes()), scanner, [&](const auto& fields) {
        if (fields[0].empty()) return;
        ads.insert(keys.pack(fields[0]), campaigns.size());
        campaigns.push_back({fields[0], fields[1]});
    });
}

}  // namespace shop
