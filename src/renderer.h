#pragma once
#include "prefab.h"
#include "shader.h"

//forward declarations
class Camera;
class Shader;

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

<<<<<<< Updated upstream
		//APPLICATION VARIABLES
		Scene* scene;
		Camera* camera;

		//Render variables
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.

		//Shadow Resolution
		int shadow_map_resolution = 2048; //Default Resolution
=======
		//NEW
		enum ePipeLine {
			FORWARD,
			DEFERRED
		};

		Scene* scene;		
		std::vector<LightEntity*> lights; //Here we store each Light to be sent to the Shadder.
		std::vector<RenderCall*> render_calls; // Here we store each RenderCall to be sent to the Shadder.
		RenderType light_render; //Whether we are rendering with Single Pass or Multi Pass. By deafult we set the flag to Single Pass.
		bool alpha_sorting; //Whether we sort render calls or not
		bool emissive_materials; //Whether we enable prefab's emissive texture or not
		bool occlusion; //Whether we enable prefab's occlusion texture or not
		bool specular_light; //Whether we enable prefab's roughness metallic texture or not
		bool normal_mapping; //Wheter we are redering with normal map or interpolated normals
		
		//NEW
		ePipeLine pipeline;
		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		bool show_gbuffers;
		

		//constructor
		Renderer(); //NEW
		
		//Sets the light render mode of the scene
		void configureRenderer(int render_type, bool normal_mapping, bool alpha_sorting, bool emissive_materials, bool occlusion_texture,bool specular_light);
>>>>>>> Stashed changes

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

		//Pipelines (NEW)
		void renderForward(Camera* camera, GTR::Scene* scene);
		void renderDeferred(Camera* camera, GTR::Scene* scene);
		void renderDrawCallToGbuffers(RenderCall* rc, Camera* camera);
	};

	Texture* CubemapFromHDRE(const char* filename);

};