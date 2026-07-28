#pragma once

#include "GameCommands/GameCommands.h"
#include <OpenLoco/Core/Stream.hpp>
#include <OpenLoco/Engine/World.hpp>
#include <cstdint>
#include <type_traits>

namespace OpenLoco::GameCommands
{
    /**
     * Portable serialization of game command arguments for the network wire
     * format.
     *
     * Commands are serialized as their logical Args struct, field by field in
     * little-endian byte order, rather than as the raw x86-style `registers`
     * blob. This makes the wire format independent of struct padding, host
     * endianness and the register packing of any particular build.
     *
     * Each migrated command provides an overload of
     *
     *     template<typename TArchive>
     *     void serializeArgs(TArchive& ar, TArgs& args);
     *
     * in CommandSerialization.cpp, listing its fields once; the same overload
     * drives both reading and writing. Commands that have not been migrated
     * yet fall back to serializing the raw `registers` blob (interoperable
     * only between identical builds).
     */

    class ArgsWriter
    {
    public:
        explicit ArgsWriter(Stream& stream)
            : _stream(stream)
        {
        }

        template<typename... T>
        void operator()(T&... values)
        {
            (write(values), ...);
        }

    private:
        template<typename T>
        void write(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                writeInt(static_cast<uint8_t>(value ? 1 : 0));
            }
            else if constexpr (std::is_integral_v<T>)
            {
                writeInt(value);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                writeInt(static_cast<std::underlying_type_t<T>>(value));
            }
            else if constexpr (std::is_same_v<T, World::Pos2> || std::is_same_v<T, World::TilePos2>)
            {
                writeInt(value.x);
                writeInt(value.y);
            }
            else if constexpr (std::is_same_v<T, World::Pos3>)
            {
                writeInt(value.x);
                writeInt(value.y);
                writeInt(value.z);
            }
            else if constexpr (std::is_bounded_array_v<T> && std::is_same_v<std::remove_extent_t<T>, char>)
            {
                _stream.write(value, sizeof(T));
            }
            else
            {
                static_assert(sizeof(T) == 0, "No serialization defined for this field type");
            }
        }

        template<typename T>
        void writeInt(T value)
        {
            auto raw = static_cast<std::make_unsigned_t<T>>(value);
            uint8_t bytes[sizeof(T)];
            for (size_t i = 0; i < sizeof(T); i++)
            {
                bytes[i] = static_cast<uint8_t>(raw >> (i * 8));
            }
            _stream.write(bytes, sizeof(bytes));
        }

        Stream& _stream;
    };

    class ArgsReader
    {
    public:
        explicit ArgsReader(Stream& stream)
            : _stream(stream)
        {
        }

        template<typename... T>
        void operator()(T&... values)
        {
            (read(values), ...);
        }

    private:
        template<typename T>
        void read(T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                value = readInt<uint8_t>() != 0;
            }
            else if constexpr (std::is_integral_v<T>)
            {
                value = readInt<T>();
            }
            else if constexpr (std::is_enum_v<T>)
            {
                value = static_cast<T>(readInt<std::underlying_type_t<T>>());
            }
            else if constexpr (std::is_same_v<T, World::Pos2> || std::is_same_v<T, World::TilePos2>)
            {
                value.x = readInt<decltype(value.x)>();
                value.y = readInt<decltype(value.y)>();
            }
            else if constexpr (std::is_same_v<T, World::Pos3>)
            {
                value.x = readInt<decltype(value.x)>();
                value.y = readInt<decltype(value.y)>();
                value.z = readInt<decltype(value.z)>();
            }
            else if constexpr (std::is_bounded_array_v<T> && std::is_same_v<std::remove_extent_t<T>, char>)
            {
                _stream.read(value, sizeof(T));
            }
            else
            {
                static_assert(sizeof(T) == 0, "No serialization defined for this field type");
            }
        }

        template<typename T>
        T readInt()
        {
            uint8_t bytes[sizeof(T)];
            _stream.read(bytes, sizeof(bytes));
            std::make_unsigned_t<T> raw{};
            for (size_t i = 0; i < sizeof(T); i++)
            {
                raw |= static_cast<std::make_unsigned_t<T>>(bytes[i]) << (i * 8);
            }
            return static_cast<T>(raw);
        }

        Stream& _stream;
    };

    /**
     * Serializes the arguments of the given command (currently packed in
     * `regs`) into `stream` in the portable wire format.
     * Returns false if serialization failed.
     */
    bool encodeCommandArgs(GameCommand command, const registers& regs, Stream& stream);

    /**
     * Deserializes command arguments from `stream` and reconstructs the
     * `registers` representation used by the dispatcher. Only the argument
     * registers are populated; the caller is responsible for esi/bl
     * (command id and flags).
     * Returns false if deserialization failed.
     */
    bool decodeCommandArgs(GameCommand command, Stream& stream, registers& regs);
}
