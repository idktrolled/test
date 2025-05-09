/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2025 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/EngineMain.h"
#include "oxygen/drawing/opengl/OpenGLDrawer.h"
#include "oxygen/helper/Logging.h"
#include "oxygen/rendering/Geometry.h"
#include "oxygen/rendering/RenderResources.h"
#include "oxygen/rendering/opengl/OpenGLRenderer.h"
#include "oxygen/rendering/software/SoftwareRenderer.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/resources/FontCollection.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/LogDisplay.h"
#include "oxygen/simulation/Simulation.h"


VideoOut::VideoOut() :
	mRenderResources(*new RenderResources())
{
	mGeometries.reserve(0x100);
}

VideoOut::~VideoOut()
{
	delete mRenderParts;
	delete &mRenderResources;
	delete mSoftwareRenderer;
#ifdef RMX_WITH_OPENGL_SUPPORT
	delete mOpenGLRenderer;
#endif
}

void VideoOut::startup()
{
	mGameResolution = Configuration::instance().mGameScreen;

	RMX_LOG_INFO("VideoOut: Setup of game screen");
	EngineMain::instance().getDrawer().createTexture(mGameScreenTexture);
	mGameScreenTexture.setupAsRenderTarget(mGameResolution.x, mGameResolution.y);

	if (nullptr == mRenderParts)
	{
		RMX_LOG_INFO("VideoOut: Creating render parts");
		mRenderParts = new RenderParts();
	}

	createRenderer(false);
}

void VideoOut::shutdown()
{
	clearGeometries();
}

void VideoOut::reset()
{
	mRenderParts->reset();
	mActiveRenderer->reset();

	mFrameInterpolation.mUseInterpolationLastUpdate = false;
	mFrameInterpolation.mUseInterpolationThisUpdate = false;
	mDebugDrawRenderingRequested = false;
	mPreviouslyHadNewRenderItems = false;
}

void VideoOut::handleActiveModsChanged()
{
	// Better reset everything, as sprite references might have become invalid and should be removed
	reset();
}

void VideoOut::createRenderer(bool reset)
{
	setActiveRenderer(Configuration::instance().mRenderMethod == Configuration::RenderMethod::OPENGL_FULL, reset);
}

void VideoOut::destroyRenderer()
{
	SAFE_DELETE(mSoftwareRenderer);
#ifdef RMX_WITH_OPENGL_SUPPORT
	SAFE_DELETE(mOpenGLRenderer);
#endif
}

void VideoOut::setActiveRenderer(bool useOpenGLRenderer, bool reset)
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (useOpenGLRenderer)
	{
		if (nullptr == mOpenGLRenderer)
		{
			RMX_LOG_INFO("VideoOut: Creating OpenGL renderer");
			mOpenGLRenderer = new OpenGLRenderer(*mRenderParts, mGameScreenTexture);

			RMX_LOG_INFO("VideoOut: Renderer initialization");
			mOpenGLRenderer->initialize();
		}
		mActiveRenderer = mOpenGLRenderer;
	}
	else
#endif
	{
		if (nullptr == mSoftwareRenderer)
		{
			RMX_LOG_INFO("VideoOut: Creating software renderer");
			mSoftwareRenderer = new SoftwareRenderer(*mRenderParts, mGameScreenTexture);

			RMX_LOG_INFO("VideoOut: Renderer initialization");
			mSoftwareRenderer->initialize();
		}
		mActiveRenderer = mSoftwareRenderer;
	}

	if (reset)
	{
		mActiveRenderer->reset();
		mActiveRenderer->setGameResolution(mGameResolution);
	}
}

void VideoOut::setScreenSize(uint32 width, uint32 height)
{
	mGameResolution.x = width;
	mGameResolution.y = height;

	mGameScreenTexture.setupAsRenderTarget(mGameResolution.x, mGameResolution.y);

	mActiveRenderer->setGameResolution(mGameResolution);

	// Render game screen again (this is particularly needed when switching from in-game Options back to the Pause Menu)
	mRequireGameScreenUpdate = true;
}

Vec2i VideoOut::getInterpolatedWorldSpaceOffset() const
{
	Vec2i offset = mRenderParts->getSpacesManager().getWorldSpaceOffset();
	if (mFrameInterpolation.mCurrentlyInterpolating)
	{
		const Vec2f interpolatedDifference = Vec2f(mLastWorldSpaceOffset - offset) * (1.0f - mFrameInterpolation.mInterFramePosition);
		offset += Vec2i(roundToInt(interpolatedDifference.x), roundToInt(interpolatedDifference.y));
	}
	return offset;
}

void VideoOut::preFrameUpdate()
{
	mRenderParts->preFrameUpdate();
	mLastWorldSpaceOffset = mRenderParts->getSpacesManager().getWorldSpaceOffset();

	// Skipped frames without rendering?
	if (mFrameState == FrameState::FRAME_READY)
	{
		// Processing of last frame (to avoid e.g. sprites rendered multiple times)
		RefreshParameters refreshParameters;
		refreshParameters.mSkipThisFrame = true;
		mRenderParts->refresh(refreshParameters);
	}

	mFrameState = FrameState::INSIDE_FRAME;
	mDebugDrawRenderingRequested = false;
}

void VideoOut::postFrameUpdate()
{
	mRenderParts->postFrameUpdate();

	// Signal for rendering
	mFrameState = FrameState::FRAME_READY;
	mLastFrameTicks = SDL_GetTicks();
	mFrameInterpolation.mUseInterpolationLastUpdate = mFrameInterpolation.mUseInterpolationThisUpdate;
	mFrameInterpolation.mUseInterpolationThisUpdate = true;		// Could be set differently, e.g. if we had a script binding to disable interpolation for an update
	mDebugDrawRenderingRequested = false;
}

void VideoOut::initAfterSaveStateLoad()
{
	mFrameState = FrameState::FRAME_READY;
	mLastFrameTicks = SDL_GetTicks();
	mFrameInterpolation.mUseInterpolationThisUpdate = false;
	mDebugDrawRenderingRequested = false;
}

void VideoOut::setInterFramePosition(float position)
{
	mFrameInterpolation.mInterFramePosition = position;
}

bool VideoOut::updateGameScreen()
{
	mFrameInterpolation.mCurrentlyInterpolating = (Configuration::instance().mFrameSync == Configuration::FrameSyncType::FRAME_INTERPOLATION && mFrameInterpolation.mUseInterpolationLastUpdate && mFrameInterpolation.mUseInterpolationThisUpdate);

	// Only render something if a frame simulation was completed in the meantime
	const bool hasNewSimulationFrame = (mFrameState == FrameState::FRAME_READY);
	if (!hasNewSimulationFrame && !mFrameInterpolation.mCurrentlyInterpolating && !mDebugDrawRenderingRequested && !mRequireGameScreenUpdate)
	{
		// No update
		return false;
	}

	mFrameState = FrameState::OUTSIDE_FRAME;
	mRequireGameScreenUpdate = false;

	RefreshParameters refreshParameters;
	refreshParameters.mSkipThisFrame = false;
	refreshParameters.mHasNewSimulationFrame = hasNewSimulationFrame;
	refreshParameters.mUsingFrameInterpolation = mFrameInterpolation.mCurrentlyInterpolating;
	refreshParameters.mInterFramePosition = mFrameInterpolation.mInterFramePosition;
	mRenderParts->refresh(refreshParameters);

	// Render a new image
	renderGameScreen();

	// Game screen got updated
	return true;
}

void VideoOut::blurGameScreen()
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (mActiveRenderer == mOpenGLRenderer)
	{
		mOpenGLRenderer->blurGameScreen();
	}
#endif
}

void VideoOut::toggleLayerRendering(int index)
{
	mRenderParts->mLayerRendering[index] = !mRenderParts->mLayerRendering[index];
}

std::string VideoOut::getLayerRenderingDebugString() const
{
	char string[10] = "basc BASC";
	for (int i = 0; i < 8; ++i)
	{
		if (!mRenderParts->mLayerRendering[i])
			string[i + i/4] = '-';
	}
	return string;
}

void VideoOut::getScreenshot(Bitmap& outBitmap)
{
	mGameScreenTexture.writeContentToBitmap(outBitmap);
}

void VideoOut::clearGeometries()
{
	for (Geometry* geometry : mGeometries)
	{
		mGeometryFactory.destroy(*geometry);
	}
	mGeometries.clear();

	// Regularly cleanup old cache items -- it's safe now that no geometry references a texture in there any more
	RenderResources::instance().mPrintedTextCache.regularCleanup();
}

void VideoOut::collectGeometries(std::vector<Geometry*>& geometries)
{
	// Add plane geometries
	{
		const PlaneManager& pm = mRenderParts->getPlaneManager();
		const Recti fullscreenRect(0, 0, mGameResolution.x, mGameResolution.y);
		Recti rectForPlaneB = fullscreenRect;
		Recti rectForPlaneA = fullscreenRect;
		Recti rectForPlaneW = fullscreenRect;
		if (pm.isPlaneUsed(PlaneManager::PLANE_W))
		{
			const int splitY = pm.getPlaneAWSplit();
			rectForPlaneA.height = splitY;
			rectForPlaneW.y = splitY;
			rectForPlaneW.height -= splitY;
		}
		else
		{
			rectForPlaneW.height = 0;
		}

		// Plane B non-prio
		if (mRenderParts->mLayerRendering[0] && pm.isDefaultPlaneEnabled(0))
		{
			geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneB, PlaneManager::PLANE_B, false, PlaneManager::PLANE_B, 0x1000));
		}

		// Plane A (and possibly plane W) non-prio
		if (mRenderParts->mLayerRendering[1] && pm.isDefaultPlaneEnabled(1))
		{
			if (rectForPlaneA.height > 0)
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneA, PlaneManager::PLANE_A, false, PlaneManager::PLANE_A, 0x2000));
			}
			if (rectForPlaneW.height > 0)
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneW, PlaneManager::PLANE_W, false, 0xff, 0x2000));
			}
		}

		// Plane B prio
		if (mRenderParts->mLayerRendering[4] && pm.isDefaultPlaneEnabled(2))
		{
			geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneB, PlaneManager::PLANE_B, true, PlaneManager::PLANE_B, 0x3000));
		}

		// Plane A (and possibly plane W) prio
		if (mRenderParts->mLayerRendering[5] && pm.isDefaultPlaneEnabled(3))
		{
			if (rectForPlaneA.height > 0)
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneA, PlaneManager::PLANE_A, true, PlaneManager::PLANE_A, 0x4000));
			}
			if (rectForPlaneW.height > 0)
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(rectForPlaneW, PlaneManager::PLANE_W, true, 0xff, 0x4000));
			}
		}

		if (!pm.getCustomPlanes().empty())
		{
			for (const auto& customPlane : pm.getCustomPlanes())
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(customPlane.mRect, customPlane.mSourcePlane & 0x03, (customPlane.mSourcePlane & 0x10) != 0, customPlane.mScrollOffsets, customPlane.mRenderQueue));
			}
		}
	}

	// Add render item geometries (sprites, texts, etc.)
	{
		SpriteManager& spriteManager = mRenderParts->getSpriteManager();
		const Vec2i worldSpaceOffset = mRenderParts->getSpacesManager().getWorldSpaceOffset();
		FontCollection& fontCollection = FontCollection::instance();

		for (int index = 0; index < RenderItem::NUM_LIFETIME_CONTEXTS; ++index)
		{
			const RenderItem::LifetimeContext lifetimeContext = (RenderItem::LifetimeContext)index;
			const std::vector<RenderItem*>& renderItems = spriteManager.getRenderItems(lifetimeContext);

			for (RenderItem* renderItem : renderItems)
			{
				switch (renderItem->getType())
				{
					case RenderItem::Type::RECTANGLE:
					{
						const renderitems::Rectangle& rectangle = static_cast<const renderitems::Rectangle&>(*renderItem);

						Color color = rectangle.mColor;
						if (rectangle.mUseGlobalComponentTint)
						{
							mRenderParts->getPaletteManager().applyGlobalComponentTint(color);
						}

						Geometry& geometry = mGeometryFactory.createRectGeometry(Recti(rectangle.mPosition, rectangle.mSize), color);
						geometry.mRenderQueue = rectangle.mRenderQueue;
						geometries.push_back(&geometry);
						break;
					}

					case RenderItem::Type::TEXT:
					{
						const renderitems::Text& text = static_cast<const renderitems::Text&>(*renderItem);

						Font* font = fontCollection.getFontByKey(text.mFontKeyHash);
						if (nullptr == font)
						{
							font = fontCollection.createFontByKey(text.mFontKeyString);
						}
						if (nullptr != font)
						{
							const PrintedTextCache::Key key(text.mFontKeyHash, text.mTextHash, (uint8)text.mSpacing);
							PrintedTextCache& cache = RenderResources::instance().mPrintedTextCache;
							PrintedTextCache::CacheItem* cacheItem = cache.getCacheItem(key);
							if (nullptr == cacheItem)
							{
								cacheItem = &cache.addCacheItem(key, *font, text.mTextString);
							}
							const Vec2i drawPosition = Font::applyAlignment(Recti(text.mPosition, Vec2i(0, 0)), cacheItem->mInnerRect, text.mAlignment);
							const Recti rect(drawPosition, cacheItem->mTexture.getSize());

							Color tintColor = text.mColor;
							Color addedColor = Color::TRANSPARENT;
							if (text.mUseGlobalComponentTint)
							{
								mRenderParts->getPaletteManager().applyGlobalComponentTint(tintColor, addedColor);
							}

							Geometry& geometry = mGeometryFactory.createTexturedRectGeometry(rect, cacheItem->mTexture, tintColor, addedColor);
							geometry.mRenderQueue = text.mRenderQueue;
							geometries.push_back(&geometry);
						}
						break;
					}

					case RenderItem::Type::VDP_SPRITE:
					case RenderItem::Type::PALETTE_SPRITE:
					case RenderItem::Type::COMPONENT_SPRITE:
					case RenderItem::Type::SPRITE_MASK:
					{
						renderitems::SpriteInfo& sprite = static_cast<renderitems::SpriteInfo&>(*renderItem);
						bool accept = true;
						switch (renderItem->getType())
						{
							case RenderItem::Type::VDP_SPRITE:
							{
								accept = (mRenderParts->mLayerRendering[sprite.mPriorityFlag ? 6 : 2]);
								break;
							}

							case RenderItem::Type::PALETTE_SPRITE:
							case RenderItem::Type::COMPONENT_SPRITE:
							{
								accept = (mRenderParts->mLayerRendering[sprite.mPriorityFlag ? 7 : 3]);
								break;
							}

							default:
								// Accept everything else
								break;
						}

						if (accept)
						{
							sprite.mInterpolatedPosition = sprite.mPosition;
							if (mFrameInterpolation.mCurrentlyInterpolating)
							{
								Vec2i difference;
								if (sprite.mHasLastPosition)
								{
									difference = sprite.mLastPositionChange;
								}
								else if (sprite.mLogicalSpace == SpriteManager::Space::WORLD)
								{
									// Assume sprite is standing still in world space, i.e. moving entirely with camera
									difference = mLastWorldSpaceOffset - worldSpaceOffset;
								}
								else
								{
									// Assume sprite is standing still in screen space, i.e. not moving on the screen
								}

								if ((difference.x != 0 || difference.y != 0) && (abs(difference.x) < 0x40 && abs(difference.y) < 0x40))
								{
									const Vec2f interpolatedDifference = Vec2f(difference) * (1.0f - mFrameInterpolation.mInterFramePosition);
									sprite.mInterpolatedPosition -= Vec2i(roundToInt(interpolatedDifference.x), roundToInt(interpolatedDifference.y));
								}
							}

							SpriteGeometry& spriteGeometry = mGeometryFactory.createSpriteGeometry(sprite);
							spriteGeometry.mRenderQueue = sprite.mRenderQueue;
							geometries.push_back(&spriteGeometry);
						}
						break;
					}

					case RenderItem::Type::VIEWPORT:
					{
						const renderitems::Viewport& viewport = static_cast<const renderitems::Viewport&>(*renderItem);

						Geometry& geometry = mGeometryFactory.createViewportGeometry(Recti(viewport.mPosition, viewport.mSize));
						geometry.mRenderQueue = viewport.mRenderQueue;
						geometries.push_back(&geometry);
						break;
					}

					default:
						break;
				}
			}
		}
	}

	// Insert blur effect geometry at the right position
	if (Configuration::instance().mBackgroundBlur > 0)
	{
		constexpr uint16 BLUR_RENDER_QUEUE = 0x1800;

		// Anything there to blur at all?
		//  -> There might be no blurred background at all (e.g. in S3K Sky Sanctuary upper levels)
		bool blurNeeded = false;
		for (const Geometry* geometry : geometries)
		{
			if (geometry->mRenderQueue < BLUR_RENDER_QUEUE)
			{
				blurNeeded = true;
				break;
			}
		}

		if (blurNeeded)
		{
			Geometry& geometry = mGeometryFactory.createEffectBlurGeometry(Configuration::instance().mBackgroundBlur);
			geometry.mRenderQueue = BLUR_RENDER_QUEUE - 1;
			geometries.push_back(&geometry);
		}
	}

	// Sort everything by render queue
	std::stable_sort(geometries.begin(), geometries.end(),
					 [](const Geometry* a, const Geometry* b) { return a->mRenderQueue < b->mRenderQueue; });
}

void VideoOut::renderGameScreen()
{
	// Collect geometries to render
	clearGeometries();
	if (mRenderParts->getActiveDisplay())
	{
		collectGeometries(mGeometries);
	}

	// Render them
	mActiveRenderer->renderGameScreen(mGeometries);
}

void VideoOut::preRefreshDebugging()
{
	mRenderParts->getSpriteManager().clearLifetimeContext(RenderItem::LifetimeContext::OUTSIDE_FRAME);
}

void VideoOut::postRefreshDebugging()
{
	const bool hasNewRenderItems = !mRenderParts->getSpriteManager().getAddedItems().empty();
	mDebugDrawRenderingRequested = hasNewRenderItems || mPreviouslyHadNewRenderItems;
	mPreviouslyHadNewRenderItems = hasNewRenderItems;

	if (hasNewRenderItems)
	{
		mRenderParts->getSpriteManager().postRefreshDebugging();
	}
}

void VideoOut::renderDebugDraw(int debugDrawMode, const Recti& rect)
{
	mActiveRenderer->renderDebugDraw(debugDrawMode, rect);
}

void VideoOut::dumpDebugDraw(int debugDrawMode)
{
	if (debugDrawMode < 2)
	{
		mRenderParts->dumpPlaneContent(debugDrawMode);
	}
	else
	{
		mRenderParts->dumpPatternsContent();
	}
}
