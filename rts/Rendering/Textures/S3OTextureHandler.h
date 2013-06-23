/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef S3O_TEXTURE_HANDLER_H
#define S3O_TEXTURE_HANDLER_H

#include <map>
#include <string>
#include <vector>
#include "Rendering/GL/myGL.h"
#include "System/Platform/Threading.h"

struct TexFile;
struct S3DModel;

class CS3OTextureHandler
{
public:
	struct S3oTex {
		int num;
		GLuint tex1;
		unsigned int tex1SizeX;
		unsigned int tex1SizeY;
		GLuint tex2;
		unsigned int tex2SizeX;
		unsigned int tex2SizeY;
	};

	CS3OTextureHandler();
	virtual ~CS3OTextureHandler();

	void LoadS3OTexture(S3DModel* model);
	int LoadS3OTextureNow(const S3DModel* model);
	void SetS3oTexture(int num);

private:
	inline void DoUpdateDraw() {
	}
	static const S3oTex* DoGetS3oTex(int num, std::vector<S3oTex *>& s3oTex) {
		if ((num < 0) || (num >= (int)s3oTex.size())) {
			return NULL;
		}
		return s3oTex[num];
	}

public:
	const S3oTex* GetS3oTex(int num) {
		return DoGetS3oTex(num, s3oTextures);
	}

	void UpdateDraw();

private:
	std::map<std::string, int> s3oTextureNames;
	std::vector<S3oTex *> s3oTextures;
	std::vector<S3oTex *> s3oTexturesDraw;
};

extern CS3OTextureHandler* texturehandlerS3O;

#endif /* S3O_TEXTURE_HANDLER_H */
