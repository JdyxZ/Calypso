#pragma once
#include "prefab.h"
#include "shader.h"
#include "mesh.h"

//forward declarations
class Camera;

namespace GTR {

	enum eFxType {
		GRAY = 0,
		CONTRAST = 1,
		BLUR1 = 2,
		BLUR2 = 3,
		MIX = 4,
		MOTIONBLUR = 5
	};

	class Prefab;
	class Material;

	class RenderCall {
	public:
		Mesh* mesh;
		Material* material;
		Matrix44 model;
		BoundingBox world_bounding_box;
		float distance_to_camera;

		RenderCall() { distance_to_camera = 10.0f;}
	};

	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:

		Renderer();

		//Application variables
		Scene* scene;
		Camera* camera;
		Vector2 window_size;

		//Mesh Cube

		Mesh cube;

		//FBOs
		FBO* shadow_fbo;
		FBO* gbuffers_fbo;
		FBO* decals_fbo;
		FBO* illumination_fbo;
		FBO* ssao_fbo;
		FBO* ssao_p_fbo;
		FBO* volumetric_fbo;

		Texture* skybox;

		//Render variables
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		LightEntity* direct_light;
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.
		std::vector<RenderCall*> transparent_objects; //Here we store the RenderCalls of the objects that need blending (for Deferred pipeline)
		std::vector<DecalEntity*> decals; //Here we store each Decal to be sent to the Shadder.

		//SSAO
		std::vector<Vector3> rand_points_ssao;
		std::vector<Vector3> rand_points_ssao_p;

		//Shadow Resolution
		int shadow_map_resolution = 2048; //Default Resolution

		//Buffer range
		int buffer_range = GL_UNSIGNED_BYTE;

		//Decals
		Texture* cloned_depth_texture;

		//PostFx Textures
		Texture* postTexA;
		Texture* postTexB;
		Texture* postTexC;
		Texture* postTexD;
		Matrix44 mvp_last;

		//Constructor
		Renderer(Scene* scene, Camera* camera, int window_width, int window_height);

		//Renders several elements of the scene with the given camera
		void renderScene();
			
		//Scene elements
		void processScene();
		void processPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);  //Processes a whole prefab (with all its nodes)
		void processNode(const Matrix44& model, GTR::Node* node, Camera* camera); //Processes one node from the prefab and its children

		//Pipeline globals
		void renderWithoutLights();
		void SinglePassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector);//Singlepass lighting
		void MultiPassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector); //Multipass lighting

		//Forward pipeline
		void renderForward();
		void setForwardSceneUniforms(Shader* shader);
		void renderMesh(Shader* shader, RenderCall* rc, Camera* camera); 

		//Deferred pipeline
		void renderDeferred();
		void setDeferredSceneUniforms(Shader* shader);
		void GBuffers();
		void DecalsFBO();
		void clearGBuffers();
		void renderGBuffers(Shader* shader, RenderCall* rc, Camera* camera);
		void SSAO();
		void getssaoBlur();
		void renderSSAO(std::vector<Vector3> rand_points);
		void IlluminationNTransparencies();
		void clearIlluminationBuffers();
		void renderDeferredIllumination();
		void renderQuadIllumination();
		void renderSphereIllumination();
		void renderTransparentObjects(); 
		void showBuffers(); 
		void viewportEmissive(); 

		//Gamma correction
		Vector3 degamma(Vector3 color);
		Vector3 gamma(Vector3 color);

		//Shadow Atlas
		void updateShadowAtlas();
		void computeShadowAtlas();
		void computeSpotShadowMap(LightEntity* light);
		void computeDirectionalShadowMap(LightEntity* light, Camera* camera);
		void renderDepthMap(RenderCall* rc, Camera* light_camera);
		void showShadowAtlas();

		//Reflections
		void renderSkybox(Camera* camera);

		//VOlumetricLight
		void InitVolumetric();
		void RenderVolumetric();
		
		//PostFx
		void applyFx(Camera* camera, Texture* color_tex, Texture* depth_tex);
		void loadFx(int FxType, FBO* fbo, Texture* current_Tex, Texture* alter_tex, char* shadername);
		void InitPostFxTextures();

	};

	//Cubemap
	Texture* CubemapFromHDRE(const char* filename);

	//SpherePoints
	std::vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);

};