/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "DataMgr/MemoryLevel.h"
#include "Shared/sqltypes.h"
#include "Shared/toString.h"

#include <memory>
#include <unordered_set>

struct TableRef {
  TableRef(int db_id_, int table_id_) : db_id(db_id_), table_id(table_id_) {}

  int db_id;
  int table_id;

  bool operator==(const TableRef& other) const {
    return table_id == other.table_id && db_id == other.db_id;
  }

  bool operator!=(const TableRef& other) const { return !(*this == other); }

  bool operator<(const TableRef& other) const {
    return std::tie(db_id, table_id) < std::tie(other.db_id, other.table_id);
  }

  std::string toString() const {
    return ::typeName(this) + "(db_id=" + std::to_string(db_id) +
           ", table_id=" + std::to_string(table_id) + ")";
  }
};

using TableRefSet = std::unordered_set<TableRef>;

struct TableInfo : public TableRef {
  TableInfo(int db_id,
            int table_id,
            const std::string name_,
            bool is_view_,
            size_t fragments_,
            size_t row_count_,
            bool is_stream_ = false)
      : TableRef(db_id, table_id)
      , name(name_)
      , is_view(is_view_)
      , fragments(fragments_)
      , row_count(row_count_)
      , is_stream(is_stream_) {}

  std::string name;
  bool is_view;
  // For addWindowFunctionPreProject.
  size_t fragments;
  size_t row_count;
  bool is_stream;

  std::string toString() const {
    return name + "(db_id=" + std::to_string(db_id) +
           ", table_id=" + std::to_string(table_id) +
           " fragments=" + std::to_string(fragments) + (is_view ? " [view]) " : ")");
  }
};

using TableInfoPtr = std::shared_ptr<TableInfo>;
using TableInfoList = std::vector<TableInfoPtr>;
using TableInfoMap = std::unordered_map<TableRef, TableInfoPtr>;

namespace std {

template <>
struct hash<TableRef> {
  size_t operator()(const TableRef& col) const { return col.db_id ^ col.table_id; }
};

}  // namespace std
