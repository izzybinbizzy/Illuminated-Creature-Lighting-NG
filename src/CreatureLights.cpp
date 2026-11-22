// Illuminated - Creature Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The lights. Nothing here runs on a clock (rule file 2 rule 19c, the plugin framework section 7). A character's own 3D slots say when
// a creature's 3D loads or goes, the game's events when it dies, casts or leaves combat and when her cell changes; each note reaches
// the main thread as ONE SKSE task per burst, and that task makes or drops the lights and starts their motion. A light's motion belongs to the game: each
// light has a dimmer controller (the game's own class) playing keys we build once, so its flicker, its death fade and a cast's surge
// run with no code of ours and end with the light. Which creatures are lit within the light cap is judged at those events, never
// while she walks inside one cell.
// Where another creature lighting mod lights a creature, ours is the light it gets and the other mod's is put out (its menu switch gives
// the creature back and ours goes off there). A mod's ability that carries a light can reach a creature at any time: the game's apply
// event says so, and what a creature wears is read once when it loads. A loading screen takes our lights out of the scene's lists and
// nothing says so, so a creature still lit when one ends has its lights made again.
//
// CREDIT: a light is made and registered the way ReLight by Truman does it (github.com/TrumanGIT/ReLight, GPL-3.0-or-later,
// used with his permission): LightKit.h CloneLight / Hang. The dimmer is the game's own; its addresses and how a light's fade is
// put on one were read off CommonLibSSE-NG #477 and Baked Goods by ItzIvy05 (MIT / GPL-3.0). The Character::Load3D hook, and hanging
// lights on the body it returns before the game stores it, follow Light Placer's Load3D hook (powerof3, GPL-3.0, with his permission).

#include "Plugin.h"
#include "Po3Tweaks.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kName = "IlluminatedCreatureLightingNGLight";
		constexpr std::size_t kCostSlot = 0;
		// the parts of one event's work, metered apart: the loaded notes and the whole re-pick (a per-actor scope was taken out: it
		// cost two clock reads per actor walked)
		constexpr std::size_t kCostLoaded = 1;
		constexpr std::size_t kCostPick = 2;
		constexpr std::size_t kCostTheirs = 3;

		using Clock = std::chrono::steady_clock;

		struct One
		{
			RE::NiPointer<RE::NiPointLight>        light;
			RE::NiPointer<RE::BSLight>             bs;
			RE::NiPointer<RE::NiInterpController>  dimmer;
			RE::NiPointer<RE::NiFloatInterpolator> loop;  // its flicker, kept to go back to after a cast's surge
			RE::NiPointer<RE::NiFloatInterpolator> cast;  // its surge and flicker, made at its first cast
			RE::NiPointer<RE::NiFloatInterpolator> half;  // its flicker at the search share, made the first time its fight searches
			std::size_t                            row{ 0 };
			float                                  fade{ 0.0f };    // what it shows at rest: the keys are built on this
			bool                                   waits{ false };  // dark until noticed: its ease plays, and its flicker follows at the end
		};

		struct Lit
		{
			RE::ActorHandle                  actor;
			RE::NiPointer<RE::NiAVObject>    root;
			std::vector<One>                 lights;
			std::uint32_t                    seed{ 0 };
			bool                             dying{ false };
			bool                             loading{ false };  // hung on a body the game has not stored yet
			std::optional<Clock::time_point> castAt;            // for the report only: no motion reads it
			float                            notice{ 1.0f };    // dark until noticed: the share of its lights its fight gives now
		};

		std::unordered_map<RE::FormID, Lit> gLit;  // main thread only
		std::atomic<std::size_t>            gLitCount{ 0 };
		// how many applied abilities that carry a light the event noted (the game check: does it announce a script's AddSpell); a sink writes it
		std::atomic<std::uint32_t> gLitAbilitiesNoted{ 0 };
		// how many creatures were still lit when a loading screen ended (the game check: does any keep its body through one); main thread only
		std::atomic<std::uint32_t> gHungAgain{ 0 };
		// how many casting lights a creature left up as its 3D went were put out (the game check for his 580); main thread only
		std::atomic<std::uint32_t> gCastLightsOut{ 0 };

		// dark until noticed: an ease that waits for its hand-over is looked at once a frame for the longest ease and two seconds more of
		// game time (unpaused frames' own seconds, whatever the frame rate) - a light nothing updates never runs its keys out
		[[nodiscard]] float HandOverSeconds() noexcept
		{
			return (std::max)(Anima::gTuning.noticeUpSeconds, Anima::gTuning.noticeDownSeconds) + 2.0f;
		}
		float             gHandOverRan{ 0.0f };  // main thread only: the seconds the looks have run since the last ease began
		std::atomic<bool> gHandOverQueued{ false };

		// a light of ours on an art object a row names (a hit art, an enchantment's art: no light record to point), on the node the art
		// attaches to, keyed by its effect's id - the effect itself is never kept (the plugin framework section 2); main thread only
		struct ArtOne
		{
			std::uint32_t                   effect{ 0 };
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
		};
		std::vector<ArtOne> gArtLit;
		// any thread: an art light is up or one is noted and not hung yet, so an art effect's end is noted; written under gNoted.lock
		std::atomic<bool> gArtArmed{ false };
		// the art-start hook also notes art on creatures for another mod's light (only when such a mod is installed)
		bool gWatchTheirArt{ false };

		// a light of ours on an impact's splash (an impact record names no light), under its particle's 3D, keyed by the particle, read
		// through the key only under gImpactLock while the entry stands. Judged once, at the game's call that sets its finished copy up; its
		// end (Detach, or a free with no Detach) takes the entry out and hands the light to the main-thread task
		struct ImpactOne
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			std::size_t                     row{ 0 };  // its row, kept so a settings change re-dresses or re-hangs it
		};
		std::mutex                                                     gImpactLock;
		std::unordered_map<const RE::BSTempEffectParticle*, ImpactOne> gImpactLit;  // under gImpactLock; no light = judged, nothing hung
		std::atomic<std::size_t>                                       gImpactCount{ 0 };
		// the main thread, recorded at install (data load runs on it): a particle updated anywhere else has its light hung by the task
		std::uint32_t              gMainThread{ 0 };
		std::atomic<std::uint64_t> gImpactSeen{ 0 };        // splashes whose copy the game set up while an impact row exists
		std::atomic<RE::FormID>    gImpactLastRecord{ 0 };  // the impact record of the last one (0 = the particle held none)
		std::mutex                 gImpactModelsLock;
		std::vector<std::string>   gImpactModels;  // the newest model names of particles no row's impact record named (DevBench)

		// what a splash that stayed dark really wore: devbench lists these names
		void NoteImpactModel(std::string_view a_name)
		{
			std::scoped_lock l{ gImpactModelsLock };
			if (std::ranges::find(gImpactModels, a_name) != gImpactModels.end()) {
				return;
			}
			if (gImpactModels.size() >= 16) {
				gImpactModels.erase(gImpactModels.begin());
			}
			gImpactModels.emplace_back(a_name);
		}

		void TakeImpact(ImpactOne& a_one, RE::ShadowSceneNode* a_scene)
		{
			LightKit::TakeOff(a_scene, a_one.bs);
			if (a_one.light && a_one.light->parent) {
				a_one.light->parent->DetachChild(a_one.light.get());
			}
			a_one = {};
		}

		// the light limit is Light Limit Fix's, linear or inverse square alike
		// lit creatures at once (the Advanced Settings file): Light Limit Fix's budget, linear or inverse square alike
		[[nodiscard]] std::size_t Cap() { return LightLimitFix() ? Anima::gTuning.litShaders : Anima::gTuning.litPlain; }

		// any thread: no light of ours in the scene as of the last task
		[[nodiscard]] bool NothingLit() noexcept { return gLitCount.load(std::memory_order_relaxed) == 0; }

		// ---------------------------------------------------------------- the game's dimmer controller (not in our pin of CommonLib)
		[[nodiscard]] RE::NiInterpController* NewDimmer()
		{
			using F = RE::NiInterpController* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(72049, 73695) };
			return make();
		}

		[[nodiscard]] RE::NiFloatInterpolator* NewInterpolator()
		{
			using F = RE::NiFloatInterpolator* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(71088, 72686) };
			return make();
		}

		[[nodiscard]] RE::NiFloatData* NewFloatData()
		{
			using F = RE::NiFloatData* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(71069, 72667) };
			return make();
		}

		[[nodiscard]] RE::NiFloatKey* NewLinearKeys(std::uint32_t a_count)
		{
			using F = RE::NiFloatKey* (*)(std::uint32_t);
			static REL::Relocation<F> make{ RELOCATION_ID(72540, 74222) };
			return make(a_count);
		}

		void ReplaceKeys(RE::NiFloatData* a_data, RE::NiFloatKey* a_keys, std::uint32_t a_count)
		{
			using F = void (*)(RE::NiFloatData*, RE::NiFloatKey*, std::uint32_t, RE::NiAnimationKey::KeyType);
			static REL::Relocation<F> replace{ RELOCATION_ID(71071, 72669) };
			replace(a_data, a_keys, a_count, RE::NiAnimationKey::KeyType::kLink);
		}

		// the dimmer's five ids are in the VR Address Library only from 0.295.0, and a missing id stops the game at its first use -
		// so with an older one no keys are made and each light holds its fade
		[[nodiscard]] bool DimmerReady()
		{
#ifdef ENABLE_SKYRIM_VR
			static const bool ready = [] {
				const bool ok = REL::IDDB::get().IsVRAddressLibraryAtLeastVersion("0.295.0");
				if (!ok) {
					SKSE::log::warn("VR Address Library older than 0.295.0: creature lights hold steady (no flicker, death fade or surge) until it is updated");
				}
				return ok;
			}();
			return ready;
#else
			return true;
#endif
		}

		// keys the game can play; empty when it would not make them (the light then simply holds its fade)
		[[nodiscard]] RE::NiPointer<RE::NiFloatInterpolator> MakeKeys(const std::vector<Anima::Key>& a_keys)
		{
			if (!DimmerReady()) {
				return {};
			}
			RE::NiPointer<RE::NiFloatInterpolator> interpolator(NewInterpolator());
			const RE::NiPointer<RE::NiFloatData>   data(NewFloatData());
			if (!interpolator || !data || a_keys.size() < 2) {
				return {};
			}
			const auto count = static_cast<std::uint32_t>(a_keys.size());
			auto*      keys = NewLinearKeys(count);
			if (!keys) {
				return {};
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				keys[i].SetTime(a_keys[i].time);
				keys[i].SetValue(a_keys[i].value);
			}
			ReplaceKeys(data.get(), keys, count);
			interpolator->floatData = data;
			return interpolator;
		}

		// these keys on this light's dimmer from their start, made the first time; the game runs it from here on. Any keys played end the
		// wait for an ease's hand-over: the caller that wants one sets it after
		void Play(One& a_one, const RE::NiPointer<RE::NiFloatInterpolator>& a_keys, float a_seconds, bool a_loop)
		{
			a_one.waits = false;
			if (!a_keys || !a_one.light) {
				return;
			}
			if (!a_one.dimmer) {
				a_one.dimmer.reset(NewDimmer());
				if (!a_one.dimmer) {
					return;
				}
				using Flag = RE::NiAVObject::Flag;
				a_one.light->GetFlags().set(Flag::kSelectiveUpdate, Flag::kSelectiveUpdateController);
				a_one.dimmer->SetTarget(a_one.light.get());
			}
			using Time = RE::NiTimeController::Flag;
			a_keys->lastIndex = 0;
			a_one.dimmer->SetInterpolator(a_keys.get(), 0);
			a_one.dimmer->flags.reset(Time::kCycleType_Mask);
			a_one.dimmer->flags.set(Time::kAnimType_AppInit, Time::kComputeScaledTime, a_loop ? Time::kCycleType_Loop : Time::kCycleType_Clamp);
			a_one.dimmer->frequency = 1.0f;
			a_one.dimmer->phase = 0.0f;
			a_one.dimmer->loKeyTime = 0.0f;
			a_one.dimmer->hiKeyTime = a_seconds;
			a_one.dimmer->Start(0.0f);
		}

		[[nodiscard]] Pulse::Kind ShapeOf(const Anima::Row& a_row, const Anima::Settings& a_s)
		{
			return Anima::Flickers(a_s) ? Anima::PulseOf(a_row.element) : Pulse::Kind::kNone;
		}

		// ---------------------------------------------------------------- the lights
		void Drop(Lit& a_lit, RE::ShadowSceneNode* a_scene)
		{
			for (auto& one : a_lit.lights) {
				// one.bs is the BSLight the scene made for one.light (Promote converts that same object), so TakeOff removes our own light
				LightKit::TakeOff(a_scene, one.bs);
				if (one.light && one.light->parent) {
					one.light->parent->DetachChild(one.light.get());
				}
			}
			a_lit = {};
		}

		void TakeArt(ArtOne& a_one, RE::ShadowSceneNode* a_scene)
		{
			LightKit::TakeOff(a_scene, a_one.bs);
			if (a_one.light && a_one.light->parent) {
				a_one.light->parent->DetachChild(a_one.light.get());
			}
			a_one = {};
		}

		void DropArt()
		{
			auto* scene = LightKit::Scene();
			for (auto& one : gArtLit) {
				TakeArt(one, scene);
			}
			gArtLit.clear();
		}

		// the creatures' lights and the art lights; the splashes' keep their entries (a settings change re-dresses them, RedressImpacts)
		void DropCreatures()
		{
			auto* scene = LightKit::Scene();
			for (auto& [id, lit] : gLit) {
				Drop(lit, scene);
			}
			gLit.clear();
			gLitCount = 0;
			DropArt();
		}

		void DropAll()
		{
			DropCreatures();
			auto*            scene = LightKit::Scene();
			std::scoped_lock l{ gImpactLock };
			for (auto& [particle, one] : gImpactLit) {
				TakeImpact(one, scene);
			}
			gImpactLit.clear();
			gImpactCount = 0;
		}

		// the light's fields from the strength and reach (Illuminated's way: inverse-square words on Community Shaders, else a plain
		// light whose radius carries the reach, with a tenth of its color as ambient - RE::Light's rule)
		void Dress(RE::NiPointLight* a_light, const Anima::Row& a_row, float a_strength, const Anima::Settings& a_s, const std::optional<std::array<float, 3>>& a_picked)
		{
			auto&             data = a_light->GetLightRuntimeData();
			const RE::NiColor color = a_picked ? RE::NiColor{ (*a_picked)[0], (*a_picked)[1], (*a_picked)[2] } : RE::NiColor{ a_row.rgb[0] / 255.0f, a_row.rgb[1] / 255.0f, a_row.rgb[2] / 255.0f };
			const float       reach = a_row.reach * static_cast<float>(a_s.reach) / 100.0f;
			const float       strength = (std::max)(a_strength, 0.0f);
			data.diffuse = color;
			if (InverseSquare()) {
				data.fade = strength;
				if (data.radius.x != reach) {
					data.radius = { reach, reach, LightKit::kLightSize };
					a_light->SetLightAttenuation(reach);
				}
				LightKit::Isl::SetOwn(a_light);
				LightKit::Isl::SetCutoff(a_light, LightKit::CutoffFor((std::max)(strength, 0.001f), reach));
			} else {
				const auto plain = LightKit::PlainAt(strength, reach, reach);
				data.fade = plain.fade;
				if (data.radius.x != plain.radius) {
					data.radius = { plain.radius, plain.radius, LightKit::kLightSize };
					a_light->SetLightAttenuation(plain.radius);
				}
				// under Community Shaders with no inverse-square shader, Light Limit Fix reads these words as flags: its own linear words, no ambient
				if (LightLimitFix()) {
					LightKit::Isl::SetOwnLinear(a_light);
				} else {
					data.ambient = LightKit::Tenth(color);
				}
			}
		}

		// the bone a row's light hangs on: the first of its nodes the creature's skeleton has, nullptr when it has none of them
		[[nodiscard]] RE::NiNode* BoneFor(RE::NiAVObject* a_root, const Anima::Row& a_row)
		{
			for (const auto& name : a_row.nodes) {
				if (auto* found = a_root->GetObjectByName(name.c_str()); found && found->AsNode()) {
					return found->AsNode();
				}
			}
			return nullptr;
		}

		// the art effects whose art a row names, each with the node its art attaches to: a_effects names which (empty with a_all: every one,
		// after a load, a loading screen or a settings change). Read under the game's lock for its list, lit after it is let go
		void LightArt(const std::vector<std::uint32_t>& a_effects, bool a_all)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			auto* scene = LightKit::Scene();
			if (!lists || !scene || !AnyArtRows()) {
				return;
			}
			const auto s = SettingsNow();
			struct Found
			{
				std::uint32_t                 effect{ 0 };
				std::size_t                   row{ 0 };
				RE::NiPointer<RE::NiAVObject> root;
			};
			std::vector<Found> found;
			lists->ForEachModelEffect([&](RE::ModelReferenceEffect* a_effect) {
				if (!a_effect) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto row = a_effect->finished ? std::nullopt : ArtRow(a_effect->artObject);
				const auto id = a_effect->effectID;
				const bool wanted = a_all || std::ranges::find(a_effects, id) != a_effects.end();
				if (row && wanted && RowShows(*row, s) && std::ranges::none_of(gArtLit, [id](const ArtOne& a_one) { return a_one.effect == id; })) {
					found.push_back({ id, *row, RE::NiPointer<RE::NiAVObject>(a_effect->GetAttachRoot()) });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto& [id, i, root] : found) {
				auto* node = root ? root->AsNode() : nullptr;
				auto* light = node ? LightKit::CloneLight() : nullptr;
				if (!light) {
					continue;
				}
				const auto& row = Rows()[i];
				light->name = kName;
				light->local.translate = { row.points.front().x, row.points.front().y, row.points.front().z };
				light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal ours: Dress attenuates it at once
				Dress(light, row, RowStrength(row, s), s, std::nullopt);
				RE::NiPointer<RE::BSLight> bs(LightKit::Hang(node, light, scene, 1.0f, true));
				if (bs) {
					gArtLit.push_back({ id, RE::NiPointer<RE::NiPointLight>(light), bs });
				}
			}
		}

		// any thread: the particle is judged once - an entry with its row claims it, and its end (Detach, a free) clears the entry
		[[nodiscard]] bool ClaimImpact(const RE::BSTempEffectParticle* a_particle, std::size_t a_row)
		{
			std::scoped_lock l{ gImpactLock };
			const bool       claimed = gImpactLit.try_emplace(a_particle, ImpactOne{ {}, {}, a_row }).second;
			gImpactCount = gImpactLit.size();
			return claimed;
		}

		// main thread: one light of its row on a claimed particle still in the world; a particle that ended meanwhile (its entry cleared, or its
		// copy no longer in the scene) gets none, and one that ends while the light is made has it taken off at once
		void HangImpact(const RE::BSTempEffectParticle* a_particle, std::size_t a_row)
		{
			// the copy is read under the lock its end takes the entry out under: the detach and the free clear the entry before the game lets go
			// of the copy, so a copy read here is held by us from then on, or never read
			RE::NiPointer<RE::NiAVObject> object;
			{
				std::scoped_lock l{ gImpactLock };
				if (!gImpactLit.contains(a_particle)) {
					return;
				}
				object = a_particle->particleObject;
			}
			const auto s = SettingsNow();
			auto*      node = object && object->parent ? object->AsNode() : nullptr;
			auto*      scene = LightKit::Scene();
			ImpactOne  one;
			one.row = a_row;
			auto* light = node && scene && RowShows(a_row, s) ? LightKit::CloneLight() : nullptr;
			if (light) {
				const auto& row = Rows()[a_row];
				light->name = kName;
				light->local.translate = { row.points.front().x, row.points.front().y, row.points.front().z };
				light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal ours: Dress attenuates it at once
				Dress(light, row, RowStrength(row, s), s, std::nullopt);
				RE::NiPointer<RE::BSLight> bs(LightKit::Hang(node, light, scene, 1.0f, true));
				if (bs) {
					one = { RE::NiPointer<RE::NiPointLight>(light), bs, a_row };
				}
			}
			if (!one.bs) {
				return;
			}
			{
				std::scoped_lock l{ gImpactLock };
				if (const auto it = gImpactLit.find(a_particle); it != gImpactLit.end()) {
					// two hang tasks for one particle (a settings change re-hung it between them): one tracked light, never two
					TakeImpact(it->second, scene);
					it->second = std::move(one);
					return;
				}
			}
			TakeImpact(one, scene);
		}

		// main thread, a settings change: each splash still in the world is judged again - its light re-dressed in place, taken off when its
		// row no longer shows, or hung when its row shows now and it had none (the game sets each splash's copy up only once)
		void RedressImpacts(const Anima::Settings& a_s)
		{
			auto*                                                                scene = LightKit::Scene();
			std::vector<std::pair<const RE::BSTempEffectParticle*, std::size_t>> hang;
			{
				std::scoped_lock l{ gImpactLock };
				for (auto& [particle, one] : gImpactLit) {
					const bool shows = one.row < Rows().size() && RowShows(one.row, a_s);
					if (one.light && shows) {
						Dress(one.light.get(), Rows()[one.row], RowStrength(Rows()[one.row], a_s), a_s, std::nullopt);
					} else if (one.light) {
						const auto row = one.row;
						TakeImpact(one, scene);
						one.row = row;
					} else if (shows) {
						hang.emplace_back(particle, one.row);
					}
				}
			}
			// HangImpact reads each particle only under the lock, and only while its entry stands
			for (const auto& [particle, row] : hang) {
				HangImpact(particle, row);
			}
		}

		void ArtGone(const std::vector<std::uint32_t>& a_effects)
		{
			auto* scene = LightKit::Scene();
			std::erase_if(gArtLit, [&](ArtOne& a_one) {
				if (std::ranges::find(a_effects, a_one.effect) == a_effects.end()) {
					return false;
				}
				TakeArt(a_one, scene);
				return true;
			});
		}

		// creatures that got up from their furniture and have not lain down again (main thread): no longer judged asleep
		std::unordered_set<RE::FormID> gWoken;

		// an undead asleep in its furniture (a draugr in its coffin or on its slab) shows no light until it gets up - the game's own eye glow
		// does the same: DraugrFXScript plays none while the draugr sleeps and plays it at OnGetUp, the furniture's exit event. Lying down
		// counts already, as the furniture's enter event comes before the sleep state reads sleeping
		[[nodiscard]] bool AsleepUndead(RE::Actor* a_actor)
		{
			// ActorTypeUndead, Skyrim.esm: the keyword the game's eye glow effect asks for
			static auto* const undead = RE::TESForm::LookupByID<RE::BGSKeyword>(0x013796);
			using Seat = RE::SIT_SLEEP_STATE;
			const auto state = a_actor->AsActorState()->GetSitSleepState();
			const bool lying = state == Seat::kWantToSleep || state == Seat::kWaitingForSleepAnim || state == Seat::kIsSleeping;
			return undead && lying && !gWoken.contains(a_actor->GetFormID()) && a_actor->HasKeyword(undead);
		}

		// the rows of this creature to light now: those the settings show, on remains only those that keep an ember, none on an undead
		// asleep in its furniture, and none where its switch has given it back to another mod
		[[nodiscard]] std::vector<std::size_t> WantedRows(RE::Actor* a_actor, const Anima::Settings& a_s)
		{
			if (!a_actor->IsDead() && AsleepUndead(a_actor)) {
				return {};
			}
			auto       rows = RowsOf(a_actor);
			const bool dead = a_actor->IsDead();
			std::erase_if(rows, [&](std::size_t a_i) {
				return !RowShows(a_i, a_s) || (dead && !Rows()[a_i].ember);
			});
			if (!rows.empty() && GivenToThem(a_actor, a_s)) {
				rows.clear();
			}
			return rows;
		}

		void ApplyNotice(Lit& a_lit, float a_share, const Anima::Settings& a_s, bool a_snap);

		// the game's own three-state combat value, as Actor::GetCombatState (1.6.1170 id 38556, the Signatures sheet) works it out: out of
		// combat, searching (its combat group's search state above 0) or in combat - read through the same two calls it makes
		[[nodiscard]] int CombatStateOf(RE::Actor* a_actor)
		{
			if (!a_actor->IsInCombat()) {
				return Anima::StartState(false, false);
			}
			const auto* group = a_actor->GetCombatGroup();
			return Anima::StartState(true, group && group->searchState > 0);
		}

		// lights this creature when a row names it and the settings show it; drops its lights when not. a_loading: the body its load
		// just returned, which a queued load hands over before the game stores it
		void Consider(RE::Actor* a_actor, const Anima::Settings& a_s, RE::NiAVObject* a_loading = nullptr)
		{
			if (!a_actor) {
				return;
			}
			auto*      scene = LightKit::Scene();
			const auto id = a_actor->GetFormID();
			auto*      stored = a_actor->Is3DLoaded() ? a_actor->Get3D() : nullptr;
			auto*      root = stored ? stored : a_loading;
			auto       it = gLit.find(id);
			if (!root && it != gLit.end() && it->second.loading) {
				return;  // nothing to judge until the game stores the body it is lit on
			}
			if (it != gLit.end() && it->second.root.get() == root && a_actor->IsDead()) {
				return;  // its death fade or ember runs by itself; Pick lets it go
			}
			const auto rowIds = root ? WantedRows(a_actor, a_s) : std::vector<std::size_t>{};
			if (rowIds.empty() || !scene) {
				if (it != gLit.end()) {
					Drop(it->second, scene);
					gLit.erase(it);
				}
				return;
			}
			if (it != gLit.end() && it->second.root.get() == root && !it->second.dying) {
				return;  // already lit on this body
			}
			if (it != gLit.end()) {
				Drop(it->second, scene);
				gLit.erase(it);
			}
			if (gLit.size() >= Cap()) {
				return;  // the next event's Pick swaps in a nearer creature for the farthest
			}
			Lit lit;
			lit.actor = RE::ActorHandle(a_actor);
			lit.root.reset(root);
			lit.seed = id * 2654435761u;
			lit.dying = a_actor->IsDead();
			lit.loading = stored == nullptr;
			for (const auto i : rowIds) {
				const auto& row = Rows()[i];
				auto*       bone = BoneFor(root, row);
				auto*       node = bone ? bone : root->AsNode();
				float       strength = RowStrength(row, a_s);
				if (lit.dying) {
					// dead before we saw it: only an ember is left
					strength *= Anima::DeathShare(Anima::gTuning.deathFadeSeconds, a_s.deathFade, row.ember);
				}
				for (const auto& p : Anima::SpotsOn(row, bone != nullptr)) {
					auto* light = node ? LightKit::CloneLight() : nullptr;
					if (!light) {
						continue;
					}
					light->name = kName;
					light->local.translate = { p.x, p.y, p.z };
					light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal ours: Dress attenuates it at once
					Dress(light, row, strength, a_s, PickedColor(i));
					// not portal strict: a creature's glow goes through doorways with it
					RE::NiPointer<RE::BSLight> bs(LightKit::Hang(node, light, scene, 1.0f, false));
					if (!bs) {
						continue;
					}
					One one{ RE::NiPointer<RE::NiPointLight>(light), bs, {}, {}, {}, {}, i, light->GetLightRuntimeData().fade, false };
					if (const auto shape = ShapeOf(row, a_s); !lit.dying && shape != Pulse::Kind::kNone) {
						one.loop = MakeKeys(Anima::FadeKeys(one.fade, shape, lit.seed, Anima::kLoopSeconds, false));
						Play(one, one.loop, Anima::kLoopSeconds, true);
					}
					lit.lights.push_back(std::move(one));
				}
			}
			if (lit.lights.empty()) {
				return;
			}
			auto& placed = gLit.emplace(id, std::move(lit)).first->second;
			// dark until noticed: its lights start at the share its fight gives now - dark when calm, part way while it searches (a creature
			// already searching when a save loads included: the game's own value, not the last event)
			if (a_s.noticed && !placed.dying) {
				if (const float share = Anima::NoticeShare(CombatStateOf(a_actor)); share < 1.0f) {
					ApplyNotice(placed, share, a_s, true);
				}
			}
		}

		// a dying creature's lights have run out (no ember): nothing left to show
		[[nodiscard]] bool RunOut(const Lit& a_lit)
		{
			return a_lit.dying && std::ranges::all_of(a_lit.lights, [](const One& a_one) {
				return !a_one.light || a_one.light->GetLightRuntimeData().fade <= 0.001f;
			});
		}

		// the characters whose 3D just came, each with the body a queued load handed over (none when the game has stored it)
		using Arrivals = std::unordered_map<RE::FormID, RE::NiPointer<RE::NiAVObject>>;

		void LookAt(RE::FormID a_id);

		// the creatures lit within the cap, the living first, then the nearest; a creature whose body went or whose light has run out let
		// go: at an event that can change them, never on a clock. a_follow: a creature that gains or loses our light is looked at again
		// (false when the caller sets every creature's other lights itself)
		void Pick(const Anima::Settings& a_s, const Arrivals& a_arrivals, bool a_follow)
		{
			auto* scene = LightKit::Scene();
			auto* player = RE::PlayerCharacter::GetSingleton();
			// another mod's light follows ours: a creature that gains or loses our light is looked at again (TheirLights)
			std::unordered_set<RE::FormID> before;
			for (const auto& [id, lit] : gLit) {
				before.insert(id);
			}
			const auto lookAtChanged = [&]() {
				if (!a_follow) {
					return;
				}
				for (const auto id : before) {
					if (!gLit.contains(id)) {
						LookAt(id);
					}
				}
				for (const auto& [id, lit] : gLit) {
					if (!before.contains(id)) {
						LookAt(id);
					}
				}
			};
			std::erase_if(gLit, [&](auto& a_entry) {
				auto& lit = a_entry.second;
				auto  actor = lit.actor.get();
				auto* root = actor && actor->Is3DLoaded() ? actor->Get3D() : nullptr;
				// a body still on its way waits only while its cell is loaded: a cell gone before the store sends no note
				const auto* cell = actor ? actor->GetParentCell() : nullptr;
				const bool  keep = Anima::KeepsLights(root != nullptr, root == lit.root.get(), RunOut(lit), lit.loading && cell && cell->IsAttached());
				if (keep) {
					lit.loading = lit.loading && !root;
					return false;
				}
				Drop(lit, scene);
				return true;
			});
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!player || !lists) {
				lookAtChanged();
				return;
			}
			const auto here = player->GetPosition();
			// who may have a light: the game's near actors that would show one (one that would not holds no place in the cap), and those
			// already lit, since one whose 3D has just attached is not on that list yet and no later event would bring it back
			std::vector<RE::Actor*>        actors;
			std::vector<Anima::Candidate>  candidates;
			std::unordered_set<RE::FormID> listed;
			// each actor's rows are worked out once
			const auto add = [&](RE::Actor* a_actor, const std::vector<std::size_t>& a_rows) {
				const bool faint = std::ranges::all_of(a_rows, [](std::size_t a_i) { return Rows()[a_i].kind == Anima::Kind::kEyes || Rows()[a_i].kind == Anima::Kind::kMachines; });
				candidates.push_back({ a_actor->IsDead(), here.GetDistance(a_actor->GetPosition()), actors.size(), faint });
				actors.push_back(a_actor);
				listed.insert(a_actor->GetFormID());
			};
			for (auto& handle : lists->highActorHandles) {
				if (auto actor = handle.get(); actor && actor->Is3DLoaded()) {
					if (const auto rows = WantedRows(actor.get(), a_s); gLit.contains(actor->GetFormID()) || !rows.empty()) {
						add(actor.get(), rows);
					}
				}
			}
			for (const auto& [id, lit] : gLit) {
				if (auto actor = lit.actor.get(); actor && !listed.contains(id)) {
					add(actor.get(), WantedRows(actor.get(), a_s));
				}
			}
			// a queued load's body is not stored yet, so no list names it, and it may well be the nearest
			for (const auto& [id, body] : a_arrivals) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || listed.contains(id)) {
					continue;
				}
				if (const auto rows = WantedRows(actor, a_s); !rows.empty()) {
					add(actor, rows);
				}
			}
			const auto                     chosen = Anima::ChooseLit(std::move(candidates), Cap());
			std::unordered_set<RE::FormID> keep;
			for (const auto i : chosen) {
				keep.insert(actors[i]->GetFormID());
			}
			std::erase_if(gLit, [&](auto& a_entry) {
				if (keep.contains(a_entry.first)) {
					return false;
				}
				Drop(a_entry.second, scene);
				return true;
			});
			for (const auto i : chosen) {
				const auto body = a_arrivals.find(actors[i]->GetFormID());
				Consider(actors[i], a_s, body != a_arrivals.end() ? body->second.get() : nullptr);
			}
			gLitCount = gLit.size();
			lookAtChanged();
		}

		// a loading screen ended: a light kept under a body through one is in none of the scene's lists and lights nothing, so each lit
		// creature's lights are made again, on the stored body or on the one still on its way
		void HangAgain()
		{
			std::vector<std::pair<RE::NiPointer<RE::Actor>, RE::NiPointer<RE::NiAVObject>>> again;
			again.reserve(gLit.size());
			for (const auto& [id, lit] : gLit) {
				if (auto actor = lit.actor.get()) {
					again.emplace_back(std::move(actor), lit.loading ? lit.root : RE::NiPointer<RE::NiAVObject>{});
				}
			}
			DropAll();
			const auto s = SettingsNow();
			for (const auto& [actor, body] : again) {
				Consider(actor.get(), s, body.get());
			}
			gHungAgain += static_cast<std::uint32_t>(again.size());
			gLitCount = gLit.size();
		}

		// a creature's light falls to its ember, or out: the game plays it and it ends itself
		void StartDeath(Lit& a_lit, const Anima::Settings& a_s)
		{
			a_lit.dying = true;
			for (auto& one : a_lit.lights) {
				const auto& row = Rows()[one.row];
				const float end = one.fade * Anima::DeathShare(Anima::gTuning.deathFadeSeconds, a_s.deathFade, row.ember);
				const float now = one.light ? one.light->GetLightRuntimeData().fade : one.fade;
				if (const auto keys = MakeKeys(Anima::DeathKeys(now, end, a_s.deathFade))) {
					Play(one, keys, a_s.deathFade ? Anima::gTuning.deathFadeSeconds : 0.05f, false);
				} else if (one.light) {
					if (one.dimmer) {
						one.dimmer->Stop();  // its flicker would write over the end
					}
					one.light->GetLightRuntimeData().fade = end;  // no keys to play (an old VR Address Library): straight to its end
					one.waits = false;
				}
				one.loop.reset();
				one.cast.reset();
				one.half.reset();
			}
		}

		// a cast's surge: up fast and back down, then the flicker it had (a light with no flicker just ends on its own fade)
		void StartSurge(Lit& a_lit, const Anima::Settings& a_s)
		{
			// dark until noticed holds it below full: a cast out of a fight (a summon's buff, a spriggan's heal) leaves it as its fight has it
			if (a_lit.dying || !a_s.surge || (a_s.noticed && a_lit.notice < 1.0f)) {
				return;
			}
			a_lit.castAt = Clock::now();
			for (auto& one : a_lit.lights) {
				const auto shape = ShapeOf(Rows()[one.row], a_s);
				const bool loops = shape != Pulse::Kind::kNone;
				const auto seconds = loops ? Anima::kCastSeconds : Anima::gTuning.surgeSeconds;
				if (!one.cast) {
					one.cast = MakeKeys(Anima::FadeKeys(one.fade, shape, a_lit.seed, seconds, true));
				}
				Play(one, one.cast, seconds, loops);
			}
		}

		void HandOverSoon();

		// dark until noticed: its lights ease to the share its fight gives (the game's combat state) and hold there, or flicker on at it
		void ApplyNotice(Lit& a_lit, float a_share, const Anima::Settings& a_s, bool a_snap)
		{
			if (a_lit.dying || a_share == a_lit.notice) {
				return;
			}
			const float known = a_snap ? a_share : a_lit.notice;
			bool        waits = false;
			for (auto& one : a_lit.lights) {
				// a change part way through an ease starts at the fade the light shows, never at the share it was heading for
				const float start = a_snap || !one.light ? one.fade * known : Anima::StartFade(one.light->GetLightRuntimeData().fade, one.fade, known);
				const auto  shape = ShapeOf(Rows()[one.row], a_s);
				const auto  keys = Anima::NoticeKeys(one.fade, shape, a_lit.seed, start, a_share);
				if (const auto played = MakeKeys(keys)) {
					Play(one, played, keys.back().time, false);
					// one dimmer cannot play the ease and then loop (ComputeScaledTime 70824): its flicker is handed over at the ease's end
					one.waits = Anima::HandsOver(shape, a_share);
					waits = waits || one.waits;
				} else if (one.light) {
					// no keys to play (an old VR Address Library): straight to the share
					if (one.dimmer) {
						one.dimmer->Stop();
					}
					one.light->GetLightRuntimeData().fade = one.fade * a_share;
					one.waits = false;
				}
			}
			a_lit.notice = a_share;
			if (waits) {
				gHandOverRan = 0.0f;
				HandOverSoon();
			}
		}

		// each light whose ease has run out goes on to its endless flicker at the share it reached, the loop's first key where the ease ended:
		// nothing in the game says when a controller's keys run out, so a waiting ease is looked at once a frame (polling allowed.json)
		void LookAtEases()
		{
			// a paused game runs no dimmer, so its frames count nothing toward the cap
			if (auto* ui = RE::UI::GetSingleton(); !ui || !ui->GameIsPaused()) {
				gHandOverRan += RE::GetSecondsSinceLastFrame();
			}
			const bool                     last = gHandOverRan >= HandOverSeconds();
			bool                           waiting = false;
			std::optional<Anima::Settings> s;  // read only when a loop has to be made
			for (auto& [id, lit] : gLit) {
				for (auto& one : lit.lights) {
					if (!one.waits) {
						continue;
					}
					if (!one.dimmer || !Anima::KeysRunOut(one.dimmer->lastTime, one.dimmer->weightedLastTime, one.dimmer->hiKeyTime)) {
						one.waits = one.dimmer && !last;
						waiting = waiting || one.waits;
						continue;
					}
					// a fight's share is full or the search share (NoticeShare), each with keys of its own, made once
					auto& loop = lit.notice >= 1.0f ? one.loop : one.half;
					if (!loop) {
						if (!s) {
							s = SettingsNow();
						}
						loop = MakeKeys(Anima::FadeKeys(one.fade * lit.notice, ShapeOf(Rows()[one.row], *s), lit.seed, Anima::kLoopSeconds, false));
					}
					Play(one, loop, Anima::kLoopSeconds, true);
				}
			}
			if (waiting) {
				HandOverSoon();
			} else {
				gHandOverRan = 0.0f;
			}
		}

		// one more look a frame later, by way of the UI's queue (LookAgainSoon's way); armed by an ease that waits, it ends with the last one
		void HandOverSoon()
		{
			if (gHandOverQueued.exchange(true)) {
				return;
			}
			Guarded("notice flicker", []() {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([]() {
						gHandOverQueued = false;
						static_cast<void>(Later("notice flicker", []() { LookAtEases(); }));
					});
				} else {
					gHandOverQueued = false;
				}
			});
		}

		// out of combat: back to the plain flicker, so a cast's keys do not come round again with no cast
		void EndSurge(Lit& a_lit)
		{
			if (a_lit.dying || !a_lit.castAt) {
				return;
			}
			a_lit.castAt.reset();
			for (auto& one : a_lit.lights) {
				Play(one, one.loop, Anima::kLoopSeconds, true);
			}
		}

		// a character's 3D came or went; node: the body a load returned, kept alive until the task hangs lights on it
		struct Loaded
		{
			RE::FormID                    id{ 0 };
			bool                          on{ false };
			RE::NiPointer<RE::NiAVObject> node;
		};

		// what the game's events noted for the main thread: a sink only writes a form id here (it may run on a job thread)
		struct Noted
		{
			std::mutex                              lock;
			std::vector<Loaded>                     loaded;
			std::vector<RE::FormID>                 died;
			std::vector<RE::FormID>                 cast;
			std::vector<RE::FormID>                 calm;
			std::vector<std::pair<RE::FormID, int>> notice;         // a combat state reached (0 none, 1 in combat, 2 searching): dark until noticed
			std::vector<RE::FormID>                 art;            // an art object started on it: another mod's light may hang on that
			std::vector<std::uint32_t>              artOn, artOff;  // art effects whose art a row names: started, ended (their ids)
			std::vector<ImpactOne>                  impactGone;     // an impact's light whose particle ended: taken off the scene here
			// a claimed particle whose update ran off the main thread, held until its light is hung here
			std::vector<std::pair<RE::NiPointer<RE::BSTempEffectParticle>, std::size_t>> impactHang;
			std::vector<std::pair<RE::FormID, RE::FormID>>                               worn;       // id, the ability's effect that carries a light
			std::vector<std::pair<RE::FormID, bool>>                                     furniture;  // id, true = got up (AsleepUndead)
			// a casting light still up when its caster's 3D went: the game leaves it in the scene with nobody there
			std::vector<std::pair<RE::FormID, RE::NiPointer<RE::BSLight>>> castLights;
			bool                                                           pick{ false };  // her cell changed, or a creature finished dying
			std::atomic<bool>                                              any{ false };
		};
		Noted gNoted;

		// art effects' ends are noted only while an art light of ours is up or one waits to be hung (main thread, after every relight)
		void RearmArt()
		{
			std::scoped_lock l{ gNoted.lock };
			gArtArmed = !gArtLit.empty() || !gNoted.artOn.empty();
		}

		// another mod's lights on a creature of ours come with no word of their own: Light Placer hangs them as the body's parts attach, a
		// lit ability's light comes at the effect's first update. So a creature that just came, or wears something new, is looked at for a
		// few frames and then left alone. Only a stored body is walked: a body still loading may be another thread's
		constexpr int kLooksWaiting = 120;  // while its body is not stored yet, about two seconds
		constexpr int kLooksStored = 12;    // once it is

		struct Looking
		{
			int left{ kLooksWaiting };
			int stored{ 0 };
		};
		std::unordered_map<RE::FormID, Looking> gLooking;  // main thread only
		std::atomic<bool>                       gLookQueued{ false };

		void LookAgainSoon();

		void LookTheirs()
		{
			const LightKit::Cost::Scope cost(kCostTheirs, "creature events: their lights");
			const auto                  s = SettingsNow();
			std::erase_if(gLooking, [&](auto& a_entry) {
				auto& look = a_entry.second;
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_entry.first);
				if (!actor) {
					return true;
				}
				auto* body = actor->Is3DLoaded() ? actor->Get3D() : nullptr;
				TheirLights(actor, body, s, gLit.contains(a_entry.first));
				look.stored += body ? 1 : 0;
				return --look.left <= 0 || look.stored >= kLooksStored;
			});
			if (!gLooking.empty()) {
				LookAgainSoon();
			}
		}

		// one more look a frame later, by way of the UI's queue: a main-thread task queued from a main-thread task can run in the same pass
		// over SKSE's queue
		void LookAgainSoon()
		{
			if (gLookQueued.exchange(true)) {
				return;
			}
			Guarded("their lights", []() {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([]() {
						gLookQueued = false;
						static_cast<void>(Later("their lights", []() { LookTheirs(); }));
					});
				} else {
					gLookQueued = false;
				}
			});
		}

		void LookAt(RE::FormID a_id)
		{
			gLooking.insert_or_assign(a_id, Looking{});
		}

		// every creature around her, after a load or a loading screen: those that came with it sent nothing of their own
		void LookAtEveryone()
		{
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (auto actor = handle.get(); actor && CouldLight(actor.get())) {
						LookAt(actor->GetFormID());
					}
				}
			}
			if (!gLooking.empty() && !gLookQueued.load()) {
				LookTheirs();
			}
		}

		// a creature that went in the middle of a cast (a summon running out, Disable, its cell unloading) leaves its casting light in the
		// scene with nobody there, lighting the ground. Out of the scene first, then its caster lets go of it: a BSLight that is
		// still held keeps lighting the geometry it touched (the plugin framework section 7)
		void PutOutCastLight(RE::FormID a_id, const RE::NiPointer<RE::BSLight>& a_bs)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (actor && actor->Is3DLoaded()) {
				// its body came back before this task ran: a light a caster still holds is the game's again, one none holds is left over
				const auto& casters = actor->GetActorRuntimeData().magicCasters;
				if (std::ranges::any_of(casters, [&](auto* a_caster) { return a_caster && a_caster->light.get() == a_bs.get(); })) {
					return;
				}
			}
			LightKit::TakeOff(LightKit::Scene(), a_bs);
			++gCastLightsOut;
			if (!actor) {
				return;
			}
			for (auto* caster : actor->GetActorRuntimeData().magicCasters) {
				if (caster && caster->light.get() == a_bs.get()) {
					caster->light.reset();
				}
			}
		}

		void Drain()
		{
			if (!gNoted.any.exchange(false)) {
				return;
			}
			const LightKit::Cost::Scope                                                  cost(kCostSlot, "creature events");
			const auto                                                                   s = SettingsNow();
			std::vector<Loaded>                                                          loaded;
			std::vector<RE::FormID>                                                      died, cast, calm, art;
			std::vector<std::pair<RE::FormID, bool>>                                     furniture;
			std::vector<std::pair<RE::FormID, int>>                                      notice;
			std::vector<std::pair<RE::FormID, RE::FormID>>                               worn;
			std::vector<std::pair<RE::FormID, RE::NiPointer<RE::BSLight>>>               castLights;
			std::vector<std::uint32_t>                                                   artOn, artOff;
			std::vector<ImpactOne>                                                       impactGone;
			std::vector<std::pair<RE::NiPointer<RE::BSTempEffectParticle>, std::size_t>> impactHang;
			bool                                                                         pick = false;
			{
				std::scoped_lock l{ gNoted.lock };
				loaded.swap(gNoted.loaded);
				died.swap(gNoted.died);
				cast.swap(gNoted.cast);
				calm.swap(gNoted.calm);
				notice.swap(gNoted.notice);
				art.swap(gNoted.art);
				artOn.swap(gNoted.artOn);
				artOff.swap(gNoted.artOff);
				impactGone.swap(gNoted.impactGone);
				impactHang.swap(gNoted.impactHang);
				worn.swap(gNoted.worn);
				furniture.swap(gNoted.furniture);
				castLights.swap(gNoted.castLights);
				pick = std::exchange(gNoted.pick, false);
			}
			// up from its coffin or slab: lit now if a row names it, within the cap (the pick below swaps in the nearest); lying down in one:
			// an undead's lights go (Consider drops a creature whose rows are none)
			for (const auto& [id, up] : furniture) {
				if (up) {
					gWoken.insert(id);
				} else {
					gWoken.erase(id);
				}
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
					Consider(actor, s);
					pick = true;
				}
			}
			for (const auto& [id, bs] : castLights) {
				PutOutCastLight(id, bs);
			}
			for (auto& one : impactGone) {
				TakeImpact(one, LightKit::Scene());
			}
			for (const auto& [particle, row] : impactHang) {
				HangImpact(particle.get(), row);
			}
			Arrivals                             arrivals;
			std::optional<LightKit::Cost::Scope> loadedCost;
			loadedCost.emplace(kCostLoaded, "creature events: loaded notes");
			for (const auto& [id, on, node] : loaded) {
				auto* actor = on ? RE::TESForm::LookupByID<RE::Actor>(id) : nullptr;
				if (actor) {
					if (!RowsOf(actor).empty()) {
						ReadWornLights(actor);
						arrivals.insert_or_assign(id, node);
						LookAt(id);
						pick = true;
					} else if (gLit.contains(id)) {
						// lit, but its new body is one no row names: the re-pick lets its lights go
						pick = true;
					}
					continue;
				}
				arrivals.erase(id);
				gLooking.erase(id);
				gWoken.erase(id);
				if (const auto it = gLit.find(id); it != gLit.end()) {
					Drop(it->second, LightKit::Scene());
					gLit.erase(it);
					pick = true;
				}
			}
			loadedCost.reset();
			for (const auto& [id, effect] : worn) {
				// an actor no row can name has nothing of ours to replace it with
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id); actor && !RowsOf(actor).empty()) {
					NoteWornLight(actor, effect);
					Consider(actor, s);
					LookAt(id);
				}
			}
			for (const auto id : art) {
				LookAt(id);
			}
			// started first: an art that began and ended within one burst is hung and taken off again here
			if (!artOn.empty()) {
				LightArt(artOn, false);
			}
			if (!artOff.empty()) {
				ArtGone(artOff);
			}
			RearmArt();
			for (const auto id : died) {
				if (const auto it = gLit.find(id); it != gLit.end() && !it->second.dying) {
					StartDeath(it->second, s);
				}
			}
			for (const auto id : cast) {
				if (const auto it = gLit.find(id); it != gLit.end()) {
					StartSurge(it->second, s);
				}
			}
			for (const auto id : calm) {
				if (const auto it = gLit.find(id); it != gLit.end()) {
					EndSurge(it->second);
				}
			}
			// after the calm ones' flicker is back: dark until noticed eases each to its fight's share
			if (s.noticed) {
				for (const auto& [id, state] : notice) {
					if (const auto it = gLit.find(id); it != gLit.end()) {
						ApplyNotice(it->second, Anima::NoticeShare(state), s, false);
					}
				}
			}
			if (pick) {
				const LightKit::Cost::Scope pickCost(kCostPick, "creature events: pick");
				Pick(s, arrivals, true);
			}
			gLitCount = gLit.size();
			if (!gLooking.empty() && !gLookQueued.load()) {
				LookTheirs();
			}
		}

		// one note; the first of a burst queues the one task that acts on them all
		template <class F>
		void Note(F&& a_add) noexcept
		{
			Guarded("event note", [&]() {
				{
					std::scoped_lock l{ gNoted.lock };
					a_add();
				}
				if (!gNoted.any.exchange(true) && !Later("creature events", []() { Drain(); })) {
					gNoted.any = false;
				}
			});
		}

		// a character's 3D arrives: every load comes through this slot, on a loader thread for a queued one, and the game's load event names
		// only a reference with a script, so most creatures send none. A queued load's body is stored later and announces nothing, so it
		// travels with the note. Every character in the world passes here: one that cannot be ours costs two reads
		struct Load3D
		{
			static RE::NiAVObject* thunk(RE::Character* a_this, bool a_background)
			{
				auto* node = func(a_this, a_background);
				if (node && CouldLight(a_this)) {
					const auto id = a_this->GetFormID();
					Note([id, node]() { gNoted.loaded.push_back({ id, true, RE::NiPointer<RE::NiAVObject>(node) }); });
				}
				return node;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// whether any of its casters still holds a light: the slots read straight off the actor, no caster made (the plugin framework section 2)
		[[nodiscard]] bool HasCastLight(RE::Character* a_actor)
		{
			return std::ranges::any_of(a_actor->GetActorRuntimeData().magicCasters, [](const auto* a_caster) { return a_caster && a_caster->light; });
		}

		// a creature of ours whose 3D goes while a caster still holds its casting light: each such light noted with it, put out on the main thread
		void NoteCastLights(RE::Character* a_actor)
		{
			std::vector<RE::NiPointer<RE::BSLight>> up;
			for (auto* caster : a_actor->GetActorRuntimeData().magicCasters) {
				if (caster && caster->light) {
					up.push_back(caster->light);
				}
			}
			if (up.empty()) {
				return;
			}
			const auto id = a_actor->GetFormID();
			Note([&]() {
				for (auto& bs : up) {
					gNoted.castLights.emplace_back(id, std::move(bs));
				}
			});
		}

		// a character's 3D let go: Disable, a cell's unload and a replaced body come through this slot, a queued load's store does not
		struct Set3D
		{
			static void thunk(RE::Character* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
			{
				func(a_this, a_object, a_queue3DTasks);
				// a body going matters only with a light in the scene or a load note not yet acted on; a race changed since it was lit still lets its lights go
				const bool on = a_object != nullptr;
				if (on ? CouldLight(a_this) : !NothingLit() || gNoted.any.load(std::memory_order_relaxed)) {
					const auto id = a_this->GetFormID();
					Note([id, on]() { gNoted.loaded.push_back({ id, on, {} }); });
				}
				// a casting light still up as a creature of ours goes is the game's leftover: the caster slots' light pointers are read
				// first (a few reads, the hook's idle path), the creature tables only for a body that leaves one up. Disable's 3D release runs off
				// the main thread, so the light is taken here (the 1.0 game check: gated to the main thread, the matron's glow stayed)
				if (!on && HasCastLight(a_this) && CouldLight(a_this)) {
					Guarded("cast light at a body's end", [a_this]() { NoteCastLights(a_this); });
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// an art object started on a character: Light Placer hung its lights on it in the original call, and a wisp's look is such an art
		struct ArtInit
		{
			static bool thunk(RE::ModelReferenceEffect* a_this)
			{
				const bool started = func(a_this);
				if (started && a_this) {
					Guarded("art start", [a_this]() {
						// an art a row names: its id only, lit on the main thread (it may start on a job thread)
						if (ArtRow(a_this->artObject)) {
							const auto eid = a_this->effectID;
							Note([eid]() {
								gNoted.artOn.push_back(eid);
								gArtArmed = true;
							});
						}
						if (!gWatchTheirArt) {
							return;
						}
						const auto target = a_this->target.get();
						auto*      actor = target ? target->As<RE::Actor>() : nullptr;
						if (actor && CouldLight(actor)) {
							const auto id = actor->GetFormID();
							Note([id]() { gNoted.art.push_back(id); });
						}
					});
				}
				return started;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// the row of a splash's model name, per thread: the game's splashes come from a few models, so the last 16 names are kept with their
		// row. An entry is found by its address and then its text, since a freed name's address can come back holding another
		[[nodiscard]] std::optional<std::size_t> RowByModelCached(const char* a_model)
		{
			struct Seen
			{
				const char*                address{ nullptr };
				std::string                name;
				std::optional<std::size_t> row;
			};
			thread_local std::array<Seen, 16> seen{};
			thread_local std::size_t          next{ 0 };
			const std::string_view            name = a_model ? a_model : "";
			for (const auto& one : seen) {
				if (one.address == a_model && one.name == name) {
					return one.row;
				}
			}
			auto& slot = seen[next];
			next = (next + 1) % seen.size();
			slot = { a_model, std::string(name), ImpactRowByModel(a_model) };
			NoteImpactModel(std::string("model ").append(name));
			return slot.row;
		}

		// devbench's list also names the root node of a splash no row answered (its model's identity where the name says little): noted once
		// per name per thread, by the pooled name's address
		void NoteRootName(const char* a_name)
		{
			thread_local std::array<const char*, 16> noted{};
			thread_local std::size_t                 next{ 0 };
			if (!a_name || std::ranges::find(noted, a_name) != noted.end()) {
				return;
			}
			noted[next] = a_name;
			next = (next + 1) % noted.size();
			NoteImpactModel(std::string("root ").append(a_name));
		}

		// an impact particle's end: its light out of the table and to the main-thread task (a free may come on any thread)
		void EndImpact(const RE::BSTempEffectParticle* a_particle)
		{
			ImpactOne one;
			{
				std::scoped_lock l{ gImpactLock };
				const auto       it = gImpactLit.find(a_particle);
				if (it == gImpactLit.end()) {
					return;
				}
				one = std::move(it->second);
				gImpactLit.erase(it);
				gImpactCount = gImpactLit.size();
			}
			if (one.bs) {
				Note([&]() { gNoted.impactGone.push_back(std::move(one)); });
			}
		}

		// a splash whose copy the game just set up: judged once (a row by its impact record, else by its model name), and the light is hung on
		// the main thread - here when the particle's update ran there, else by the task (measured: its update runs off it)
		void AfterCopy(RE::BSTempEffectParticle* a_this, std::optional<std::size_t> a_named)
		{
			if (!a_this->particleObject) {
				return;
			}
			// devbench tells an empty impactData from a record that is not ours by these two
			gImpactSeen.fetch_add(1, std::memory_order_relaxed);
			gImpactLastRecord.store(a_this->impactData ? a_this->impactData->GetFormID() : 0, std::memory_order_relaxed);
			auto row = ImpactRow(a_this->impactData);
			if (!row) {
				row = a_named;
			}
			if (!row) {
				NoteRootName(a_this->particleObject->name.c_str());
				return;
			}
			if (!ClaimImpact(a_this, *row)) {
				return;
			}
			if (REX::W32::GetCurrentThreadId() == gMainThread) {
				HangImpact(a_this, *row);
			} else {
				Note([a_this, row]() { gNoted.impactHang.emplace_back(RE::NiPointer<RE::BSTempEffectParticle>(a_this), *row); });
			}
		}

		// an impact's splash (BSTempEffectParticle, read in the 1.6.1170 exe): the particle's update checks its copy task first (30074, the
		// update's first call), and once the copy is finished it moves it into the particle and calls 30073 to set it up and attach it - once per
		// splash, its model name still held until that call returns. So the light comes at that call and every splash passes it once, never per
		// frame; its Detach or its destructor take it off
		struct ImpactCopied
		{
			static void thunk(RE::BSTempEffectParticle* a_this)
			{
				std::optional<std::size_t> named;
				if (a_this && a_this->modelName) {
					Guarded("impact name", [&]() { named = RowByModelCached(a_this->modelName); });
				}
				func(a_this);
				if (a_this) {
					Guarded("impact copied", [&]() { AfterCopy(a_this, named); });
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// the code a_from points at, when it is the game's own: an address outside the exe's code (another plugin's hook in that place) gives 0
		[[nodiscard]] std::uintptr_t GameCode(std::uintptr_t a_from, std::size_t a_span)
		{
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			return a_from >= text.address() && a_from + a_span <= text.address() + text.size() ? a_from : 0;
		}

		// in a_span bytes from a_fn: the call whose next instruction lets go of the particle's model name (mov qword ptr [reg + 0x40], 0) -
		// 30074 +0xAA on 1.6.1170; 0 when the code there has another shape
		[[nodiscard]] std::uintptr_t CallBeforeNameClear(std::uintptr_t a_fn, std::size_t a_span)
		{
			const auto* b = reinterpret_cast<const std::uint8_t*>(a_fn);
			for (std::size_t i = 0; i + 13 <= a_span; ++i) {
				const bool rex = b[i + 5] == 0x48 || b[i + 5] == 0x49;
				const bool store = b[i + 6] == 0xC7 && (b[i + 7] & 0xF8) == 0x40 && (b[i + 7] & 7) != 4 && b[i + 8] == 0x40;
				if (b[i] == 0xE8 && rex && store && b[i + 9] == 0 && b[i + 10] == 0 && b[i + 11] == 0 && b[i + 12] == 0) {
					return a_fn + i;
				}
			}
			return 0;
		}

		// the call that sets a splash's copy up. AE by its id; SE (its code not read here) found from the particle's update in the vtable: the
		// update's first call is the copy check (30081 +0x11 on 1.6.1170), and in it the call before the name is let go - taken only where its
		// code has that shape, and a splash there stays unlit otherwise (logged)
		[[nodiscard]] std::uintptr_t FindImpactCopiedSite()
		{
			// AE: the place read in the exe by its Address Library id (30074 +0xAA), taken only when the code there still has that shape - a
			// byte scan could take an operand byte for a call
			if (REL::Module::IsAE()) {
				const REL::Relocation<std::uintptr_t> check{ REL::ID(30074) };
				const auto                            site = CallBeforeNameClear(check.address() + 0xAA, 13);
				return site == check.address() + 0xAA ? site : 0;
			}
			const REL::Relocation<std::uintptr_t> table{ RE::BSTempEffectParticle::VTABLE[0] };
			const auto                            update = GameCode(*reinterpret_cast<const std::uintptr_t*>(table.address() + 0x28 * sizeof(std::uintptr_t)), 0x40);
			const auto*                           b = reinterpret_cast<const std::uint8_t*>(update);
			for (std::size_t i = 0; update && i + 5 <= 0x40; ++i) {
				if (b[i] == 0xE8) {
					std::int32_t rel = 0;
					std::memcpy(&rel, b + i + 1, sizeof(rel));
					const auto check = GameCode(update + i + 5 + static_cast<std::intptr_t>(rel), 0x140);
					return check ? CallBeforeNameClear(check, 0x140) : 0;
				}
			}
			return 0;
		}

		struct ImpactDetach
		{
			static void thunk(RE::BSTempEffectParticle* a_this)
			{
				if (a_this && gImpactCount.load(std::memory_order_relaxed)) {
					Guarded("impact end", [a_this]() { EndImpact(a_this); });
				}
				func(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ImpactFree
		{
			static RE::BSTempEffectParticle* thunk(RE::BSTempEffectParticle* a_this, std::uint32_t a_flags)
			{
				if (a_this && gImpactCount.load(std::memory_order_relaxed)) {
					Guarded("impact freed", [a_this]() { EndImpact(a_this); });
				}
				return func(a_this, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// an art effect's end (the slot Light Placer takes its art lights off at): every art effect passes here, so one read with no art light
		// of ours up or noted; its light is taken off on the main thread
		struct ArtDetach
		{
			static void thunk(RE::ModelReferenceEffect* a_this)
			{
				if (a_this && gArtArmed.load(std::memory_order_relaxed)) {
					const auto eid = a_this->effectID;
					Note([eid]() { gNoted.artOff.push_back(eid); });
				}
				func(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// the game's events: each sink only notes a form id (the plugin framework, section 2). A death, a cast or a fight's end changes
		// nothing with no light in the scene: anyone's in the loaded world comes through here, and those stop at one read
		class Events final :
			public RE::BSTEventSink<RE::TESDeathEvent>,
			public RE::BSTEventSink<RE::TESSpellCastEvent>,
			public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>,
			public RE::BSTEventSink<RE::TESCombatEvent>,
			public RE::BSTEventSink<RE::TESFurnitureEvent>,
			public RE::BSTEventSink<RE::BGSActorCellEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			// the first of the two death events (still dying): the fade starts as the creature falls. The second: the living come before
			// the dead in the cap, so a creature waiting for a light can have its place
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (NothingLit()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event && a_event->actorDying && !a_event->dead) {
					Note([a_event]() { gNoted.died.push_back(a_event->actorDying->GetFormID()); });
				} else if (a_event && a_event->actorDying) {
					Note([]() { gNoted.pick = true; });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!NothingLit() && a_event && a_event->object) {
					Note([a_event]() { gNoted.cast.push_back(a_event->object->GetFormID()); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// an ability that carries a light reached a creature (every other effect is passed over here, by a look in a table that no longer changes)
			RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				if (a_event && a_event->target && IsAbilityLight(a_event->magicEffect)) {
					++gLitAbilitiesNoted;
					Note([a_event]() { gNoted.worn.emplace_back(a_event->target->GetFormID(), a_event->magicEffect); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*) override
			{
				if (!a_event || !a_event->actor) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto state = a_event->newState.get();
				const auto id = a_event->actor->GetFormID();
				// every state is noted for dark until noticed (the task reads whether it is on); a calm one also ends a cast's keys. A creature
				// lit later starts at the game's own combat value (CombatStateOf), so nothing is kept with nothing lit
				if (!NothingLit()) {
					Note([id, state]() {
						gNoted.notice.emplace_back(id, static_cast<int>(state));
						if (state == RE::ACTOR_COMBAT_STATE::kNone) {
							gNoted.calm.push_back(id);
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// a creature lying down in its furniture or leaving it: an undead asleep there is dark (AsleepUndead), judged again at each. Rare,
			// so a creature no row can name costs two reads
			RE::BSEventNotifyControl ProcessEvent(const RE::TESFurnitureEvent* a_event, RE::BSTEventSource<RE::TESFurnitureEvent>*) override
			{
				if (!a_event || !a_event->actor) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (auto* actor = a_event->actor->As<RE::Actor>(); actor && CouldLight(actor)) {
					const auto id = actor->GetFormID();
					const bool up = a_event->type.get() == RE::TESFurnitureEvent::FurnitureEventType::kExit;
					Note([id, up]() { gNoted.furniture.emplace_back(id, up); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// her cell changed: the creatures nearest her may have too
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				if (a_event && a_event->flags.get() == RE::BGSActorCellEvent::CellFlag::kEnter) {
					Note([]() { gNoted.pick = true; });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// Quit to Main Menu: the world our lights hang in is gone (Soul Gem Hub's way). A loading screen's end: HangAgain
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view name(a_event->menuName.c_str());
				if (a_event->opening && name == RE::MainMenu::MENU_NAME) {
					static_cast<void>(Later("main menu", []() { DropAll(); }));
				} else if (!a_event->opening && name == RE::LoadingMenu::MENU_NAME) {
					static_cast<void>(Later("loading screen", []() {
						if (!NothingLit()) {
							HangAgain();
						}
						// a loading screen takes our lights out of the scene's lists, the art ones too: made again on the art still playing
						DropArt();
						LightArt({}, true);
						RearmArt();
						LookAtEveryone();
					}));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	namespace
	{
		// the splash's copy call and the particle's two ends: none on VR, where this plugin asks for no trampoline; the ends only with the call
		void InstallImpactCopied()
		{
			if (REL::Module::IsVR()) {
				SKSE::log::info("impact rows: VR - a splash keeps no light of mine");
				return;
			}
			auto&      trampoline = SKSE::GetTrampoline();
			const auto site = FindImpactCopiedSite();
			if (!site || !LightKit::IsCallAt(site) || trampoline.free_size() < kImpactSiteBytes) {
				SKSE::log::warn(
					"impact rows: the call that sets a splash up was not found (this game's code has another shape, or another plugin "
					"changed it) or the trampoline is full; a splash keeps no light of mine");
				return;
			}
			ImpactCopied::func = trampoline.write_call<5>(site, ImpactCopied::thunk);
			REL::Relocation<std::uintptr_t> particle{ RE::BSTempEffectParticle::VTABLE[0] };
			ImpactDetach::func = particle.write_vfunc(0x27, ImpactDetach::thunk);
			ImpactFree::func = particle.write_vfunc(0x00, ImpactFree::thunk);
			SKSE::log::info("impact rows: a splash is lit at the call that sets its copy up (+0x{:X} in the exe)", site - REL::Module::get().base());
		}
	}

	void InstallLights()
	{
		static Events events;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESDeathEvent>(&events);
			holder->AddEventSink<RE::TESSpellCastEvent>(&events);
			holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&events);
			holder->AddEventSink<RE::TESCombatEvent>(&events);
			holder->AddEventSink<RE::TESFurnitureEvent>(&events);
		}
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->AsBGSActorCellEventSource()->AddEventSink(&events);
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
		}
		REL::Relocation<std::uintptr_t> vtbl{ RE::Character::VTABLE[0] };
		Load3D::func = vtbl.write_vfunc(0x6A, Load3D::thunk);
		Set3D::func = vtbl.write_vfunc(0x6C, Set3D::thunk);
		// after Light Placer's own hook of this slot (its hooks go in at plugin load, ours at data load), so its lights are there; none with no file to answer
		gWatchTheirArt = OthersLightBodies();
		REL::Relocation<std::uintptr_t> art{ RE::ModelReferenceEffect::VTABLE[0] };
		if (gWatchTheirArt || AnyArtRows()) {
			ArtInit::func = art.write_vfunc(0x36, ArtInit::thunk);
		}
		if (AnyArtRows()) {
			ArtDetach::func = art.write_vfunc(0x27, ArtDetach::thunk);
		}
		gMainThread = REX::W32::GetCurrentThreadId();
		if (AnyImpactRows()) {
			InstallImpactCopied();
		}
		SKSE::log::info("lights follow the game's events; their motion is the game's own dimmer controllers (light cap {})", Cap());
		InstallObjectLights();
	}

	void DropLights()
	{
		DropObjectLights();
		DropAll();
		ForgetWornLights();
		{
			std::scoped_lock l{ gNoted.lock };
			gNoted.loaded.clear();
			gNoted.died.clear();
			gNoted.cast.clear();
			gNoted.calm.clear();
			gNoted.notice.clear();
			gNoted.art.clear();
			gNoted.artOn.clear();
			gNoted.artOff.clear();
			gNoted.impactHang.clear();
			for (auto& one : gNoted.impactGone) {
				TakeImpact(one, LightKit::Scene());  // a light handed on is still in the scene: off, never only forgotten
			}
			gNoted.impactGone.clear();
			gNoted.worn.clear();
			gNoted.furniture.clear();
			gNoted.castLights.clear();
			gNoted.pick = false;
		}
		gLooking.clear();
		gWoken.clear();
		LightKit::Cost::Reset();
	}

	void LookAround()
	{
		static_cast<void>(Later("look around", []() {
			// the creatures loaded with the save sent no load event: what they wear is read here, once
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (auto actor = handle.get(); actor && !RowsOf(actor.get()).empty()) {
						ReadWornLights(actor.get());
					}
				}
			}
			Pick(SettingsNow(), {}, true);
			LightArt({}, true);
			RearmArt();
			LookAtEveryone();
		}));
	}

	void ApplyNow(bool a_othersToo)
	{
		const auto s = SettingsNow();
		ApplySpellRecords(s);
		ApplyEnbShaderLights(s);
		RefreshStreams();
		// a body a queued load handed over is in no list until the game stores it: carried, as HangAgain carries it
		Arrivals still;
		for (const auto& [id, lit] : gLit) {
			if (lit.loading && lit.root) {
				still.insert_or_assign(id, lit.root);
			}
		}
		DropCreatures();
		Pick(s, still, false);
		LightArt({}, true);
		RearmArt();
		RedressImpacts(s);
		ApplyObjects();
		// a switch: every creature's other lights follow at once, those Pick does not hold a light for too
		if (auto* lists = a_othersToo ? RE::ProcessLists::GetSingleton() : nullptr) {
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (auto& handle : *handles) {
					if (auto actor = handle.get(); actor && actor->Is3DLoaded() && CouldLight(actor.get())) {
						TheirLights(actor.get(), actor->Get3D(), s, gLit.contains(actor->GetFormID()));
					}
				}
			}
		}
	}

	std::size_t LitCount() { return gLitCount.load(); }

	std::uint32_t LitAbilitiesNoted() { return gLitAbilitiesNoted.load(); }

	std::uint32_t LightsHungAgain() { return gHungAgain.load(); }

	std::uint32_t CastLightsPutOut() { return gCastLightsOut.load(); }

	std::size_t ArtLitCount() { return gArtLit.size(); }

	std::size_t ImpactLitCount() { return gImpactCount.load(); }

	std::pair<std::uint64_t, RE::FormID> ImpactSeen() { return { gImpactSeen.load(std::memory_order_relaxed), gImpactLastRecord.load(std::memory_order_relaxed) }; }

	std::vector<std::string> ImpactModelsSeen()
	{
		std::scoped_lock l{ gImpactModelsLock };
		return gImpactModels;
	}

	void DressRowLight(RE::NiPointLight* a_light, std::size_t a_row, const Anima::Settings& a_s)
	{
		const auto& row = Rows()[a_row];
		Dress(a_light, row, RowStrength(row, a_s), a_s, PickedColor(a_row));
	}

	nlohmann::json LitReport()
	{
		auto        out = nlohmann::json::array();
		const auto& rows = Rows();
		for (const auto& [id, lit] : gLit) {
			auto actor = lit.actor.get();
			auto lights = nlohmann::json::array();
			for (const auto& one : lit.lights) {
				if (!one.light) {
					continue;
				}
				const auto& d = one.light->GetLightRuntimeData();
				const auto  w = one.light->world.translate;
				lights.push_back({ { "row", one.row < rows.size() ? rows[one.row].name : std::string() },
					{ "color", { d.diffuse.red, d.diffuse.green, d.diffuse.blue } }, { "fade", d.fade }, { "radius", d.radius.x },
					{ "at", { w.x, w.y, w.z } }, { "easeWaits", one.waits } });
			}
			const bool casting = lit.castAt && std::chrono::duration<float>(Clock::now() - *lit.castAt).count() < Anima::gTuning.surgeSeconds;
			out.push_back({ { "formID", std::format("{:08X}", id) }, { "name", actor ? actor->GetName() : "" },
				{ "editorID", Po3Tweaks::EditorID(actor ? actor->GetActorBase() : nullptr) }, { "dying", lit.dying }, { "casting", casting }, { "notice", lit.notice }, { "lights", std::move(lights) } });
		}
		return out;
	}
}
