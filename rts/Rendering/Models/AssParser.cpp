/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "AssParser.h"

#include "3DModel.h"
#include "3DModelLog.h"
#include "S3OParser.h"
#include "AssIO.h"

#include "Lua/LuaParser.h"
#include "Sim/Misc/CollisionVolume.h"
#include "System/Util.h"
#include "System/Log/ILog.h"
#include "System/Platform/errorhandler.h"
#include "System/Exceptions.h"
#include "System/ScopedFPUSettings.h"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileSystem.h"

#include "lib/assimp/include/assimp/config.h"
#include "lib/assimp/include/assimp/defs.h"
#include "lib/assimp/include/assimp/types.h"
#include "lib/assimp/include/assimp/scene.h"
#include "lib/assimp/include/assimp/postprocess.h"
#include "lib/assimp/include/assimp/Importer.hpp"
#include "lib/assimp/include/assimp/DefaultLogger.hpp"
#include "Rendering/Textures/S3OTextureHandler.h"
#ifndef BITMAP_NO_OPENGL
	#include "Rendering/GL/myGL.h"
#endif


#define IS_QNAN(f) (f != f)
static const float DEGTORAD = PI / 180.0f;
static const float RADTODEG = 180.0f / PI;

// triangulate guarantees the most complex mesh is a triangle
// sortbytype ensure only 1 type of primitive type per mesh is used
static const unsigned int ASS_POSTPROCESS_OPTIONS =
	  aiProcess_RemoveComponent
	| aiProcess_FindInvalidData
	| aiProcess_CalcTangentSpace
	| aiProcess_GenSmoothNormals
	| aiProcess_Triangulate
	| aiProcess_GenUVCoords
	| aiProcess_SortByPType
	| aiProcess_JoinIdenticalVertices
	//| aiProcess_ImproveCacheLocality // FIXME crashes in an assert in VertexTriangleAdjancency.h (date 04/2011)
	| aiProcess_SplitLargeMeshes;

static const unsigned int ASS_IMPORTER_OPTIONS =
	aiComponent_CAMERAS |
	aiComponent_LIGHTS |
	aiComponent_TEXTURES |
	aiComponent_ANIMATIONS;
static const unsigned int ASS_LOGGING_OPTIONS =
	Assimp::Logger::Debugging |
	Assimp::Logger::Info |
	Assimp::Logger::Err |
	Assimp::Logger::Warn;



static inline float3 aiVectorToFloat3(const aiVector3D v)
{
	// default (AssImp's internal coordinate-system matches Spring's!)
	return float3(v.x, v.y, v.z);

	// Blender --> Spring
	// return float3(v.x, v.z, -v.y);
}

static inline CMatrix44f aiMatrixToMatrix(const aiMatrix4x4t<float>& m)
{
	CMatrix44f n;

	// a{1, ..., 4} represent the first column of an AssImp matrix
	// b{1, ..., 4} represent the second column of an AssImp matrix
	// AssImp matrix data (colums) is transposed wrt. Spring's data
	n[ 0] = m.a1; n[ 1] = m.a2; n[ 2] = m.a3; n[ 3] = m.a4;
	n[ 4] = m.b1; n[ 5] = m.b2; n[ 6] = m.b3; n[ 7] = m.b4;
	n[ 8] = m.c1; n[ 9] = m.c2; n[10] = m.c3; n[11] = m.c4;
	n[12] = m.d1; n[13] = m.d2; n[14] = m.d3; n[15] = m.d4;

	// AssImp (row-major) --> Spring (column-major)
	return (n.Transpose());

	// default
	// return (CMatrix44f(n.GetPos(), n.GetX(), n.GetY(), n.GetZ()));

	// Blender --> Spring
	// return (CMatrix44f(n.GetPos(), n.GetX(), n.GetZ(), -n.GetY()));
}

static float3 aiQuaternionToRadianAngles(const aiQuaternion q1)
{
	const float sqw = q1.w * q1.w;
	const float sqx = q1.x * q1.x;
	const float sqy = q1.y * q1.y;
	const float sqz = q1.z * q1.z;
	// <unit> is 1 if normalised, otherwise correction factor
	const float unit = sqx + sqy + sqz + sqw;
	const float test = q1.x * q1.y + q1.z * q1.w;

	aiVector3D angles;

	if (test > (0.499f * unit)) {
		// singularity at north pole
		angles.x = 2.0f * math::atan2(q1.x, q1.w);
		angles.y = PI * 0.5f;
	} else if (test < (-0.499f * unit)) {
		// singularity at south pole
		angles.x = -2.0f * math::atan2(q1.x, q1.w);
		angles.y = -PI * 0.5f;
	} else {
		angles.x = math::atan2(2.0f * q1.y * q1.w - 2.0f * q1.x * q1.z,  sqx - sqy - sqz + sqw);
		angles.y = math::asin((2.0f * test) / unit);
		angles.z = math::atan2(2.0f * q1.x * q1.w - 2.0f * q1.y * q1.z, -sqx + sqy - sqz + sqw);
	}

	return (aiVectorToFloat3(angles));
}





class AssLogStream : public Assimp::LogStream
{
public:
	void write(const char* message) {
		LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "Assimp: %s", message);
	}
};



S3DModel* CAssParser::Load(const std::string& modelFilePath)
{
	LOG_S(LOG_SECTION_MODEL, "Loading model: %s", modelFilePath.c_str());

	const std::string& modelPath = FileSystem::GetDirectory(modelFilePath);
	const std::string& modelName = FileSystem::GetBasename(modelFilePath);

	// Load the lua metafile. This contains properties unique to Spring models and must return a table
	std::string metaFileName = modelFilePath + ".lua";

	if (!CFileHandler::FileExists(metaFileName, SPRING_VFS_ZIP)) {
		// Try again without the model file extension
		metaFileName = modelPath + '/' + modelName + ".lua";
	}
	if (!CFileHandler::FileExists(metaFileName, SPRING_VFS_ZIP)) {
		LOG_S(LOG_SECTION_MODEL, "No meta-file '%s'. Using defaults.", metaFileName.c_str());
	}

	LuaParser metaFileParser(metaFileName, SPRING_VFS_MOD_BASE, SPRING_VFS_ZIP);

	if (!metaFileParser.Execute()) {
		LOG_SL(LOG_SECTION_MODEL, L_ERROR, "'%s': %s. Using defaults.", metaFileName.c_str(), metaFileParser.GetErrorLog().c_str());
	}

	// Get the (root-level) model table
	const LuaTable& modelTable = metaFileParser.GetRoot();

	if (!modelTable.IsValid()) {
		LOG_S(LOG_SECTION_MODEL, "No valid model metadata in '%s' or no meta-file", metaFileName.c_str());
	}


	// Create a model importer instance
	Assimp::Importer importer;

	// Create a logger for debugging model loading issues
	Assimp::DefaultLogger::create("", Assimp::Logger::VERBOSE);
	Assimp::DefaultLogger::get()->attachStream(new AssLogStream(), ASS_LOGGING_OPTIONS);

	// Give the importer an IO class that handles Spring's VFS
	importer.SetIOHandler(new AssVFSSystem());
	// Speed-up processing by skipping things we don't need
	importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, ASS_IMPORTER_OPTIONS);

#ifndef BITMAP_NO_OPENGL
	{
		// Optimize VBO-Mesh sizes/ranges
		GLint maxIndices  = 1024;
		GLint maxVertices = 1024;
		// FIXME returns non-optimal data, at best compute it ourselves (pre-TL cache size!)
		glGetIntegerv(GL_MAX_ELEMENTS_INDICES,  &maxIndices);
		glGetIntegerv(GL_MAX_ELEMENTS_VERTICES, &maxVertices);
		importer.SetPropertyInteger(AI_CONFIG_PP_SLM_VERTEX_LIMIT,   maxVertices);
		importer.SetPropertyInteger(AI_CONFIG_PP_SLM_TRIANGLE_LIMIT, maxIndices / 3);
	}
#endif

	// Read the model file to build a scene object
	LOG_S(LOG_SECTION_MODEL, "Importing model file: %s", modelFilePath.c_str());

	const aiScene* scene;
	{
		// ASSIMP spams many SIGFPEs atm in normal & tangent generation
		ScopedDisableFpuExceptions fe;
		scene = importer.ReadFile(modelFilePath, ASS_POSTPROCESS_OPTIONS);
	}

	if (scene != NULL) {
		LOG_S(LOG_SECTION_MODEL,
			"Processing scene for model: %s (%d meshes / %d materials / %d textures)",
			modelFilePath.c_str(), scene->mNumMeshes, scene->mNumMaterials,
			scene->mNumTextures);
	} else {
		throw content_error("[AssimpParser] Model Import: " + std::string(importer.GetErrorString()));
	}

	S3DModel* model = new S3DModel();
	model->name = modelFilePath;
	model->type = MODELTYPE_ASS;

	// Load textures
	FindTextures(model, scene, modelTable, modelPath, modelName);
	LOG_S(LOG_SECTION_MODEL, "Loading textures. Tex1: '%s' Tex2: '%s'", model->tex1.c_str(), model->tex2.c_str());
	texturehandlerS3O->LoadS3OTexture(model);

	// Load all pieces in the model
	LOG_S(LOG_SECTION_MODEL, "Loading pieces from root node '%s'", scene->mRootNode->mName.data);
	LoadPiece(model, scene->mRootNode, scene, modelTable);

	// Update piece hierarchy based on metadata
	BuildPieceHierarchy(model);
	CalculateModelProperties(model, modelTable);

	// Verbose logging of model properties
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->name: %s", model->name.c_str());
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->numobjects: %d", model->numPieces);
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->radius: %f", model->radius);
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->height: %f", model->height);
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->drawRadius: %f", model->drawRadius);
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->mins: (%f,%f,%f)", model->mins[0], model->mins[1], model->mins[2]);
	LOG_SL(LOG_SECTION_MODEL, L_DEBUG, "model->maxs: (%f,%f,%f)", model->maxs[0], model->maxs[1], model->maxs[2]);
	LOG_S(LOG_SECTION_MODEL, "Model %s Imported.", model->name.c_str());
	return model;
}


/*
void CAssParser::CalculateModelMeshBounds(S3DModel* model, const aiScene* scene)
{
	model->meshBounds.resize(scene->mNumMeshes * 2);

	// calculate bounds for each individual mesh of
	// the model; currently we have no use for this
	// and S3DModel has only one pair of bounds
	//
	for (size_t i = 0; i < scene->mNumMeshes; i++) {
		const aiMesh* mesh = scene->mMeshes[i];

		float3& mins = model->meshBounds[i*2 + 0];
		float3& maxs = model->meshBounds[i*2 + 1];

		mins = DEF_MIN_SIZE;
		maxs = DEF_MAX_SIZE;

		for (size_t vertexIndex= 0; vertexIndex < mesh->mNumVertices; vertexIndex++) {
			const aiVector3D& aiVertex = mesh->mVertices[vertexIndex];
			mins = std::min(mins, aiVectorToFloat3(aiVertex));
			maxs = std::max(maxs, aiVectorToFloat3(aiVertex));
		}

		if (mins == DEF_MIN_SIZE) { mins = ZeroVector; }
		if (maxs == DEF_MAX_SIZE) { maxs = ZeroVector; }
	}
}
*/


void CAssParser::LoadPieceTransformations(
	SAssPiece* piece,
	const S3DModel* model,
	const aiNode* pieceNode,
	const LuaTable& pieceTable
) {
	aiVector3D aiScaleVec, aiTransVec;
	aiQuaternion aiRotateQuat;

	// process transforms
	pieceNode->mTransformation.Decompose(aiScaleVec, aiRotateQuat, aiTransVec);

	// metadata-scaling
	piece->scales   = pieceTable.GetFloat3("scale", aiVectorToFloat3(aiScaleVec));
	piece->scales.x = pieceTable.GetFloat("scalex", piece->scales.x);
	piece->scales.y = pieceTable.GetFloat("scaley", piece->scales.y);
	piece->scales.z = pieceTable.GetFloat("scalez", piece->scales.z);

	if (piece->scales.x != piece->scales.y || piece->scales.y != piece->scales.z) {
		// LOG_SL(LOG_SECTION_MODEL, L_WARNING, "Spring doesn't support non-uniform scaling");
		piece->scales.y = piece->scales.x;
		piece->scales.z = piece->scales.x;
	}

	// metadata-translation
	piece->offset   = pieceTable.GetFloat3("offset", aiVectorToFloat3(aiTransVec));
	piece->offset.x = pieceTable.GetFloat("offsetx", piece->offset.x);
	piece->offset.y = pieceTable.GetFloat("offsety", piece->offset.y);
	piece->offset.z = pieceTable.GetFloat("offsetz", piece->offset.z);

	// metadata-rotation
	// NOTE:
	//   these rotations are "pre-scripting" but "post-modelling"
	//   together with the (baked) aiRotateQuad they determine the
	//   model's pose *before* any animations execute
	//
	// float3 rotAngles = pieceTable.GetFloat3("rotate", aiQuaternionToRadianAngles(aiRotateQuat) * RADTODEG);
	float3 pieceRotAngles = pieceTable.GetFloat3("rotate", ZeroVector);

	pieceRotAngles.x = pieceTable.GetFloat("rotatex", pieceRotAngles.x);
	pieceRotAngles.y = pieceTable.GetFloat("rotatey", pieceRotAngles.y);
	pieceRotAngles.z = pieceTable.GetFloat("rotatez", pieceRotAngles.z);
	pieceRotAngles  *= DEGTORAD;

	LOG_S(LOG_SECTION_PIECE,
		"(%d:%s) Assimp offset (%f,%f,%f), rotate (%f,%f,%f,%f), scale (%f,%f,%f)",
		model->numPieces, piece->name.c_str(),
		aiTransVec.x, aiTransVec.y, aiTransVec.z,
		aiRotateQuat.w, aiRotateQuat.x, aiRotateQuat.y, aiRotateQuat.z,
		aiScaleVec.x, aiScaleVec.y, aiScaleVec.z
	);
	LOG_S(LOG_SECTION_PIECE,
		"(%d:%s) Relative offset (%f,%f,%f), rotate (%f,%f,%f), scale (%f,%f,%f)",
		model->numPieces, piece->name.c_str(),
		piece->offset.x, piece->offset.y, piece->offset.z,
		pieceRotAngles.x, pieceRotAngles.y, pieceRotAngles.z,
		piece->scales.x, piece->scales.y, piece->scales.z
	);

	// NOTE:
	//   at least collada (.dae) files generated by Blender represent
	//   a coordinate-system that differs from the "standard" formats
	//   (3DO, S3O, ...) for which existing tools at least have prior
	//   knowledge of Spring's expectations --> let the user override
	//   the ROOT rotational transform and the rotation-axis mapping
	//   used by animation scripts (but re-modelling/re-exporting is
	//   always preferred!) even though AssImp should convert models
	//   to its own system which matches that of Spring
	//
	//   .dae  : x=Rgt, y=-Fwd, z= Up, as=(-1, -1, 1), am=AXIS_XZY (if Z_UP)
	//   .dae  : x=Rgt, y=-Fwd, z= Up, as=(-1, -1, 1), am=AXIS_XZY (if Y_UP) [!?]
	//   .blend: ????
	piece->bakedRotMatrix = aiMatrixToMatrix(aiMatrix4x4t<float>(aiRotateQuat.GetMatrix()));

	if (piece == model->GetRootPiece()) {
		const float3 xaxis = pieceTable.GetFloat3("xaxis", piece->bakedRotMatrix.GetX());
		const float3 yaxis = pieceTable.GetFloat3("yaxis", piece->bakedRotMatrix.GetY());
		const float3 zaxis = pieceTable.GetFloat3("zaxis", piece->bakedRotMatrix.GetZ());

		if (math::fabs(xaxis.SqLength() - yaxis.SqLength()) < 0.01f && math::fabs(yaxis.SqLength() - zaxis.SqLength()) < 0.01f) {
			piece->bakedRotMatrix = CMatrix44f(ZeroVector, xaxis, yaxis, zaxis);
		}
	}

	piece->rotAxisSigns = pieceTable.GetFloat3("rotAxisSigns", float3(-OnesVector));
	piece->axisMapType = AxisMappingType(pieceTable.GetInt("rotAxisMap", AXIS_MAPPING_XYZ));

	// construct 'baked' part of the piece-space matrix
	// AssImp order is translate * rotate * scale * v;
	// we leave the translation and scale parts out and
	// put those in <offset> and <scales> --> transform
	// is just R instead of T * R * S
	//
	// note: for all non-AssImp models this is identity!
	//
	piece->ComposeRotation(piece->bakedRotMatrix, pieceRotAngles);
	piece->SetHasIdentityRotation(piece->bakedRotMatrix.IsIdentity() == 0);

	assert(piece->bakedRotMatrix.IsOrthoNormal() == 0);
}

bool CAssParser::SetModelRadiusAndHeight(
	S3DModel* model,
	const SAssPiece* piece,
	const aiNode* pieceNode,
	const LuaTable& pieceTable
) {
	// check if this piece is "special" (ie, used to set Spring model properties)
	// if so, extract them and then remove the piece from the hierarchy entirely
	//
	if (piece->name == "SpringHeight") {
		// set the model height to this node's Y-value (FIXME: 'y' is Assimp/Blender-specific)
		if (!pieceTable.KeyExists("height")) {
			model->height = piece->offset.y;

			LOG_S(LOG_SECTION_MODEL, "Model height of %f set by special node 'SpringHeight'", model->height);
		}

		--model->numPieces;
		delete piece;
		return true;
	}

	if (piece->name == "SpringRadius") {
		if (!pieceTable.KeyExists("midpos")) {
			CMatrix44f scaleRotMat;
			piece->ComposeTransform(scaleRotMat, ZeroVector, ZeroVector, piece->scales);

			// NOTE:
			//   this makes little sense because the "SpringRadius"
			//   piece can be placed anywhere within the hierarchy
			model->relMidPos = scaleRotMat.Mul(piece->offset);

			LOG_S(LOG_SECTION_MODEL,
				"Model midpos of (%f,%f,%f) set by special node 'SpringRadius'",
				model->relMidPos.x, model->relMidPos.y, model->relMidPos.z);
		}
		if (!pieceTable.KeyExists("radius")) {
			if (true || piece->maxs.x <= 0.00001f) {
				// scales have been set at this point
				// the Blender import script only sets the scale property [?]
				//
				// model->radius = piece->scales.Length();
				model->radius = piece->scales.x;
			} else {
				// FIXME:
				//   geometry bounds are calculated by LoadPieceGeometry
				//   which is called after SetModelRadiusAndHeight -> can
				//   not take this branch yet
				// use the transformed mesh extents (FIXME: the bounds are NOT
				// actually transformed but derived from raw vertex positions!)
				//
				// model->radius = ((piece->maxs - piece->mins) * 0.5f).Length();
				model->radius = piece->maxs.x;
			}

			LOG_S(LOG_SECTION_MODEL, "Model radius of %f set by special node 'SpringRadius'", model->radius);
		}

		--model->numPieces;
		delete piece;
		return true;
	}

	return false;
}

void CAssParser::SetPieceName(SAssPiece* piece, const S3DModel* model, const aiNode* pieceNode)
{
	assert(piece->name.empty());
	piece->name = std::string(pieceNode->mName.data);

	if (piece->name.empty()) {
		if (piece == model->GetRootPiece()) {
			// root is always the first piece created, so safe to assign this
			piece->name = "$$root$$";
			return;
		} else {
			piece->name = "$$piece$$";
		}
	}

	// find a new name if none given or if a piece with the same name already exists
	ModelPieceMap::const_iterator it = model->pieceMap.find(piece->name);

	for (unsigned int i = 0; it != model->pieceMap.end(); i++) {
		const std::string newPieceName = piece->name + IntToString(i, "%02i");

		if ((it = model->pieceMap.find(newPieceName)) == model->pieceMap.end()) {
			piece->name = newPieceName; break;
		}
	}
}

void CAssParser::SetPieceParentName(
	SAssPiece* piece,
	const S3DModel* model,
	const aiNode* pieceNode,
	const LuaTable& pieceTable
) {
	// Get parent name from metadata or model
	if (pieceTable.KeyExists("parent")) {
		piece->parentName = pieceTable.GetString("parent", "");
	} else if (pieceNode->mParent != NULL) {
		if (pieceNode->mParent->mParent != NULL) {
			piece->parentName = std::string(pieceNode->mParent->mName.data);
		} else {
			// my parent is the root (which must already exist)
			assert(model->GetRootPiece() != NULL);
			piece->parentName = model->GetRootPiece()->name;
		}
	}
}

void CAssParser::LoadPieceGeometry(SAssPiece* piece, const aiNode* pieceNode, const aiScene* scene)
{
	// Get vertex data from node meshes
	for (unsigned meshListIndex = 0; meshListIndex < pieceNode->mNumMeshes; ++meshListIndex) {
		const unsigned int meshIndex = pieceNode->mMeshes[meshListIndex];
		const aiMesh* mesh = scene->mMeshes[meshIndex];

		LOG_SL(LOG_SECTION_PIECE, L_DEBUG, "Fetching mesh %d from scene", meshIndex);
		LOG_SL(LOG_SECTION_PIECE, L_DEBUG,
			"Processing vertices for mesh %d (%d vertices)",
			meshIndex, mesh->mNumVertices);
		LOG_SL(LOG_SECTION_PIECE, L_DEBUG,
			"Normals: %s Tangents/Bitangents: %s TexCoords: %s",
			(mesh->HasNormals() ? "Y" : "N"),
			(mesh->HasTangentsAndBitangents() ? "Y" : "N"),
			(mesh->HasTextureCoords(0) ? "Y" : "N"));

		piece->vertices.reserve(piece->vertices.size() + mesh->mNumVertices);
		piece->vertexDrawIndices.reserve(piece->vertexDrawIndices.size() + mesh->mNumFaces * 3);

		std::vector<unsigned> mesh_vertex_mapping;

		// extract vertex data per mesh
		for (unsigned vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex) {
			const aiVector3D& aiVertex = mesh->mVertices[vertexIndex];

			SAssVertex vertex;

			// vertex coordinates
			vertex.pos = aiVectorToFloat3(aiVertex);

			// update piece min/max extents
			piece->mins = std::min(piece->mins, vertex.pos);
			piece->maxs = std::max(piece->maxs, vertex.pos);

			// vertex normal
			LOG_SL(LOG_SECTION_PIECE, L_DEBUG, "Fetching normal for vertex %d", vertexIndex);

			const aiVector3D& aiNormal = mesh->mNormals[vertexIndex];

			if (!IS_QNAN(aiNormal)) {
				vertex.normal = aiVectorToFloat3(aiNormal);
			}

			// vertex tangent, x is positive in texture axis
			if (mesh->HasTangentsAndBitangents()) {
				LOG_SL(LOG_SECTION_PIECE, L_DEBUG, "Fetching tangent for vertex %d", vertexIndex);

				const aiVector3D& aiTangent = mesh->mTangents[vertexIndex];
				const aiVector3D& aiBitangent = mesh->mBitangents[vertexIndex];

				vertex.sTangent = aiVectorToFloat3(aiTangent);
				vertex.tTangent = aiVectorToFloat3(aiBitangent);
			}

			// vertex tex-coords per channel
			for (unsigned int uvChanIndex = 0; uvChanIndex < NUM_MODEL_UVCHANNS; uvChanIndex++) {
				if (!mesh->HasTextureCoords(uvChanIndex))
					break;

				piece->SetNumTexCoorChannels(uvChanIndex + 1);

				vertex.texCoords[uvChanIndex].x = mesh->mTextureCoords[uvChanIndex][vertexIndex].x;
				vertex.texCoords[uvChanIndex].y = mesh->mTextureCoords[uvChanIndex][vertexIndex].y;
			}

			mesh_vertex_mapping.push_back(piece->vertices.size());
			piece->vertices.push_back(vertex);
		}

		// extract face data
		LOG_SL(LOG_SECTION_PIECE, L_DEBUG, "Processing faces for mesh %d (%d faces)", meshIndex, mesh->mNumFaces);

		/*
		 * since aiProcess_SortByPType is being used,
		 * we're sure we'll get only 1 type here,
		 * so combination check isn't needed, also
		 * anything more complex than triangles is
		 * being split thanks to aiProcess_Triangulate
		 */
		for (unsigned faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
			const aiFace& face = mesh->mFaces[faceIndex];

			// some models contain lines (mNumIndices == 2) which
			// we cannot render and they would need a 2nd drawcall)
			if (face.mNumIndices != 3)
				continue;

			for (unsigned vertexListID = 0; vertexListID < face.mNumIndices; ++vertexListID) {
				const unsigned int vertexFaceIdx = face.mIndices[vertexListID];
				const unsigned int vertexDrawIdx = mesh_vertex_mapping[vertexFaceIdx];
				piece->vertexDrawIndices.push_back(vertexDrawIdx);
			}
		}
	}

	piece->SetHasGeometryData(!piece->vertices.empty());
}

SAssPiece* CAssParser::LoadPiece(
	S3DModel* model,
	const aiNode* pieceNode,
	const aiScene* scene,
	const LuaTable& modelTable
) {
	++model->numPieces;

	SAssPiece* piece = new SAssPiece();

	if (pieceNode->mParent == NULL) {
		// set the model's root piece ASAP, needed later
		assert(pieceNode == scene->mRootNode);
		assert(model->GetRootPiece() == NULL);
		model->SetRootPiece(piece);
	}

	SetPieceName(piece, model, pieceNode);

	LOG_S(LOG_SECTION_PIECE, "Converting node '%s' to piece '%s' (%d meshes).", pieceNode->mName.data, piece->name.c_str(), pieceNode->mNumMeshes);

	// Load additional piece properties from metadata
	const LuaTable& pieceTable = modelTable.SubTable("pieces").SubTable(piece->name);

	if (pieceTable.IsValid()) {
		LOG_S(LOG_SECTION_PIECE, "Found metadata for piece '%s'", piece->name.c_str());
	}

	// Load transforms
	LoadPieceTransformations(piece, model, pieceNode, pieceTable);

	if (SetModelRadiusAndHeight(model, piece, pieceNode, pieceTable))
		return NULL;

	LoadPieceGeometry(piece, pieceNode, scene);
	SetPieceParentName(piece, model, pieceNode, pieceTable);

	// Verbose logging of piece properties
	LOG_S(LOG_SECTION_PIECE, "Loaded model piece: %s with %d meshes", piece->name.c_str(), pieceNode->mNumMeshes);
	LOG_S(LOG_SECTION_PIECE, "piece->name: %s", piece->name.c_str());
	LOG_S(LOG_SECTION_PIECE, "piece->parent: %s", piece->parentName.c_str());

	// Recursively process all child pieces
	for (unsigned int i = 0; i < pieceNode->mNumChildren; ++i) {
		LoadPiece(model, pieceNode->mChildren[i], scene, modelTable);
	}

	model->pieceMap[piece->name] = piece;
	return piece;
}


// Because of metadata overrides we don't know the true hierarchy until all pieces have been loaded
void CAssParser::BuildPieceHierarchy(S3DModel* model)
{
	// Loop through all pieces and create missing hierarchy info
	for (ModelPieceMap::const_iterator it = model->pieceMap.begin(); it != model->pieceMap.end(); ++it) {
		SAssPiece* piece = static_cast<SAssPiece*>(it->second);

		if (piece == model->GetRootPiece()) {
			assert(piece->parent == NULL);
			assert(model->GetRootPiece() == piece);
			continue;
		}

		if (!piece->parentName.empty()) {
			if ((piece->parent = model->FindPiece(piece->parentName)) == NULL) {
				LOG_SL(LOG_SECTION_PIECE, L_ERROR,
					"Missing piece '%s' declared as parent of '%s'.",
					piece->parentName.c_str(), piece->name.c_str());
			} else {
				piece->parent->children.push_back(piece);
			}

			continue;
		}

		// a piece with no named parent that isn't the root (orphan)
		// link these to the root piece if it exists (which it should)
		if ((piece->parent = model->GetRootPiece()) == NULL) {
			LOG_SL(LOG_SECTION_PIECE, L_ERROR, "Missing root piece");
		} else {
			piece->parent->children.push_back(piece);
		}
	}
}


// Iterate over the model and calculate its overall dimensions
void CAssParser::CalculateModelDimensions(S3DModel* model, S3DModelPiece* piece)
{
	CMatrix44f scaleRotMat;
	piece->ComposeTransform(scaleRotMat, ZeroVector, ZeroVector, piece->scales);

	// cannot set this until parent relations are known, so either here or in BuildPieceHierarchy()
	piece->goffset = scaleRotMat.Mul(piece->offset) + ((piece->parent != NULL)? piece->parent->goffset: ZeroVector);

	// update model min/max extents
	model->mins = std::min(piece->goffset + piece->mins, model->mins);
	model->maxs = std::max(piece->goffset + piece->maxs, model->maxs);

	piece->SetCollisionVolume(new CollisionVolume("box", piece->maxs - piece->mins, (piece->maxs + piece->mins) * 0.5f));

	// Repeat with children
	for (unsigned int i = 0; i < piece->children.size(); i++) {
		CalculateModelDimensions(model, piece->children[i]);
	}
}

// Calculate model radius from the min/max extents
void CAssParser::CalculateModelProperties(S3DModel* model, const LuaTable& modelTable)
{
	CalculateModelDimensions(model, model->rootPiece);

	// note: overrides default midpos of the SpringRadius piece
	model->relMidPos.y = (model->maxs.y + model->mins.y) * 0.5f;

	// Simplified dimensions used for rough calculations
	model->radius = modelTable.GetFloat("radius", std::max(std::fabs(model->maxs), std::fabs(model->mins)).Length());
	model->height = modelTable.GetFloat("height", model->maxs.y);
	model->relMidPos = modelTable.GetFloat3("midpos", model->relMidPos);
	model->mins = modelTable.GetFloat3("mins", model->mins);
	model->maxs = modelTable.GetFloat3("maxs", model->maxs);

	model->drawRadius = model->radius;
}


static std::string FindTexture(std::string testTextureFile, const std::string& modelPath, const std::string& fallback)
{
	if (testTextureFile.empty())
		return fallback;

	// blender denotes relative paths with "//..", remove it
	if (testTextureFile.find("//..") == 0)
		testTextureFile = testTextureFile.substr(4);

	if (CFileHandler::FileExists(testTextureFile, SPRING_VFS_ZIP_FIRST))
		return testTextureFile;

	if (CFileHandler::FileExists("unittextures/" + testTextureFile, SPRING_VFS_ZIP_FIRST))
		return "unittextures/" + testTextureFile;

	if (CFileHandler::FileExists(modelPath + testTextureFile, SPRING_VFS_ZIP_FIRST))
		return modelPath + testTextureFile;

	return fallback;
}


static std::string FindTextureByRegex(const std::string& regex_path, const std::string& regex)
{
	//FIXME instead of ".*" only check imagetypes!
	const std::vector<std::string>& files = CFileHandler::FindFiles(regex_path, regex + ".*");

	if (!files.empty()) {
		return FindTexture(FileSystem::GetFilename(files[0]), "", "");
	}
	return "";
}


void CAssParser::FindTextures(
	S3DModel* model,
	const aiScene* scene,
	const LuaTable& modelTable,
	const std::string& modelPath,
	const std::string& modelName
) {
	// Assign textures
	// The S3O texture handler uses two textures.
	// The first contains diffuse color (RGB) and teamcolor (A)
	// The second contains glow (R), reflectivity (G) and 1-bit Alpha (A).


	// 1. try to find by name (lowest prioriy)
	if (model->tex1.empty()) model->tex1 = FindTextureByRegex("unittextures/", modelName); // high priority
	if (model->tex1.empty()) model->tex1 = FindTextureByRegex("unittextures/", modelName + "1");
	if (model->tex2.empty()) model->tex2 = FindTextureByRegex("unittextures/", modelName + "2");
	if (model->tex1.empty()) model->tex1 = FindTextureByRegex(modelPath, "tex1");
	if (model->tex2.empty()) model->tex2 = FindTextureByRegex(modelPath, "tex2");
	if (model->tex1.empty()) model->tex1 = FindTextureByRegex(modelPath, "diffuse");
	if (model->tex2.empty()) model->tex2 = FindTextureByRegex(modelPath, "glow");          // low priority


	// 2. gather model-defined textures of first material (medium priority)
	if (scene->mNumMaterials > 0) {
		const unsigned int texTypes[] = {
			aiTextureType_SPECULAR,
			aiTextureType_UNKNOWN,
			aiTextureType_DIFFUSE,
			/*
			// TODO: support these too (we need to allow constructing tex1 & tex2 from several sources)
			aiTextureType_EMISSIVE,
			aiTextureType_HEIGHT,
			aiTextureType_NORMALS,
			aiTextureType_SHININESS,
			aiTextureType_OPACITY,
			*/
		};
		for (unsigned int n = 0; n < (sizeof(texTypes) / sizeof(texTypes[0])); n++) {
			aiString textureFile;

			if (scene->mMaterials[0]->Get(AI_MATKEY_TEXTURE(texTypes[n], 0), textureFile) != aiReturn_SUCCESS)
				continue;

			assert(textureFile.length > 0);
			model->tex1 = FindTexture(textureFile.data, modelPath, model->tex1);
		}
	}


	// 3. try to load from metafile (highest priority)
	model->tex1 = FindTexture(modelTable.GetString("tex1", ""), modelPath, model->tex1);
	model->tex2 = FindTexture(modelTable.GetString("tex2", ""), modelPath, model->tex2);

	model->invertTexYAxis = modelTable.GetBool("fliptextures", true); // Flip texture upside down
	model->invertTexAlpha = modelTable.GetBool("invertteamcolor", true); // Reverse teamcolor levels
}


void SAssPiece::UploadGeometryVBOs()
{
	if (!hasGeometryData)
		return;

	//FIXME share 1 VBO for ALL models
	vboAttributes.Bind(GL_ARRAY_BUFFER);
	vboAttributes.Resize(vertices.size() * sizeof(SAssVertex), GL_STATIC_DRAW, &vertices[0]);
	vboAttributes.Unbind();

	vboIndices.Bind(GL_ELEMENT_ARRAY_BUFFER);
	vboIndices.Resize(vertexDrawIndices.size() * sizeof(unsigned int), GL_STATIC_DRAW, &vertexDrawIndices[0]);
	vboIndices.Unbind();

	// NOTE: wasteful to keep these around, but still needed (eg. for Shatter())
	// vertices.clear();
	// vertexDrawIndices.clear();
}


void SAssPiece::DrawForList() const
{
	if (!hasGeometryData)
		return;

	vboAttributes.Bind(GL_ARRAY_BUFFER);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(3, GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, pos)));

		glEnableClientState(GL_NORMAL_ARRAY);
		glNormalPointer(GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, normal)));

		// primary and secondary texture use first UV channel
		for (unsigned int n = 0; n < NUM_MODEL_TEXTURES; n++) {
			glClientActiveTexture(GL_TEXTURE0 + n);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glTexCoordPointer(2, GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, texCoords) + (0 * sizeof(float2))));
		}

		// extra UV channels (currently at most one)
		for (unsigned int n = 1; n < GetNumTexCoorChannels(); n++) {
			glClientActiveTexture(GL_TEXTURE0 + NUM_MODEL_TEXTURES + (n - 1));
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);

			glTexCoordPointer(2, GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, texCoords) + (n * sizeof(float2))));
		}

		glClientActiveTexture(GL_TEXTURE5);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(3, GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, sTangent)));

		glClientActiveTexture(GL_TEXTURE6);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(3, GL_FLOAT, sizeof(SAssVertex), vboAttributes.GetPtr(offsetof(SAssVertex, tTangent)));
	vboAttributes.Unbind();

	vboIndices.Bind(GL_ELEMENT_ARRAY_BUFFER);
		/*
		* since aiProcess_SortByPType is being used,
		* we're sure we'll get only 1 type here,
		* so combination check isn't needed, also
		* anything more complex than triangles is
		* being split thanks to aiProcess_Triangulate
		*/
		glDrawRangeElements(GL_TRIANGLES, 0, vertices.size() - 1, vertexDrawIndices.size(), GL_UNSIGNED_INT, vboIndices.GetPtr());
	vboIndices.Unbind();

	glClientActiveTexture(GL_TEXTURE6);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE5);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE2);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
}

