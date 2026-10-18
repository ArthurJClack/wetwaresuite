#include "achclwss/core/workspace.hpp"

namespace achclwss {

std::string Workspace::add(std::shared_ptr<Document> doc, const std::string& prefix) {
    std::string id;
    do {
        id = prefix + std::to_string(++counters_[prefix]);
    } while (docs_.count(id));
    docs_[id] = std::move(doc);
    return id;
}

void Workspace::put(const std::string& id, std::shared_ptr<Document> doc) { docs_[id] = std::move(doc); }

std::shared_ptr<Document> Workspace::get(const std::string& id) const {
    auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : it->second;
}

std::shared_ptr<Document> Workspace::take(const std::string& id) {
    auto it = docs_.find(id);
    if (it == docs_.end()) return nullptr;
    auto doc = std::move(it->second);
    docs_.erase(it);
    return doc;
}

std::vector<std::string> Workspace::ids() const {
    std::vector<std::string> out;
    for (const auto& [id, doc] : docs_) out.push_back(id);
    return out;
}

}  // namespace achclwss
