// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/client/scan_batch.h"

#include <cstring>
#include <string>

#include <glog/logging.h>

#include "kudu/client/row_result.h"
#include "kudu/client/scanner-internal.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/bitmap.h"
#include "kudu/util/int128.h"
#include "kudu/util/logging.h"

using std::string;
using strings::Substitute;

namespace kudu {
namespace client {

////////////////////////////////////////////////////////////
// KuduScanBatch
////////////////////////////////////////////////////////////

KuduScanBatch::KuduScanBatch() : data_(new Data()) {}

KuduScanBatch::~KuduScanBatch() {
  delete data_;
}

int KuduScanBatch::NumRows() const {
  return data_->num_rows();
}

KuduRowResult KuduScanBatch::Row(int idx) const {
  return data_->row(idx);
}

const KuduSchema* KuduScanBatch::projection_schema() const {
  return data_->client_projection_;
}

Slice KuduScanBatch::direct_data() const {
  return data_->direct_data_;
}

Slice KuduScanBatch::indirect_data() const {
  return data_->indirect_data_;
}

////////////////////////////////////////////////////////////
// KuduScanBatch::RowPtr
////////////////////////////////////////////////////////////

namespace {

// Just enough of a "cell" to support the Schema::DebugCellAppend calls
// made by KuduScanBatch::RowPtr::ToString.
class RowCell {
 public:
  RowCell(const KuduScanBatch::RowPtr* row, int idx)
    : row_(row),
      col_idx_(idx) {
  }

  bool is_null() const {
    return row_->IsNull(col_idx_);
  }
  const void* ptr() const {
    return row_->cell(col_idx_);
  }

 private:
  const KuduScanBatch::RowPtr* row_;
  const int col_idx_;
};

Status BadTypeStatus(const char* provided_type_name, const ColumnSchema& col) {
  return Status::InvalidArgument(
      Substitute("invalid type $0 provided for column '$1' (expected $2)",
                 provided_type_name, col.name(), col.type_info()->name()));
}

} // anonymous namespace

bool KuduScanBatch::RowPtr::IsNull(int col_idx) const {
  const ColumnSchema& col = schema_->column(col_idx);
  if (!col.is_nullable()) {
    return false;
  }

  return BitmapTest(row_data_ + schema_->byte_size(), col_idx);
}

bool KuduScanBatch::RowPtr::IsNull(const Slice& col_name) const {
  int col_idx;
  CHECK_OK(schema_->FindColumn(col_name, &col_idx));
  return IsNull(col_idx);
}

Status KuduScanBatch::RowPtr::IsDeleted(bool* val) const {
  int col_idx = schema_->first_is_deleted_virtual_column_idx();
  if (col_idx == Schema::kColumnNotFound) {
    return Status::NotFound("IS_DELETED virtual column not found");
  }
  return Get<TypeTraits<IS_DELETED> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetBool(const Slice& col_name, bool* val) const {
  return Get<TypeTraits<BOOL> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetInt8(const Slice& col_name, int8_t* val) const {
  return Get<TypeTraits<INT8> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetInt16(const Slice& col_name, int16_t* val) const {
  return Get<TypeTraits<INT16> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetInt32(const Slice& col_name, int32_t* val) const {
  return Get<TypeTraits<INT32> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetInt64(const Slice& col_name, int64_t* val) const {
  return Get<TypeTraits<INT64> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetUnixTimeMicros(const Slice& col_name,
                                                int64_t* micros_since_utc_epoch) const {
  return Get<TypeTraits<UNIXTIME_MICROS> >(col_name, micros_since_utc_epoch);
}

Status KuduScanBatch::RowPtr::GetDate(const Slice& col_name, int32_t* days_since_unix_epoch) const {
  return Get<TypeTraits<DATE> >(col_name, days_since_unix_epoch);
}

Status KuduScanBatch::RowPtr::GetFloat(const Slice& col_name, float* val) const {
  return Get<TypeTraits<FLOAT> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetDouble(const Slice& col_name, double* val) const {
  return Get<TypeTraits<DOUBLE> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetUnscaledDecimal(const Slice& col_name, int128_t* val) const {
  int col_idx;
  RETURN_NOT_OK(schema_->FindColumn(col_name, &col_idx));
  return GetUnscaledDecimal(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetString(const Slice& col_name, Slice* val) const {
  return Get<TypeTraits<STRING> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetBinary(const Slice& col_name, Slice* val) const {
  return Get<TypeTraits<BINARY> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetVarchar(const Slice& col_name, Slice* val) const {
  return Get<TypeTraits<VARCHAR> >(col_name, val);
}

Status KuduScanBatch::RowPtr::GetBool(int col_idx, bool* val) const {
  return Get<TypeTraits<BOOL> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetInt8(int col_idx, int8_t* val) const {
  return Get<TypeTraits<INT8> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetInt16(int col_idx, int16_t* val) const {
  return Get<TypeTraits<INT16> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetInt32(int col_idx, int32_t* val) const {
  return Get<TypeTraits<INT32> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetInt64(int col_idx, int64_t* val) const {
  return Get<TypeTraits<INT64> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetUnixTimeMicros(int col_idx,
                                                int64_t* micros_since_utc_epoch) const {
  return Get<TypeTraits<UNIXTIME_MICROS> >(col_idx, micros_since_utc_epoch);
}

Status KuduScanBatch::RowPtr::GetDate(int col_idx, int32_t* days_since_unix_epoch) const {
  return Get<TypeTraits<DATE> >(col_idx, days_since_unix_epoch);
}

Status KuduScanBatch::RowPtr::GetFloat(int col_idx, float* val) const {
  return Get<TypeTraits<FLOAT> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetDouble(int col_idx, double* val) const {
  return Get<TypeTraits<DOUBLE> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetString(int col_idx, Slice* val) const {
  return Get<TypeTraits<STRING> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetBinary(int col_idx, Slice* val) const {
  return Get<TypeTraits<BINARY> >(col_idx, val);
}

Status KuduScanBatch::RowPtr::GetVarchar(int col_idx, Slice* val) const {
  return Get<TypeTraits<VARCHAR> >(col_idx, val);
}

template<typename T>
Status KuduScanBatch::RowPtr::Get(const Slice& col_name, typename T::cpp_type* val) const {
  int col_idx;
  RETURN_NOT_OK(schema_->FindColumn(col_name, &col_idx));
  return Get<T>(col_idx, val);
}

template<typename T>
Status KuduScanBatch::RowPtr::Get(int col_idx, typename T::cpp_type* val) const {
  const ColumnSchema& col = schema_->column(col_idx);
  if (PREDICT_FALSE(col.type_info()->type() != T::type)) {
    // TODO(todd): at some point we could allow type coercion here.
    // Explicitly out-of-line the construction of this Status in order to
    // keep the getter code footprint as small as possible.
    return BadTypeStatus(T::name(), col);
  }

  if (PREDICT_FALSE(col.is_nullable() && IsNull(col_idx))) {
    return Status::NotFound("column is NULL");
  }

  memcpy(val, row_data_ + schema_->column_offset(col_idx), sizeof(*val));
  return Status::OK();
}

const void* KuduScanBatch::RowPtr::cell(int col_idx) const {
  return row_data_ + schema_->column_offset(col_idx);
}

//------------------------------------------------------------
// Template instantiations: We instantiate all possible templates to avoid linker issues.
// see: https://isocpp.org/wiki/faq/templates#separate-template-fn-defn-from-decl
// TODO We can probably remove this when we move to c++11 and can use "extern template"
//------------------------------------------------------------

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<BOOL> >(const Slice& col_name, bool* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT8> >(const Slice& col_name, int8_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT16> >(const Slice& col_name, int16_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT32> >(const Slice& col_name, int32_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT64> >(const Slice& col_name, int64_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT128> >(const Slice& col_name, int128_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<UNIXTIME_MICROS> >(
    const Slice& col_name, int64_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DATE> >(const Slice& col_name, int32_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<FLOAT> >(const Slice& col_name, float* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DOUBLE> >(const Slice& col_name, double* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<STRING> >(const Slice& col_name, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<BINARY> >(const Slice& col_name, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<VARCHAR> >(const Slice& col_name, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<BOOL> >(int col_idx, bool* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT8> >(int col_idx, int8_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT16> >(int col_idx, int16_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT32> >(int col_idx, int32_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT64> >(int col_idx, int64_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<INT128> >(int col_idx, int128_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<UNIXTIME_MICROS> >(int col_idx, int64_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DATE> >(int col_idx, int32_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<FLOAT> >(int col_idx, float* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DOUBLE> >(int col_idx, double* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<STRING> >(int col_idx, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<BINARY> >(int col_idx, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<VARCHAR> >(int col_idx, Slice* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DECIMAL32> >(int col_idx, int32_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DECIMAL64> >(int col_idx, int64_t* val) const;

template
Status KuduScanBatch::RowPtr::Get<TypeTraits<DECIMAL128> >(int col_idx, int128_t* val) const;

Status KuduScanBatch::RowPtr::GetUnscaledDecimal(int col_idx, int128_t* val) const {
  const ColumnSchema& col = schema_->column(col_idx);
  const DataType col_type = col.type_info()->type();
  switch (col_type) {
    case DECIMAL32:
      int32_t i32_val;
      RETURN_NOT_OK(Get<TypeTraits<DECIMAL32> >(col_idx, &i32_val));
      *val = i32_val;
      return Status::OK();
    case DECIMAL64:
      int64_t i64_val;
      RETURN_NOT_OK(Get<TypeTraits<DECIMAL64> >(col_idx, &i64_val));
      *val = i64_val;
      return Status::OK();
    case DECIMAL128:
      int128_t i128_val;
      RETURN_NOT_OK(Get<TypeTraits<DECIMAL128> >(col_idx, &i128_val));
      *val = i128_val;
      return Status::OK();
    default:
      return Status::InvalidArgument(
          Substitute("invalid type $0 provided for column '$1' (expected decimal)",
                     col.type_info()->name(), col.name()));
  }
}

string KuduScanBatch::RowPtr::ToString() const {
  // Client-users calling ToString() will likely expect it to not be redacted.
  ScopedDisableRedaction no_redaction;

  string ret;
  ret.append("(");
  bool first = true;
  for (int i = 0; i < schema_->num_columns(); i++) {
    if (!first) {
      ret.append(", ");
    }
    RowCell cell(this, i);
    schema_->column(i).DebugCellAppend(cell, &ret);
    first = false;
  }
  ret.append(")");
  return ret;
}

string* KuduScanBatch::RowPtr::ToCSVRowString(std::string& ret) const {
  ret.clear();
  //returned ret=1,2,"efg"
  //string value will be double quatoted
  //string value handled by c style escaping and csv escaping
  ScopedDisableRedaction no_redaction;
  bool first = true;
  for (int i = 0; i < schema_->num_columns(); i++) {
    if (!first) {
      ret.append(",");
    }
    RowCell cell(this, i);
    schema_->column(i).DebugCSVCellAppend(cell, &ret);
    first=false;
  }
  return &ret;
}

} // namespace client
} // namespace kudu
