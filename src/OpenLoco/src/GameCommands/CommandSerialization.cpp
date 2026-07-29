#include "GameCommands/CommandSerialization.h"
#include "GameCommands/Airports/CreateAirport.h"
#include "GameCommands/Airports/RemoveAirport.h"
#include "GameCommands/Buildings/CreateBuilding.h"
#include "GameCommands/Buildings/RemoveBuilding.h"
#include "GameCommands/Cheats/Cheat.h"
#include "GameCommands/Company/BuildCompanyHeadquarters.h"
#include "GameCommands/Company/ChangeCompanyColour.h"
#include "GameCommands/Company/ChangeCompanyFace.h"
#include "GameCommands/Company/ChangeLoan.h"
#include "GameCommands/Company/CreatePlayerCompany.h"
#include "GameCommands/Company/RemoveCompanyHeadquarters.h"
#include "GameCommands/Company/UpdateOwnerStatus.h"
#include "GameCommands/CompanyAi/AiCreateRoadAndStation.h"
#include "GameCommands/CompanyAi/AiCreateTrackAndStation.h"
#include "GameCommands/CompanyAi/AiTrackReplacement.h"
#include "GameCommands/Docks/CreatePort.h"
#include "GameCommands/Docks/RemovePort.h"
#include "GameCommands/General/LoadSaveQuit.h"
#include "GameCommands/General/SetGameSpeed.h"
#include "GameCommands/General/TogglePause.h"
#include "GameCommands/Industries/CreateIndustry.h"
#include "GameCommands/Industries/RemoveIndustry.h"
#include "GameCommands/Road/CreateRoad.h"
#include "GameCommands/Road/CreateRoadMod.h"
#include "GameCommands/Road/CreateRoadStation.h"
#include "GameCommands/Road/RemoveRoad.h"
#include "GameCommands/Road/RemoveRoadMod.h"
#include "GameCommands/Road/RemoveRoadStation.h"
#include "GameCommands/Terraform/ChangeLandMaterial.h"
#include "GameCommands/Terraform/ClearLand.h"
#include "GameCommands/Terraform/CreateTree.h"
#include "GameCommands/Terraform/CreateWall.h"
#include "GameCommands/Terraform/LowerLand.h"
#include "GameCommands/Terraform/LowerRaiseLandMountain.h"
#include "GameCommands/Terraform/LowerWater.h"
#include "GameCommands/Terraform/RaiseLand.h"
#include "GameCommands/Terraform/RaiseWater.h"
#include "GameCommands/Terraform/RemoveTree.h"
#include "GameCommands/Terraform/RemoveWall.h"
#include "GameCommands/Town/CreateTown.h"
#include "GameCommands/Town/RemoveTown.h"
#include "GameCommands/Track/CreateSignal.h"
#include "GameCommands/Track/CreateTrack.h"
#include "GameCommands/Track/CreateTrackMod.h"
#include "GameCommands/Track/CreateTrainStation.h"
#include "GameCommands/Track/RemoveSignal.h"
#include "GameCommands/Track/RemoveTrack.h"
#include "GameCommands/Track/RemoveTrackMod.h"
#include "GameCommands/Track/RemoveTrainStation.h"
#include "GameCommands/Vehicles/CloneVehicle.h"
#include "GameCommands/Vehicles/CreateVehicle.h"
#include "GameCommands/Vehicles/VehicleChangeRunningMode.h"
#include "GameCommands/Vehicles/VehicleOrderDelete.h"
#include "GameCommands/Vehicles/VehicleOrderDown.h"
#include "GameCommands/Vehicles/VehicleOrderInsert.h"
#include "GameCommands/Vehicles/VehicleOrderReverse.h"
#include "GameCommands/Vehicles/VehicleOrderSkip.h"
#include "GameCommands/Vehicles/VehicleOrderUp.h"
#include "GameCommands/Vehicles/VehiclePassSignal.h"
#include "GameCommands/Vehicles/VehiclePickup.h"
#include "GameCommands/Vehicles/VehiclePickupAir.h"
#include "GameCommands/Vehicles/VehiclePickupWater.h"
#include "GameCommands/Vehicles/VehiclePlace.h"
#include "GameCommands/Vehicles/VehiclePlaceAir.h"
#include "GameCommands/Vehicles/VehiclePlaceWater.h"
#include "GameCommands/Vehicles/VehicleRearrange.h"
#include "GameCommands/Vehicles/VehicleRefit.h"
#include "GameCommands/Vehicles/VehicleRepaint.h"
#include "GameCommands/Vehicles/VehicleReverse.h"
#include "GameCommands/Vehicles/VehicleSell.h"
#include "GameCommands/Vehicles/VehicleSpeedControl.h"
#include "Logging.h"
#include <array>
#include <cstring>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco::GameCommands
{
    // Per-command field lists. Each overload names the Args fields once and is
    // used for both reading and writing. Keep the field order stable: it IS
    // the wire format for that command.

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RaiseLandArgs& args)
    {
        ar(args.centre, args.pointA, args.pointB, args.corner);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, LowerLandArgs& args)
    {
        ar(args.centre, args.pointA, args.pointB, args.corner);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleRearrangeArgs& args)
    {
        ar(args.source, args.dest);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehiclePlacementArgs& args)
    {
        ar(args.pos, args.trackAndDirection, args.trackProgress, args.head, args.convertGhost);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehiclePickupArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleReverseArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehiclePassSignalArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleCreateArgs& args)
    {
        ar(args.vehicleId, args.vehicleType);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleSellArgs& args)
    {
        ar(args.car);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrackPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.mods, args.unkFlags, args.bridge, args.trackObjectId, args.unk);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrackRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.trackObjectId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, ChangeLoanArgs& args)
    {
        ar(args.newLoan);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleChangeRunningModeArgs& args)
    {
        ar(args.head, args.mode);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, SignalPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.type, args.trackObjType, args.sides);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, SignalRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.trackObjType, args.flags);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrainStationPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.trackObjectId, args.type);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrainStationRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.type);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrackModsPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.type, args.trackObjType, args.modSection);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TrackModsRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.index, args.type, args.trackObjType, args.modSection);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, ChangeCompanyColourSchemeArgs& args)
    {
        ar(args.companyId, args.isPrimary, args.value, args.colourType, args.setColourMode);
    }

    // Verified symmetric: Args(regs) reconstructs companyId from bh and
    // objHeader from a straight byte-for-byte repacking of
    // eax/ecx/edx/edi (no lossy transformation either way), and
    // operator registers() undoes exactly that repacking. So for any X =
    // ChangeCompanyFaceArgs(regs), ChangeCompanyFaceArgs(registers(X)) == X
    // - safe for the generic typed codec, given archive support for
    // ObjectHeader (added to CommandSerialization.h).
    template<typename TArchive>
    static void serializeArgs(TArchive& ar, ChangeCompanyFaceArgs& args)
    {
        ar(args.companyId, args.objHeader);
    }

    // Verified symmetric: OwnerStatus(regs.ax, regs.cx) stores the two
    // registers verbatim in data[0]/data[1] (the entity-id/position meaning
    // is derived from those values, not carried separately), and
    // OwnerStatus::getData() returns them verbatim back to ax/cx. So this is
    // a true fixed point, unlike the rename commands - safe for the generic
    // typed codec, given archive support for OwnerStatus (added to
    // CommandSerialization.h).
    template<typename TArchive>
    static void serializeArgs(TArchive& ar, UpdateOwnerStatusArgs& args)
    {
        ar(args.ownerStatus);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive&, PauseGameArgs&)
    {
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, LoadSaveQuitGameArgs& args)
    {
        ar(args.loadQuitMode, args.saveMode);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TreeRemovalArgs& args)
    {
        ar(args.pos, args.type, args.quadrant, args.rotation);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TreePlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type, args.quadrant, args.colour, args.buildImmediately, args.requiresFullClearance);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, ChangeLandMaterialArgs& args)
    {
        ar(args.pointA, args.pointB, args.landType);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, LowerRaiseLandMountainArgs& args)
    {
        ar(args.centre, args.pointA, args.pointB, args.adjustment);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RaiseWaterArgs& args)
    {
        ar(args.pointA, args.pointB);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, LowerWaterArgs& args)
    {
        ar(args.pointA, args.pointB);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, WallPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type, args.primaryColour, args.secondaryColour, args.tertiaryColour);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, WallRemovalArgs& args)
    {
        ar(args.pos, args.rotation);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderInsertArgs& args)
    {
        ar(args.head, args.orderOffset, args.rawOrder);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderDeleteArgs& args)
    {
        ar(args.head, args.orderOffset);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderSkipArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.mods, args.bridge, args.roadObjectId, args.unkFlags);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.sequenceIndex, args.objectId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadModsPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.index, args.type, args.roadObjType, args.modSection);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadModsRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.index, args.type, args.roadObjType, args.modSection);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadStationPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.index, args.roadObjectId, args.type);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, RoadStationRemovalArgs& args)
    {
        ar(args.pos, args.rotation, args.roadId, args.index, args.roadObjectId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, BuildingPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type, args.variation, args.colour, args.buildImmediately);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, BuildingRemovalArgs& args)
    {
        ar(args.pos);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, IndustryPlacementArgs& args)
    {
        ar(args.pos, args.type, args.buildImmediately, args.srand0, args.srand1);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, IndustryRemovalArgs& args)
    {
        ar(args.industryId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TownPlacementArgs& args)
    {
        ar(args.pos, args.size);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, TownRemovalArgs& args)
    {
        ar(args.townId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, AiTrackAndStationPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackObjectId, args.stationObjectId, args.stationLength, args.mods, args.unk1, args.bridge);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, AiTrackReplacementArgs& args)
    {
        ar(args.pos, args.rotation, args.trackId, args.sequenceIndex, args.trackObjectId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, AiRoadAndStationPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.roadObjectId, args.stationObjectId, args.stationLength, args.mods, args.unk1, args.bridge);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, HeadquarterPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type, args.buildImmediately);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, HeadquarterRemovalArgs& args)
    {
        ar(args.pos);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, AirportPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, AirportRemovalArgs& args)
    {
        ar(args.pos);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleAirPlacementArgs& args)
    {
        ar(args.stationId, args.airportNode, args.head, args.convertGhost);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehiclePickupAirArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, PortPlacementArgs& args)
    {
        ar(args.pos, args.rotation, args.type);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, PortRemovalArgs& args)
    {
        ar(args.pos);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleWaterPlacementArgs& args)
    {
        ar(args.pos, args.head, args.convertGhost);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehiclePickupWaterArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleRefitArgs& args)
    {
        ar(args.head, args.cargoType);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, ClearLandArgs& args)
    {
        ar(args.centre, args.pointA, args.pointB);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleSpeedControlArgs& args)
    {
        ar(args.head, args.speed);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderUpArgs& args)
    {
        ar(args.head, args.orderOffset);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderDownArgs& args)
    {
        ar(args.head, args.orderOffset);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleApplyShuntCheatArgs& args)
    {
        ar(args.head);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive&, ApplyFreeCashCheatArgs&)
    {
    }

    template<typename TArchive>
    static void serializeArgs(TArchive&, CreatePlayerCompanyArgs&)
    {
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleCloneArgs& args)
    {
        ar(args.vehicleHeadId);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, GenericCheatArgs& args)
    {
        ar(args.subcommand, args.param1, args.param2, args.param3);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, SetGameSpeedArgs& args)
    {
        ar(args.newSpeed);
    }

    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleOrderReverseArgs& args)
    {
        ar(args.head);
    }

    // Verified symmetric: each ColourScheme in colours[] is built from a
    // 16-bit register value masked to the 5-bit primary (bits 0-4) and 5-bit
    // secondary (bits 8-12) fields; operator registers() repacks them via
    // the same bit positions (primary | secondary << 8) with no additional
    // masking. Because primary/secondary are always in [0,31] once stored in
    // a ColourScheme (Colour is a 5-bit palette id), repacking then
    // re-unpacking reproduces the same bits exactly, so for any
    // X = VehicleRepaintArgs(regs), VehicleRepaintArgs(registers(X)) == X.
    // Safe for the generic typed codec, given archive support for
    // std::array<ColourScheme, 4> (added to CommandSerialization.h).
    template<typename TArchive>
    static void serializeArgs(TArchive& ar, VehicleRepaintArgs& args)
    {
        ar(args.head, args.colours, args.paintFlags);
    }

    // ---------------------------------------------------------------------

    using EncodeFn = bool (*)(const registers& regs, Stream& stream);
    using DecodeFn = bool (*)(Stream& stream, registers& regs);

    struct CommandCodec
    {
        EncodeFn encode{};
        DecodeFn decode{};
    };

    template<typename TArgs>
    static bool encodeTyped(const registers& regs, Stream& stream)
    {
        TArgs args(regs);
        ArgsWriter ar(stream);
        serializeArgs(ar, args);
        return true;
    }

    template<typename TArgs>
    static bool decodeTyped(Stream& stream, registers& regs)
    {
        TArgs args{};
        ArgsReader ar(stream);
        serializeArgs(ar, args);
        regs = static_cast<registers>(args);
        return true;
    }

    // Fallback for commands that do not have a typed serializer yet: ship the
    // raw registers blob. Only interoperable between identical builds.
    static bool encodeRawRegisters(const registers& regs, Stream& stream)
    {
        stream.write(&regs, sizeof(regs));
        return true;
    }

    static bool decodeRawRegisters(Stream& stream, registers& regs)
    {
        stream.read(&regs, sizeof(regs));
        return true;
    }

    // The six rename commands share vanilla's chunked-name protocol: cx is the
    // target id, ax the chunk index, and edx/ebp/edi carry 12 bytes of the
    // name. Their Args structs are asymmetric (the registers constructor
    // stores the incoming chunk at the start of the buffer, while
    // operator registers() extracts the chunk from a fully populated name
    // buffer), so they cannot round-trip through the generic typed codec.
    // Serialize the chunk protocol explicitly instead: id and index as
    // integers, the chunk as raw bytes (portable regardless of host layout).
    static bool encodeRenameChunk(const registers& regs, Stream& stream)
    {
        ArgsWriter ar(stream);
        auto id = static_cast<uint16_t>(regs.cx);
        auto chunkIndex = static_cast<uint16_t>(regs.ax);
        ar(id, chunkIndex);

        uint8_t chunk[12];
        std::memcpy(chunk, &regs.edx, 4);
        std::memcpy(chunk + 4, &regs.ebp, 4);
        std::memcpy(chunk + 8, &regs.edi, 4);
        stream.write(chunk, sizeof(chunk));
        return true;
    }

    static bool decodeRenameChunk(Stream& stream, registers& regs)
    {
        ArgsReader ar(stream);
        uint16_t id{};
        uint16_t chunkIndex{};
        ar(id, chunkIndex);
        regs.cx = static_cast<int16_t>(id);
        regs.ax = static_cast<int16_t>(chunkIndex);

        uint8_t chunk[12];
        stream.read(chunk, sizeof(chunk));
        std::memcpy(&regs.edx, chunk, 4);
        std::memcpy(&regs.ebp, chunk + 4, 4);
        std::memcpy(&regs.edi, chunk + 8, 4);
        return true;
    }

    static constexpr size_t kNumGameCommands = 85;

    template<typename TArgs>
    static consteval CommandCodec makeTypedCodec()
    {
        return CommandCodec{ encodeTyped<TArgs>, decodeTyped<TArgs> };
    }

    static consteval std::array<CommandCodec, kNumGameCommands> makeCodecTable()
    {
        std::array<CommandCodec, kNumGameCommands> table{};
        for (auto& codec : table)
        {
            codec = CommandCodec{ encodeRawRegisters, decodeRawRegisters };
        }

        table[static_cast<size_t>(GameCommand::vehicleRearrange)] = makeTypedCodec<VehicleRearrangeArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePlace)] = makeTypedCodec<VehiclePlacementArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePickup)] = makeTypedCodec<VehiclePickupArgs>();
        table[static_cast<size_t>(GameCommand::vehicleReverse)] = makeTypedCodec<VehicleReverseArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePassSignal)] = makeTypedCodec<VehiclePassSignalArgs>();
        table[static_cast<size_t>(GameCommand::vehicleCreate)] = makeTypedCodec<VehicleCreateArgs>();
        table[static_cast<size_t>(GameCommand::vehicleSell)] = makeTypedCodec<VehicleSellArgs>();
        table[static_cast<size_t>(GameCommand::createTrack)] = makeTypedCodec<TrackPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeTrack)] = makeTypedCodec<TrackRemovalArgs>();
        table[static_cast<size_t>(GameCommand::changeLoan)] = makeTypedCodec<ChangeLoanArgs>();
        table[static_cast<size_t>(GameCommand::vehicleChangeRunningMode)] = makeTypedCodec<VehicleChangeRunningModeArgs>();
        table[static_cast<size_t>(GameCommand::createSignal)] = makeTypedCodec<SignalPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeSignal)] = makeTypedCodec<SignalRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createTrainStation)] = makeTypedCodec<TrainStationPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeTrainStation)] = makeTypedCodec<TrainStationRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createTrackMod)] = makeTypedCodec<TrackModsPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeTrackMod)] = makeTypedCodec<TrackModsRemovalArgs>();
        table[static_cast<size_t>(GameCommand::changeCompanyColourScheme)] = makeTypedCodec<ChangeCompanyColourSchemeArgs>();
        table[static_cast<size_t>(GameCommand::pauseGame)] = makeTypedCodec<PauseGameArgs>();
        table[static_cast<size_t>(GameCommand::loadSaveQuitGame)] = makeTypedCodec<LoadSaveQuitGameArgs>();
        table[static_cast<size_t>(GameCommand::removeTree)] = makeTypedCodec<TreeRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createTree)] = makeTypedCodec<TreePlacementArgs>();
        table[static_cast<size_t>(GameCommand::changeLandMaterial)] = makeTypedCodec<ChangeLandMaterialArgs>();
        table[static_cast<size_t>(GameCommand::raiseLand)] = makeTypedCodec<RaiseLandArgs>();
        table[static_cast<size_t>(GameCommand::lowerLand)] = makeTypedCodec<LowerLandArgs>();
        table[static_cast<size_t>(GameCommand::lowerRaiseLandMountain)] = makeTypedCodec<LowerRaiseLandMountainArgs>();
        table[static_cast<size_t>(GameCommand::raiseWater)] = makeTypedCodec<RaiseWaterArgs>();
        table[static_cast<size_t>(GameCommand::lowerWater)] = makeTypedCodec<LowerWaterArgs>();
        table[static_cast<size_t>(GameCommand::createWall)] = makeTypedCodec<WallPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeWall)] = makeTypedCodec<WallRemovalArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderInsert)] = makeTypedCodec<VehicleOrderInsertArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderDelete)] = makeTypedCodec<VehicleOrderDeleteArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderSkip)] = makeTypedCodec<VehicleOrderSkipArgs>();
        table[static_cast<size_t>(GameCommand::createRoad)] = makeTypedCodec<RoadPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeRoad)] = makeTypedCodec<RoadRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createRoadMod)] = makeTypedCodec<RoadModsPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeRoadMod)] = makeTypedCodec<RoadModsRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createRoadStation)] = makeTypedCodec<RoadStationPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeRoadStation)] = makeTypedCodec<RoadStationRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createBuilding)] = makeTypedCodec<BuildingPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeBuilding)] = makeTypedCodec<BuildingRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createIndustry)] = makeTypedCodec<IndustryPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeIndustry)] = makeTypedCodec<IndustryRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createTown)] = makeTypedCodec<TownPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeTown)] = makeTypedCodec<TownRemovalArgs>();
        table[static_cast<size_t>(GameCommand::aiCreateTrackAndStation)] = makeTypedCodec<AiTrackAndStationPlacementArgs>();
        table[static_cast<size_t>(GameCommand::aiTrackReplacement)] = makeTypedCodec<AiTrackReplacementArgs>();
        table[static_cast<size_t>(GameCommand::aiCreateRoadAndStation)] = makeTypedCodec<AiRoadAndStationPlacementArgs>();
        table[static_cast<size_t>(GameCommand::buildCompanyHeadquarters)] = makeTypedCodec<HeadquarterPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeCompanyHeadquarters)] = makeTypedCodec<HeadquarterRemovalArgs>();
        table[static_cast<size_t>(GameCommand::createPlayerCompany)] = makeTypedCodec<CreatePlayerCompanyArgs>();
        table[static_cast<size_t>(GameCommand::createAirport)] = makeTypedCodec<AirportPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removeAirport)] = makeTypedCodec<AirportRemovalArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePlaceAir)] = makeTypedCodec<VehicleAirPlacementArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePickupAir)] = makeTypedCodec<VehiclePickupAirArgs>();
        table[static_cast<size_t>(GameCommand::createPort)] = makeTypedCodec<PortPlacementArgs>();
        table[static_cast<size_t>(GameCommand::removePort)] = makeTypedCodec<PortRemovalArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePlaceWater)] = makeTypedCodec<VehicleWaterPlacementArgs>();
        table[static_cast<size_t>(GameCommand::vehiclePickupWater)] = makeTypedCodec<VehiclePickupWaterArgs>();
        table[static_cast<size_t>(GameCommand::vehicleRefit)] = makeTypedCodec<VehicleRefitArgs>();
        table[static_cast<size_t>(GameCommand::clearLand)] = makeTypedCodec<ClearLandArgs>();
        table[static_cast<size_t>(GameCommand::vehicleSpeedControl)] = makeTypedCodec<VehicleSpeedControlArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderUp)] = makeTypedCodec<VehicleOrderUpArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderDown)] = makeTypedCodec<VehicleOrderDownArgs>();
        table[static_cast<size_t>(GameCommand::vehicleApplyShuntCheat)] = makeTypedCodec<VehicleApplyShuntCheatArgs>();
        table[static_cast<size_t>(GameCommand::applyFreeCashCheat)] = makeTypedCodec<ApplyFreeCashCheatArgs>();
        table[static_cast<size_t>(GameCommand::vehicleClone)] = makeTypedCodec<VehicleCloneArgs>();
        table[static_cast<size_t>(GameCommand::cheat)] = makeTypedCodec<GenericCheatArgs>();
        table[static_cast<size_t>(GameCommand::setGameSpeed)] = makeTypedCodec<SetGameSpeedArgs>();
        table[static_cast<size_t>(GameCommand::vehicleOrderReverse)] = makeTypedCodec<VehicleOrderReverseArgs>();
        table[static_cast<size_t>(GameCommand::changeCompanyFace)] = makeTypedCodec<ChangeCompanyFaceArgs>();
        table[static_cast<size_t>(GameCommand::updateOwnerStatus)] = makeTypedCodec<UpdateOwnerStatusArgs>();
        table[static_cast<size_t>(GameCommand::vehicleRepaint)] = makeTypedCodec<VehicleRepaintArgs>();

        constexpr CommandCodec kRenameChunkCodec{ encodeRenameChunk, decodeRenameChunk };
        table[static_cast<size_t>(GameCommand::vehicleRename)] = kRenameChunkCodec;
        table[static_cast<size_t>(GameCommand::changeStationName)] = kRenameChunkCodec;
        table[static_cast<size_t>(GameCommand::changeCompanyName)] = kRenameChunkCodec;
        table[static_cast<size_t>(GameCommand::changeCompanyOwnerName)] = kRenameChunkCodec;
        table[static_cast<size_t>(GameCommand::renameTown)] = kRenameChunkCodec;
        table[static_cast<size_t>(GameCommand::renameIndustry)] = kRenameChunkCodec;

        return table;
    }

    static constexpr std::array<CommandCodec, kNumGameCommands> kCodecTable = makeCodecTable();

    bool encodeCommandArgs(GameCommand command, const registers& regs, Stream& stream)
    {
        auto index = static_cast<size_t>(command);
        if (index >= kCodecTable.size())
        {
            Logging::error("Cannot serialize unknown game command id {}", index);
            return false;
        }
        return kCodecTable[index].encode(regs, stream);
    }

    bool decodeCommandArgs(GameCommand command, Stream& stream, registers& regs)
    {
        auto index = static_cast<size_t>(command);
        if (index >= kCodecTable.size())
        {
            Logging::error("Cannot deserialize unknown game command id {}", index);
            return false;
        }
        return kCodecTable[index].decode(stream, regs);
    }
}
