#ifndef SCENE_H
#define SCENE_H

#include "framework.h"
#include "camera.h"
#include <string>

//forward declaration
class cJSON; 
class FBO;
class Texture;


//our namespace
namespace GTR {

	enum EntityType {
		NONE = 0,
		PREFAB = 1,
		LIGHT = 2,
		CAMERA = 3,
		REFLECTION_PROBE = 4,
		DECALL = 5
	};

	enum LightType {
		POINT = 0,
		SPOT = 1,
		DIRECTIONAL = 2
	};

	class Scene;
	class Prefab;

	//represents one element of the scene (could be lights, prefabs, cameras, etc)
	class BaseEntity
	{
	public:
		Scene* scene;
		std::string name;
		EntityType entity_type;
		Matrix44 model;
		bool visible;
		BaseEntity() { entity_type = NONE; visible = true;}
		virtual ~BaseEntity() {}
		virtual void renderInMenu();
		virtual void configure(cJSON* json) {};
	};

	//represents one prefab in the scene
	class PrefabEntity : public GTR::BaseEntity
	{
	public:
		std::string filename;
		Prefab* prefab;
		
		PrefabEntity();
		PrefabEntity(std::string filename);
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
	};

	class LightEntity : public GTR::BaseEntity
	{
	public:

		//General features
		Vector3 color;
		float intensity;
		LightType light_type;
		float max_distance;

		//Spot Light
		float cone_angle;
		float cone_exp;
		bool spot_shadow_trigger;// Triggers changes in spotlight properties that affect shadows for atlas rebuilding task.

		//Directional Light
		float area_size; 
		bool directional_shadow_trigger;// Triggers changes in spotlight properties that affect shadows for atlas rebuilding task.

		//Shadows
		bool cast_shadows;
		int shadow_index;
		float shadow_bias;
		Camera* light_camera;

		LightEntity();
		LightEntity(LightType light_type);
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
	};

	//contains all entities of the scene
	class Scene
	{
	public:

		enum RenderPipeline {
			Forward = 0,
			Deferred = 1
		};

		enum LightModel {
			Phong = 0,
			BRDF = 1
		};

		enum BufferRange {
			SDR = 0, //1 byte per color component
			HDR = 1, //4 bytes per color component
		};

		enum DiffuseReflection
		{
			Lambert = 0,
			Burley = 1
		};

		enum SmithGGXAproximation
		{
			G1 = 0,
			G2 = 1
		};

		enum LightPass {
			Multipass = 0,
			Singlepass = 1,
		};

		//Singleton
		static Scene* instance;

		//Scene properties
		Vector3 background_color;
		Vector3 ambient_light;
		float color_scale;
		float avarage_lum;
		float white_lum;
		Camera* main_camera;

		//Scene algorithms
		bool light_status; //Shows or hides scene lights (like a switch)
		bool alpha_sorting; //Whether we sort render calls or not.
		bool emissive_materials; //Whether we enable prefab's emissive texture or not.
		bool occlusion; //Whether we enable prefab's occlusion texture or not.
		bool specular_light; //Whether we enable prefab's roughness metallic texture or not.
		bool normal_mapping; //Whether we are redering with normal map or interpolated normals.

		//Render properties
		RenderPipeline render_pipeline; //Whether we are rendering with forward or deferred pipeline. By deafult we set the flag to Deferred.
		LightModel light_model; //The light equation we are using
		DiffuseReflection diffuse_reflection; // Diffuse equation
		SmithGGXAproximation smith_aproximation; // Smith geomtry shadowing equation aproximation
		LightPass light_pass; //Whether we are rendering with Single Pass or Multi Pass. By deafult we set the flag to Single Pass.

		//Shadows
		Texture* shadow_atlas; //Shadow map of the lights of the scene
		bool show_atlas; //Enables or disables the display of the shadow atlas.
		const char* shadow_resolutions[4] = { "512 x 512", "1024 x 1024", "2048 x 2048", "4096 x 4096" };
		int atlas_resolution_index; //The corresponding index in the array of shadow atlas resolutions
		int atlas_scope; //The current shadow scope in case that all shadow maps doesn't fit into the screen.
		int num_shadows; //The number of shadows in the scene.

		//Deferred buffers
		bool show_buffers;
		bool toggle_buffers;
		BufferRange buffer_range;

		//Color correction
		bool gamma_correction; // Show the difference between working in linear space and reconverting to gamma space and only work in gamma space.
		bool tone_mapper; //Use tone mapper to improve visual perception.

		//Scene triggers
		bool light_switch_trigger; //Triggers if the user has checked or un checked the light switch checkbox.
		bool resolution_trigger; //Triggers if a resolution 
		bool entity_trigger; //Triggers if an entity has changed its visibility or a visible entity has changed its model.
		bool prefab_trigger; //Triggers if a new prefab has been added to the scene or an old one has been deleted.
		bool light_trigger; //Triggers if a new light has been added to the scene or an old one has been deleted.
		bool shadow_visibility_trigger; //Triggers changes in shadow casting or light visibility for lights that cast shadows.
		bool shadow_resolution_trigger; //Triggers if shadow resolution has been changed.
		bool light_model_trigger; //Triggers if light model has been changed.
		bool buffer_range_trigger; //Triggers if the deferred buffer range has changed.

		//FX properties
		float contrast;
		float saturation;
		float vigneting;

		//Input text buffer
		const static int buffer_size = 25;
		char buffer[buffer_size] = {};

		Scene();

		std::string filename;
		std::vector<BaseEntity*> entities;

		void clear();
		void addEntity(BaseEntity* entity);
		void removeEntity(BaseEntity* entity);
		std::string nameEntity(std::string default_name);
		BaseEntity* createEntity(std::string type);

		//JSON methods
		bool load(const char* filename);
		bool save();

		//Trigger method
		void resetTriggers();

		//Light switch
		void LightSwitch();

		//Light model correction
		void SwitchLightModel();
	};

};

#endif