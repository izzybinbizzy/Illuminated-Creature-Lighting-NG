// Illuminated - Creature Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The spells creatures use and their explosions, lit the game's own way (Illuminated's record route, RecordLights.cpp): the
// game already gives a spell its lights through four record slots - a magic effect's casting light (the art in a creature's
// hands or mouth while it casts), a projectile's light, an explosion's light and a hazard's light - and it makes, moves and
// removes those lights itself, on every lighting and in VR. So each record a spell or burst row names (by its model, or by the
// record itself) is pointed, in memory, at a light of ours made from the row: the row's color, strength and reach, written in
// Community Shaders' inverse-square words on Community Shaders. Nothing of ours runs while the game plays; a menu switch points
// the records back at their own lights (or none). Nothing reaches a save: these are base records the game never writes.
//
// RE::Light files, only when the download carries them and RE::Light runs: a record with a config there is lit by RE::Light, which
// hangs an "RL" light on the reference as it loads, so the record keeps its own light and no light of ours is pointed at it. Nothing
// announces those lights, so each kind of reference RE::Light lights is hooked after it (its Load3D hooks go in at plugin load, ours at
// data load, so the original call runs its light-making first), and a row the menu switched off culls them there - but never on a record
// another mod's RE::Light file names too, whose light may be theirs.
//
// A creature switched off in the menu turns off its spells' lights too: the records are shared with the player and other casters, so its spells' lights
// of ours are culled on that one caster as the game makes each: a casting light or a projectile's at the game's own light-making call
// (the sites RE::Light and RELight hook; Load3D's own body makes no projectile light), an explosion's or a hazard's
// right after the call in its update that makes it (the places Light Placer wraps), a beam's row of ours on the frames after it loads.

#include "Plugin.h"
#include "Po3Tweaks.h"
#include "StreamRows.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t kWhiteLight = 0x012FD6;  // Skyrim.esm MagicLightWhite01: a plain light to copy where a record has none

		struct Target
		{
			RE::TESObjectLIGH** slot{ nullptr };
			RE::TESObjectLIGH*  own{ nullptr };  // the record's own light (or none), given back when the row is not shown
			RE::TESObjectLIGH*  ours{ nullptr };
			std::size_t         row{ 0 };
			const RE::TESForm*  record{ nullptr };
			const char*         model{ nullptr };  // what RE::Light's mesh keys are matched on; none for a casting art, which it cannot reach
			bool                inFiles{ false };  // an RE::Light file of ours lights this record
			bool                shared{ false };   // another mod's RE::Light file names it too: its "RL" light may be theirs, never culled
			bool                tint{ false };     // a stream row's projectile: its own light worn in the row's color, nothing else of it changed
		};
		std::vector<Target> gTargets;

		// an art object a row names by its model, hung on as its effect starts (CreatureLights.cpp); written at data load before any hook goes in
		std::unordered_map<const RE::BGSArtObject*, std::size_t> gArts;
		// an impact record a row names by its model, and the row: a short list, read by a hook every particle passes (one compare each)
		std::vector<std::pair<const RE::BGSImpactData*, std::size_t>> gImpacts;
		// the same rows by their model's file name, lower case: a magic impact's particle holds no impact record, only its model name
		std::vector<std::pair<std::string, std::size_t>> gImpactNames;

		constexpr const char*       kReLightConfigs = "Data/SKSE/Plugins/ReLight/Configs";
		constexpr std::wstring_view kOurFolder = L"illuminated creature lighting ng";  // ours, under kReLightConfigs (lower case)

		// written at data load before any hook goes in; read from loader threads
		std::unordered_map<RE::FormID, std::size_t> gInFiles;  // a base record -> its place in gTargets
		std::vector<LightKit::Relaxed<bool>>        gCull;     // by that place: its row is off, so RE::Light's lights on it go out
		LightKit::Relaxed<bool>                     gAnyCull{ false };
		// any creature switched off, set at every settings change; every light of ours a record slot can point at, written at data load;
		// the spell lights culled on switched-off casters (projectiles, explosions, hazards, casting lights, beam row lights)
		LightKit::Relaxed<bool>                      gAnyCreatureOff{ false };
		std::unordered_set<const RE::TESObjectLIGH*> gOurLights;
		std::array<std::atomic<std::uint64_t>, 5>    gSwitchCulls{};

		// RE::Light named a light of its own "RL" and hung it under the reference; a culled one lights nothing (RELight's Options.cpp way)
		void CullFileLights(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_node) noexcept
		{
			Guarded("RE::Light file lights", [&]() {
				const auto* base = a_ref->GetBaseObject();
				const auto  it = base ? gInFiles.find(base->GetFormID()) : gInFiles.end();
				if (it == gInFiles.end() || !gCull[it->second]) {
					return;
				}
				RE::BSVisit::TraverseScenegraphLights(a_node, [](RE::NiPointLight* a_light) {
					if (a_light && std::string_view(a_light->name.c_str()).starts_with("RL")) {
						a_light->SetAppCulled(true);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			});
		}

		// a light the game made from a record of ours (each caller has tested that), on a reference a switched-off creature sent: culled
		void CullOne(RE::NiLight* a_light, const RE::TESObjectLIGH* a_record, RE::Actor* a_owner, std::size_t a_kind)
		{
			if (a_light && a_record && SwitchedOff(a_owner, SettingsNow())) {
				a_light->SetAppCulled(true);
				++gSwitchCulls[a_kind];
			}
		}

		// a beam's row of ours, which the shared StreamRows hangs under the projectile's root now or on a later frame (a row the light cap
		// held back is hung on a later retry) and never culls itself: looked at each frame while the projectile flies, so Illuminated's and
		// RELight's copies of StreamRows stay untouched
		// The projectile is held by pointer only until the first look (its Load3D may run on a loader thread, where no handle is made); that
		// look, on the main thread, takes its handle and every later one resolves it again, so a projectile that has died is never kept.
		// The looks last as long as the projectile does (a row the cap frees late is still caught), one chain per projectile, and the
		// projectile's own time alive (the game's livingTime) ends a chain on one that never goes
		constexpr float kMostCullWatch = 30.0f;

		// the projectiles a chain looks at now; touched only inside the UI tasks (main thread)
		std::unordered_set<std::uint32_t> gCullWatched;

		void CullRowsLater(RE::NiPointer<RE::TESObjectREFR> a_first, RE::ObjectRefHandle a_handle)
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddUITask([a_first, a_handle]() {
				Guarded("a switched-off creature's beam", [&]() {
					const auto handle = a_first ? a_first->GetHandle() : a_handle;
					const auto key = handle.native_handle();
					// a second Load3D of the same projectile: the chain already looking at it carries on alone
					if (a_first && !gCullWatched.insert(key).second) {
						return;
					}
					const auto  ref = a_first ? a_first : handle.get();
					const auto* proj = ref ? ref->As<RE::Projectile>() : nullptr;
					// the shooter's switch as it is now: switched back on, the row lights this chain culled show again and the chain ends
					const auto shooter = proj ? proj->GetProjectileRuntimeData().shooter.get() : RE::NiPointer<RE::TESObjectREFR>();
					const bool off = SwitchedOff(shooter ? shooter->As<RE::Actor>() : nullptr, SettingsNow());
					// a paused game moves no row and its projectile's livingTime stands still: while off, the chain waits without walking
					auto*      ui = RE::UI::GetSingleton();
					const bool paused = ui && ui->GameIsPaused();
					auto*      root = ref && !ref->IsDeleted() && (!off || !paused) ? ref->Get3D() : nullptr;
					if (root) {
						RE::BSVisit::TraverseScenegraphLights(root, [off](RE::NiPointLight* a_light) {
							if (StreamRows::IsRowLight(a_light) && a_light->GetAppCulled() != off) {
								a_light->SetAppCulled(off);
								if (off) {
									++gSwitchCulls[4];
								}
							}
							return RE::BSVisit::BSVisitControl::kContinue;
						});
					}
					// a projectile whose queued load has not stored its body yet, or whose row the cap may free later, is looked at again;
					// the chain ends when it is gone or its shooter is switched on
					// the next look goes by the main-thread queue: queued straight back on the UI queue it ran in the same drain, never a frame
					// later, so the projectile's livingTime never moved and the game froze (the 21:44 hang of 2026-11-22)
					const bool again = off && proj && !ref->IsDeleted() && handle && proj->GetProjectileRuntimeData().livingTime < kMostCullWatch &&
					                   Later("a switched-off creature's beam", [handle]() { CullRowsLater(nullptr, handle); });
					if (!again) {
						gCullWatched.erase(key);
					}
				});
			});
		}

		// a casting light (its caster made it) or a projectile's (its shooter sent it), as the game's call made it; any thread, so only
		// the fresh light's own flag is written
		void CullMade(RE::NiLight* a_made, const RE::TESObjectLIGH* a_record, RE::TESObjectREFR* a_ref)
		{
			if (!a_made || !a_ref || !gOurLights.contains(a_record)) {
				return;
			}
			if (auto* caster = a_ref->As<RE::Actor>()) {
				CullOne(a_made, a_record, caster, 3);
			} else if (auto* projectile = a_ref->As<RE::Projectile>()) {
				const auto shooter = projectile->GetProjectileRuntimeData().shooter.get();
				CullOne(a_made, a_record, shooter ? shooter->As<RE::Actor>() : nullptr, 0);
			}
		}

		// a beam's row of ours on a switched-off creature's projectile (its own light is culled where the game makes it, LightSite)
		void CullSwitchedOff(RE::Projectile* a_ref)
		{
			const auto* base = a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::BGSProjectile>() : nullptr;
			if (!base || !StreamRows::Claims(base->GetFormID())) {
				return;
			}
			const auto shooter = a_ref->GetProjectileRuntimeData().shooter.get();
			if (SwitchedOff(shooter ? shooter->As<RE::Actor>() : nullptr, SettingsNow())) {
				CullRowsLater(RE::NiPointer<RE::TESObjectREFR>(a_ref), {});
			}
		}

		// an explosion's or a hazard's light, after its update made it: one already culled, or not ours, is passed over (each update while
		// a creature is switched off; an explosion lives a second or two)
		void CullSwitchedOff(RE::Explosion* a_ref)
		{
			auto&       d = a_ref->GetExplosionRuntimeData();
			const auto* base = a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::BGSExplosion>() : nullptr;
			if (!d.light || d.light->GetAppCulled() || !base || !gOurLights.contains(base->data.light)) {
				return;
			}
			const auto owner = d.actorOwner.get();
			CullOne(d.light.get(), base->data.light, owner.get(), 1);
		}

		void CullSwitchedOff(RE::Hazard* a_ref)
		{
			// the record the game makes its light from: the hazard's own record field, as its light call reads it
			auto&       d = a_ref->GetHazardRuntimeData();
			const auto* base = d.hazard;
			if (!d.light || d.light->GetAppCulled() || !base || !gOurLights.contains(base->data.light)) {
				return;
			}
			const auto owner = d.ownerActor.get();
			CullOne(d.light.get(), base->data.light, owner.get(), 2);
		}

		// the game's own light-making call for a casting light and a projectile's light (TESObjectLIGH::GenDynamic, at the sites RE::Light
		// and RELight hook - their method, Truman's; ours goes in at data load, after theirs, so the light is made through them first). It
		// also runs on the game's loader and job threads. With no creature switched off: the original call and one read
		using MakeLightFn = RE::NiPointLight*(RE::TESObjectLIGH*, RE::TESObjectREFR*, RE::NiNode*, bool, bool, bool);
		template <std::size_t N>
		struct LightSite
		{
			static RE::NiPointLight* thunk(RE::TESObjectLIGH* a_light, RE::TESObjectREFR* a_ref, RE::NiNode* a_node, bool a_forceDynamic,
				bool a_useLightRadius, bool a_affectRequesterOnly)
			{
				auto* made = func(a_light, a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly);
				if (made && gAnyCreatureOff) {
					Guarded("a switched-off creature's spell light", [&]() { CullMade(made, a_light, a_ref); });
				}
				return made;
			}
			static inline REL::Relocation<MakeLightFn> func;
		};

		// an explosion's and a hazard's update: the call in it that makes the light on its first pass (the places Light Placer and
		// Illuminated wrap), then a switched-off creature's light culled. The update's time step is still live at the call (read in
		// Ghidra), so it is passed on. With no creature switched off: the original call and one read
		template <class T>
		struct UpdateSite
		{
			static bool thunk(T* a_this, float a_delta)
			{
				const bool result = func(a_this, a_delta);
				if (gAnyCreatureOff && a_this) {
					Guarded("a switched-off creature's burst", [a_this]() { CullSwitchedOff(a_this); });
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// the six call sites, at data load (after every plugin loaded): the Address Library's places on SE and AE; a site whose call is not
		// there (another plugin rewrote it) is left alone. None in VR, where this plugin has no trampoline
		void InstallSwitchSites()
		{
			if (REL::Module::IsVR()) {
				SKSE::log::info("creature switches: VR - a switched-off creature's casting lights, bolts and bursts keep their light");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			if (trampoline.free_size() < kSpellSiteBytes) {
				SKSE::log::warn("creature switches: {} of the {} trampoline bytes free; a switched-off creature's spells keep their light", trampoline.free_size(),
					kSpellSiteBytes);
				return;
			}
			std::size_t hooked = 0, wanted = 0;
			const auto  wrap = [&](std::uintptr_t a_site, auto& a_next, auto a_thunk, std::string_view a_what) {
				++wanted;
				if (!LightKit::IsCallAt(a_site)) {
					SKSE::log::warn("creature switches: {} is not the call it should be (another plugin rewrote it?); left alone", a_what);
					return;
				}
				// trampoline sites: 6
				a_next = trampoline.write_call<5>(a_site, a_thunk);
				++hooked;
			};
			using Site = REL::Relocation<std::uintptr_t>;
			wrap(Site{ RELOCATION_ID(33603, 34381), REL::VariantOffset(0xAC, 0xE2, 0xAC) }.address(), LightSite<0>::func, LightSite<0>::thunk, "a casting light's call");
			wrap(Site{ RELOCATION_ID(33391, 34151), REL::VariantOffset(0x86, 0xCD, 0x86) }.address(), LightSite<1>::func, LightSite<1>::thunk, "a casting light's second call");
			wrap(Site{ RELOCATION_ID(42965, 44222), REL::VariantOffset(0x58, 0x36D, 0x58) }.address(), LightSite<2>::func, LightSite<2>::thunk, "a projectile light's call");
			if (REL::Module::IsAE()) {
				// the casting art's own light call exists on AE only (CheckAttachCastingArt)
				wrap(Site{ RELOCATION_ID(33403, 34185), 0x407 }.address(), LightSite<3>::func, LightSite<3>::thunk, "the casting art's light call");
			}
			wrap(Site{ RELOCATION_ID(42664, 43836), 0x11 }.address(), UpdateSite<RE::Explosion>::func, UpdateSite<RE::Explosion>::thunk, "the explosion update's call");
			wrap(Site{ RELOCATION_ID(42791, 43959), REL::VariantOffset(0x11, 0x1F, 0x11) }.address(), UpdateSite<RE::Hazard>::func, UpdateSite<RE::Hazard>::thunk,
				"the hazard update's call");
			SKSE::log::info("creature switches: {} of {} spell-light call sites hooked", hooked, wanted);
		}

		// every class a record of ours makes and RE::Light hooks. With no row off and no creature switched off: the original call, two reads
		template <class T>
		struct FileLoad3D
		{
			static RE::NiAVObject* thunk(T* a_this, bool a_background)
			{
				auto* node = func(a_this, a_background);
				if (node && a_this && (gAnyCull || gAnyCreatureOff)) {
					if (gAnyCull) {
						CullFileLights(a_this, node);
					}
					if constexpr (std::is_base_of_v<RE::Projectile, T>) {
						if (gAnyCreatureOff) {
							Guarded("a switched-off creature's beam", [a_this]() { CullSwitchedOff(a_this); });
						}
					}
				}
				return node;
			}
			static void                                    Install() { func = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x6A, thunk); }
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ConfigKeys
		{
			std::vector<std::string>               keys;
			std::unordered_set<const RE::TESForm*> bases;
		};

		[[nodiscard]] bool IsOurFolder(const std::filesystem::path& a_path)
		{
			const auto name = a_path.filename().wstring();
			return std::ranges::equal(name, kOurFolder, [](wchar_t a, wchar_t b) { return std::towlower(a) == b; });
		}

		// the mesh keys and base forms the RE::Light configs under kReLightConfigs name: ours (our folder) or every other mod's
		void ReadConfigs(bool a_ours, ConfigKeys& a_out)
		{
			std::error_code ec;
			const auto      root = std::filesystem::current_path(ec) / kReLightConfigs;
			using enum std::filesystem::directory_options;
			for (std::filesystem::recursive_directory_iterator it(root, skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code one;
				if (it.depth() == 0 && it->is_directory(one) && IsOurFolder(it->path()) != a_ours) {
					it.disable_recursion_pending();
					continue;
				}
				if (it.depth() == 0 && a_ours) {
					continue;  // a file straight in the configs folder is no file of ours
				}
				if (!it->is_regular_file(one) || it->path().extension() != ".json") {
					continue;
				}
				std::ifstream in(it->path(), std::ios::binary);
				const auto    doc = nlohmann::json::parse(in, nullptr, false, true);
				if (!doc.is_array()) {
					continue;
				}
				for (const auto& entry : doc) {
					if (!entry.is_object()) {
						continue;
					}
					if (const auto mesh = entry.find("meshPath"); mesh != entry.end() && mesh->is_array()) {
						for (const auto& key : *mesh) {
							if (key.is_string()) {
								a_out.keys.push_back(key.get<std::string>());
							}
						}
					}
					// RE::Light names a base form only as "0x<id>~<plugin>": an editor ID there is not one of its lights
					if (const auto id = entry.find("baseID"); id != entry.end() && id->is_string()) {
						if (const auto text = id->get<std::string>(); Po3TweaksText::ParseKey(text).kind == Po3TweaksText::Key::kFormID) {
							if (const auto* form = FormOfKey(text)) {
								a_out.bases.insert(form);
							}
						}
					}
				}
			}
		}

		[[nodiscard]] bool Names(const ConfigKeys& a_files, const Target& a_t)
		{
			return a_t.record && (a_files.bases.contains(a_t.record) || (a_t.model && Anima::MeshKeyNames(a_t.model, a_files.keys)));
		}

		// which records our RE::Light files name, by their base form or a mesh key; none unless RE::Light runs and the files are there
		void FindFileRows()
		{
			if (!REX::W32::GetModuleHandleA("ReLight.dll")) {
				return;
			}
			ConfigKeys ours;
			ReadConfigs(true, ours);
			if (ours.keys.empty() && ours.bases.empty()) {
				return;
			}
			ConfigKeys others;
			ReadConfigs(false, others);
			gCull.assign(gTargets.size(), false);
			std::size_t shared = 0;
			for (std::size_t i = 0; i < gTargets.size(); ++i) {
				auto& t = gTargets[i];
				if (Names(ours, t)) {
					t.inFiles = true;
					t.shared = Names(others, t);
					shared += t.shared ? 1 : 0;
					gInFiles.try_emplace(t.record->GetFormID(), i);
				}
			}
			SKSE::log::info("creature spells: RE::Light runs with our files: {} record(s) lit by them, {} also named by another mod's file (never culled)",
				gInFiles.size(), shared);
		}

		// the record hooks, after RE::Light's (theirs at plugin load, these at data load): its files' lights culled where a row is off, and a
		// switched-off creature's beam rows culled (every projectile class); the explosion and the hazard only for the files
		void InstallRecordHooks()
		{
			FileLoad3D<RE::FlameProjectile>::Install();
			FileLoad3D<RE::BarrierProjectile>::Install();
			FileLoad3D<RE::BeamProjectile>::Install();
			FileLoad3D<RE::ConeProjectile>::Install();
			FileLoad3D<RE::MissileProjectile>::Install();
			FileLoad3D<RE::ArrowProjectile>::Install();
			FileLoad3D<RE::GrenadeProjectile>::Install();
			if (!gInFiles.empty()) {
				FileLoad3D<RE::Hazard>::Install();
				FileLoad3D<RE::Explosion>::Install();
			}
		}

		// a light of ours for a row, copied from the record's own light (its flags and model) or the game's plain white one
		[[nodiscard]] RE::TESObjectLIGH* MakeLight(const Anima::Row& a_row, const RE::TESObjectLIGH* a_base)
		{
			auto* out = a_base ? LightKit::NewForm<RE::TESObjectLIGH>() : nullptr;
			if (!out) {
				return nullptr;
			}
			out->data = a_base->data;
			out->fade = a_base->fade;
			out->emittanceColor = a_base->emittanceColor;
			out->lensFlare = a_base->lensFlare;
			out->SetModel(a_base->GetModel());
			// read after ReadLighting: the lighting does not change within a game
			const auto& rgb = InverseSquare() && a_row.rgbIsl ? *a_row.rgbIsl : a_row.rgb;
			out->data.color = RE::Color(static_cast<std::uint8_t>(rgb[0]), static_cast<std::uint8_t>(rgb[1]), static_cast<std::uint8_t>(rgb[2]), 0);
			return out;
		}

		// the row's strength and reach onto our light, the sliders on them (Community Shaders: inverse square, the cutoff in the falloff
		// field and the size in the field of view, as it reads a record; else a plain light whose radius carries the reach)
		void Dress(RE::TESObjectLIGH* a_light, const Anima::Row& a_row, const Anima::Settings& a_s)
		{
			const float strength = RowStrength(a_row, a_s);
			const float reach = a_row.reach * static_cast<float>(a_s.reach) / 100.0f;
			if (InverseSquare()) {
				a_light->fade = strength;
				a_light->data.radius = static_cast<std::uint32_t>(reach);
				a_light->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(a_light->data.flags.underlying() | LightKit::kRecordInverseSquare);
				a_light->data.fov = LightKit::LightPlacerSize(a_row.size);
				a_light->data.fallofExponent = LightKit::CutoffFor(strength, reach, a_row.size);
			} else {
				const auto plain = LightKit::PlainAt(strength, reach, reach);
				a_light->fade = plain.fade;
				a_light->data.radius = static_cast<std::uint32_t>(plain.radius);
			}
		}

		void Add(RE::TESObjectLIGH*& a_slot, std::size_t a_row, const RE::TESObjectLIGH* a_white, const RE::TESForm* a_record, const char* a_model, bool a_tint)
		{
			if (std::ranges::any_of(gTargets, [&](const Target& a_t) { return a_t.slot == &a_slot; })) {
				return;  // the first row that names a record wins
			}
			auto* ours = MakeLight(Rows()[a_row], a_slot ? a_slot : a_white);
			if (ours) {
				gTargets.push_back({ &a_slot, a_slot, ours, a_row, a_record, a_model, false, false, a_tint });
			}
		}

		// the art objects the rows' art= lines name: a hit art or an enchantment's art carries no light record, so a light of ours hangs on it
		void ReadArts()
		{
			gArts.clear();
			gImpacts.clear();
			gImpactNames.clear();
			const auto&                                  rows = Rows();
			std::unordered_map<std::string, std::size_t> byModel, byImpact;
			for (std::size_t i = 0; i < rows.size(); ++i) {
				if (rows[i].kind == Anima::Kind::kSpell || rows[i].kind == Anima::Kind::kBurst) {
					for (const auto& model : rows[i].arts) {
						if (const auto [at, added] = byModel.try_emplace(model, i); !added) {
							SKSE::log::info("creature spells: {} names art {} that {} names already - the first row lights it", rows[i].name, model, rows[at->second].name);
						}
					}
					for (const auto& model : rows[i].impacts) {
						if (const auto [at, added] = byImpact.try_emplace(model, i); !added) {
							SKSE::log::info("creature spells: {} names impact {} that {} names already - the first row lights it", rows[i].name, model, rows[at->second].name);
						}
					}
				}
			}
			if (!byImpact.empty()) {
				for (const auto& [model, row] : byImpact) {
					const auto cut = model.find_last_of("\\/");
					gImpactNames.emplace_back(cut == std::string::npos ? model : model.substr(cut + 1), row);
				}
				for (const auto* impact : LightKit::FormsOf<RE::BGSImpactData>()) {
					const char* model = impact ? impact->GetModel() : nullptr;
					if (const auto it = model && *model ? byImpact.find(Anima::NormalPath(model)) : byImpact.end(); it != byImpact.end()) {
						gImpacts.emplace_back(impact, it->second);
					}
				}
				SKSE::log::info("creature spells: {} impact record(s) lit as their splash plays", gImpacts.size());
			}
			if (byModel.empty()) {
				return;
			}
			// Light Placer lights these art rows from our own file in LightPlacer\IlluminatedCreatureLightingNG (it is required on
			// Community Shaders): wherever it runs, ours stands down, so no art is lit twice
			if (ArtLeftToLightPlacer()) {
				SKSE::log::info("creature spells: {} art row model(s) left to Light Placer, which lights them from our own file", byModel.size());
				return;
			}
			for (const auto* art : LightKit::FormsOf<RE::BGSArtObject>()) {
				const char* model = art ? art->GetModel() : nullptr;
				if (const auto it = model && *model ? byModel.find(Anima::NormalPath(model)) : byModel.end(); it != byModel.end()) {
					gArts.try_emplace(art, it->second);
				}
			}
			SKSE::log::info("creature spells: {} art object(s) lit as their effects start", gArts.size());
		}
	}

	bool ArtLeftToLightPlacer()
	{
		static const bool loaded = REX::W32::GetModuleHandleA("po3_LightPlacer.dll") != nullptr;
		return loaded;
	}

	void WarnIfNoLightPlacer(std::string_view a_mod)
	{
		if (ArtLeftToLightPlacer() || LightingFound() != Lighting::kShaders) {
			return;
		}
		SKSE::log::error("Light Placer is required on Community Shaders and is not loaded - this mod lights the spell art glow itself until it is installed");
		const auto text = std::format("{} needs Light Placer on Community Shaders. Install Light Placer from Nexus (mod 127557).", a_mod);
		RE::DebugMessageBox(text.c_str());
	}

	void ReadSpellRecords()
	{
		gTargets.clear();
		const auto&                                  rows = Rows();
		std::unordered_map<std::string, std::size_t> byModel;
		for (std::size_t i = 0; i < rows.size(); ++i) {
			// a stream row is drawn by StreamRows (Streams.cpp): its projectiles keep their own light
			if ((rows[i].kind != Anima::Kind::kSpell && rows[i].kind != Anima::Kind::kBurst) || rows[i].stream != Anima::Stream::kNone) {
				continue;
			}
			for (const auto& model : rows[i].models) {
				byModel.try_emplace(model, i);
			}
		}
		auto*       dh = RE::TESDataHandler::GetSingleton();
		const auto* white = dh ? dh->LookupForm<RE::TESObjectLIGH>(kWhiteLight, "Skyrim.esm") : nullptr;
		const auto  rowOf = [&](const char* a_model) -> std::optional<std::size_t> {
			if (!a_model || !*a_model) {
				return std::nullopt;
			}
			const auto it = byModel.find(Anima::NormalPath(a_model));
			return it != byModel.end() ? std::optional<std::size_t>(it->second) : std::nullopt;
		};
		// a projectile a stream row claims wears its own light in the row's color, as Illuminated drew it: no model-matched row lights it,
		// and only a row naming its record by form dresses it instead (the spider's bolt, whose own light Illuminated made from its data)
		std::unordered_set<const RE::BGSProjectile*> streamed;
		for (const auto& row : rows) {
			if (row.stream != Anima::Stream::kNone) {
				for (const auto& key : row.records) {
					auto* form = FormOfKey(key);
					if (const auto* p = form ? form->As<RE::BGSProjectile>() : nullptr; RowDrawn(p)) {
						streamed.insert(p);
					}
				}
			}
		}
		// the records named by form first: they win over a model shared with other records
		for (std::size_t i = 0; i < rows.size(); ++i) {
			if (rows[i].stream != Anima::Stream::kNone) {
				// a stream row's record StreamRows cannot draw (a missile) is lit here: one with a light of its own wears it in the row's color,
				// as the tint below does, one without gets a light of ours
				for (const auto& key : rows[i].records) {
					auto* form = FormOfKey(key);
					if (auto* p = form ? form->As<RE::BGSProjectile>() : nullptr; p && !RowDrawn(p)) {
						Add(p->data.light, i, white, p, p->GetModel(), p->data.light != nullptr);
					}
				}
				continue;
			}
			for (const auto& key : rows[i].records) {
				auto* form = FormOfKey(key);
				if (auto* p = form ? form->As<RE::BGSProjectile>() : nullptr) {
					Add(p->data.light, i, white, p, p->GetModel(), false);
				} else if (auto* e = form ? form->As<RE::BGSExplosion>() : nullptr) {
					Add(e->data.light, i, white, e, e->GetModel(), false);
				} else if (auto* h = form ? form->As<RE::BGSHazard>() : nullptr) {
					Add(h->data.light, i, white, h, h->GetModel(), false);
				} else if (auto* m = form ? form->As<RE::EffectSetting>() : nullptr) {
					Add(m->data.light, i, white, m, nullptr, false);
				} else {
					SKSE::log::info("creature spells: {} names {} - not a projectile, explosion, hazard or magic effect of this load order{}",
						rows[i].name, key, Po3Tweaks::MissingHint(key));
				}
			}
		}
		// then each stream row's projectiles that have a light of their own: that light in the row's color, its numbers left as they are
		for (std::size_t i = 0; i < rows.size(); ++i) {
			if (rows[i].stream == Anima::Stream::kNone) {
				continue;
			}
			for (const auto& key : rows[i].records) {
				auto* form = FormOfKey(key);
				if (auto* p = form ? form->As<RE::BGSProjectile>() : nullptr; p && p->data.light && RowDrawn(p)) {
					Add(p->data.light, i, white, p, p->GetModel(), true);
				}
			}
		}
		if (!byModel.empty()) {
			for (auto* p : LightKit::FormsOf<RE::BGSProjectile>()) {
				if (const auto r = p && !streamed.contains(p) ? rowOf(p->GetModel()) : std::nullopt) {
					Add(p->data.light, *r, white, p, p->GetModel(), false);
				}
			}
			for (auto* e : LightKit::FormsOf<RE::BGSExplosion>()) {
				if (const auto r = e ? rowOf(e->GetModel()) : std::nullopt) {
					Add(e->data.light, *r, white, e, e->GetModel(), false);
				}
			}
			for (auto* h : LightKit::FormsOf<RE::BGSHazard>()) {
				if (const auto r = h ? rowOf(h->GetModel()) : std::nullopt) {
					Add(h->data.light, *r, white, h, h->GetModel(), false);
				}
			}
			// a magic effect's casting light shows while its casting art is in the caster's hands (or a creature's mouth)
			for (auto* m : LightKit::FormsOf<RE::EffectSetting>()) {
				const auto* art = m ? m->data.castingArt : nullptr;
				if (const auto r = art ? rowOf(art->GetModel()) : std::nullopt) {
					Add(m->data.light, *r, white, m, nullptr, false);
				}
			}
		}
		SKSE::log::info("creature spells: {} record(s) given a light of ours", gTargets.size());
		ReadArts();
		FindFileRows();
		for (const auto& t : gTargets) {
			gOurLights.insert(t.ours);
			// a record with an RE::Light file points back at its own light: a switched-off caster's light from it goes out too
			if (t.inFiles && t.own) {
				gOurLights.insert(t.own);
			}
		}
		if (!gTargets.empty()) {
			InstallRecordHooks();
			InstallSwitchSites();
		}
	}

	std::optional<std::size_t> ArtRow(const RE::BGSArtObject* a_art)
	{
		const auto it = a_art ? gArts.find(a_art) : gArts.end();
		return it != gArts.end() ? std::optional<std::size_t>(it->second) : std::nullopt;
	}

	bool AnyArtRows()
	{
		return !gArts.empty();
	}

	std::optional<std::size_t> ImpactRow(const RE::BGSImpactData* a_impact)
	{
		for (const auto& [impact, row] : gImpacts) {
			if (impact == a_impact) {
				return row;
			}
		}
		return std::nullopt;
	}

	bool AnyImpactRows()
	{
		return !gImpacts.empty() || !gImpactNames.empty();
	}

	std::optional<std::size_t> ImpactRowByModel(const char* a_model)
	{
		if (!a_model || gImpactNames.empty()) {
			return std::nullopt;
		}
		const std::string_view model(a_model);
		const auto             cut = model.find_last_of("\\/");
		const auto             name = cut == std::string_view::npos ? model : model.substr(cut + 1);
		for (const auto& [tail, row] : gImpactNames) {
			if (name.size() == tail.size() && std::ranges::equal(name, tail, [](char a_a, char a_b) {
					return std::tolower(static_cast<unsigned char>(a_a)) == static_cast<unsigned char>(a_b);
				})) {
				return row;
			}
		}
		return std::nullopt;
	}

	void ApplySpellRecords(const Anima::Settings& a_s)
	{
		const auto& rows = Rows();
		bool        anyCull = false;
		for (std::size_t i = 0; i < gTargets.size(); ++i) {
			auto& t = gTargets[i];
			// another mod's Light Placer file on this art lights it, and a projectile or explosion has no creature to put that light out on: ours stays off
			const bool shown = t.row < rows.size() && Anima::Shown(rows[t.row], a_s) && !RowModelsLitByOthers(t.row);
			if (t.inFiles) {
				const bool cull = !shown && !t.shared;
				gCull[i] = cull;
				anyCull = anyCull || cull;
			}
			const bool ours = shown && !t.inFiles;
			// a tint keeps its own light's numbers: Illuminated left a copy of the game's light unscaled, only its color set
			if (ours && !t.tint) {
				Dress(t.ours, rows[t.row], a_s);
			} else if (ours) {
				// Illuminated's copy wears its row's strength and reach (the game's own numbers for these lights), not the load order's: another
				// mod raising the dragon fire light to 13 made ours four times Illuminated's. Only the Brightness slider reaches it
				t.ours->fade = rows[t.row].strength * static_cast<float>(a_s.brightness) / 100.0f;
				t.ours->data.radius = static_cast<std::uint32_t>(rows[t.row].reach);
			}
			*t.slot = ours ? t.ours : t.own;
		}
		gAnyCull = anyCull;
		gAnyCreatureOff = a_s.creaturesOff && !a_s.creaturesOff->empty();
	}

	std::size_t SpellRecordCount()
	{
		return gTargets.size();
	}

	std::vector<std::string> SpellRecordsOurs()
	{
		std::vector<std::string> out;
		for (const auto& t : gTargets) {
			if (t.record && t.slot && *t.slot == t.ours) {
				out.push_back(std::format("{:08X}", t.record->GetFormID()));
			}
		}
		return out;
	}

	bool AnyCreatureOff()
	{
		return gAnyCreatureOff;
	}

	std::array<std::uint64_t, 5> SwitchCulls()
	{
		return { gSwitchCulls[0].load(std::memory_order_relaxed), gSwitchCulls[1].load(std::memory_order_relaxed), gSwitchCulls[2].load(std::memory_order_relaxed),
			gSwitchCulls[3].load(std::memory_order_relaxed), gSwitchCulls[4].load(std::memory_order_relaxed) };
	}
}
