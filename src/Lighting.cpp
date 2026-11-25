// Illuminated - Creature Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with: the shared GameLighting.h finds it once, at data load (Community Shaders, ENB or Vanilla,
// never picked); this file keeps the answer for the rest of the plugin. On Community Shaders a light's reach comes from its fade
// and cutoff where a shader draws inverse square; elsewhere, and on a Community Shaders with no such shader, every light of ours
// is drawn plain, reaching its own radius (Dress, LightKit::Isl::SetOwnLinear where Light Limit Fix still reads its words).

#include "Plugin.h"

#include "GameLighting.h"

#include <filesystem>
#include <system_error>

namespace Plugin
{
	static_assert(static_cast<int>(Lighting::kShaders) == static_cast<int>(GameLighting::Lighting::kShaders) &&
				  static_cast<int>(Lighting::kEnb) == static_cast<int>(GameLighting::Lighting::kEnb) &&
				  static_cast<int>(Lighting::kVanilla) == static_cast<int>(GameLighting::Lighting::kVanilla));

	namespace
	{
		std::atomic<Lighting> gFound{ Lighting::kVanilla };
		std::atomic<bool>     gLightLimitFix{ false };  // CommunityShaders.dll loaded: Light Limit Fix reads every light's first words
		std::atomic<bool>     gInverseSquare{ false };  // ... and a shader of it draws inverse square
	}

	void ReadLighting()
	{
		const auto found = GameLighting::Read();
		gLightLimitFix = found.communityShaders;
		gInverseSquare = found.inverseSquare;
		gFound = static_cast<Lighting>(found.lighting);
		const auto lines = GameLighting::Describe(found);
		SKSE::log::info("{}", lines.info);
		for (const auto& warning : lines.warnings) {
			SKSE::log::warn("{}", warning);
		}
	}

	Lighting LightingFound()
	{
		return gFound.load();
	}

	const char* LightingName(Lighting a_lighting)
	{
		return GameLighting::Name(static_cast<GameLighting::Lighting>(a_lighting));
	}

	bool InverseSquare()
	{
		return gInverseSquare.load();
	}

	bool LightLimitFix()
	{
		return gLightLimitFix.load();
	}

	// the optional Bottled Shaders Shadows download is only this marker file (his checklist 761): with it a creature's body glow casts a
	// shadow (CreatureLights.cpp). Open Shaders' Promote makes every light a caster by itself. RELight's HandLights.cpp ShadowPatch way
	bool ShadowPatch()
	{
		static const bool on = [] {
			std::error_code ec;
			const bool      there = std::filesystem::exists("Data/SKSE/Plugins/IlluminatedCreatureLightingNG/Bottled Shaders Shadows.ini", ec);
			SKSE::log::info("shadows: the Bottled Shaders Shadows download is {}", there ? "installed - body glows cast shadows" : "not installed");
			return there;
		}();
		return on;
	}

	float RowStrength(const Anima::Row& a_row, const Anima::Settings& a_s)
	{
		return a_row.strength * static_cast<float>(a_s.brightness) / 100.0f * Anima::TheirVanillaShare(a_row, LightingFound() == Lighting::kVanilla);
	}
}
