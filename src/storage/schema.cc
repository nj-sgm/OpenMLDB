//
// schema.cc
// Copyright (C) 2017 4paradigm.com
// Author denglong
// Date 2020-03-11
//

#include "storage/schema.h"

#include <utility>

namespace rtidb {
namespace storage {

ColumnDef::ColumnDef(const std::string& name, uint32_t id,
                     ::rtidb::type::DataType type)
    : name_(name), id_(id), type_(type) {}

std::shared_ptr<ColumnDef> TableColumn::GetColumn(uint32_t idx) {
    if (idx < columns_.size()) {
        return columns_.at(idx);
    }
    return std::shared_ptr<ColumnDef>();
}

std::shared_ptr<ColumnDef> TableColumn::GetColumn(const std::string& name) {
    auto it = column_map_.find(name);
    if (it != column_map_.end()) {
        return it->second;
    } else {
        return std::shared_ptr<ColumnDef>();
    }
}

const std::vector<std::shared_ptr<ColumnDef>>& TableColumn::GetAllColumn() {
    return columns_;
}

const std::vector<uint32_t>& TableColumn::GetBlobIdxs() {
    return blob_idxs;
}

void TableColumn::AddColumn(std::shared_ptr<ColumnDef> column_def) {
    columns_.push_back(column_def);
    column_map_.insert(std::make_pair(column_def->GetName(), column_def));
    if (column_def->GetType() == rtidb::type::kBlob) {
        blob_idxs.push_back(column_def->GetId());
    }
}

IndexDef::IndexDef(const std::string& name, uint32_t id)
    : name_(name), index_id_(id), status_(IndexStatus::kReady) {}

IndexDef::IndexDef(const std::string& name, uint32_t id, IndexStatus status)
    : name_(name), index_id_(id), status_(status) {}

IndexDef::IndexDef(const std::string& name, uint32_t id,
                   const IndexStatus& status, ::rtidb::type::IndexType type,
                   const std::vector<ColumnDef>& columns)
    : name_(name),
      index_id_(id),
      status_(status),
      type_(type),
      columns_(columns) {}

bool ColumnDefSortFunc(const ColumnDef& cd_a, const ColumnDef& cd_b) {
    return (cd_a.GetId() < cd_b.GetId());
}

TableIndex::TableIndex() {
    indexs_ = std::make_shared<std::vector<std::shared_ptr<IndexDef>>>();
    pk_index_ = std::shared_ptr<IndexDef>();
    combine_col_name_map_ = std::make_shared<
        std::unordered_map<std::string, std::shared_ptr<IndexDef>>>();
    col_name_vec_ = std::make_shared<std::vector<std::string>>();
}

void TableIndex::ReSet() {
    auto new_indexs =
        std::make_shared<std::vector<std::shared_ptr<IndexDef>>>();
    std::atomic_store_explicit(&indexs_, new_indexs, std::memory_order_relaxed);
    pk_index_ = std::shared_ptr<IndexDef>();
    auto new_map = std::make_shared<
        std::unordered_map<std::string, std::shared_ptr<IndexDef>>>();
    std::atomic_store_explicit(&combine_col_name_map_, new_map,
                               std::memory_order_relaxed);
    auto new_vec = std::make_shared<std::vector<std::string>>();
    std::atomic_store_explicit(&col_name_vec_, new_vec,
            std::memory_order_relaxed);
}

std::shared_ptr<IndexDef> TableIndex::GetIndex(uint32_t idx) {
    auto indexs =
        std::atomic_load_explicit(&indexs_, std::memory_order_relaxed);
    if (idx < indexs->size()) {
        return indexs->at(idx);
    }
    return std::shared_ptr<IndexDef>();
}

std::shared_ptr<IndexDef> TableIndex::GetIndex(const std::string& name) {
    auto indexs =
        std::atomic_load_explicit(&indexs_, std::memory_order_relaxed);
    for (const auto& index : *indexs) {
        if (index->GetName() == name) {
            return index;
        }
    }
    return std::shared_ptr<IndexDef>();
}

std::vector<std::shared_ptr<IndexDef>> TableIndex::GetAllIndex() {
    return *std::atomic_load_explicit(&indexs_, std::memory_order_relaxed);
}

int TableIndex::AddIndex(std::shared_ptr<IndexDef> index_def) {
    auto old_indexs =
        std::atomic_load_explicit(&indexs_, std::memory_order_relaxed);
    if (old_indexs->size() >= MAX_INDEX_NUM) {
        return -1;
    }
    auto new_indexs =
        std::make_shared<std::vector<std::shared_ptr<IndexDef>>>(*old_indexs);
    new_indexs->push_back(index_def);
    std::atomic_store_explicit(&indexs_, new_indexs, std::memory_order_relaxed);
    if (index_def->GetType() == ::rtidb::type::kPrimaryKey ||
        index_def->GetType() == ::rtidb::type::kAutoGen) {
        pk_index_ = index_def;
    }
    auto old_vec = std::atomic_load_explicit(&col_name_vec_,
            std::memory_order_relaxed);
    auto new_vec = std::make_shared<std::vector<std::string>>(*old_vec);
    std::string combine_name = "";
    int count = 0;
    for (auto& col_def : index_def->GetColumns()) {
        if (count++ > 0) {
            combine_name.append("_");
        }
        combine_name.append(col_def.GetName());
        new_vec->push_back(col_def.GetName());
    }
    std::atomic_store_explicit(&col_name_vec_, new_vec,
            std::memory_order_relaxed);
    auto old_map = std::atomic_load_explicit(&combine_col_name_map_,
                                             std::memory_order_relaxed);
    auto new_map = std::make_shared<
        std::unordered_map<std::string, std::shared_ptr<IndexDef>>>(*old_map);
    new_map->insert(std::make_pair(combine_name, index_def));
    std::atomic_store_explicit(&combine_col_name_map_, new_map,
                               std::memory_order_relaxed);
    return 0;
}

bool TableIndex::HasAutoGen() {
    if (pk_index_->GetType() == ::rtidb::type::kAutoGen) {
        return true;
    }
    return false;
}

std::shared_ptr<IndexDef> TableIndex::GetPkIndex() { return pk_index_; }

const std::shared_ptr<IndexDef> TableIndex::GetIndexByCombineStr(
    const std::string& combine_str) {
    auto map = std::atomic_load_explicit(&combine_col_name_map_,
                                         std::memory_order_relaxed);
    auto it = map->find(combine_str);
    if (it != map->end()) {
        return it->second;
    } else {
        return std::shared_ptr<IndexDef>();
    }
}

bool TableIndex::FindColName(const std::string& name) {
    auto vec = std::atomic_load_explicit(&col_name_vec_,
            std::memory_order_relaxed);
    auto iter = std::find(vec->begin(), vec->end(), name);
    if (iter == vec->end()) {
        return false;
    }
    return true;
}

}  // namespace storage
}  // namespace rtidb
