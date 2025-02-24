// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include "SegmentInterface.h"

#include <cstdint>

#include "Utils.h"
#include "common/EasyAssert.h"
#include "common/SystemProperty.h"
#include "common/Tracer.h"
#include "common/Types.h"
#include "query/generated/ExecPlanNodeVisitor.h"

namespace milvus::segcore {

void
SegmentInternalInterface::FillPrimaryKeys(const query::Plan* plan,
                                          SearchResult& results) const {
    std::shared_lock lck(mutex_);
    AssertInfo(plan, "empty plan");
    auto size = results.distances_.size();
    AssertInfo(results.seg_offsets_.size() == size,
               "Size of result distances is not equal to size of ids");
    Assert(results.primary_keys_.size() == 0);
    results.primary_keys_.resize(size);

    auto pk_field_id_opt = get_schema().get_primary_field_id();
    AssertInfo(pk_field_id_opt.has_value(),
               "Cannot get primary key offset from schema");
    auto pk_field_id = pk_field_id_opt.value();
    AssertInfo(IsPrimaryKeyDataType(get_schema()[pk_field_id].get_data_type()),
               "Primary key field is not INT64 or VARCHAR type");

    auto field_data =
        bulk_subscript(pk_field_id, results.seg_offsets_.data(), size);
    results.pk_type_ = DataType(field_data->type());

    ParsePksFromFieldData(results.primary_keys_, *field_data.get());
}

void
SegmentInternalInterface::FillTargetEntry(const query::Plan* plan,
                                          SearchResult& results) const {
    std::shared_lock lck(mutex_);
    AssertInfo(plan, "empty plan");
    auto size = results.distances_.size();
    AssertInfo(results.seg_offsets_.size() == size,
               "Size of result distances is not equal to size of ids");

    // fill other entries except primary key by result_offset
    for (auto field_id : plan->target_entries_) {
        auto field_data =
            bulk_subscript(field_id, results.seg_offsets_.data(), size);
        results.output_fields_data_[field_id] = std::move(field_data);
    }
}

std::unique_ptr<SearchResult>
SegmentInternalInterface::Search(
    const query::Plan* plan,
    const query::PlaceholderGroup* placeholder_group,
    Timestamp timestamp) const {
    std::shared_lock lck(mutex_);
    milvus::tracer::AddEvent("obtained_segment_lock_mutex");
    check_search(plan);
    query::ExecPlanNodeVisitor visitor(*this, timestamp, placeholder_group);
    auto results = std::make_unique<SearchResult>();
    *results = visitor.get_moved_result(*plan->plan_node_);
    results->segment_ = (void*)this;
    return results;
}

std::unique_ptr<proto::segcore::RetrieveResults>
SegmentInternalInterface::Retrieve(const query::RetrievePlan* plan,
                                   Timestamp timestamp,
                                   int64_t limit_size) const {
    std::shared_lock lck(mutex_);
    auto results = std::make_unique<proto::segcore::RetrieveResults>();
    query::ExecPlanNodeVisitor visitor(*this, timestamp);
    auto retrieve_results = visitor.get_retrieve_result(*plan->plan_node_);
    retrieve_results.segment_ = (void*)this;

    auto result_rows = retrieve_results.result_offsets_.size();
    int64_t output_data_size = 0;
    for (auto field_id : plan->field_ids_) {
        output_data_size += get_field_avg_size(field_id) * result_rows;
    }
    if (output_data_size > limit_size) {
        throw SegcoreError(
            RetrieveError,
            fmt::format("query results exceed the limit size ", limit_size));
    }

    if (plan->plan_node_->is_count_) {
        AssertInfo(retrieve_results.field_data_.size() == 1,
                   "count result should only have one column");
        *results->add_fields_data() = retrieve_results.field_data_[0];
        return results;
    }

    results->mutable_offset()->Add(retrieve_results.result_offsets_.begin(),
                                   retrieve_results.result_offsets_.end());

    auto fields_data = results->mutable_fields_data();
    auto ids = results->mutable_ids();
    auto pk_field_id = plan->schema_.get_primary_field_id();
    for (auto field_id : plan->field_ids_) {
        if (SystemProperty::Instance().IsSystem(field_id)) {
            auto system_type =
                SystemProperty::Instance().GetSystemFieldType(field_id);

            auto size = retrieve_results.result_offsets_.size();
            FixedVector<int64_t> output(size);
            bulk_subscript(system_type,
                           retrieve_results.result_offsets_.data(),
                           size,
                           output.data());

            auto data_array = std::make_unique<DataArray>();
            data_array->set_field_id(field_id.get());
            data_array->set_type(milvus::proto::schema::DataType::Int64);

            auto scalar_array = data_array->mutable_scalars();
            auto data = reinterpret_cast<const int64_t*>(output.data());
            auto obj = scalar_array->mutable_long_data();
            obj->mutable_data()->Add(data, data + size);
            fields_data->AddAllocated(data_array.release());
            continue;
        }

        auto& field_meta = plan->schema_[field_id];

        auto col = bulk_subscript(field_id,
                                  retrieve_results.result_offsets_.data(),
                                  retrieve_results.result_offsets_.size());
        if (field_meta.get_data_type() == DataType::ARRAY) {
            col->mutable_scalars()->mutable_array_data()->set_element_type(
                proto::schema::DataType(field_meta.get_element_type()));
        }
        auto col_data = col.release();
        fields_data->AddAllocated(col_data);
        if (pk_field_id.has_value() && pk_field_id.value() == field_id) {
            switch (field_meta.get_data_type()) {
                case DataType::INT64: {
                    auto int_ids = ids->mutable_int_id();
                    auto& src_data = col_data->scalars().long_data();
                    int_ids->mutable_data()->Add(src_data.data().begin(),
                                                 src_data.data().end());
                    break;
                }
                case DataType::VARCHAR: {
                    auto str_ids = ids->mutable_str_id();
                    auto& src_data = col_data->scalars().string_data();
                    for (auto i = 0; i < src_data.data_size(); ++i) {
                        *(str_ids->mutable_data()->Add()) = src_data.data(i);
                    }
                    break;
                }
                default: {
                    PanicInfo(DataTypeInvalid,
                              fmt::format("unsupported datatype {}",
                                          field_meta.get_data_type()));
                }
            }
        }
    }
    return results;
}

int64_t
SegmentInternalInterface::get_real_count() const {
#if 0
    auto insert_cnt = get_row_count();
    BitsetType bitset_holder;
    bitset_holder.resize(insert_cnt, false);
    mask_with_delete(bitset_holder, insert_cnt, MAX_TIMESTAMP);
    return bitset_holder.size() - bitset_holder.count();
#endif
    auto plan = std::make_unique<query::RetrievePlan>(get_schema());
    plan->plan_node_ = std::make_unique<query::RetrievePlanNode>();
    plan->plan_node_->is_count_ = true;
    auto res = Retrieve(plan.get(), MAX_TIMESTAMP, INT64_MAX);
    AssertInfo(res->fields_data().size() == 1,
               "count result should only have one column");
    AssertInfo(res->fields_data()[0].has_scalars(),
               "count result should match scalar");
    AssertInfo(res->fields_data()[0].scalars().has_long_data(),
               "count result should match long data");
    AssertInfo(res->fields_data()[0].scalars().long_data().data_size() == 1,
               "count result should only have one row");
    return res->fields_data()[0].scalars().long_data().data(0);
}

int64_t
SegmentInternalInterface::get_field_avg_size(FieldId field_id) const {
    AssertInfo(field_id.get() >= 0,
               "invalid field id, should be greater than or equal to 0");
    if (SystemProperty::Instance().IsSystem(field_id)) {
        if (field_id == TimestampFieldID || field_id == RowFieldID) {
            return sizeof(int64_t);
        }

        throw SegcoreError(FieldIDInvalid, "unsupported system field id");
    }

    auto schema = get_schema();
    auto& field_meta = schema[field_id];
    auto data_type = field_meta.get_data_type();

    std::shared_lock lck(mutex_);
    if (datatype_is_variable(data_type)) {
        if (variable_fields_avg_size_.find(field_id) ==
            variable_fields_avg_size_.end()) {
            return 0;
        }

        return variable_fields_avg_size_.at(field_id).second;
    } else {
        return field_meta.get_sizeof();
    }
}

void
SegmentInternalInterface::set_field_avg_size(FieldId field_id,
                                             int64_t num_rows,
                                             int64_t field_size) {
    AssertInfo(field_id.get() >= 0,
               "invalid field id, should be greater than or equal to 0");
    auto schema = get_schema();
    auto& field_meta = schema[field_id];
    auto data_type = field_meta.get_data_type();

    std::unique_lock lck(mutex_);
    if (datatype_is_variable(data_type)) {
        AssertInfo(num_rows > 0,
                   "The num rows of field data should be greater than 0");
        if (variable_fields_avg_size_.find(field_id) ==
            variable_fields_avg_size_.end()) {
            variable_fields_avg_size_.emplace(field_id, std::make_pair(0, 0));
        }

        auto& field_info = variable_fields_avg_size_.at(field_id);
        auto size = field_info.first * field_info.second + field_size;
        field_info.first = field_info.first + num_rows;
        field_info.second = size / field_info.first;
    }
}

void
SegmentInternalInterface::timestamp_filter(BitsetType& bitset,
                                           Timestamp timestamp) const {
    auto& timestamps = get_timestamps();
    auto cnt = bitset.size();
    if (timestamps[cnt - 1] <= timestamp) {
        // no need to filter out anything.
        return;
    }

    auto pilot = upper_bound(timestamps, 0, cnt, timestamp);
    // offset bigger than pilot should be filtered out.
    for (int offset = pilot; offset < cnt; offset = bitset.find_next(offset)) {
        if (offset == BitsetType::npos) {
            return;
        }
        bitset[offset] = false;
    }
}

void
SegmentInternalInterface::timestamp_filter(BitsetType& bitset,
                                           const std::vector<int64_t>& offsets,
                                           Timestamp timestamp) const {
    auto& timestamps = get_timestamps();
    auto cnt = bitset.size();
    if (timestamps[cnt - 1] <= timestamp) {
        // no need to filter out anything.
        return;
    }

    // point query, faster than binary search.
    for (auto& offset : offsets) {
        if (timestamps[offset] > timestamp) {
            bitset.set(offset, true);
        }
    }
}

const SkipIndex&
SegmentInternalInterface::GetSkipIndex() const {
    return skipIndex_;
}

void
SegmentInternalInterface::LoadPrimitiveSkipIndex(milvus::FieldId field_id,
                                                 int64_t chunk_id,
                                                 milvus::DataType data_type,
                                                 const void* chunk_data,
                                                 int64_t count) {
    skipIndex_.LoadPrimitive(field_id, chunk_id, data_type, chunk_data, count);
}

void
SegmentInternalInterface::LoadStringSkipIndex(
    milvus::FieldId field_id,
    int64_t chunk_id,
    const milvus::VariableColumn<std::string>& var_column) {
    skipIndex_.LoadString(field_id, chunk_id, var_column);
}

void
SegmentInternalInterface::check_metric_type(
    const query::Plan* plan, const IndexMetaPtr index_meta) const {
    auto& metric_str = plan->plan_node_->search_info_.metric_type_;
    auto searched_field_id = plan->plan_node_->search_info_.field_id_;
    auto field_index_meta =
        index_meta->GetFieldIndexMeta(FieldId(searched_field_id));
    if (metric_str.empty()) {
        metric_str = field_index_meta.GeMetricType();
    }
    if (metric_str != field_index_meta.GeMetricType()) {
        throw SegcoreError(
            MetricTypeNotMatch,
            fmt::format("metric type not match, expected {}, actual {}.",
                        field_index_meta.GeMetricType(),
                        metric_str));
    }
}

}  // namespace milvus::segcore
