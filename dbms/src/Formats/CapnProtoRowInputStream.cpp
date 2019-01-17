#include <Common/config.h>
#if USE_CAPNP

#include <Common/escapeForFileName.h>
#include <IO/ReadBuffer.h>
#include <Interpreters/Context.h>
#include <Formats/CapnProtoRowInputStream.h> // Y_IGNORE
#include <Formats/FormatFactory.h>
#include <Formats/BlockInputStreamFromRowInputStream.h>
#include <capnp/serialize.h> // Y_IGNORE
#include <capnp/dynamic.h> // Y_IGNORE
#include <capnp/common.h> // Y_IGNORE
#include <boost/algorithm/string.hpp>
#include <boost/range/join.hpp>
#include <common/logger_useful.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_TYPE_OF_FIELD;
    extern const int BAD_ARGUMENTS;
    extern const int THERE_IS_NO_COLUMN;
}

static String getSchemaPath(const String & schema_dir, const String & schema_file)
{
    return schema_dir + escapeForFileName(schema_file) + ".capnp";
}

CapnProtoRowInputStream::NestedField split(const Block & header, size_t i)
{
    CapnProtoRowInputStream::NestedField field = {{}, i};

    // Remove leading dot in field definition, e.g. ".msg" -> "msg"
    String name(header.safeGetByPosition(i).name);
    if (name.size() > 0 && name[0] == '.')
        name.erase(0, 1);

    boost::split(field.tokens, name, boost::is_any_of("._"));
    return field;
}


Field convertNodeToField(capnp::DynamicValue::Reader value)
{
    switch (value.getType())
    {
        case capnp::DynamicValue::UNKNOWN:
            throw Exception("Unknown field type", ErrorCodes::BAD_TYPE_OF_FIELD);
        case capnp::DynamicValue::VOID:
            return Field();
        case capnp::DynamicValue::BOOL:
            return value.as<bool>() ? 1u : 0u;
        case capnp::DynamicValue::INT:
            return value.as<int64_t>();
        case capnp::DynamicValue::UINT:
            return value.as<uint64_t>();
        case capnp::DynamicValue::FLOAT:
            return value.as<double>();
        case capnp::DynamicValue::TEXT:
        {
            auto arr = value.as<capnp::Text>();
            return String(arr.begin(), arr.size());
        }
        case capnp::DynamicValue::DATA:
        {
            auto arr = value.as<capnp::Data>().asChars();
            return String(arr.begin(), arr.size());
        }
        case capnp::DynamicValue::LIST:
        {
            auto listValue = value.as<capnp::DynamicList>();
            Array res(listValue.size());
            for (auto i : kj::indices(listValue))
                res[i] = convertNodeToField(listValue[i]);

            return res;
        }
        case capnp::DynamicValue::ENUM:
            return value.as<capnp::DynamicEnum>().getRaw();
        case capnp::DynamicValue::STRUCT:
        {
            auto structValue = value.as<capnp::DynamicStruct>();
            const auto & fields = structValue.getSchema().getFields();

            Field field = Tuple(TupleBackend(fields.size()));
            TupleBackend & tuple = get<Tuple &>(field).toUnderType();
            for (auto i : kj::indices(fields))
                tuple[i] = convertNodeToField(structValue.get(fields[i]));

            return field;
        }
        case capnp::DynamicValue::CAPABILITY:
            throw Exception("CAPABILITY type not supported", ErrorCodes::BAD_TYPE_OF_FIELD);
        case capnp::DynamicValue::ANY_POINTER:
            throw Exception("ANY_POINTER type not supported", ErrorCodes::BAD_TYPE_OF_FIELD);
    }
    return Field();
}

capnp::StructSchema::Field getFieldOrThrow(capnp::StructSchema node, const std::string & field)
{
    KJ_IF_MAYBE(child, node.findFieldByName(field))
        return *child;
    else
        throw Exception("Field " + field + " doesn't exist in schema " + node.getShortDisplayName().cStr(), ErrorCodes::THERE_IS_NO_COLUMN);
}

void CapnProtoRowInputStream::createActions(const NestedFieldList & sortedFields, capnp::StructSchema reader)
{
    // Store parents and their tokens in order to backtrack
    std::vector<capnp::StructSchema::Field> parents;
    std::vector<std::string> tokens;

    capnp::StructSchema cur_reader = reader;
    size_t level = 0;
    for (const auto & field : sortedFields)
    {
        // Backtrack to common parent
        while(level > (field.tokens.size() - 1) || !checkEqualFrom(tokens, field.tokens, level - 1))
        {
            level--;
            actions.push_back({Action::POP});
            tokens.pop_back();
            parents.pop_back();
            
            if (level > 0)
            {
                cur_reader = parents[level-1].getType().asStruct();
            }
            else
            {
                cur_reader = reader;
                break;
            }
        }
        
        // Go forward
        for (; level < field.tokens.size() - 1; ++level)
        {

            auto node = getFieldOrThrow(cur_reader, field.tokens[level]);
            if (node.getType().isStruct())
            {
                // Descend to field structure
                parents.push_back(node);
                tokens.push_back(field.tokens[level]);

                cur_reader = node.getType().asStruct();
                actions.push_back({Action::PUSH, node});
            }
            else if (node.getType().isList())
            {
                break; // Collect list
            }
            else
                throw Exception("Field " + field.tokens[level] + "is neither Struct nor List", ErrorCodes::BAD_TYPE_OF_FIELD);
        }

        // Read field from the structure
        auto node = getFieldOrThrow(cur_reader, field.tokens[level]);
        if (node.getType().isList() && actions.size() > 0 && actions.back().field == node)
        {
            // The field list here flattens Nested elements into multiple arrays
            // In order to map Nested types in Cap'nProto back, they need to be collected
            // Since the field names are sorted, the order of field positions must be preserved
            // For example, if the fields are { b @0 :Text, a @1 :Text }, the `a` would come first
            // even though it's position is second.
            auto & columns = actions.back().columns;
            auto it = std::upper_bound(columns.cbegin(), columns.cend(), field.pos);
            columns.insert(it, field.pos);
        }
        else
        {
            actions.push_back({Action::READ, node, {field.pos}});
        }
    }
}

CapnProtoRowInputStream::CapnProtoRowInputStream(ReadBuffer & istr_, const Block & header_, const String & schema_dir, const String & schema_file, const String & root_object)
    : istr(istr_), header(header_), parser(std::make_shared<SchemaParser>())
{

    // Parse the schema and fetch the root object

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto schema = parser->impl.parseDiskFile(schema_file, getSchemaPath(schema_dir, schema_file), {});
#pragma GCC diagnostic pop

    root = schema.getNested(root_object).asStruct();

    /**
     * The schema typically consists of fields in various nested structures.
     * Here we gather the list of fields and sort them in a way so that fields in the same structure are adjacent,
     * and the nesting level doesn't decrease to make traversal easier.
     */
    NestedFieldList list;
    size_t num_columns = header.columns();
    for (size_t i = 0; i < num_columns; ++i)
        list.push_back(split(header, i));

    // Order list first by value of strings then by length of sting vector.
    std::sort(list.begin(), list.end(), [](const NestedField & a, const NestedField & b) { return a.tokens < b.tokens; });
    createActions(list, root);
}


bool CapnProtoRowInputStream::read(MutableColumns & columns, RowReadExtension &)
{
    if (istr.eof())
        return false;

    // Read from underlying buffer directly
    auto buf = istr.buffer();
    auto base = reinterpret_cast<const capnp::word *>(istr.position());

    // Check if there's enough bytes in the buffer to read the full message
    kj::Array<capnp::word> heap_array;
    auto array = kj::arrayPtr(base, buf.size() - istr.offset());
    auto expected_words = capnp::expectedSizeInWordsFromPrefix(array);
    if (expected_words * sizeof(capnp::word) > array.size())
    {
        // We'll need to reassemble the message in a contiguous buffer
        heap_array = kj::heapArray<capnp::word>(expected_words);
        istr.readStrict(heap_array.asChars().begin(), heap_array.asChars().size());
        array = heap_array.asPtr();
    }


#if CAPNP_VERSION >= 8000
    capnp::UnalignedFlatArrayMessageReader msg(array);
#else
    capnp::FlatArrayMessageReader msg(array);
#endif
    std::vector<capnp::DynamicStruct::Reader> stack;
    stack.push_back(msg.getRoot<capnp::DynamicStruct>(root));

    for (auto action : actions)
    {
        switch (action.type)
        {
            case Action::READ:
            {
                Field value = convertNodeToField(stack.back().get(action.field));
                if (action.columns.size() > 1)
                {
                    // Nested columns must be flattened into several arrays
                    // e.g. Array(Tuple(x ..., y ...)) -> Array(x ...), Array(y ...)
                    const Array & collected = DB::get<const Array &>(value);
                    size_t size = collected.size();
                    // The flattened array contains an array of a part of the nested tuple
                    Array flattened(size);
                    for (size_t column_index = 0; column_index < action.columns.size(); ++column_index)
                    {
                        // Populate array with a single tuple elements
                        for (size_t off = 0; off < size; ++off)
                        {
                            const TupleBackend & tuple = DB::get<const Tuple &>(collected[off]).toUnderType();
                            flattened[off] = tuple[column_index];
                        }
                        auto & col = columns[action.columns[column_index]];
                        col->insert(flattened);
                    }
                }
                else
                {
                    auto & col = columns[action.columns[0]];
                    col->insert(value);
                }

                break;
            }
            case Action::POP:
                stack.pop_back();
                break;
            case Action::PUSH:
                stack.push_back(stack.back().get(action.field).as<capnp::DynamicStruct>());
                break;
        }
    }

    // Advance buffer position if used directly
    if (heap_array.size() == 0)
    {
        auto parsed = (msg.getEnd() - base) * sizeof(capnp::word);
        istr.position() += parsed;
    }

    return true;
}

void registerInputFormatCapnProto(FormatFactory & factory)
{
    factory.registerInputFormat("CapnProto", [](
        ReadBuffer & buf,
        const Block & sample,
        const Context & context,
        size_t max_block_size,
        const FormatSettings & settings)
    {
        std::vector<String> tokens;
        auto schema_and_root = context.getSettingsRef().format_schema.toString();
        boost::split(tokens, schema_and_root, boost::is_any_of(":"));
        if (tokens.size() != 2)
            throw Exception("Format CapnProto requires 'format_schema' setting to have a schema_file:root_object format, e.g. 'schema.capnp:Message'",
                ErrorCodes::BAD_ARGUMENTS);

        const String & schema_dir = context.getFormatSchemaPath();

        return std::make_shared<BlockInputStreamFromRowInputStream>(
            std::make_shared<CapnProtoRowInputStream>(buf, sample, schema_dir, tokens[0], tokens[1]),
            sample, max_block_size, settings);
    });
}

}

#else

namespace DB
{
    class FormatFactory;
    void registerInputFormatCapnProto(FormatFactory &) {}
}

#endif // USE_CAPNP
