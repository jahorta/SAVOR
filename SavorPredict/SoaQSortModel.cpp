#include "SoaQSortModel.h"

#include <numeric>
#include <utility>

namespace savor::predict {
namespace {

int compare_keys(int lhs, int rhs) {
    return lhs - rhs;
}

void swap_entries(std::vector<int>& indices, std::vector<int>& keys, std::size_t lhs, std::size_t rhs) {
    using std::swap;
    swap(indices[lhs], indices[rhs]);
    swap(keys[lhs], keys[rhs]);
}

} // namespace

std::vector<int> soa_qsort_indices_by_key_ascending(const std::vector<int>& sort_keys) {
    std::vector<int> indices(sort_keys.size());
    std::iota(indices.begin(), indices.end(), 0);
    auto keys = sort_keys;

    if (keys.size() < 2) {
        return indices;
    }

    std::size_t count = keys.size();
    std::size_t uVar8 = (count >> 1) + 1;
    std::size_t pMidpoint = count >> 1;
    std::size_t pEnd = count - 1;

    while (true) {
        if (uVar8 < 2) {
            swap_entries(indices, keys, pMidpoint, pEnd);
            --count;
            if (count == 1) {
                return indices;
            }
            --pEnd;
        } else {
            --pMidpoint;
            --uVar8;
        }

        std::size_t pvVar6 = uVar8 - 1;
        std::size_t uVar7 = uVar8;
        while ((uVar7 << 1) <= count) {
            uVar7 *= 2;
            std::size_t pvVar5 = uVar7 - 1;

            if (uVar7 < count) {
                const int compare_siblings = compare_keys(keys[pvVar5], keys[pvVar5 + 1]);
                if (compare_siblings < 0) {
                    ++uVar7;
                    ++pvVar5;
                }
            }

            const int compare_parent = compare_keys(keys[pvVar6], keys[pvVar5]);
            if (compare_parent >= 0) {
                break;
            }

            swap_entries(indices, keys, pvVar6, pvVar5);
            pvVar6 = pvVar5;
        }
    }
}

} // namespace savor::predict
