#include "stdafx.h"
#include "EndermanRenderer.h"
#include "EndermanModel.h"
#include "..\Minecraft.World\net.minecraft.world.entity.monster.h"
#include "..\Minecraft.World\net.minecraft.world.level.tile.h"

EndermanRenderer::EndermanRenderer() : MobRenderer(new EndermanModel(), 0.5f)
{
	model = (EndermanModel *) MobRenderer::model;
	m_allowEyesOnly = true;
	this->setArmor(model);
}

void EndermanRenderer::render(shared_ptr<Entity> _mob, double x, double y, double z, float rot, float a)
{
	// 4J - original version used generics and thus had an input parameter of type Boat rather than shared_ptr<Entity>  we have here - 
	// do some casting around instead
	shared_ptr<EnderMan> mob = dynamic_pointer_cast<EnderMan>(_mob);

	model->carrying = mob->getCarryingTile() > 0;
	model->creepy = mob->isCreepy();

#ifdef _WINDOWS64
	static int endermanDebug = -1;
	if (endermanDebug < 0)
	{
		const char *value = getenv("MC_ENDERMAN_DEBUG");
		endermanDebug = (value != NULL && value[0] == '1') ? 1 : 0;
	}
	if (endermanDebug)
	{
		static int endermanDebugCount = 0;
		if ((endermanDebugCount++ % 60) == 0)
		{
			app.DebugPrintf("ENDERMAN_RENDER: mob=%.1f,%.1f,%.1f eyesOnly=%d invisible=%d\n",
				mob->x, mob->y, mob->z, (int) model->eyesOnly, (int) mob->isInvisible());
		}
	}
#endif

	if (mob->isCreepy())
	{
		double d = 0.02;
		x += random.nextGaussian() * d;
		z += random.nextGaussian() * d;
	}

	// The eyes layer is drawn by MobRenderer using the shared armor model. It
	// is switched to head-only in prepareArmor() and must always be reset,
	// otherwise the next main model pass would lose the body, arms and legs.
	model->eyesOnly = false;
	MobRenderer::render(mob, x, y, z, rot, a);
	model->eyesOnly = false;
}

void EndermanRenderer::additionalRendering(shared_ptr<Mob> _mob, float a)
{
	// 4J - original version used generics and thus had an input parameter of type Boat rather than shared_ptr<Entity>  we have here - 
	// do some casting around instead
	shared_ptr<EnderMan> mob = dynamic_pointer_cast<EnderMan>(_mob);

	// additionalRendering() runs after the eyes pass but before the hurt/death
	// overlay, which redraws the whole model. Clear the head-only mode here so
	// that overlay is not clipped to the head.
	model->eyesOnly = false;

	MobRenderer::additionalRendering(_mob, a);

	if (mob->getCarryingTile() > 0)
	{
		glEnable(GL_RESCALE_NORMAL);
		glPushMatrix();

		float s = 8 / 16.0f;
		glTranslatef(-0 / 16.0f, 11 / 16.0f, -12 / 16.0f);
		s *= 1.00f;
		glRotatef(20, 1, 0, 0);
		glRotatef(45, 0, 1, 0);
		glScalef(s, -s, s);


		if (SharedConstants::TEXTURE_LIGHTING)
		{
			int col = mob->getLightColor(a);
			int u = col % 65536;
			int v = col / 65536;

			glMultiTexCoord2f(GL_TEXTURE1, u / 1.0f, v / 1.0f);
			glColor4f(1, 1, 1, 1);
		}

		glColor4f(1, 1, 1, 1);
		bindTexture(TN_TERRAIN);	// 4J was L"/terrain.png"
		tileRenderer->renderTile(Tile::tiles[mob->getCarryingTile()], mob->getCarryingData(), 1);
		glPopMatrix();
		glDisable(GL_RESCALE_NORMAL);
	}
}

int EndermanRenderer::prepareArmor(shared_ptr<Mob> _mob, int layer, float a)
{
	// 4J - original version used generics and thus had an input parameter of type Boat rather than shared_ptr<Entity>  we have here - 
	// do some casting around instead
	shared_ptr<EnderMan> mob = dynamic_pointer_cast<EnderMan>(_mob);

	if (layer != 0) return -1;

	bindTexture(TN_MOB_ENDERMAN_EYES);	// 4J was L"/mob/enderman_eyes.png"
	if (m_allowEyesOnly)
	{
		// The eye texture is only opaque on the head, so the second pass only
		// needs to draw the head and its outer layer at the same transform.
		model->eyesOnly = true;
	}
	float br = 1;
	glEnable(GL_BLEND);
	// 4J Stu - We probably don't need to do this on 360 either (as we force it back on the renderer)
	// However we do want it off for other platforms that don't force it on in the render lib CBuff handling
	// Several texture packs have fully transparent bits that break if this is off
#ifdef _XBOX
    glDisable(GL_ALPHA_TEST);
#endif
	glBlendFunc(GL_ONE, GL_ONE);
	glDisable(GL_LIGHTING);

	if (mob->isInvisible())	glDepthMask(false);
    else					glDepthMask(true);

	if (SharedConstants::TEXTURE_LIGHTING)
	{
		int col = 0xf0f0;
		int u = col % 65536;
		int v = col / 65536;

		glMultiTexCoord2f(GL_TEXTURE1, u / 1.0f, v / 1.0f);
		glColor4f(1, 1, 1, 1);
	}

	glEnable(GL_LIGHTING);
	glColor4f(1, 1, 1, br);
	return 1;
}

int EndermanRenderer::prepareArmorOverlay(shared_ptr<Mob> _mob, int layer, float a)
{
	// prepareArmorOverlay() draws the hurt/death red overlay and calls
	// prepareArmor() internally. That pass must draw the whole model, so keep
	// the head-only eyes mode disabled for it.
	bool prevAllowed = m_allowEyesOnly;
	m_allowEyesOnly = false;
	model->eyesOnly = false;
	int result = MobRenderer::prepareArmorOverlay(_mob, layer, a);
	m_allowEyesOnly = prevAllowed;
	return result;
}