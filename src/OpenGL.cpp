#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <time.h>       /* time_t, struct tm, difftime, time, mktime */

#include "Types.h"
#include "GLideN64.h"
#include "OpenGL.h"
#include "RDP.h"
#include "RSP.h"
#include "N64.h"
#include "gSP.h"
#include "gDP.h"
#include "Textures.h"
#include "Combiner.h"
#include "GLSLCombiner.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "FrameBufferInfo.h"
#include "GLideNHQ/Ext_TxFilter.h"
#include "VI.h"
#include "Config.h"
#include "wst.h"
#include "Log.h"
#include "TextDrawer.h"
#include "PluginAPI.h"
#include "PostProcessor.h"

using namespace std;

bool checkFBO() {
	GLenum e = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch (e) {
//		case GL_FRAMEBUFFER_UNDEFINED:
//			printf("FBO Undefined\n");
//			break;
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
			LOG(LOG_ERROR, "[gles2GlideN64]: FBO Incomplete Attachment\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT :
			LOG(LOG_ERROR, "[gles2GlideN64]: FBO Missing Attachment\n");
			break;
//		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER :
//			printf("FBO Incomplete Draw Buffer\n");
//			break;
		case GL_FRAMEBUFFER_UNSUPPORTED :
			LOG(LOG_ERROR, "[gles2GlideN64]: FBO Unsupported\n");
			break;
		case GL_FRAMEBUFFER_COMPLETE:
			LOG(LOG_VERBOSE, "[gles2GlideN64]: FBO OK\n");
			break;
//		case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT:
//			printf("framebuffer FRAMEBUFFER_DIMENSIONS\n");
//			break;
//		case GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT:
//			printf("framebuffer INCOMPLETE_FORMATS\n");
//			break;
		default:
			LOG(LOG_ERROR, "[gles2GlideN64]: FBO Problem?\n");
	}
	return e == GL_FRAMEBUFFER_COMPLETE;
}

const char* GLErrorString(GLenum errorCode)
{
	static const struct {
		GLenum code;
		const char *string;
	} errors[]=
	{
		/* GL */
	{GL_NO_ERROR, "no error"},
	{GL_INVALID_ENUM, "invalid enumerant"},
	{GL_INVALID_VALUE, "invalid value"},
	{GL_INVALID_OPERATION, "invalid operation"},
#ifndef GLESX
	{GL_STACK_OVERFLOW, "stack overflow"},
	{GL_STACK_UNDERFLOW, "stack underflow"},
#endif
	{GL_OUT_OF_MEMORY, "out of memory"},

	{0, NULL }
};

	int i;

	for (i=0; errors[i].string; i++)
	{
		if (errors[i].code == errorCode)
		{
			return errors[i].string;
		}
	}

	return NULL;
}

bool isGLError()
{
	GLenum errCode;
	const char* errString;

	if ((errCode = glGetError()) != GL_NO_ERROR) {
		errString = GLErrorString(errCode);
		if (errString != NULL)
			fprintf (stderr, "OpenGL Error: %s\n", errString);
		return true;
	}
	return false;
}

bool OGLVideo::isExtensionSupported(const char *extension)
{
	GLubyte *where = (GLubyte *)strchr(extension, ' ');
	if (where || *extension == '\0')
		return false;

	const GLubyte *extensions = glGetString(GL_EXTENSIONS);

	const GLubyte *start = extensions;
	for (;;) {
		where = (GLubyte *)strstr((const char *)start, extension);
		if (where == NULL)
			break;

		GLubyte *terminator = where + strlen(extension);
		if (where == start || *(where - 1) == ' ')
		if (*terminator == ' ' || *terminator == '\0')
			return true;

		start = terminator;
	}

	return false;
}

void OGLVideo::start()
{
	_start(); // TODO: process initialization error
	initGLFunctions();
	m_render._initData();
	m_buffersSwapCount = 0;
}

void OGLVideo::stop()
{
	m_render._destroyData();
	_stop();
}

void OGLVideo::restart()
{
	m_bResizeWindow = true;
}

void OGLVideo::swapBuffers()
{
	_swapBuffers();
	gDP.otherMode.l = 0;
	if ((config.generalEmulation.hacks & hack_doNotResetTLUTmode) == 0)
		gDPSetTextureLUT(G_TT_NONE);
	++m_buffersSwapCount;
}

void OGLVideo::setCaptureScreen(const char * const _strDirectory)
{
	::mbstowcs(m_strScreenDirectory, _strDirectory, PLUGIN_PATH_SIZE-1);
	m_bCaptureScreen = true;
}

void OGLVideo::saveScreenshot()
{
	if (!m_bCaptureScreen)
		return;
	_saveScreenshot();
	m_bCaptureScreen = false;
}

bool OGLVideo::changeWindow()
{
	if (!m_bToggleFullscreen)
		return false;
	m_render._destroyData();
	_changeWindow();
	updateScale();
	m_render._initData();
	m_bToggleFullscreen = false;
	return true;
}

void OGLVideo::setWindowSize(u32 _width, u32 _height)
{
	if (m_width != _width || m_height != _height) {
		m_resizeWidth = _width;
		m_resizeHeight = _height;
		m_bResizeWindow = true;
	}
}

bool OGLVideo::resizeWindow()
{
	if (!m_bResizeWindow)
		return false;
	m_render._destroyData();
	if (!_resizeWindow())
		_start();
	updateScale();
	m_render._initData();
	m_bResizeWindow = false;
	return true;
}

void OGLVideo::updateScale()
{
	if (VI.width == 0 || VI.height == 0)
		return;
	m_scaleX = m_width / (float)VI.width;
	m_scaleY = m_height / (float)VI.height;
}

void OGLVideo::_setBufferSize()
{
	m_bAdjustScreen = false;
	if (config.frameBufferEmulation.enable) {
		switch (config.frameBufferEmulation.aspect) {
		case Config::aStretch: // stretch
			m_width = m_screenWidth;
			m_height = m_screenHeight;
			break;
		case Config::a43: // force 4/3
			if (m_screenWidth * 3 / 4 > m_screenHeight) {
				m_height = m_screenHeight;
				m_width = m_screenHeight * 4 / 3;
			} else if (m_screenHeight * 4 / 3 > m_screenWidth) {
				m_width = m_screenWidth;
				m_height = m_screenWidth * 3 / 4;
			} else {
				m_width = m_screenWidth;
				m_height = m_screenHeight;
			}
			break;
		case Config::a169: // force 16/9
			if (m_screenWidth * 9 / 16 > m_screenHeight) {
				m_height = m_screenHeight;
				m_width = m_screenHeight * 16 / 9;
			} else if (m_screenHeight * 16 / 9 > m_screenWidth) {
				m_width = m_screenWidth;
				m_height = m_screenWidth * 9 / 16;
			} else {
				m_width = m_screenWidth;
				m_height = m_screenHeight;
			}
			break;
		case Config::aAdjust: // adjust
			m_width = m_screenWidth;
			m_height = m_screenHeight;
			if (m_screenWidth * 3 / 4 > m_screenHeight) {
				f32 width43 = m_screenHeight * 4.0f / 3.0f;
				m_adjustScale = width43 / m_screenWidth;
				m_bAdjustScreen = true;
			}
			break;
		default:
			assert(false && "Unknown aspect ratio");
			m_width = m_screenWidth;
			m_height = m_screenHeight;
		}
	} else {
		m_width = m_screenWidth;
		m_height = m_screenHeight;
	}
}

void OGLVideo::readScreen(void **_pDest, long *_pWidth, long *_pHeight )
{
	*_pWidth = m_width;
	*_pHeight = m_height;

	*_pDest = malloc( m_height * m_width * 3 );
	if (*_pDest == NULL)
		return;

#ifndef GLESX
	const GLenum format = GL_BGR_EXT;
	glReadBuffer( GL_FRONT );
#else
	const GLenum format = GL_RGB;
#endif
	glReadPixels( 0, m_heightOffset, m_width, m_height, format, GL_UNSIGNED_BYTE, *_pDest );
}

void OGLVideo::readScreen2(void * _dest, int * _width, int * _height, int _front)
{
	if (_width == NULL || _height == NULL)
		return;

	*_width = m_screenWidth;
	*_height = m_screenHeight;

	if (_dest == NULL)
		return;

#ifndef GLES2
	GLint oldMode;
	glGetIntegerv(GL_READ_BUFFER, &oldMode);
	if (_front != 0)
		glReadBuffer(GL_FRONT);
	else
		glReadBuffer(GL_BACK);
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGB, GL_UNSIGNED_BYTE, _dest);
	glReadBuffer(oldMode);
#else
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGB, GL_UNSIGNED_BYTE, _dest);
#endif
}

void OGLRender::addTriangle(int _v0, int _v1, int _v2)
{
	const u32 firstIndex = triangles.num;
	triangles.elements[triangles.num++] = _v0;
	triangles.elements[triangles.num++] = _v1;
	triangles.elements[triangles.num++] = _v2;
	m_modifyVertices |= triangles.vertices[_v0].modify |
		triangles.vertices[_v1].modify |
		triangles.vertices[_v2].modify;

	if ((gSP.geometryMode & G_LIGHTING) == 0) {
		if ((gSP.geometryMode & G_SHADE) == 0) {
			// Prim shading
			for (u32 i = firstIndex; i < triangles.num; ++i) {
				SPVertex & vtx = triangles.vertices[triangles.elements[i]];
				vtx.flat_r = gDP.primColor.r;
				vtx.flat_g = gDP.primColor.g;
				vtx.flat_b = gDP.primColor.b;
				vtx.flat_a = gDP.primColor.a;
			}
		} else if ((gSP.geometryMode & G_SHADING_SMOOTH) == 0) {
			// Flat shading
			SPVertex & vtx0 = triangles.vertices[triangles.elements[firstIndex + ((RSP.w1 >> 24) & 3)]];
			for (u32 i = firstIndex; i < triangles.num; ++i) {
				SPVertex & vtx = triangles.vertices[triangles.elements[i]];
				vtx.r = vtx.flat_r = vtx0.r;
				vtx.g = vtx.flat_g = vtx0.g;
				vtx.b = vtx.flat_b = vtx0.b;
				vtx.a = vtx.flat_a = vtx0.a;
				vtx.a = vtx0.a;
			}
		}
	}

	if (gDP.otherMode.depthSource == G_ZS_PRIM) {
		for (u32 i = firstIndex; i < triangles.num; ++i) {
			SPVertex & vtx = triangles.vertices[triangles.elements[i]];
			vtx.z = gDP.primDepth.z * vtx.w;
		}
	}

#ifdef GLESX
	if (GBI.isNoN() && gDP.otherMode.depthCompare == 0 && gDP.otherMode.depthUpdate == 0) {
		for (u32 i = firstIndex; i < triangles.num; ++i) {
			SPVertex & vtx = triangles.vertices[triangles.elements[i]];
			vtx.z = 0.0f;
		}
	}
#endif
}

void OGLRender::_setBlendMode() const
{
	if (gDP.otherMode.forceBlender != 0 && gDP.otherMode.cycleType < G_CYC_COPY) {
		GLenum srcFactor = GL_ONE;
		GLenum dstFactor = GL_ZERO;
		u32 memFactorSource = 2, muxA, muxB;
		if (gDP.otherMode.cycleType == G_CYC_2CYCLE) {
			muxA = gDP.otherMode.c2_m1b;
			muxB = gDP.otherMode.c2_m2b;
			if (gDP.otherMode.c2_m1a == 1) {
				if (gDP.otherMode.c2_m2a == 1) {
					glEnable(GL_BLEND);
					glBlendFunc(GL_ZERO, GL_ONE);
					return;
				}
				memFactorSource = 0;
			} else if (gDP.otherMode.c2_m2a == 1) {
				memFactorSource = 1;
			}
			if (gDP.otherMode.c2_m2a == 0 && gDP.otherMode.c2_m2b == 1) {
				// c_in * a_mem
				srcFactor = GL_DST_ALPHA;
			}
		} else {
			muxA = gDP.otherMode.c1_m1b;
			muxB = gDP.otherMode.c1_m2b;
			if (gDP.otherMode.c1_m1a == 1) {
				if (gDP.otherMode.c1_m2a == 1) {
					glEnable(GL_BLEND);
					glBlendFunc(GL_ZERO, GL_ONE);
					return;
				}
				memFactorSource = 0;
			} else if (gDP.otherMode.c1_m2a == 1) {
				memFactorSource = 1;
			}
			if (gDP.otherMode.c1_m2a == 0 && gDP.otherMode.c1_m2b == 1) {
				// c_pixel * a_mem
				srcFactor = GL_DST_ALPHA;
			}
		}
		switch (memFactorSource) {
		case 0:
			switch (muxA) {
			case 0:
				dstFactor = GL_SRC_ALPHA;
				break;
			case 1:
				glBlendColor(gDP.fogColor.r, gDP.fogColor.g, gDP.fogColor.b, gDP.fogColor.a);
				dstFactor = GL_CONSTANT_ALPHA;
				break;
			case 2:
				assert(false); // shade alpha
				dstFactor = GL_SRC_ALPHA;
				break;
			case 3:
				dstFactor = GL_ZERO;
				break;
			}
			break;
		case 1:
			switch (muxB) {
			case 0:
				// 1.0 - muxA
				switch (muxA) {
				case 0:
					dstFactor = GL_ONE_MINUS_SRC_ALPHA;
					break;
				case 1:
					glBlendColor(gDP.fogColor.r, gDP.fogColor.g, gDP.fogColor.b, gDP.fogColor.a);
					dstFactor = GL_ONE_MINUS_CONSTANT_ALPHA;
					break;
				case 2:
					assert(false); // shade alpha
					dstFactor = GL_ONE_MINUS_SRC_ALPHA;
					break;
				case 3:
					dstFactor = GL_ONE;
					break;
				}
				break;
			case 1:
				dstFactor = GL_DST_ALPHA;
				break;
			case 2:
				dstFactor = GL_ONE;
				break;
			case 3:
				dstFactor = GL_ZERO;
				break;
			}
			break;
		default:
			dstFactor = GL_ZERO;
		}
		glEnable( GL_BLEND );
		glBlendFunc(srcFactor, dstFactor);
	} else if ((config.generalEmulation.hacks & hack_pilotWings) != 0 && gDP.otherMode.clearOnCvg != 0) { //CLR_ON_CVG without FORCE_BL
		glEnable(GL_BLEND);
		glBlendFunc(GL_ZERO, GL_ONE);
	} else if ((config.generalEmulation.hacks & hack_blastCorps) != 0 && gDP.otherMode.cycleType < G_CYC_COPY && gSP.texture.on == 0 && currentCombiner()->usesTexture()) { // Blast Corps
		glEnable(GL_BLEND);
		glBlendFunc(GL_ZERO, GL_ONE);
	} else if ((gDP.otherMode.forceBlender == 0 && gDP.otherMode.cycleType < G_CYC_COPY)) {
		if (gDP.otherMode.c1_m1a == 1 && gDP.otherMode.c1_m2a == 1) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_ZERO, GL_ONE);
		} else {
			glDisable(GL_BLEND);
		}
	} else {
		glDisable( GL_BLEND );
	}
}

void OGLRender::_updateCullFace() const
{
	if (gSP.geometryMode & G_CULL_BOTH) {
		glEnable( GL_CULL_FACE );

		if (gSP.geometryMode & G_CULL_BACK)
			glCullFace(GL_BACK);
		else
			glCullFace(GL_FRONT);
	}
	else
		glDisable( GL_CULL_FACE );
}

inline
float _adjustViewportX(f32 _X0)
{
		const float halfX = gDP.colorImage.width / 2.0f;
		const float halfVP = gSP.viewport.width / 2.0f;
		return (_X0 + halfVP - halfX) * video().getAdjustScale() + halfX - halfVP;
}

void OGLRender::_updateViewport() const
{
	OGLVideo & ogl = video();
	FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();
	if (pCurrentBuffer == NULL) {
		const f32 scaleX = ogl.getScaleX();
		const f32 scaleY = ogl.getScaleY();
		float Xf = gSP.viewport.vscale[0] < 0 ? (gSP.viewport.x + gSP.viewport.vscale[0] * 2.0f) : gSP.viewport.x;
		if (ogl.isAdjustScreen() && gSP.viewport.width < gDP.colorImage.width && gDP.colorImage.width > VI.width * 98 / 100)
			Xf = _adjustViewportX(Xf);
		const GLint X = (GLint)(Xf * scaleX);
		const GLint Y = gSP.viewport.vscale[1] < 0 ? (GLint)((gSP.viewport.y + gSP.viewport.vscale[1] * 2.0f) * scaleY) : (GLint)((VI.height - (gSP.viewport.y + gSP.viewport.height)) * scaleY);
		glViewport(X, Y + ogl.getHeightOffset(),
			max((GLint)(gSP.viewport.width * scaleX), 0), max((GLint)(gSP.viewport.height * scaleY), 0));
	} else {
		const f32 scaleX = pCurrentBuffer->m_scaleX;
		const f32 scaleY = pCurrentBuffer->m_scaleY;
		float Xf = gSP.viewport.vscale[0] < 0 ? (gSP.viewport.x + gSP.viewport.vscale[0] * 2.0f) : gSP.viewport.x;
		if (ogl.isAdjustScreen() && gSP.viewport.width < gDP.colorImage.width && gDP.colorImage.width > VI.width * 98 / 100)
			Xf = _adjustViewportX(Xf);
		const GLint X = (GLint)(Xf * scaleX);
		const GLint Y = gSP.viewport.vscale[1] < 0 ? (GLint)((gSP.viewport.y + gSP.viewport.vscale[1] * 2.0f) * scaleY) : (GLint)((pCurrentBuffer->m_height - (gSP.viewport.y + gSP.viewport.height)) * scaleY);
		glViewport(X, Y,
			max((GLint)(gSP.viewport.width * scaleX), 0), max((GLint)(gSP.viewport.height * scaleY), 0));
	}
	gSP.changed &= ~CHANGED_VIEWPORT;
}

void OGLRender::_updateScreenCoordsViewport() const
{
	OGLVideo & ogl = video();
	FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();
	if (pCurrentBuffer == NULL)
		glViewport(0, ogl.getHeightOffset(), ogl.getScreenWidth(), ogl.getScreenHeight());
	else
		glViewport(0, 0, pCurrentBuffer->m_width*pCurrentBuffer->m_scaleX, pCurrentBuffer->m_height*pCurrentBuffer->m_scaleY);
	gSP.changed |= CHANGED_VIEWPORT;
}

inline
void _adjustScissorX(f32 & _X0, f32 & _X1, float _scale)
{
	const float halfX = gDP.colorImage.width / 2.0f;
	_X0 = (_X0 - halfX) * _scale + halfX;
	_X1 = (_X1 - halfX) * _scale + halfX;
}

void OGLRender::updateScissor(FrameBuffer * _pBuffer) const
{
	OGLVideo & ogl = video();
	f32 scaleX, scaleY;
	u32 heightOffset, screenHeight;
	if (_pBuffer == NULL) {
		scaleX = ogl.getScaleX();
		scaleY = ogl.getScaleY();
		heightOffset = ogl.getHeightOffset();
		screenHeight = VI.height;
	}
	else {
		scaleX = _pBuffer->m_scaleX;
		scaleY = _pBuffer->m_scaleY;
		heightOffset = 0;
		screenHeight = (_pBuffer->m_height == 0) ? VI.height : _pBuffer->m_height;
	}

	float SX0 = gDP.scissor.ulx;
	float SX1 = gDP.scissor.lrx;
	if (ogl.isAdjustScreen() && gSP.viewport.width < gDP.colorImage.width && gDP.colorImage.width > VI.width * 98 / 100)
		_adjustScissorX(SX0, SX1, ogl.getAdjustScale());

	glScissor((GLint)(SX0 * scaleX), (GLint)((screenHeight - gDP.scissor.lry) * scaleY + heightOffset),
		max((GLint)((SX1 - SX0) * scaleX), 0), max((GLint)((gDP.scissor.lry - gDP.scissor.uly) * scaleY), 0));
	gDP.changed &= ~CHANGED_SCISSOR;
}

void OGLRender::_updateDepthUpdate() const
{
	if (gDP.otherMode.depthUpdate != 0)
		glDepthMask( TRUE );
	else
		glDepthMask( FALSE );
}

void OGLRender::_updateStates(RENDER_STATE _renderState) const
{
	OGLVideo & ogl = video();

	CombinerInfo & cmbInfo = CombinerInfo::get();
	cmbInfo.update();

	if (gSP.changed & CHANGED_GEOMETRYMODE) {
		_updateCullFace();
		gSP.changed &= ~CHANGED_GEOMETRYMODE;
	}

	if (config.frameBufferEmulation.N64DepthCompare) {
		glDisable( GL_DEPTH_TEST );
		glDepthMask( FALSE );
	} else if ((gDP.changed & (CHANGED_RENDERMODE | CHANGED_CYCLETYPE)) != 0) {
		if (((gSP.geometryMode & G_ZBUFFER) || gDP.otherMode.depthSource == G_ZS_PRIM) && gDP.otherMode.cycleType <= G_CYC_2CYCLE) {
			if (gDP.otherMode.depthCompare != 0) {
				switch (gDP.otherMode.depthMode) {
					case ZMODE_OPA:
					glDisable(GL_POLYGON_OFFSET_FILL);
					glDepthFunc(GL_LEQUAL);
					break;
					case ZMODE_INTER:
					glDisable(GL_POLYGON_OFFSET_FILL);
					glDepthFunc(GL_LEQUAL);
					break;
					case ZMODE_XLU:
					// Max || Infront;
					glDisable(GL_POLYGON_OFFSET_FILL);
					if (gDP.otherMode.depthSource == G_ZS_PRIM && gDP.primDepth.z == 1.0f)
						// Max
						glDepthFunc(GL_LEQUAL);
					else
						// Infront
						glDepthFunc(GL_LESS);
					break;
					case ZMODE_DEC:
					glEnable(GL_POLYGON_OFFSET_FILL);
					glDepthFunc(GL_LEQUAL);
					break;
				}
			} else {
				glDisable(GL_POLYGON_OFFSET_FILL);
				glDepthFunc(GL_ALWAYS);
			}

			_updateDepthUpdate();

			glEnable(GL_DEPTH_TEST);
#ifndef GLESX
			if (!GBI.isNoN())
				glDisable(GL_DEPTH_CLAMP);
#endif
		} else {
			glDisable(GL_DEPTH_TEST);
#ifndef GLESX
			if (!GBI.isNoN())
				glEnable(GL_DEPTH_CLAMP);
#endif
		}
	}

	if (gDP.changed & CHANGED_SCISSOR)
		updateScissor(frameBufferList().getCurrent());

	if (gSP.changed & CHANGED_VIEWPORT)
		_updateViewport();

	if (gSP.changed & CHANGED_LIGHT)
		cmbInfo.updateLightParameters();

	if ((gSP.changed & CHANGED_TEXTURE) ||
		(gDP.changed & (CHANGED_TILE|CHANGED_TMEM)) ||
		cmbInfo.isChanged() ||
		_renderState == rsTexRect) {
		//For some reason updating the texture cache on the first frame of LOZ:OOT causes a NULL Pointer exception...
		ShaderCombiner * pCurrentCombiner = cmbInfo.getCurrent();
		if (pCurrentCombiner != NULL) {
			for (u32 t = 0; t < 2; ++t) {
				if (pCurrentCombiner->usesTile(t))
					textureCache().update(t);
				else
					textureCache().activateDummy(t);
			}
			pCurrentCombiner->updateFrameBufferInfo();
		}
		if (pCurrentCombiner->usesTexture() && (_renderState == rsTriangle || _renderState == rsLine))
			cmbInfo.updateTextureParameters();
		gDP.changed &= ~(CHANGED_TILE | CHANGED_TMEM);
		gSP.changed &= ~(CHANGED_TEXTURE);
	}

	if ((gDP.changed & (CHANGED_RENDERMODE | CHANGED_CYCLETYPE))) {
		_setBlendMode();
		gDP.changed &= ~(CHANGED_RENDERMODE | CHANGED_CYCLETYPE);
	}

	cmbInfo.updateParameters(_renderState);

#ifndef GLES2
	if (gDP.colorImage.address == gDP.depthImageAddress &&
		gDP.otherMode.cycleType != G_CYC_FILL &&
		(config.generalEmulation.hacks & hack_ZeldaMM) == 0
	) {
		FrameBuffer * pCurBuf = frameBufferList().getCurrent();
		if (pCurBuf != nullptr && pCurBuf->m_pDepthBuffer != nullptr) {
			if (gDP.otherMode.depthCompare != 0) {
				CachedTexture * pDepthTexture = pCurBuf->m_pDepthBuffer->copyDepthBufferTexture(pCurBuf);
				if (pDepthTexture == nullptr)
					return;
				glActiveTexture(GL_TEXTURE0 + g_depthTexIndex);
				glBindTexture(GL_TEXTURE_2D, pDepthTexture->glName);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			}
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_ALWAYS);
			glDepthMask(TRUE);
			gDP.changed |= CHANGED_RENDERMODE;
		}
	}
#endif
}

void OGLRender::_setColorArray() const
{
	if (currentCombiner()->usesShade() || gDP.otherMode.c1_m1b == 2)
		// combiner uses shade or blender uses shade alpha
		glEnableVertexAttribArray(SC_COLOR);
	else
		glDisableVertexAttribArray(SC_COLOR);
}

void OGLRender::_setTexCoordArrays() const
{
	if (m_renderState == rsTriangle) {
		glDisableVertexAttribArray(SC_TEXCOORD1);
		if (currentCombiner()->usesTexture())
			glEnableVertexAttribArray(SC_TEXCOORD0);
		else
			glDisableVertexAttribArray(SC_TEXCOORD0);
	} else {
		if (currentCombiner()->usesTile(0))
			glEnableVertexAttribArray(SC_TEXCOORD0);
		else
			glDisableVertexAttribArray(SC_TEXCOORD0);

		if (currentCombiner()->usesTile(1))
			glEnableVertexAttribArray(SC_TEXCOORD1);
		else
			glDisableVertexAttribArray(SC_TEXCOORD1);
	}
}

void OGLRender::_prepareDrawTriangle(bool _dma)
{
#ifdef GL_IMAGE_TEXTURES_SUPPORT
	if (m_bImageTexture && config.frameBufferEmulation.N64DepthCompare != 0)
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
#endif // GL_IMAGE_TEXTURES_SUPPORT

	if ((m_modifyVertices & MODIFY_XY) != 0)
		gSP.changed &= ~CHANGED_VIEWPORT;

	if (gSP.changed || gDP.changed)
		_updateStates(rsTriangle);

	const bool updateArrays = m_renderState != rsTriangle;
	if (updateArrays || CombinerInfo::get().isChanged()) {
		m_renderState = rsTriangle;
		_setColorArray();
		_setTexCoordArrays();
	}
	currentCombiner()->updateRenderState();

	bool bFlatColors = false;
	if (!RSP.bLLE && (gSP.geometryMode & G_LIGHTING) == 0) {
		bFlatColors = (gSP.geometryMode & G_SHADE) == 0;
		bFlatColors |= (gSP.geometryMode & G_SHADING_SMOOTH) == 0;
	}

	const bool updateColorArrays = m_bFlatColors != bFlatColors;
	m_bFlatColors = bFlatColors;

	if (updateArrays) {
		SPVertex * pVtx = _dma ? triangles.dmaVertices.data() : &triangles.vertices[0];
		glVertexAttribPointer(SC_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->x);
		if (m_bFlatColors)
			glVertexAttribPointer(SC_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->flat_r);
		else
			glVertexAttribPointer(SC_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->r);
		glVertexAttribPointer(SC_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->s);
		if (config.generalEmulation.enableHWLighting) {
			glEnableVertexAttribArray(SC_NUMLIGHTS);
			glVertexAttribPointer(SC_NUMLIGHTS, 1, GL_BYTE, GL_FALSE, sizeof(SPVertex), &pVtx->HWLight);
		}
		glEnableVertexAttribArray(SC_MODIFY);
		glVertexAttribPointer(SC_MODIFY, 4, GL_BYTE, GL_FALSE, sizeof(SPVertex), &pVtx->modify);
	} else if (updateColorArrays) {
		SPVertex * pVtx = _dma ? triangles.dmaVertices.data() : &triangles.vertices[0];
		if (m_bFlatColors)
			glVertexAttribPointer(SC_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->flat_r);
		else
			glVertexAttribPointer(SC_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &pVtx->r);
	}

	if ((m_modifyVertices & MODIFY_XY) != 0)
		_updateScreenCoordsViewport();
	m_modifyVertices = 0;
}

bool OGLRender::_canDraw() const
{
	return config.frameBufferEmulation.enable == 0 || frameBufferList().getCurrent() != NULL;
}

void OGLRender::drawLLETriangle(u32 _numVtx)
{
	if (_numVtx == 0 || !_canDraw())
		return;

	for (u32 i = 0; i < _numVtx; ++i) {
		SPVertex & vtx = triangles.vertices[i];
		vtx.modify = MODIFY_ALL;
	}
	m_modifyVertices = MODIFY_ALL;

	gSP.changed &= ~CHANGED_GEOMETRYMODE; // Don't update cull mode
	_prepareDrawTriangle(false);
	glDisable(GL_CULL_FACE);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, _numVtx);
	triangles.num = 0;

	frameBufferList().setBufferChanged();
	gSP.changed |= CHANGED_GEOMETRYMODE;
}

void OGLRender::drawDMATriangles(u32 _numVtx)
{
	if (_numVtx == 0 || !_canDraw())
		return;
	_prepareDrawTriangle(true);
	glDrawArrays(GL_TRIANGLES, 0, _numVtx);
}

void OGLRender::drawTriangles()
{
	if (triangles.num == 0 || !_canDraw()) {
		triangles.num = 0;
		return;
	}

	_prepareDrawTriangle(false);
	glDrawElements(GL_TRIANGLES, triangles.num, GL_UNSIGNED_BYTE, triangles.elements);
//	glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
	triangles.num = 0;
}

void OGLRender::drawLine(int _v0, int _v1, float _width)
{
	if (!_canDraw())
		return;

	if ((triangles.vertices[_v0].modify & MODIFY_XY) != 0)
		gSP.changed &= ~CHANGED_VIEWPORT;
	if (gSP.changed || gDP.changed)
		_updateStates(rsLine);

	FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();

	if (m_renderState != rsLine || CombinerInfo::get().isChanged()) {
		_setColorArray();
		glDisableVertexAttribArray(SC_TEXCOORD0);
		glDisableVertexAttribArray(SC_TEXCOORD1);
		glVertexAttribPointer(SC_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &triangles.vertices[0].x);
		glVertexAttribPointer(SC_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(SPVertex), &triangles.vertices[0].r);
		glEnableVertexAttribArray(SC_MODIFY);
		glVertexAttribPointer(SC_MODIFY, 1, GL_BYTE, GL_FALSE, sizeof(SPVertex), &triangles.vertices[0].modify);

		m_renderState = rsLine;
		currentCombiner()->updateRenderState();
	}

	if ((triangles.vertices[_v0].modify & MODIFY_XY) != 0)
		_updateScreenCoordsViewport();

	unsigned short elem[2];
	elem[0] = _v0;
	elem[1] = _v1;
	if (config.frameBufferEmulation.nativeResFactor == 0)
		glLineWidth(_width * video().getScaleX());
	else
		glLineWidth(_width * config.frameBufferEmulation.nativeResFactor);
	glDrawElements(GL_LINES, 2, GL_UNSIGNED_SHORT, elem);
}

void OGLRender::drawRect(int _ulx, int _uly, int _lrx, int _lry, float *_pColor)
{
	if (!_canDraw())
		return;
	gSP.changed &= ~CHANGED_GEOMETRYMODE; // Don't update cull mode
	if (gSP.changed || gDP.changed)
		_updateStates(rsRect);

	const bool updateArrays = m_renderState != rsRect;
	if (updateArrays || CombinerInfo::get().isChanged()) {
		m_renderState = rsRect;
		glDisableVertexAttribArray(SC_COLOR);
		glDisableVertexAttribArray(SC_TEXCOORD0);
		glDisableVertexAttribArray(SC_TEXCOORD1);
		glDisableVertexAttribArray(SC_NUMLIGHTS);
		glDisableVertexAttribArray(SC_MODIFY);
	}

	if (updateArrays)
		glVertexAttribPointer(SC_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &m_rect[0].x);
	currentCombiner()->updateRenderState();

	FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();
	OGLVideo & ogl = video();
	if (pCurrentBuffer == NULL)
		glViewport( 0, ogl.getHeightOffset(), ogl.getScreenWidth(), ogl.getScreenHeight());
	else {
		glViewport(0, 0, pCurrentBuffer->m_width*pCurrentBuffer->m_scaleX, pCurrentBuffer->m_height*pCurrentBuffer->m_scaleY);
	}
	glDisable(GL_CULL_FACE);

	const float scaleX = pCurrentBuffer != NULL ? 1.0f / pCurrentBuffer->m_width : VI.rwidth;
	const float scaleY = pCurrentBuffer != NULL ? 1.0f / pCurrentBuffer->m_height : VI.rheight;
	const float Z = (gDP.otherMode.depthSource == G_ZS_PRIM) ? gDP.primDepth.z : gSP.viewport.nearz;
	const float W = 1.0f;
	m_rect[0].x = (float)_ulx * (2.0f * scaleX) - 1.0;
	m_rect[0].y = (float)_uly * (-2.0f * scaleY) + 1.0;
	m_rect[0].z = Z;
	m_rect[0].w = W;
	m_rect[1].x = (float)_lrx * (2.0f * scaleX) - 1.0;
	m_rect[1].y = m_rect[0].y;
	m_rect[1].z = Z;
	m_rect[1].w = W;
	m_rect[2].x = m_rect[0].x;
	m_rect[2].y = (float)_lry * (-2.0f * scaleY) + 1.0;
	m_rect[2].z = Z;
	m_rect[2].w = W;
	m_rect[3].x = m_rect[1].x;
	m_rect[3].y = m_rect[2].y;
	m_rect[3].z = Z;
	m_rect[3].w = W;

	if (ogl.isAdjustScreen() && (gDP.colorImage.width > VI.width * 98 / 100) && (_lrx - _ulx < VI.width * 9 / 10)) {
		const float scale = ogl.getAdjustScale();
		for (u32 i = 0; i < 4; ++i)
			m_rect[i].x *= scale;
	}

	if (gDP.otherMode.cycleType == G_CYC_FILL)
		glVertexAttrib4fv(SC_COLOR, _pColor);
	else
		glVertexAttrib4f(SC_COLOR, 0.0f, 0.0f, 0.0f, 0.0f);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gSP.changed |= CHANGED_GEOMETRYMODE | CHANGED_VIEWPORT;
}

static
bool texturedRectShadowMap(const OGLRender::TexturedRectParams &)
{
	FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();
	if (pCurrentBuffer != NULL) {
		if (gDP.textureImage.size == 2 && gDP.textureImage.address >= gDP.depthImageAddress &&  gDP.textureImage.address < (gDP.depthImageAddress + gDP.colorImage.width*gDP.colorImage.width * 6 / 4)) {
#ifdef GL_IMAGE_TEXTURES_SUPPORT
			pCurrentBuffer->m_pDepthBuffer->activateDepthBufferTexture(pCurrentBuffer);
			SetDepthFogCombiner();
#else
			return true;
#endif
		}
	}
	return false;
}

u32 rectDepthBufferCopyFrame = 0xFFFFFFFF;
static
bool texturedRectDepthBufferCopy(const OGLRender::TexturedRectParams & _params)
{
	// Copy one line from depth buffer into auxiliary color buffer with height = 1.
	// Data from depth buffer loaded into TMEM and then rendered to RDRAM by texrect.
	// Works only with depth buffer emulation enabled.
	// Load of arbitrary data to that area causes weird camera rotation in CBFD.
	const gDPTile * pTile = gSP.textureTile[0];
	if (pTile->loadType == LOADTYPE_BLOCK && gDP.textureImage.size == 2 && gDP.textureImage.address >= gDP.depthImageAddress &&  gDP.textureImage.address < (gDP.depthImageAddress + gDP.colorImage.width*gDP.colorImage.width * 6 / 4)) {
		if (config.frameBufferEmulation.copyDepthToRDRAM == 0)
			return true;
		FrameBuffer * pBuffer = frameBufferList().getCurrent();
		if (pBuffer == NULL)
			return true;
		pBuffer->m_cleared = true;
		if (rectDepthBufferCopyFrame != video().getBuffersSwapCount()) {
			rectDepthBufferCopyFrame = video().getBuffersSwapCount();
			if (!FrameBuffer_CopyDepthBuffer(gDP.colorImage.address))
				return true;
		}
		RDP_RepeatLastLoadBlock();

		const u32 width = (u32)(_params.lrx - _params.ulx);
		const u32 ulx = (u32)_params.ulx;
		u16 * pSrc = ((u16*)TMEM) + (u32)floorf(_params.uls + 0.5f);
		u16 *pDst = (u16*)(RDRAM + gDP.colorImage.address);
		for (u32 x = 0; x < width; ++x)
			pDst[(ulx + x) ^ 1] = swapword(pSrc[x]);

		return true;
	}
	return false;
}

static
bool texturedRectCopyToItself(const OGLRender::TexturedRectParams & _params)
{
	FrameBuffer * pCurrent = frameBufferList().getCurrent();
	if (pCurrent != NULL && pCurrent->m_size == G_IM_SIZ_8b && gSP.textureTile[0]->frameBuffer == pCurrent)
		return true;
	return texturedRectDepthBufferCopy(_params);
}

static
bool texturedRectBGCopy(const OGLRender::TexturedRectParams & _params)
{
	if (GBI.getMicrocodeType() != S2DEX)
		return false;

	float flry = _params.lry;
	if (flry > gDP.scissor.lry)
		flry = gDP.scissor.lry;

	const u32 width = (u32)(_params.lrx - _params.ulx);
	const u32 tex_width = gSP.textureTile[0]->line << 3;
	const u32 uly = (u32)_params.uly;
	const u32 lry = flry;

	u8 * texaddr = RDRAM + gDP.loadInfo[gSP.textureTile[0]->tmem].texAddress + tex_width*(u32)_params.ult + (u32)_params.uls;
	u8 * fbaddr = RDRAM + gDP.colorImage.address + (u32)_params.ulx;
//	LOG(LOG_VERBOSE, "memrect (%d, %d, %d, %d), ci_width: %d texaddr: 0x%08lx fbaddr: 0x%08lx\n", (u32)_params.ulx, uly, (u32)_params.lrx, lry, gDP.colorImage.width, gSP.textureTile[0]->imageAddress + tex_width*(u32)_params.ult + (u32)_params.uls, gDP.colorImage.address + (u32)_params.ulx);

	for (u32 y = uly; y < lry; ++y) {
		u8 *src = texaddr + (y - uly) * tex_width;
		u8 *dst = fbaddr + y * gDP.colorImage.width;
		memcpy(dst, src, width);
	}
	frameBufferList().removeBuffer(gDP.colorImage.address);
	return true;
}

static
bool texturedRectPaletteMod(const OGLRender::TexturedRectParams & _params)
{
	if (gDP.textureImage.address == 0x400) {
		// Paper Mario uses complex set of actions to prepare darkness texture.
		// It includes manipulations with texture formats and drawing buffer into itsels.
		// All that stuff is hardly possible to reproduce with GL, so I just use dirty hacks to emualte it.

		if (gDP.colorImage.address == 0x400 && gDP.colorImage.width == 64) {
			memcpy(RDRAM + 0x400, RDRAM + 0x14d500, 4096);
			return true;
		}

		if (gDP.textureImage.width == 64) {
			gDPTile & curTile = gDP.tiles[0];
			curTile.frameBuffer = nullptr;
			curTile.textureMode = TEXTUREMODE_NORMAL;
			textureCache().update(0);
			currentCombiner()->updateFrameBufferInfo();
		}
		return false;
	}

	// Modify palette for Paper Mario "2D lighting" effect
	if (gDP.scissor.lrx != 16 || gDP.scissor.lry != 1 || _params.lrx != 16 || _params.lry != 1)
		return false;
	u8 envr = (u8)(gDP.envColor.r * 31.0f);
	u8 envg = (u8)(gDP.envColor.g * 31.0f);
	u8 envb = (u8)(gDP.envColor.b * 31.0f);
	u16 env16 = (u16)((envr << 11) | (envg << 6) | (envb << 1) | 1);
	u8 prmr = (u8)(gDP.primColor.r * 31.0f);
	u8 prmg = (u8)(gDP.primColor.g * 31.0f);
	u8 prmb = (u8)(gDP.primColor.b * 31.0f);
	u16 prim16 = (u16)((prmr << 11) | (prmg << 6) | (prmb << 1) | 1);
	u16 * src = (u16*)&TMEM[256];
	u16 * dst = (u16*)(RDRAM + gDP.colorImage.address);
	for (u32 i = 0; i < 16; ++i)
		dst[i ^ 1] = (src[i<<2] & 0x100) ? prim16 : env16;
	return true;
}

static
bool texturedRectMonochromeBackground(const OGLRender::TexturedRectParams & _params)
{
	if (gDP.textureImage.address >= gDP.colorImage.address && gDP.textureImage.address <= (gDP.colorImage.address + gDP.colorImage.width*gDP.colorImage.height * 2)) {
#ifdef GL_IMAGE_TEXTURES_SUPPORT
		FrameBuffer * pCurrentBuffer = frameBufferList().getCurrent();
		if (pCurrentBuffer != NULL) {
			FrameBuffer_ActivateBufferTexture(0, pCurrentBuffer);
			SetMonochromeCombiner();
			return false;
		} else
#endif
			return true;
	}
	return false;
}

// Special processing of textured rect.
// Return true if actuial rendering is not necessary
bool(*texturedRectSpecial)(const OGLRender::TexturedRectParams & _params) = NULL;

void OGLRender::drawTexturedRect(const TexturedRectParams & _params)
{
	gSP.changed &= ~CHANGED_GEOMETRYMODE; // Don't update cull mode
	if (_params.texrectCmd && (gSP.changed | gDP.changed) != 0)
		_updateStates(rsTexRect);

	const bool updateArrays = m_renderState != rsTexRect;
	if (updateArrays || CombinerInfo::get().isChanged()) {
		m_renderState = rsTexRect;
		glDisableVertexAttribArray(SC_COLOR);
		_setTexCoordArrays();

		GLfloat alpha = 0.0f;
		if (currentCombiner()->usesShade()) {
			gDPCombine combine;
			combine.mux = currentCombiner()->getKey();
			if (combine.mA0 == G_ACMUX_0 && combine.aA0 == G_ACMUX_SHADE)
				alpha = 1.0f;
		}
		glVertexAttrib4f(SC_COLOR, 0, 0, 0, alpha);
	}

	if (updateArrays) {
#ifdef RENDERSTATE_TEST
		StateChanges++;
#endif
		glVertexAttribPointer(SC_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &m_rect[0].x);
		glVertexAttribPointer(SC_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &m_rect[0].s0);
		glVertexAttribPointer(SC_TEXCOORD1, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &m_rect[0].s1);
		glDisableVertexAttribArray(SC_NUMLIGHTS);
		glDisableVertexAttribArray(SC_MODIFY);
	}
	currentCombiner()->updateRenderState();

	if (_params.texrectCmd && texturedRectSpecial != NULL && texturedRectSpecial(_params)) {
		gSP.changed |= CHANGED_GEOMETRYMODE;
		return;
	}

	if (!_canDraw())
		return;

	const FrameBuffer * pCurrentBuffer = _params.pBuffer;
	OGLVideo & ogl = video();
	if (pCurrentBuffer == NULL)
		glViewport( 0, ogl.getHeightOffset(), ogl.getScreenWidth(), ogl.getScreenHeight());
	else
		glViewport(0, 0, pCurrentBuffer->m_width*pCurrentBuffer->m_scaleX, pCurrentBuffer->m_height*pCurrentBuffer->m_scaleY);
	glDisable( GL_CULL_FACE );

	const float scaleX = pCurrentBuffer != NULL ? 1.0f / pCurrentBuffer->m_width : VI.rwidth;
	const float scaleY = pCurrentBuffer != NULL ? 1.0f / pCurrentBuffer->m_height : VI.rheight;
	const float Z = (gDP.otherMode.depthSource == G_ZS_PRIM) ? gDP.primDepth.z : gSP.viewport.nearz;
	const float W = 1.0f;
	m_rect[0].x = (float)_params.ulx * (2.0f * scaleX) - 1.0f;
	m_rect[0].y = (float)_params.uly * (-2.0f * scaleY) + 1.0f;
	m_rect[0].z = Z;
	m_rect[0].w = W;
	m_rect[1].x = (float)(_params.lrx) * (2.0f * scaleX) - 1.0f;
	m_rect[1].y = m_rect[0].y;
	m_rect[1].z = Z;
	m_rect[1].w = W;
	m_rect[2].x = m_rect[0].x;
	m_rect[2].y = (float)(_params.lry) * (-2.0f * scaleY) + 1.0f;
	m_rect[2].z = Z;
	m_rect[2].w = W;
	m_rect[3].x = m_rect[1].x;
	m_rect[3].y = m_rect[2].y;
	m_rect[3].z = Z;
	m_rect[3].w = W;

	TextureCache & cache = textureCache();
	struct
	{
		float s0, t0, s1, t1;
	} texST[2] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }; //struct for texture coordinates

	for (u32 t = 0; t < 2; ++t) {
		if (currentCombiner()->usesTile(t) && cache.current[t] && gSP.textureTile[t]) {
			f32 shiftScaleS = 1.0f;
			f32 shiftScaleT = 1.0f;
			getTextureShiftScale(t, cache, shiftScaleS, shiftScaleT);
			if (_params.uls > _params.lrs) {
				texST[t].s0 = (_params.uls + _params.dsdx) * shiftScaleS - gSP.textureTile[t]->fuls;
				texST[t].s1 = _params.lrs * shiftScaleS - gSP.textureTile[t]->fuls;
			} else {
				texST[t].s0 = _params.uls * shiftScaleS - gSP.textureTile[t]->fuls;
				texST[t].s1 = (_params.lrs + _params.dsdx) * shiftScaleS - gSP.textureTile[t]->fuls;
			}
			if (_params.ult > _params.lrt) {
				texST[t].t0 = (_params.ult + _params.dtdy) * shiftScaleT - gSP.textureTile[t]->fult;
				texST[t].t1 = _params.lrt * shiftScaleT - gSP.textureTile[t]->fult;
			} else {
				texST[t].t0 = _params.ult * shiftScaleT - gSP.textureTile[t]->fult;
				texST[t].t1 = (_params.lrt + _params.dtdy) * shiftScaleT - gSP.textureTile[t]->fult;
			}

			if (cache.current[t]->frameBufferTexture != CachedTexture::fbNone) {
				texST[t].s0 = cache.current[t]->offsetS + texST[t].s0;
				texST[t].t0 = cache.current[t]->offsetT - texST[t].t0;
				texST[t].s1 = cache.current[t]->offsetS + texST[t].s1;
				texST[t].t1 = cache.current[t]->offsetT - texST[t].t1;
			}

			glActiveTexture(GL_TEXTURE0 + t);

			if ((cache.current[t]->mirrorS == 0 && cache.current[t]->maskS == 0 &&
				(texST[t].s0 < texST[t].s1 ?
				texST[t].s0 >= 0.0 && texST[t].s1 <= (float)cache.current[t]->width :
				texST[t].s1 >= 0.0 && texST[t].s0 <= (float)cache.current[t]->width))
				|| (cache.current[t]->maskS == 0 && (texST[t].s0 < -1024.0f || texST[t].s1 > 1023.99f)))
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

			if (cache.current[t]->mirrorT == 0 &&
				(texST[t].t0 < texST[t].t1 ?
				texST[t].t0 >= 0.0f && texST[t].t1 <= (float)cache.current[t]->height :
				texST[t].t1 >= 0.0f && texST[t].t0 <= (float)cache.current[t]->height))
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			texST[t].s0 *= cache.current[t]->scaleS;
			texST[t].t0 *= cache.current[t]->scaleT;
			texST[t].s1 *= cache.current[t]->scaleS;
			texST[t].t1 *= cache.current[t]->scaleT;
		}
	}

	if (gDP.otherMode.cycleType == G_CYC_COPY) {
		glActiveTexture( GL_TEXTURE0 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	}

	m_rect[0].s0 = texST[0].s0;
	m_rect[0].t0 = texST[0].t0;
	m_rect[0].s1 = texST[1].s0;
	m_rect[0].t1 = texST[1].t0;

	m_rect[3].s0 = texST[0].s1;
	m_rect[3].t0 = texST[0].t1;
	m_rect[3].s1 = texST[1].s1;
	m_rect[3].t1 = texST[1].t1;

	if (_params.flip) {
		m_rect[1].s0 = texST[0].s0;
		m_rect[1].t0 = texST[0].t1;
		m_rect[1].s1 = texST[1].s0;
		m_rect[1].t1 = texST[1].t1;

		m_rect[2].s0 = texST[0].s1;
		m_rect[2].t0 = texST[0].t0;
		m_rect[2].s1 = texST[1].s1;
		m_rect[2].t1 = texST[1].t0;
	} else {
		m_rect[1].s0 = texST[0].s1;
		m_rect[1].t0 = texST[0].t0;
		m_rect[1].s1 = texST[1].s1;
		m_rect[1].t1 = texST[1].t0;

		m_rect[2].s0 = texST[0].s0;
		m_rect[2].t0 = texST[0].t1;
		m_rect[2].s1 = texST[1].s0;
		m_rect[2].t1 = texST[1].t1;
	}

	if (ogl.isAdjustScreen() &&
		(_params.forceAjustScale ||
		((gDP.colorImage.width > VI.width * 98 / 100) && (_params.lrx - _params.ulx < VI.width * 9 / 10))))
	{
		const float scale = ogl.getAdjustScale();
		for (u32 i = 0; i < 4; ++i)
			m_rect[i].x *= scale;
	}

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gSP.changed |= CHANGED_GEOMETRYMODE | CHANGED_VIEWPORT;
}

void OGLRender::correctTexturedRectParams(TexturedRectParams & _params)
{
    if (config.generalEmulation.correctTexrectCoords == Config::tcSmart) {
        if (_params.ulx == m_texrectParams.ulx && _params.lrx == m_texrectParams.lrx) {
            if (fabsf(_params.uly - m_texrectParams.lry) < 0.51f)
                _params.uly = m_texrectParams.lry;
            else if (fabsf(_params.lry - m_texrectParams.uly) < 0.51f)
                _params.lry = m_texrectParams.uly;
        } else if (_params.uly == m_texrectParams.uly && _params.lry == m_texrectParams.lry) {
            if (fabsf(_params.ulx - m_texrectParams.lrx) < 0.51f)
                _params.ulx = m_texrectParams.lrx;
            else if (fabsf(_params.lrx - m_texrectParams.ulx) < 0.51f)
                _params.lrx = m_texrectParams.ulx;
        }
    } else if (config.generalEmulation.correctTexrectCoords == Config::tcForce) {
        _params.lrx += 0.25f;
        _params.lry += 0.25f;
    }

    m_texrectParams = _params;
}

void OGLRender::drawText(const char *_pText, float x, float y)
{
	m_renderState = rsNone;
	TextDrawer::get().renderText(_pText, x, y);
}

void OGLRender::clearDepthBuffer(u32 _uly, u32 _lry)
{
	if (!_canDraw())
		return;

	depthBufferList().clearBuffer(_uly, _lry);

	glDisable( GL_SCISSOR_TEST );
	glDepthMask( TRUE );
	glClear( GL_DEPTH_BUFFER_BIT );

	_updateDepthUpdate();

	glEnable( GL_SCISSOR_TEST );
}

void OGLRender::clearColorBuffer(float *_pColor )
{
	glDisable(GL_SCISSOR_TEST);

	if (_pColor != nullptr)
		glClearColor( _pColor[0], _pColor[1], _pColor[2], _pColor[3] );
	else
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	glEnable( GL_SCISSOR_TEST );
}

FBOTextureFormats fboFormats;

void FBOTextureFormats::init()
{
#ifdef GLES2
	monochromeInternalFormat = GL_RGB;
	monochromeFormat = GL_RGB;
	monochromeType = GL_UNSIGNED_SHORT_5_6_5;
	monochromeFormatBytes = 2;

	depthInternalFormat = GL_DEPTH_COMPONENT;
	depthFormat = GL_DEPTH_COMPONENT;
	depthType = GL_UNSIGNED_INT;
	depthFormatBytes = 4;

	if (OGLVideo::isExtensionSupported("GL_OES_rgb8_rgba8")) {
		colorInternalFormat = GL_RGBA;
		colorFormat = GL_RGBA;
		colorType = GL_UNSIGNED_BYTE;
		colorFormatBytes = 4;
	} else {
		colorInternalFormat = GL_RGB;
		colorFormat = GL_RGB;
		colorType = GL_UNSIGNED_SHORT_5_6_5;
		colorFormatBytes = 2;
	}
#elif defined(GLES3) || defined (GLES3_1)
	colorInternalFormat = GL_RGBA;
	colorFormat = GL_RGBA;
	colorType = GL_UNSIGNED_BYTE;
	colorFormatBytes = 4;

	monochromeInternalFormat = GL_RED;
	monochromeFormat = GL_RED;
	monochromeType = GL_UNSIGNED_BYTE;
	monochromeFormatBytes = 1;

	depthInternalFormat = GL_DEPTH_COMPONENT32F;
	depthFormat = GL_DEPTH_COMPONENT;
	depthType = GL_FLOAT;
	depthFormatBytes = 4;

	depthImageInternalFormat = GL_RGBA32F;
	depthImageFormat = GL_RGBA;
	depthImageType = GL_FLOAT;
	depthImageFormatBytes = 16;

	lutInternalFormat = GL_R32UI;
	lutFormat = GL_RED;
	lutType = GL_UNSIGNED_INT;
	lutFormatBytes = 4;
#else
	colorInternalFormat = GL_RGBA;
	colorFormat = GL_RGBA;
	colorType = GL_UNSIGNED_BYTE;
	colorFormatBytes = 4;

	monochromeInternalFormat = GL_RED;
	monochromeFormat = GL_RED;
	monochromeType = GL_UNSIGNED_BYTE;
	monochromeFormatBytes = 1;

	depthInternalFormat = GL_DEPTH_COMPONENT;
	depthFormat = GL_DEPTH_COMPONENT;
	depthType = GL_FLOAT;
	depthFormatBytes = 4;

	depthImageInternalFormat = GL_RG32F;
	depthImageFormat = GL_RG;
	depthImageType = GL_FLOAT;
	depthImageFormatBytes = 8;

	lutInternalFormat = GL_R16;
	lutFormat = GL_RED;
	lutType = GL_UNSIGNED_SHORT;
	lutFormatBytes = 2;

#endif
}

void OGLRender::_initExtensions()
{
	LOG(LOG_VERBOSE, "OpenGL version string: %s\n", glGetString(GL_VERSION));
	LOG(LOG_VERBOSE, "OpenGL vendor: %s\n", glGetString(GL_VENDOR));
	const GLubyte * strRenderer = glGetString(GL_RENDERER);
	if (strstr((const char*)strRenderer, "Adreno") != NULL)
		m_oglRenderer = glrAdreno;
	LOG(LOG_VERBOSE, "OpenGL renderer: %s\n", strRenderer);

	fboFormats.init();

#ifndef GLES2
	GLint majorVersion = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
	LOG(LOG_VERBOSE, "OpenGL major version: %d\n", majorVersion);
	assert(majorVersion >= 3 && "Plugin requires GL version 3 or higher.");
#endif

#ifdef GL_IMAGE_TEXTURES_SUPPORT
	GLint minorVersion = 0;
	glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
	LOG(LOG_VERBOSE, "OpenGL minor version: %d\n", minorVersion);
#ifndef GLESX
	m_bImageTexture = (majorVersion >= 4) && (minorVersion >= 3) && (glBindImageTexture != NULL);
#elif defined(GLES3_1)
	m_bImageTexture = (majorVersion >= 3) && (minorVersion >= 1) && (glBindImageTexture != NULL);
#else
	m_bImageTexture = false;
#endif
#else
	m_bImageTexture = false;
#endif
	LOG(LOG_VERBOSE, "ImageTexture support: %s\n", m_bImageTexture ? "yes" : "no");
	if (!m_bImageTexture)
		LOG(LOG_WARNING, "N64 depth compare and depth based fog will not work without Image Textures support provided in OpenGL >= 4.3 or GLES >= 3.1");

	if (config.texture.maxAnisotropy != 0 && OGLVideo::isExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &config.texture.maxAnisotropyF);
		config.texture.maxAnisotropyF = min(config.texture.maxAnisotropyF, (f32)config.texture.maxAnisotropy);
	} else
		config.texture.maxAnisotropyF = 0.0f;
	LOG(LOG_VERBOSE, "Max Anisotropy: %f\n", config.texture.maxAnisotropyF);
}

void OGLRender::_initStates()
{
	glDisable(GL_CULL_FACE);
	glEnableVertexAttribArray(SC_POSITION);
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_ALWAYS );
	glDepthMask( GL_FALSE );
	glEnable( GL_SCISSOR_TEST );

	if (config.frameBufferEmulation.N64DepthCompare != 0) {
		glDisable( GL_DEPTH_TEST );
		glDisable( GL_POLYGON_OFFSET_FILL );
		glDepthFunc( GL_ALWAYS );
		glDepthMask( FALSE );
	} else {
#ifdef ANDROID
		if(config.generalEmulation.forcePolygonOffset != 0)
			glPolygonOffset(config.generalEmulation.polygonOffsetFactor, config.generalEmulation.polygonOffsetUnits);
		else
#endif
			glPolygonOffset(-3.0f, -3.0f);
	}

	OGLVideo & ogl = video();
	glViewport(0, ogl.getHeightOffset(), ogl.getScreenWidth(), ogl.getScreenHeight());

	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	srand( time(NULL) );

	ogl.swapBuffers();
}

void OGLRender::_initData()
{
	glState.reset();
	_initExtensions();
	_initStates();
	_setSpecialTexrect();

	textureCache().init();
	DepthBuffer_Init();
	FrameBuffer_Init();
	Combiner_Init();
	TextDrawer::get().init();
	TFH.init();
	PostProcessor::get().init();
	FBInfo::fbInfo.reset();
	m_renderState = rsNone;

	gSP.changed = gDP.changed = 0xFFFFFFFF;

	memset(triangles.vertices, 0, VERTBUFF_SIZE * sizeof(SPVertex));
	memset(triangles.elements, 0, ELEMBUFF_SIZE * sizeof(GLubyte));
	for (u32 i = 0; i < VERTBUFF_SIZE; ++i)
		triangles.vertices[i].w = 1.0f;
	triangles.num = 0;
}

void OGLRender::_destroyData()
{
	m_renderState = rsNone;
	if (config.bloomFilter.enable != 0)
		PostProcessor::get().destroy();
	if (TFH.optionsChanged())
		TFH.shutdown();
	TextDrawer::get().destroy();
	Combiner_Destroy();
	FrameBuffer_Destroy();
	DepthBuffer_Destroy();
	textureCache().destroy();
}

void OGLRender::_setSpecialTexrect() const
{
	const char * name = RSP.romname;
	if (strstr(name, (const char *)"Beetle") || strstr(name, (const char *)"BEETLE") || strstr(name, (const char *)"HSV")
		|| strstr(name, (const char *)"DUCK DODGERS") || strstr(name, (const char *)"DAFFY DUCK"))
		texturedRectSpecial = texturedRectShadowMap;
	else if (strstr(name, (const char *)"Perfect Dark") || strstr(name, (const char *)"PERFECT DARK"))
		texturedRectSpecial = texturedRectDepthBufferCopy; // See comments to that function!
	else if (strstr(name, (const char *)"CONKER BFD"))
		texturedRectSpecial = texturedRectCopyToItself;
	else if (strstr(name, (const char *)"YOSHI STORY"))
		texturedRectSpecial = texturedRectBGCopy;
	else if (strstr(name, (const char *)"PAPER MARIO") || strstr(name, (const char *)"MARIO STORY"))
		texturedRectSpecial = texturedRectPaletteMod;
	else if (strstr(name, (const char *)"ZELDA"))
		texturedRectSpecial = texturedRectMonochromeBackground;
	else
		texturedRectSpecial = NULL;
}

static
u32 textureFilters[] = {
	NO_FILTER, //"None"
	SMOOTH_FILTER_1, //"Smooth filtering 1"
	SMOOTH_FILTER_2, //"Smooth filtering 2"
	SMOOTH_FILTER_3, //"Smooth filtering 3"
	SMOOTH_FILTER_4, //"Smooth filtering 4"
	SHARP_FILTER_1,  //"Sharp filtering 1"
	SHARP_FILTER_2,  //"Sharp filtering 2"
};

static
u32 textureEnhancements[] = {
	NO_ENHANCEMENT,    //"None"
	NO_ENHANCEMENT,    //"Store"
	X2_ENHANCEMENT,    //"X2"
	X2SAI_ENHANCEMENT, //"X2SAI"
	HQ2X_ENHANCEMENT,  //"HQ2X"
	HQ2XS_ENHANCEMENT, //"HQ2XS"
	LQ2X_ENHANCEMENT,  //"LQ2X"
	LQ2XS_ENHANCEMENT, //"LQ2XS"
	HQ4X_ENHANCEMENT,  //"HQ4X"
	BRZ2X_ENHANCEMENT, //"2XBRZ"
	BRZ3X_ENHANCEMENT, //"3XBRZ"
	BRZ4X_ENHANCEMENT, //"4XBRZ"
	BRZ5X_ENHANCEMENT, //"5XBRZ"
	BRZ6X_ENHANCEMENT  //"6XBRZ"
};

void displayLoadProgress(const wchar_t *format, ...)
{
	va_list args;
	wchar_t wbuf[INFO_BUF];
	char buf[INFO_BUF];

	// process input
#ifdef ANDROID
	const u32 bufSize = 2048;
	char cbuf[bufSize];
	char fmt[bufSize];
	wcstombs(fmt, format, bufSize);
	va_start(args, format);
	vsprintf(cbuf, fmt, args);
	va_end(args);
	mbstowcs(wbuf, cbuf, INFO_BUF);
#else
	va_start(args, format);
	vswprintf(wbuf, INFO_BUF, format, args);
	va_end(args);
#endif

	// XXX: convert to multibyte
	wcstombs(buf, wbuf, INFO_BUF);

	FrameBuffer* pBuffer = frameBufferList().getCurrent();
	if (pBuffer != NULL)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	OGLRender & render = video().getRender();
	render.clearColorBuffer(nullptr);
	render.drawText(buf, -0.9f, 0);
	video().swapBuffers();

	if (pBuffer != NULL)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pBuffer->m_FBO);
}

u32 TextureFilterHandler::_getConfigOptions() const
{
	u32 options = textureFilters[config.textureFilter.txFilterMode] | textureEnhancements[config.textureFilter.txEnhancementMode];
	if (config.textureFilter.txHiresEnable)
		options |= RICE_HIRESTEXTURES;
	if (config.textureFilter.txForce16bpp)
		options |= FORCE16BPP_TEX | FORCE16BPP_HIRESTEX;
	if (config.textureFilter.txCacheCompression)
		options |= GZ_TEXCACHE | GZ_HIRESTEXCACHE;
	if (config.textureFilter.txSaveCache)
		options |= (DUMP_TEXCACHE | DUMP_HIRESTEXCACHE);
	if (config.textureFilter.txHiresFullAlphaChannel)
		options |= LET_TEXARTISTS_FLY;
	if (config.textureFilter.txDump)
		options |= DUMP_TEX;
	if (config.textureFilter.txDeposterize)
		options |= DEPOSTERIZE;
	return options;
}

void TextureFilterHandler::init()
{
	if (isInited())
		return;

	m_inited = config.textureFilter.txFilterMode | config.textureFilter.txEnhancementMode | config.textureFilter.txHiresEnable;
	if (m_inited == 0)
		return;

	m_options = _getConfigOptions();

	GLint maxTextureSize;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	wchar_t wRomName[32];
	::mbstowcs(wRomName, RSP.romname, 32);
	wchar_t txPath[PLUGIN_PATH_SIZE + 16];
	wchar_t * pTexPackPath = config.textureFilter.txPath;
	if (::wcslen(config.textureFilter.txPath) == 0) {
		api().GetUserDataPath(txPath);
		gln_wcscat(txPath, wst("/hires_texture"));
		pTexPackPath = txPath;
	}
	wchar_t txCachePath[PLUGIN_PATH_SIZE];
	api().GetUserCachePath(txCachePath);

	m_inited = txfilter_init(maxTextureSize, // max texture width supported by hardware
		maxTextureSize, // max texture height supported by hardware
		32, // max texture bpp supported by hardware
		m_options,
		config.textureFilter.txCacheSize, // cache texture to system memory
		txCachePath, // path to store cache files
		pTexPackPath, // path to texture packs folder
		wRomName, // name of ROM. must be no longer than 256 characters
		displayLoadProgress);

}

void TextureFilterHandler::shutdown()
{
	if (isInited()) {
		txfilter_shutdown();
		m_inited = m_options = 0;
	}
}

TextureFilterHandler TFH;
