#pragma once
#include "Model.h"
#include "ModelPart.h"

class DragonModel : public Model
{
public:
	static const int MODEL_ID = 4;

	// M2: the ender-eyes layer only has non-transparent pixels on the head
	// texture. When this is set, render() keeps the exact same head transform
	// but skips the neck, body, wings, legs and tail, which halves the cost of
	// the second (eyes) pass.
	bool eyesOnly;

private:
	ModelPart *head;
	ModelPart *neck;
	ModelPart *jaw;
	ModelPart *body;
	ModelPart *rearLeg;
	ModelPart *frontLeg;
	ModelPart *rearLegTip;
	ModelPart *frontLegTip;
	ModelPart *rearFoot;
	ModelPart *frontFoot;
	ModelPart *wing;
	ModelPart *wingTip;
	float a;

public:

    ModelPart *cubes[5];
    DragonModel(float g);
	void prepareMobModel(shared_ptr<Mob> mob, float time, float r, float a);
    virtual void render(shared_ptr<Entity> entity, float time, float r, float bob, float yRot, float xRot, float scale, bool usecompiled);

private:
	float rotWrap(double d);

};