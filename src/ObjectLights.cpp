// Illuminated - Creature Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Lights on placed objects an object row names (object=yes): the torchbugs the critter spawner places and moves, and the jars of bugs.
// They are not characters, so the creature route never meets them. Nothing announces a placed object's 3D, so its own two 3D slots
// say when one arrives or goes (Light Placer's Load3D hook on every reference, powerof3, GPL-3.0, with his permission); each note
// reaches the main thread as ONE task per burst, which hangs or drops the lights. The nearest within a cap of their own are lit,
// judged at those notes and when her cell changes. Where another mod's Light Placer file also lights the model, ours replaces its light
// on the object, and that mod's switch gives the object back (the creature route's rule).

#include "Plugin.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Plugin
{
	namespace
	{
		constexpr const char* kName = "IlluminatedCreatureLightingNGObjectLight";
		// a torchbug swarm is dozens of objects: the nearest few carry a light, fewer where a few lights fill a room's budget
		constexpr std::size_t kCostSlot = 8;

		// written once at data load, before the hooks go in; read on any thread after
		std::unordered_map<const RE::TESForm*, std::size_t> gBases;
		bool                                                gWatchActivators{ false };
		bool                                                gWatchMisc{ false };
		bool                                                gWatchStatics{ false };
		std::atomic<bool>                                   gArmed{ false };

		struct Noted
		{
			RE::FormID                    id{ 0 };
			bool                          on{ false };
			RE::NiPointer<RE::NiAVObject> node;  // a queued load's body travels with its note: the game stores it later and says nothing
		};
		std::mutex               gLock;
		std::vector<Noted>       gNoted;  // under gLock
		std::atomic<bool>        gQueued{ false };
		std::atomic<std::size_t> gLitCount{ 0 };

		// every loaded object of ours, lit or waiting for a place under the cap; main thread only
		struct Object
		{
			RE::NiPointer<RE::NiAVObject>   root;
			std::size_t                     row{ 0 };
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			std::optional<bool>             told;  // whether ours showed when the other mods' lights on this body were last set
		};
		std::unordered_map<RE::FormID, Object> gObjects;

		// with RE::Light loaded the objects have no cap: as many as RE::Light itself allows
		[[nodiscard]] bool ReLightLoaded()
		{
			static const bool loaded = REX::W32::GetModuleHandleA("ReLight.dll") != nullptr;
			return loaded;
		}

		// lit objects at once (the Advanced Settings file), none with RE::Light
		[[nodiscard]] std::size_t Cap()
		{
			if (ReLightLoaded()) {
				return std::numeric_limits<std::size_t>::max();
			}
			return LightLimitFix() ? Anima::gTuning.objectsShaders : Anima::gTuning.objectsPlain;
		}

		// any thread: one of the base objects a row names; the form type first (one read), the table only for a type a row can name
		[[nodiscard]] std::optional<std::size_t> RowOfBase(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return std::nullopt;
			}
			const auto type = a_base->GetFormType();
			const bool watched = (type == RE::FormType::Activator && gWatchActivators) || (type == RE::FormType::Misc && gWatchMisc) ||
			                     (type == RE::FormType::Static && gWatchStatics);
			if (!watched) {
				return std::nullopt;
			}
			const auto it = gBases.find(a_base);
			return it != gBases.end() ? std::optional<std::size_t>{ it->second } : std::nullopt;
		}

		void TakeOff(Object& a_object, RE::ShadowSceneNode* a_scene)
		{
			LightKit::TakeOff(a_scene, a_object.bs);
			if (a_object.light && a_object.light->parent) {
				a_object.light->parent->DetachChild(a_object.light.get());
			}
			a_object.light.reset();
			a_object.bs.reset();
		}

		void Hang(Object& a_object, RE::TESObjectREFR* a_ref, const Anima::Settings& a_s, RE::ShadowSceneNode* a_scene)
		{
			// the stored 3D once the game has it, else the body its queued load handed over (Light Placer's way)
			RE::NiAVObject* root = a_ref->Get3D();
			if (!root) {
				root = a_object.root.get();
			}
			auto* node = root ? root->AsNode() : nullptr;
			auto* light = node ? LightKit::CloneLight() : nullptr;
			if (!light) {
				return;
			}
			const auto& row = Rows()[a_object.row];
			light->name = kName;
			light->local.translate = { row.points.front().x, row.points.front().y, row.points.front().z };
			light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal ours: the dressing attenuates it at once
			DressRowLight(light, a_object.row, a_s);
			RE::NiPointer<RE::BSLight> bs(LightKit::Hang(node, light, a_scene, 1.0f, true));
			if (bs) {
				a_object.light = RE::NiPointer<RE::NiPointLight>(light);
				a_object.bs = bs;
				// hung on the stored 3D: the carried body is let go, so an object whose 3D later goes with no null Set3D is dropped at the next pick
				if (a_ref->Get3D()) {
					a_object.root.reset();
				}
			}
		}

		// main thread: the nearest objects the settings show get a light, up to the cap; the rest let theirs go
		void Pick(const Anima::Settings& a_s)
		{
			auto* scene = LightKit::Scene();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!scene || !player) {
				return;
			}
			const auto                                at = player->GetPosition();
			std::vector<std::pair<float, RE::FormID>> wanted;
			std::erase_if(gObjects, [&](auto& a_pair) {
				auto& [id, object] = a_pair;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!ref || ref->IsDisabled() || ref->IsDeleted() || (!ref->Get3D() && !object.root)) {
					TakeOff(object, scene);
					return true;
				}
				if (RowShows(object.row, a_s) && !ObjectGivenToThem(object.row, a_s)) {
					const auto  p = ref->GetPosition();
					const float dx = p.x - at.x;
					const float dy = p.y - at.y;
					const float dz = p.z - at.z;
					wanted.emplace_back(dx * dx + dy * dy + dz * dz, id);
				}
				return false;
			});
			// only more than the cap needs the nearest first (uncapped with RE::Light, the list is never sorted)
			if (wanted.size() > Cap()) {
				std::ranges::nth_element(wanted, wanted.begin() + static_cast<std::ptrdiff_t>(Cap()));
				wanted.resize(Cap());
			}
			std::unordered_set<RE::FormID> chosen;
			chosen.reserve(wanted.size());
			for (const auto& w : wanted) {
				chosen.insert(w.second);
			}
			for (auto& [id, object] : gObjects) {
				const bool lit = chosen.contains(id);
				auto*      ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!lit && object.light) {
					TakeOff(object, scene);
				} else if (lit && !object.light && ref) {
					Hang(object, ref, a_s, scene);
				}
				// ours replaces another mod's light on it, and gives it back where ours does not show; set again only when that changed
				const bool ours = static_cast<bool>(object.light);
				if (ref && object.told != ours) {
					auto* root = ref->Get3D();
					TheirObjectLights(root ? root : object.root.get(), object.row, a_s, ours);
					object.told = ours;
				}
			}
			gLitCount = static_cast<std::size_t>(std::ranges::count_if(gObjects, [](const auto& a_pair) { return static_cast<bool>(a_pair.second.light); }));
		}

		// a loading screen ended: our lights are in none of the scene's lists, so each is made again
		void HangAgain()
		{
			auto* scene = LightKit::Scene();
			for (auto& [id, object] : gObjects) {
				TakeOff(object, scene);
				object.told.reset();
			}
			Pick(SettingsNow());
		}

		void DropAll()
		{
			auto* scene = LightKit::Scene();
			for (auto& [id, object] : gObjects) {
				TakeOff(object, scene);
			}
			gObjects.clear();
			gLitCount = 0;
		}

		// main thread: every note of a burst acted on, then one pick
		void Drain()
		{
			if (!gQueued.exchange(false)) {
				return;
			}
			const LightKit::Cost::Scope cost(kCostSlot, "object events");
			std::vector<Noted>          noted;
			{
				std::scoped_lock l{ gLock };
				noted.swap(gNoted);
			}
			auto* scene = LightKit::Scene();
			for (auto& [id, on, node] : noted) {
				auto*      ref = on ? RE::TESForm::LookupByID<RE::TESObjectREFR>(id) : nullptr;
				const auto row = ref ? RowOfBase(ref->GetBaseObject()) : std::nullopt;
				if (row) {
					auto& object = gObjects[id];
					if (object.light && node && static_cast<RE::NiAVObject*>(object.light->parent) != node.get()) {
						// a new body: its light is made again on it
						TakeOff(object, scene);
					}
					if (node) {
						// carried only until the game has stored it; a body (new or again) has its other mods' lights set afresh
						object.root = ref->Get3D() == node.get() ? nullptr : node;
						object.told.reset();
					}
					object.row = *row;
					continue;
				}
				if (const auto it = gObjects.find(id); it != gObjects.end()) {
					TakeOff(it->second, scene);
					gObjects.erase(it);
				}
			}
			Pick(SettingsNow());
		}

		// one note; the first of a burst queues the one task that acts on them all
		void Note(RE::FormID a_id, bool a_on, RE::NiAVObject* a_node) noexcept
		{
			Guarded("object note", [&]() {
				{
					std::scoped_lock l{ gLock };
					gNoted.push_back({ a_id, a_on, RE::NiPointer<RE::NiAVObject>(a_node) });
				}
				if (!gQueued.exchange(true) && !Later("object events", []() { Drain(); })) {
					gQueued = false;
				}
			});
		}

		// a placed object's 3D arrives (on a loader thread for a queued load): one relaxed read, then its base's form type, for every object
		struct Load3D
		{
			static RE::NiAVObject* thunk(RE::TESObjectREFR* a_this, bool a_background)
			{
				auto* node = func(a_this, a_background);
				if (node && gArmed.load(std::memory_order_relaxed) && RowOfBase(a_this->GetBaseObject())) {
					Note(a_this->GetFormID(), true, node);
				}
				return node;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// a placed object's 3D set or let go (Disable, a cell's unload, the critter spawner deleting a torchbug)
		struct Set3D
		{
			static void thunk(RE::TESObjectREFR* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
			{
				func(a_this, a_object, a_queue3DTasks);
				if (gArmed.load(std::memory_order_relaxed) && RowOfBase(a_this->GetBaseObject())) {
					Note(a_this->GetFormID(), a_object != nullptr, a_object);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// her cell changed: the objects nearest her may have too. A loading screen takes our lights out of the scene's lists and nothing says
		// so: every light is made again when one ends. Quit to the main menu: the world our lights hang in is gone
		class Events final :
			public RE::BSTEventSink<RE::BGSActorCellEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				if (a_event && a_event->flags.get() == RE::BGSActorCellEvent::CellFlag::kEnter) {
					static_cast<void>(Later("object cell", []() { Pick(SettingsNow()); }));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view name(a_event->menuName.c_str());
				if (a_event->opening && name == RE::MainMenu::MENU_NAME) {
					static_cast<void>(Later("object main menu", []() { DropAll(); }));
				} else if (!a_event->opening && name == RE::LoadingMenu::MENU_NAME) {
					static_cast<void>(Later("object loading screen", []() { HangAgain(); }));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void InstallObjectLights()
	{
		const auto&                                  rows = Rows();
		std::unordered_map<std::string, std::size_t> byModel;
		for (std::size_t i = 0; i < rows.size(); ++i) {
			if (!rows[i].object) {
				continue;
			}
			if (RowModelsLitByOthers(i)) {
				SKSE::log::info("object row {}: another mod's Light Placer file lights its model too - ours replaces it unless its switch gives it back", rows[i].name);
			}
			for (const auto& model : rows[i].models) {
				byModel.try_emplace(model, i);
			}
		}
		if (byModel.empty()) {
			return;
		}
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return;
		}
		const auto take = [&](auto& a_forms, bool& a_watch) {
			for (auto* form : a_forms) {
				const auto* model = form ? form->template As<RE::TESModel>() : nullptr;
				const char* path = model ? model->GetModel() : nullptr;
				if (!path || !*path) {
					continue;
				}
				if (const auto it = byModel.find(Anima::NormalPath(path)); it != byModel.end()) {
					gBases.try_emplace(form, it->second);
					a_watch = true;
				}
			}
		};
		take(handler->GetFormArray<RE::TESObjectACTI>(), gWatchActivators);
		take(handler->GetFormArray<RE::TESObjectMISC>(), gWatchMisc);
		take(handler->GetFormArray<RE::TESObjectSTAT>(), gWatchStatics);
		SKSE::log::info("object lights: {} base object(s) named by {} model(s); {}", gBases.size(), byModel.size(),
			ReLightLoaded() ? "RE::Light is loaded, so every one is lit (no cap)" : "the nearest are lit, up to the Advanced Settings cap");
		if (gBases.empty()) {
			return;
		}
		static Events events;
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->AsBGSActorCellEventSource()->AddEventSink(&events);
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
		}
		// slots below 0x82: the same on VR (the plugin framework section 2); Character's own table keeps the creature route's hooks
		REL::Relocation<std::uintptr_t> vtbl{ RE::TESObjectREFR::VTABLE[0] };
		Load3D::func = vtbl.write_vfunc(0x6A, Load3D::thunk);
		Set3D::func = vtbl.write_vfunc(0x6C, Set3D::thunk);
		gArmed = true;
	}

	void DropObjectLights()
	{
		DropAll();
		std::scoped_lock l{ gLock };
		gNoted.clear();
	}

	void ApplyObjects()
	{
		// a setting changed: a light that stays is dressed again where it hangs; Pick takes off what no longer shows and lights the rest
		const auto s = SettingsNow();
		for (auto& [id, object] : gObjects) {
			if (object.light && RowShows(object.row, s) && !ObjectGivenToThem(object.row, s)) {
				DressRowLight(object.light.get(), object.row, s);
			}
			object.told.reset();  // a switch for another mod may have changed
		}
		Pick(s);
	}

	std::size_t ObjectLitCount() { return gLitCount.load(); }
}
