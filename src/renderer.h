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

		//APPLICATION VARIABLES
		Scene* scene;
		Camera* camera;

		//Render variables
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.

		//Shadow Resolution
		int shadow_map_resolution = 2048; //Default Resolution

		//Renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
	
		//Processes a whole prefab (with all its nodes)
		void processPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//Processes one node from the prefab and its children
		void processNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//Render a draw call
		void renderDrawCall(RenderCall* rc, Camera* camera);

		//Render a basic draw call
		void renderDepthMap(RenderCall* rc, Camera* light_camera);

		//Singlepass lighting
		void SinglePassLoop(Mesh* mesh, Shader* shader);

		//Multipass lighting
		void MultiPassLoop(Mesh* mesh, Shader* shader);

		//Shadow Atlas
		void createShadowAtlas();
		void computeSpotShadowMap(LightEntity* light);
		void computeDirectionalShadowMap(LightEntity* light);
		void showShadowAtlas();

	};

	Texture* CubemapFromHDRE(const char* filename);

};