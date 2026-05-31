#pragma once

#include <unordered_map>
#include <vector>

namespace NQqb {

template<typename T, typename Hash = std::hash<T>, typename Equal = std::equal_to<T>>
class TUnionFind {
public:
    int Touch(const T& elem) {
        auto [it, inserted] = Index_.emplace(elem, static_cast<int>(Parent_.size()));
        if (inserted) {
            Parent_.push_back(it->second);
            Rank_.push_back(0);
        }
        return it->second;
    }

    bool Union(const T& a, const T& b) {
        int ra = Find(Touch(a));
        int rb = Find(Touch(b));
        if (ra == rb) {
            return false;
        }
        if (Rank_[ra] < Rank_[rb]) {
            std::swap(ra, rb);
        }
        Parent_[rb] = ra;
        if (Rank_[ra] == Rank_[rb]) {
            ++Rank_[ra];
        }
        return true;
    }

    int Find(const T& elem) {
        return Find(Touch(elem));
    }

    bool Connected(const T& a, const T& b) {
        auto ia = Index_.find(a);
        auto ib = Index_.find(b);
        if (ia == Index_.end() || ib == Index_.end()) {
            return false;
        }
        return Find(ia->second) == Find(ib->second);
    }

    int Size() const { return static_cast<int>(Parent_.size()); }

private:
    int Find(int id) {
        while (Parent_[id] != id) {
            Parent_[id] = Parent_[Parent_[id]];
            id = Parent_[id];
        }
        return id;
    }

    std::unordered_map<T, int, Hash, Equal> Index_;
    std::vector<int> Parent_;
    std::vector<int> Rank_;
};

} // namespace NQqb
