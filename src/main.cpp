#include "MathUtils.h"
#include "Utilities.h"
#include <Windows.h>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace RE;
using std::unordered_map;
using std::vector;

bool IsInADS(Actor* a)
{
	return (a->gunState == (GUN_STATE)0x8 || a->gunState == (GUN_STATE)0x6);
}

PlayerCharacter* p;
PlayerCamera* pcam;
ConsoleLog* clog;
NiPoint3 lastCalculated;
TESIdleForm* test;
uint32_t sleepSec;
vector<std::string> helperNodeList = { "_SightHelper", "ReticleNode" };
const F4SE::TaskInterface* taskInterface;
const static float offsetX = 0.f;
const static float offsetZ = 0.f;

namespace TullFramework
{
	BGSKeyword* supportKeyword;
	BGSKeyword* sideAimAnimKeyword;
	bool isInstalled = false;
	bool zeroingRequired = false;
	bool isSideAiming = false;
	bool isWeaponSupported = false;
	TESObjectWEAP* lastWeapon = nullptr;
	REL::Relocation<uintptr_t> ptr_PCUpdateMainThread{ REL::ID(633524), 0x22D };
	uintptr_t PCUpdateMainThreadOrig;
	static size_t sampleCount = 10;
	static std::deque<NiPoint3> zoomDataQueue;
	static NiPoint3 lastZoomData;
	static bool delayAdjustment = false;

	struct TullSwitchAimEvent
	{
		int8_t mode;
	};

	void AutoCalculateZoomData()
	{
		if (!p->currentProcess || !p->currentProcess->middleHigh)
			return;

		if (*(float*)((uintptr_t)pcam->cameraStates[CameraState::kFirstPerson].get() + 0x78) <= 0.95f)
			return;

		NiNode* node = (NiNode*)p->Get3D(true);
		if (node) {
			NiNode* helper = nullptr;
			if (!isSideAiming)
				helper = (NiNode*)node->GetObjectByName("_SightHelperMain");
			else
				helper = (NiNode*)node->GetObjectByName("_SightHelperSide");

			NiNode* camera = (NiNode*)node->GetObjectByName("Camera");
			bhkCharacterController* con = p->currentProcess->middleHigh->charController.get();
			if (helper && camera && con) {
				float actorScale = GetActorScale(p);
				NiPoint3 diff = (helper->world.translate - camera->world.translate) / actorScale;
				diff = camera->world.rotate * diff;
				float x = diff.x + offsetX;
				float z = diff.y + offsetZ;
				BSTArray<EquippedItem> equipped = p->currentProcess->middleHigh->equippedItems;
				if (equipped.size() != 0 && equipped[0].item.instanceData) {
					TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped[0].item.instanceData.get();
					if (instance && instance->zoomData) {
						lastZoomData = NiPoint3(x, 0, z);
						zoomDataQueue.push_back(lastZoomData);
						if (zoomDataQueue.size() > sampleCount) {
							zoomDataQueue.pop_front();
						}

						if (zoomDataQueue.size() == sampleCount) {
							float avgX = 0.f;
							float avgZ = 0.f;
							for (auto it = zoomDataQueue.begin(); it != zoomDataQueue.end(); ++it) {
								avgX += it->x;
								avgZ += it->z;
							}
							avgX /= zoomDataQueue.size();
							avgZ /= zoomDataQueue.size();

							instance->zoomData->zoomData.cameraOffset.x += min(max((avgX - instance->zoomData->zoomData.cameraOffset.x) / 6.f, -0.1f), 0.1f);
							instance->zoomData->zoomData.cameraOffset.z += min(max((avgZ - instance->zoomData->zoomData.cameraOffset.z) / 6.f, -0.1f), 0.1f);
						}
					}
				}
			}
		}
	}

	void CheckWeaponKeywords()
	{
		if (p->currentProcess && p->currentProcess->middleHigh) {
			BSTArray<EquippedItem> equipped = p->currentProcess->middleHigh->equippedItems;
			if (equipped.size() != 0 && equipped[0].item.instanceData) {
				lastWeapon = (TESObjectWEAP*)equipped[0].item.object;
				TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped[0].item.instanceData.get();
				if (instance && instance->keywords && instance->keywords->HasKeyword(supportKeyword, instance)) {
					isWeaponSupported = true;
					zeroingRequired = true;
					_MESSAGE("(TullFramework) Weapon supported");
					if (instance->keywords->HasKeyword(sideAimAnimKeyword, instance)) {
						isSideAiming = true;
						_MESSAGE("(TullFramework) Is Side Aiming");
					}
				}
			}
		}
	}

	void DelayAdjustmentFor(float ms)
	{
		delayAdjustment = false;
		std::thread([ms]() {
			delayAdjustment = true;
			int timeout = 0;
			while (delayAdjustment && timeout < ms) {
				++timeout;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			delayAdjustment = false;
		}).detach();
	}

	void ResetVariables()
	{
		isWeaponSupported = false;
		zeroingRequired = false;
		isSideAiming = false;
		delayAdjustment = false;
		lastWeapon = nullptr;
		zoomDataQueue.clear();
	}

	void HookedUpdate()
	{
		if (isWeaponSupported && !delayAdjustment && pcam->currentState == pcam->cameraStates[CameraStates::kFirstPerson] && IsInADS(p) && *(float*)((uintptr_t)pcam->currentState.get() + 0x78) == 1.f) {
			AutoCalculateZoomData();
		} else {
			if (zoomDataQueue.size() > 0) {
				zoomDataQueue.clear();
			}
		}

		typedef void (*FnUpdate)();
		FnUpdate fn = (FnUpdate)PCUpdateMainThreadOrig;
		if (fn)
			(*fn)();
	}

	class EquipWatcher : public BSTEventSink<TESEquipEvent>
	{
	public:
		virtual BSEventNotifyControl ProcessEvent(const TESEquipEvent& evn, BSTEventSource<TESEquipEvent>* a_source)
		{
			if (evn.a == p) {
				TESForm* item = TESForm::GetFormByID(evn.formId);
				if (item) {
					if (evn.flag != 0x00000000ff000000) {
						if (!lastWeapon && item->formType == ENUM_FORM_ID::kWEAP) {
							CheckWeaponKeywords();
						}
					} else {
						if (lastWeapon && evn.formId == lastWeapon->formID) {
							ResetVariables();
						}
					}
				}
			}
			return BSEventNotifyControl::kContinue;
		}
		F4_HEAP_REDEFINE_NEW(EquipWatcher);
	};

	class SwitchAimWatcher : public BSTEventSink<TullSwitchAimEvent>
	{
	public:
		virtual BSEventNotifyControl ProcessEvent(const TullSwitchAimEvent& evn, BSTEventSource<TullSwitchAimEvent>* a_source)
		{
			if (evn.mode == 0) {
				if (isSideAiming) {
					zeroingRequired = true;
					isSideAiming = false;
					DelayAdjustmentFor(150);
					zoomDataQueue.clear();
					_MESSAGE("(TullFramework) Switched to Normal Aim");
				}
			} else if (evn.mode == 1) {
				if (!isSideAiming) {
					zeroingRequired = true;
					isSideAiming = true;
					DelayAdjustmentFor(150);
					zoomDataQueue.clear();
					_MESSAGE("(TullFramework) Switched to Side Aim");
				}
			}
			return BSEventNotifyControl::kContinue;
		}
		F4_HEAP_REDEFINE_NEW(SwitchAimWatcher);
	};

	void CheckTullFrameworkSupport()
	{
		supportKeyword = (BGSKeyword*)GetFormFromMod("Tull_Framework.esp", 0x80F);
		sideAimAnimKeyword = (BGSKeyword*)GetFormFromMod("Tull_Framework.esp", 0x804);
		HMODULE hTULL = GetModuleHandleA("TullFramework.dll");
		typedef BSTEventSource<TullSwitchAimEvent>* (*GetSwitchAimSource)();
		GetSwitchAimSource fnGetSwitchAimSource = (GetSwitchAimSource)GetProcAddress(hTULL, "GetSwitchAimSource");
		if (supportKeyword && sideAimAnimKeyword && hTULL && fnGetSwitchAimSource()) {
			isInstalled = true;
			EquipWatcher* ew = new EquipWatcher();
			EquipEventSource::GetSingleton()->RegisterSink(ew);
			F4SE::Trampoline& trampoline = F4SE::GetTrampoline();
			PCUpdateMainThreadOrig = trampoline.write_call<5>(ptr_PCUpdateMainThread.address(), &HookedUpdate);
			SwitchAimWatcher* saw = new SwitchAimWatcher();
			fnGetSwitchAimSource()->RegisterSink(saw);
			_MESSAGE("(TullFramework) TullFramework found.");
		}
	}
}

void PrintConsole(const char* c)
{
	clog->AddString("(Sight Helper) ");
	clog->AddString(c);
	clog->AddString("\n");
}

void GetEquippedWeaponMods(TESObjectWEAP* currwep)
{
	if (!p->inventoryList) {
		return;
	}
	for (auto invitem = p->inventoryList->data.begin(); invitem != p->inventoryList->data.end(); ++invitem) {
		if (invitem->object->formType == ENUM_FORM_ID::kWEAP) {
			TESObjectWEAP* wep = (TESObjectWEAP*)(invitem->object);
			if (invitem->stackData->IsEquipped() && wep == currwep) {
				if (invitem->stackData->extra) {
					BGSObjectInstanceExtra* extraData = (BGSObjectInstanceExtra*)invitem->stackData->extra->GetByType(EXTRA_DATA_TYPE::kObjectInstance);
					if (extraData) {
						auto data = extraData->values;
						if (data && data->buffer) {
							uintptr_t buf = (uintptr_t)(data->buffer);
							for (uint32_t i = 0; i < data->size / 0x8; i++) {
								BGSMod::Attachment::Mod* omod = (BGSMod::Attachment::Mod*)TESForm::GetFormByID(*(uint32_t*)(buf + i * 0x8));
								_MESSAGE("Mod : %s (0x%04x)", omod->fullName.c_str(), omod->formID);
								//_MESSAGE("Model : %s", omod->model.c_str());
							}
						}
					}
				}
			}
		}
	}
}

void PrintCalculatedZoomData(float x, float z)
{
	NiPoint3 zoomData = NiPoint3();
	if (p->currentProcess && p->currentProcess->middleHigh) {
		BSTArray<EquippedItem> equipped = p->currentProcess->middleHigh->equippedItems;
		if (equipped.size() != 0 && equipped[0].item.instanceData) {
			TESObjectWEAP* weap = (TESObjectWEAP*)equipped[0].item.object;
			TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped[0].item.instanceData.get();
			if (instance->type == 9) {
				_MESSAGE("Weapon : %s", weap->fullName.c_str());
				GetEquippedWeaponMods(weap);
				if (instance->zoomData) {
					zoomData = instance->zoomData->zoomData.cameraOffset;
				}
			}
		}
	}
	char consolebuf[1024] = { 0 };
	snprintf(consolebuf, sizeof(consolebuf), "Calculated Difference x %f z %f", x, z);
	_MESSAGE(consolebuf);
	PrintConsole(consolebuf);
	snprintf(consolebuf, sizeof(consolebuf), "Current Zoom Data x %f z %f", zoomData.x, zoomData.z);
	_MESSAGE(consolebuf);
	PrintConsole(consolebuf);
}

void CalculateZoomData()
{
	if (TullFramework::isInstalled && TullFramework::isWeaponSupported) {
		PrintCalculatedZoomData(TullFramework::lastZoomData.x, TullFramework::lastZoomData.z);
	} else {
		NiNode* node = (NiNode*)p->Get3D(true);
		if (node) {
			NiNode* helper = nullptr;
			int i = 0;
			while (helper == nullptr && i < helperNodeList.size()) {
				helper = (NiNode*)node->GetObjectByName(helperNodeList.at(i));
				++i;
			}
			NiNode* camera = (NiNode*)node->GetObjectByName("Camera");
			bhkCharacterController* con = nullptr;
			if (p->currentProcess) {
				con = p->currentProcess->middleHigh->charController.get();
			}
			if (helper && camera && con) {
				NiPoint3 pos, dir;
				p->GetEyeVector(pos, dir, true);
				float actorScale = GetActorScale(p);
				NiPoint3 diff = (helper->world.translate - camera->world.translate) / actorScale;
				diff = camera->world.rotate * diff;
				float x = diff.x + offsetX;
				float z = diff.y + offsetZ;
				lastCalculated = NiPoint3(x, 0, z);
				PrintCalculatedZoomData(x, z);
			} else {
				PrintConsole("Helper node not found.");
			}
		}
	}
}

void ApplyZoomData()
{
	bool zoomDataFound = false;
	if (p->currentProcess && p->currentProcess->middleHigh) {
		BSTArray<EquippedItem> equipped = p->currentProcess->middleHigh->equippedItems;
		if (equipped.size() != 0 && equipped[0].item.instanceData) {
			TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped[0].item.instanceData.get();
			if (instance->type == 9 && instance->zoomData) {
				zoomDataFound = true;
				instance->zoomData->zoomData.cameraOffset = lastCalculated;
				PrintConsole("Applied new zoom data.");
			}
		}
	}
	if (!zoomDataFound) {
		PrintConsole("Current weapon has no zoom data.");
	}
}

void PrintWeaponAddress()
{
	if (p->currentProcess && p->currentProcess->middleHigh) {
		BSTArray<EquippedItem> equipped = p->currentProcess->middleHigh->equippedItems;
		if (equipped.size() != 0 && equipped[0].item.instanceData) {
			TESObjectWEAP* weap = (TESObjectWEAP*)equipped[0].item.object;
			TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped[0].item.instanceData.get();
			_MESSAGE("base %llx instance %llx", weap, instance);
			if (instance->type == 9) {
				_MESSAGE("Weapon : %s", weap->fullName.c_str());
				GetEquippedWeaponMods(weap);
			}
		}
	}
}

class SightHelperInputHandler : public BSInputEventReceiver
{
public:
	typedef void (SightHelperInputHandler::*FnPerformInputProcessing)(const InputEvent* a_queueHead);

	void HandleMultipleButtonEvent(const ButtonEvent* evn)
	{
		if (evn->eventType != INPUT_EVENT_TYPE::kButton) {
			if (evn->next)
				HandleMultipleButtonEvent((ButtonEvent*)evn->next);
			return;
		}
		uint32_t id = evn->idCode;
		if (evn->device == INPUT_DEVICE::kMouse)
			id += 0x100;
		else if (evn->device == INPUT_DEVICE::kGamepad)
			id += 0x10000;

		if (pcam->currentState == pcam->cameraStates[CameraStates::kFirstPerson] && evn->heldDownSecs == 0 && evn->value == 1) {
			if (id == 0x6D) {
				if (IsInADS(p)) {
					CalculateZoomData();
				} else {
					PrintConsole("You must ADS first.");
				}
			} else if (id == 0x6B) {
				ApplyZoomData();
			} else if (id == 0x69) {
				PrintWeaponAddress();
			}
			/*else if (id == 0x68) {
				SwitchFireMode();
			}
			else if (id == 0x67) {
				SwitchSideAim();
			}*/
		}

		if (evn->next)
			HandleMultipleButtonEvent((ButtonEvent*)evn->next);
	}

	void HookedPerformInputProcessing(const InputEvent* a_queueHead)
	{
		if (a_queueHead) {
			HandleMultipleButtonEvent((ButtonEvent*)a_queueHead);
		}
		FnPerformInputProcessing fn = fnHash.at(*(uint64_t*)this);
		if (fn) {
			(this->*fn)(a_queueHead);
		}
	}

	void HookSink()
	{
		uint64_t vtable = *(uint64_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnPerformInputProcessing fn = SafeWrite64Function(vtable, &SightHelperInputHandler::HookedPerformInputProcessing);
			fnHash.insert(std::pair<uint64_t, FnPerformInputProcessing>(vtable, fn));
		}
	}

protected:
	static unordered_map<uint64_t, FnPerformInputProcessing> fnHash;
};
unordered_map<uint64_t, SightHelperInputHandler::FnPerformInputProcessing> SightHelperInputHandler::fnHash;

void InitializePlugin()
{
	p = PlayerCharacter::GetSingleton();
	pcam = PlayerCamera::GetSingleton();
	((SightHelperInputHandler*)((uint64_t)pcam + 0x38))->HookSink();
	clog = ConsoleLog::GetSingleton();
	test = (TESIdleForm*)TESForm::GetFormByID(0x181E30);
	_MESSAGE("PlayerCharacter %llx", p);
	_MESSAGE("PlayerCamera %llx", pcam);
	TullFramework::CheckTullFrameworkSupport();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::warn);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical("loaded in editor"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	F4SE::AllocTrampoline(8 * 8);

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		} else if (TullFramework::isInstalled) {
			if (msg->type == F4SE::MessagingInterface::kPreLoadGame) {
				TullFramework::ResetVariables();
			} else if (msg->type == F4SE::MessagingInterface::kPostLoadGame) {
				TullFramework::CheckWeaponKeywords();
			} else if (msg->type == F4SE::MessagingInterface::kNewGame) {
				TullFramework::ResetVariables();
			}
		}
	});
	taskInterface = F4SE::GetTaskInterface();
	return true;
}
