#pragma once

#include "base.hpp"
#include <switch.h>
#include <span>

namespace sphaira::yati::container {

struct Nsp final : Base {
    using Base::Base;
    Result GetCollections(Collections& out) override;
    Result GetCollections(Collections& out, s64 off);
    Result GetCollections(Collections& out, s64 off, s64 container_size);

    auto GetDataOffset() const {
        return m_data_offset;
    }

    // builds nsp meta data and the size of the entier nsp.
    static auto Build(std::span<const CollectionEntry> collections, s64& size) -> std::vector<u8>;

private:
    s64 m_data_offset{};
};

} // namespace sphaira::yati::container
