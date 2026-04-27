/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalModelData.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Rendering/Common/ModelDrawerData.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Models/ModelsMemStorage.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/ContainerUtil.h"
#include "System/EventHandler.h"
#include "System/Misc/TracyDefs.h"
#include "System/Threading/ThreadPool.h"

#include <algorithm>
#include <memory>

namespace {

bool metalModelDrawerMT = true;

template<typename T>
bool ContainsObject(const std::vector<T*>& objects, const T* object)
{
	return std::find(objects.begin(), objects.end(), object) != objects.end();
}

class MetalUnitDrawerData : public CUnitDrawerDataBase
{
public:
	MetalUnitDrawerData()
		: CUnitDrawerDataBase("[MetalUnitDrawerData]", 271828, metalModelDrawerMT)
	{
		eventHandler.AddClient(this);
	}

	bool WantsEvent(const std::string& eventName) override
	{
		return
			eventName == "RenderUnitPreCreated" ||
			eventName == "RenderUnitCreated" ||
			eventName == "RenderUnitDestroyed";
	}

	void RenderUnitPreCreated(const CUnit* unit) override { AddOrRefresh(unit); }
	void RenderUnitCreated(const CUnit* unit, int) override { AddOrRefresh(unit); }
	void RenderUnitDestroyed(const CUnit* unit) override
	{
		DelObject(unit, true);
	}

	void Update() override
	{
		for (CUnit* unit : unitHandler.GetActiveUnits()) {
			AddOrRefresh(unit);
		}

		auto updateBody = [this](CUnit* unit) {
			UpdateDrawPos(unit);
			UpdateCommon(unit);
		};

		if (mtModelDrawer) {
			for_mt_chunk(0, unsortedObjects.size(), [this, &updateBody](const int k) {
				updateBody(unsortedObjects[k]);
			}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
		} else {
			for (CUnit* unit : unsortedObjects) {
				updateBody(unit);
			}
		}
	}

	bool IsAlpha(const CUnit* unit) const override { return unit->IsCloaked(); }

protected:
	void UpdateObjectDrawFlags(CSolidObject* object) const override
	{
		CUnit* unit = static_cast<CUnit*>(object);
		unit->ResetDrawFlag();

		if (unit->noDraw || unit->IsInVoid())
			return;
		if (!(unit->losStatus[gu->myAllyTeam] & LOS_INLOS) && !gu->spectatingFullView)
			return;

		const CCamera* cam = CCameraHandler::GetActiveCamera();
		if (cam != nullptr && !cam->InView(unit->drawMidPos, unit->GetDrawRadius()))
			return;

		unit->SetDrawFlag(IsAlpha(unit) ? DrawFlags::SO_ALPHAF_FLAG : DrawFlags::SO_OPAQUE_FLAG);
	}

private:
	void AddOrRefresh(const CUnit* unit)
	{
		if (unit == nullptr || unit->model == nullptr)
			return;

		if (ContainsObject(unsortedObjects, unit))
			return;

		UpdateObject(unit, true);
	}

	static void UpdateDrawPos(CUnit* unit)
	{
		if (const CUnit* transporter = unit->GetTransporter(); transporter != nullptr) {
			unit->drawPos = unit->GetDrawPosOther(transporter->preFrameTra.t, transporter->pos, globalRendering->timeOffset);
		} else {
			unit->drawPos = unit->GetDrawPos(globalRendering->timeOffset);
		}

		unit->drawMidPos = unit->GetMdlDrawMidPos();
	}
};

class MetalFeatureDrawerData : public CFeatureDrawerDataBase
{
public:
	MetalFeatureDrawerData()
		: CFeatureDrawerDataBase("[MetalFeatureDrawerData]", 313373, metalModelDrawerMT)
	{
		eventHandler.AddClient(this);
	}

	bool WantsEvent(const std::string& eventName) override
	{
		return
			eventName == "RenderFeaturePreCreated" ||
			eventName == "RenderFeatureCreated" ||
			eventName == "RenderFeatureDestroyed";
	}

	void RenderFeaturePreCreated(const CFeature* feature) override { AddOrRefresh(feature); }
	void RenderFeatureCreated(const CFeature* feature) override { AddOrRefresh(feature); }
	void RenderFeatureDestroyed(const CFeature* feature) override
	{
		DelObject(feature, true);
	}

	void Update() override
	{
		for (int id : featureHandler.GetActiveFeatureIDs()) {
			AddOrRefresh(featureHandler.GetFeature(id));
		}

		auto updateBody = [this](CFeature* feature) {
			UpdateDrawPos(feature);
			UpdateCommon(feature);
		};

		if (mtModelDrawer) {
			for_mt_chunk(0, unsortedObjects.size(), [this, &updateBody](const int k) {
				updateBody(unsortedObjects[k]);
			}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
		} else {
			for (CFeature* feature : unsortedObjects) {
				updateBody(feature);
			}
		}
	}

	bool IsAlpha(const CFeature* feature) const override
	{
		return feature->drawAlpha < 1.0f;
	}

protected:
	void UpdateObjectDrawFlags(CSolidObject* object) const override
	{
		CFeature* feature = static_cast<CFeature*>(object);
		feature->ResetDrawFlag();

		if (feature->noDraw || feature->IsInVoid())
			return;
		if (!feature->IsInLosForAllyTeam(gu->myAllyTeam) && !gu->spectatingFullView)
			return;

		const CCamera* cam = CCameraHandler::GetActiveCamera();
		if (cam != nullptr && !cam->InView(feature->drawMidPos, feature->GetDrawRadius()))
			return;

		feature->SetDrawFlag(IsAlpha(feature) ? DrawFlags::SO_ALPHAF_FLAG : DrawFlags::SO_OPAQUE_FLAG);
		if (feature->alwaysUpdateMat || (feature->drawFlag > DrawFlags::SO_NODRAW_FLAG && feature->drawFlag < DrawFlags::SO_DRICON_FLAG)) {
			feature->UpdateTransform(feature->drawPos, false);
		}
	}

private:
	void AddOrRefresh(const CFeature* feature)
	{
		if (feature == nullptr || feature->model == nullptr)
			return;

		if (ContainsObject(unsortedObjects, feature))
			return;

		UpdateObject(feature, true);
	}

	static void UpdateDrawPos(CFeature* feature)
	{
		feature->drawPos = feature->GetDrawPos(globalRendering->timeOffset);
		feature->drawMidPos = feature->GetMdlDrawMidPos();
	}
};

std::unique_ptr<MetalUnitDrawerData> unitData;
std::unique_ptr<MetalFeatureDrawerData> featureData;

} // namespace

namespace MetalModelData
{
void Init()
{
	if (unitData == nullptr)
		unitData = std::make_unique<MetalUnitDrawerData>();
	if (featureData == nullptr)
		featureData = std::make_unique<MetalFeatureDrawerData>();
}

void Kill()
{
	featureData.reset();
	unitData.reset();
}

void Update()
{
	Init();
	unitData->Update();
	featureData->Update();
}

const ScopedTransformMemAlloc& GetTransformMemAlloc(const CUnit* unit)
{
	return (unitData != nullptr) ? unitData->GetObjectTransformMemAlloc(unit) : ScopedTransformMemAlloc::Dummy();
}

const ScopedTransformMemAlloc& GetTransformMemAlloc(const CFeature* feature)
{
	return (featureData != nullptr) ? featureData->GetObjectTransformMemAlloc(feature) : ScopedTransformMemAlloc::Dummy();
}
}

#endif // RENDER_BACKEND_METAL
