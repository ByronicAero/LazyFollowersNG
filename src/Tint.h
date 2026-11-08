#pragma once

// Tint - writes an actor's face tint texture to a .dds file (what "skee preset-save" did with
// Textures\CharGen\Exported\<name>.dds, except RaceMenu always wrote the PLAYER's tint there).

namespace Tint
{
    // Main thread only (uses the game's D3D11 device context between frames). The texture is read from the
    // actor's loaded face (FaceGen tint of its Face head part). False + a_error on failure.
    bool ExportFaceTint(RE::Actor* a_actor, const std::filesystem::path& a_file, std::string& a_error);
}
