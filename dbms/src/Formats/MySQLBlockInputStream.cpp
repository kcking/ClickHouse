#include "config_core.h"
#if USE_MYSQL

#    include <vector>
#    include <Columns/ColumnNullable.h>
#    include <Columns/ColumnString.h>
#    include <Columns/ColumnsNumber.h>
#    include <IO/ReadHelpers.h>
#    include <IO/WriteHelpers.h>
#    include <ext/range.h>
#    include "MySQLBlockInputStream.h"


namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
}


MySQLBlockInputStream::MySQLBlockInputStream(
    const mysqlxx::PoolWithFailover::Entry & entry_, const std::string & query_str, const Block & sample_block, const UInt64 max_block_size_, const bool auto_close_)
    : entry{entry_}, query{this->entry->query(query_str)}, result{query.use()}, max_block_size{max_block_size_}, auto_close{auto_close_}
{
    if (sample_block.columns() != result.getNumFields())
        throw Exception{"mysqlxx::UseQueryResult contains " + toString(result.getNumFields()) + " columns while "
                            + toString(sample_block.columns()) + " expected",
                        ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH};

    description.init(sample_block);
}


namespace
{
    using ValueType = ExternalResultDescription::ValueType;

    void insertValue(IColumn & column, const ValueType type, const mysqlxx::Value & value)
    {
        switch (type)
        {
            case ValueType::vtUInt8:
                static_cast<ColumnUInt8 &>(column).insertValue(value.getUInt());
                break;
            case ValueType::vtUInt16:
                static_cast<ColumnUInt16 &>(column).insertValue(value.getUInt());
                break;
            case ValueType::vtUInt32:
                static_cast<ColumnUInt32 &>(column).insertValue(value.getUInt());
                break;
            case ValueType::vtUInt64:
                static_cast<ColumnUInt64 &>(column).insertValue(value.getUInt());
                break;
            case ValueType::vtInt8:
                static_cast<ColumnInt8 &>(column).insertValue(value.getInt());
                break;
            case ValueType::vtInt16:
                static_cast<ColumnInt16 &>(column).insertValue(value.getInt());
                break;
            case ValueType::vtInt32:
                static_cast<ColumnInt32 &>(column).insertValue(value.getInt());
                break;
            case ValueType::vtInt64:
                static_cast<ColumnInt64 &>(column).insertValue(value.getInt());
                break;
            case ValueType::vtFloat32:
                static_cast<ColumnFloat32 &>(column).insertValue(value.getDouble());
                break;
            case ValueType::vtFloat64:
                static_cast<ColumnFloat64 &>(column).insertValue(value.getDouble());
                break;
            case ValueType::vtString:
                static_cast<ColumnString &>(column).insertData(value.data(), value.size());
                break;
            case ValueType::vtDate:
                static_cast<ColumnUInt16 &>(column).insertValue(UInt16(value.getDate().getDayNum()));
                break;
            case ValueType::vtDateTime:
                static_cast<ColumnUInt32 &>(column).insertValue(UInt32(value.getDateTime()));
                break;
            case ValueType::vtUUID:
                static_cast<ColumnUInt128 &>(column).insert(parse<UUID>(value.data(), value.size()));
                break;
        }
    }

    void insertDefaultValue(IColumn & column, const IColumn & sample_column) { column.insertFrom(sample_column, 0); }
}


Block MySQLBlockInputStream::readImpl()
{
    auto row = result.fetch();
    if (!row)
    {
        if (auto_close)
           entry.disconnect();
        return {};
    }

    MutableColumns columns(description.sample_block.columns());
    for (const auto i : ext::range(0, columns.size()))
        columns[i] = description.sample_block.getByPosition(i).column->cloneEmpty();

    size_t num_rows = 0;
    while (row)
    {
        for (const auto idx : ext::range(0, row.size()))
        {
            const auto value = row[idx];
            if (!value.isNull())
            {
                if (description.types[idx].second)
                {
                    ColumnNullable & column_nullable = static_cast<ColumnNullable &>(*columns[idx]);
                    insertValue(column_nullable.getNestedColumn(), description.types[idx].first, value);
                    column_nullable.getNullMapData().emplace_back(0);
                }
                else
                    insertValue(*columns[idx], description.types[idx].first, value);
            }
            else
                insertDefaultValue(*columns[idx], *description.sample_block.getByPosition(idx).column);
        }

        ++num_rows;
        if (num_rows == max_block_size)
            break;

        row = result.fetch();
    }
    if (auto_close)
        entry.disconnect();
    return description.sample_block.cloneWithColumns(std::move(columns));
}

}

#endif
