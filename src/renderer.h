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

		//Render variables
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.

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
		Shader* getShader(); //Get the shader to render the scene with
		void SinglePassLoop(Shader* shader, Mesh* mesh); //Singlepass lighting
		void MultiPassLoop(Shader* shader, Mesh* mesh); //Multipass lighting

		//Forward pipeline
		void renderForward();
		void renderDrawCall(Shader* shader, RenderCall* rc, Camera* camera); //Render a draw call	

		//Deferred pipeline
		void renderDeferred();
		void renderGBuffers(Shader* shader, RenderCall* rc, Camera* camera);
		void renderDeferredIllumination(Shader* shader, Camera* camera);
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

	Texture* CubemapFromHDRE(const char* filename);

};