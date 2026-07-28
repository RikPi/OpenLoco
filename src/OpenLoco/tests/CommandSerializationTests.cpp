#include <OpenLoco/Core/MemoryStream.h>
#include <OpenLoco/GameCommands/CommandSerialization.h>
#include <OpenLoco/GameCommands/Terraform/RaiseLand.h>
#include <OpenLoco/Network/Packet.h>
#include <gtest/gtest.h>

using namespace OpenLoco;
using namespace OpenLoco::GameCommands;

TEST(CommandSerializationTests, writerProducesLittleEndian)
{
    MemoryStream ms;
    ArgsWriter ar(ms);

    uint16_t value16 = 0x1234;
    int32_t value32 = 0x0A0B0C0D;
    ar(value16, value32);

    ASSERT_EQ(ms.getLength(), 6u);
    const auto* bytes = reinterpret_cast<const uint8_t*>(ms.data());
    EXPECT_EQ(bytes[0], 0x34);
    EXPECT_EQ(bytes[1], 0x12);
    EXPECT_EQ(bytes[2], 0x0D);
    EXPECT_EQ(bytes[3], 0x0C);
    EXPECT_EQ(bytes[4], 0x0B);
    EXPECT_EQ(bytes[5], 0x0A);
}

TEST(CommandSerializationTests, typedRoundTripRaiseLand)
{
    RaiseLandArgs original;
    original.centre = World::Pos2(1234, -320);
    original.pointA = World::Pos2(1216, -352);
    original.pointB = World::Pos2(1280, -288);
    original.corner = World::MapSelectionType::corner2;

    auto regs = static_cast<registers>(original);

    MemoryStream ms;
    ASSERT_TRUE(encodeCommandArgs(GameCommand::raiseLand, regs, ms));

    // Typed serialization writes the logical fields, not the 28-byte blob
    EXPECT_LT(ms.getLength(), sizeof(registers));

    ms.setPosition(0);
    registers decodedRegs;
    ASSERT_TRUE(decodeCommandArgs(GameCommand::raiseLand, ms, decodedRegs));

    RaiseLandArgs decoded(decodedRegs);
    EXPECT_EQ(decoded.centre, original.centre);
    EXPECT_EQ(decoded.pointA, original.pointA);
    EXPECT_EQ(decoded.pointB, original.pointB);
    EXPECT_EQ(decoded.corner, original.corner);
}

TEST(CommandSerializationTests, rawFallbackRoundTrip)
{
    // changeCompanyFace carries an ObjectHeader and stays on the raw fallback
    constexpr auto kRawCommand = GameCommand::changeCompanyFace;

    registers original;
    original.eax = 0x11223344;
    original.ebx = 0x55667788;
    original.ecx = -12345;
    original.edx = 42;
    original.esi = static_cast<int32_t>(kRawCommand);
    original.edi = 0x0F0E0D0C;
    original.ebp = -1;

    MemoryStream ms;
    ASSERT_TRUE(encodeCommandArgs(kRawCommand, original, ms));

    ms.setPosition(0);
    registers decoded;
    ASSERT_TRUE(decodeCommandArgs(kRawCommand, ms, decoded));

    EXPECT_EQ(0, std::memcmp(&original, &decoded, sizeof(registers)));
}

TEST(CommandSerializationTests, boolFieldRoundTrip)
{
    MemoryStream ms;
    ArgsWriter writer(ms);
    bool valueTrue = true;
    bool valueFalse = false;
    writer(valueTrue, valueFalse);

    // bool serializes as one byte each
    ASSERT_EQ(ms.getLength(), 2u);

    ms.setPosition(0);
    ArgsReader reader(ms);
    bool decodedTrue{};
    bool decodedFalse{ true };
    reader(decodedTrue, decodedFalse);
    EXPECT_TRUE(decodedTrue);
    EXPECT_FALSE(decodedFalse);
}

TEST(CommandSerializationTests, wirePacketRoundTrip)
{
    RaiseLandArgs args;
    args.centre = World::Pos2(512, 768);
    args.pointA = World::Pos2(480, 736);
    args.pointB = World::Pos2(544, 800);
    args.corner = World::MapSelectionType::full;

    Network::QueuedGameCommand original;
    original.index = 42;
    original.tick = 123456;
    original.company = CompanyId(3);
    original.flags = Flags::apply;
    original.command = GameCommand::raiseLand;
    original.regs = static_cast<registers>(args);

    Network::GameCommandPacket packet;
    ASSERT_TRUE(Network::toWirePacket(original, packet));
    EXPECT_EQ(packet.commandId, static_cast<uint8_t>(GameCommand::raiseLand));
    EXPECT_LT(packet.dataSize, sizeof(registers));

    Network::QueuedGameCommand decoded;
    ASSERT_TRUE(Network::fromWirePacket(packet, decoded));

    EXPECT_EQ(decoded.index, original.index);
    EXPECT_EQ(decoded.tick, original.tick);
    EXPECT_EQ(decoded.company, original.company);
    EXPECT_EQ(decoded.flags, original.flags);
    EXPECT_EQ(decoded.command, original.command);

    // The dispatcher's view must be fully reconstructed
    EXPECT_EQ(decoded.regs.esi, static_cast<int32_t>(GameCommand::raiseLand));
    EXPECT_EQ(decoded.regs.bl, Flags::apply);

    RaiseLandArgs decodedArgs(decoded.regs);
    EXPECT_EQ(decodedArgs.centre, args.centre);
    EXPECT_EQ(decodedArgs.pointA, args.pointA);
    EXPECT_EQ(decodedArgs.pointB, args.pointB);
    EXPECT_EQ(decodedArgs.corner, args.corner);
}

TEST(CommandSerializationTests, malformedPacketIsRejected)
{
    Network::GameCommandPacket packet;
    packet.commandId = static_cast<uint8_t>(GameCommand::raiseLand);
    packet.dataSize = 2; // truncated: raiseLand payload is larger

    Network::QueuedGameCommand decoded;
    EXPECT_FALSE(Network::fromWirePacket(packet, decoded));
}
