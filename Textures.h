#ifndef TEXTURES_H
#define TEXTURES_H

#ifdef ANDROID
#include <GLES2/gl2.h>
#elif defined (OS_MAC_OS_X)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif //ANDROID

#include <map>

#include "CRC.h"
#include "convert.h"

struct CachedTexture
{
	CachedTexture(GLuint _glName) : glName(_glName), max_level(0) {}

	GLuint	glName;
	u32		crc;
//	float	fulS, fulT;
//	WORD	ulS, ulT, lrS, lrT;
	float	offsetS, offsetT;
	u8		maskS, maskT;
	u8		clampS, clampT;
	u8		mirrorS, mirrorT;
	u16		line;
	u16		size;
	u16		format;
	u32		tMem;
	u32		palette;
	u16		width, height;			  // N64 width and height
	u16		clampWidth, clampHeight;  // Size to clamp to
	u16		realWidth, realHeight;	  // Actual texture size
	f32		scaleS, scaleT;			  // Scale to map to 0.0-1.0
	f32		shiftScaleS, shiftScaleT; // Scale to shift
	u32		textureBytes;

	u32		lastDList;
	u32		address;
	u8		max_level;
	u8		frameBufferTexture;
};


struct TextureCache
{
	CachedTexture * current[2];

	void init();
	void destroy();
	CachedTexture * addFrameBufferTexture();
	void addFrameBufferTextureSize(u32 _size) {m_cachedBytes += _size;}
	void removeFrameBufferTexture(CachedTexture * _pTexture);
	void activateTexture(u32 _t, CachedTexture *_pTexture);
	void activateDummy(u32 _t);
	void update(u32 _t);

	static TextureCache & get() {
		static TextureCache cache;
		return cache;
	}

private:
	TextureCache() : m_pDummy(NULL), m_hits(0), m_misses(0), m_maxBytes(0), m_cachedBytes(0)
	{
		current[0] = NULL;
		current[1] = NULL;
		CRC_BuildTable();
	}
	TextureCache(const TextureCache &);

	void _checkCacheSize();
	CachedTexture * _addTexture(u32 _crc32);
	void _setTextureParameters(CachedTexture *_pTexture);
	void _load(u32 _tile, CachedTexture *_pTexture);
	bool _loadHiresTexture(u32 _tile, CachedTexture *_pTexture);
	void _loadBackground(CachedTexture *pTexture);
	bool _loadHiresBackground(CachedTexture *_pTexture);
	void _updateBackground();

	typedef std::map<u32, CachedTexture> Textures;
	Textures m_textures;
	Textures m_fbTextures;
	CachedTexture * m_pDummy;
	u32 m_hits, m_misses;
	u32 m_maxBytes;
	u32 m_cachedBytes;
};

inline TextureCache & textureCache()
{
	return TextureCache::get();
}

inline u32 pow2( u32 dim )
{
	u32 i = 1;

	while (i < dim) i <<= 1;

	return i;
}

inline u32 powof( u32 dim )
{
	u32 num = 1;
	u32 i = 0;

	while (num < dim)
	{
		num <<= 1;
		i++;
	}

	return i;
}
#endif
