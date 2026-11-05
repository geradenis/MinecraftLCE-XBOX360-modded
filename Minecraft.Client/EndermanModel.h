#pragma once

#include "HumanoidModel.h"

class EndermanModel : public HumanoidModel
{
public:
	bool carrying;
	bool creepy;
	// M2: the ender-eyes texture is only opaque on the head. When this is set,
	// render() draws only the head and its outer layer, which removes the
	// redundant full-body second pass.
	bool eyesOnly;

	EndermanModel();
	virtual void setupAnim(float time, float r, float bob, float yRot, float xRot, float scale, unsigned int uiBitmaskOverrideAnim=0);
	virtual void render(shared_ptr<Entity> entity, float time, float r, float bob, float yRot, float xRot, float scale, bool usecompiled);
};