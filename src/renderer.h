#pragma once
#include "prefab.h"
#include "shader.h"

//forward declarations
class Camera;

namespace GTR {

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

		//Application variables
		Scene* scene;
		Camera* camera;
		Vector2 window_size;

		//FBOs
		FBO* shadow_fbo;
		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		FBO* ssao_fbo;
		FBO* ssao_p_fbo;

		//Render variables
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.

		//SSAO
		std::vector<Vector3> rand_points_ssao;
		std::vector<Vector3> rand_points_ssao_p;		

		//Shadow Resolution
		int shadow_map_resolution = 2048; //Default Resolution

		//Constructor
		Renderer(Scene* scene, Camera* camera, int window_width, int window_height);

		//Renders several elements of the scene with the given camera
		void renderScene();
			
		//Scene elements
		void processScene();
		void processPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);  //Processes a whole prefab (with all its nodes)
		void processNode(const Matrix44& model, GTR::Node* node, Camera* camera); //Processes one node from the prefab and its children

		//Pipeline globals
		void SinglePassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector);//Singlepass lighting
		void MultiPassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector); //Multipass lighting

		//Forward pipeline
		void renderForward();
		void renderMesh(Shader* shader, RenderCall* rc, Camera* camera); 

		//Deferred pipeline
		void renderDeferred();
		void renderGBuffers(Shader* shader, RenderCall* rc, Camera* camera);
		void getssaoBlur();
		void renderSSAO(std::vector<Vector3> rand_points);
		void renderDeferredIllumination();
		void renderQuadIllumination();
		void renderSphereIllumination();

		void renderTransparentObjects(std::vector<RenderCall*>& transparent_objects);
		void clearGBuffers(); 
		void clearIlluminationBuffers(); 
		void showBuffers(); 
		void viewportEmissive(); 

		//Shadow Atlas
		void updateShadowAtlas();
		void computeShadowAtlas();
		void computeSpotShadowMap(LightEntity* light);
		void computeDirectionalShadowMap(LightEntity* light, Camera* camera);
		void renderDepthMap(RenderCall* rc, Camera* light_camera);
		void showShadowAtlas();

	};

	//Cubemap
	Texture* CubemapFromHDRE(const char* filename);

	//SpherePoints
	std::vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);

};