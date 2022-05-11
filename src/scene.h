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

	enum eEntityType {
		NONE = 0,
		PREFAB = 1,
		LIGHT = 2,
		CAMERA = 3,
		REFLECTION_PROBE = 4,
		DECALL = 5
	};

	enum eLightType {
		POINT = 0,
		SPOT = 1,
		DIRECTIONAL = 2
	};

	enum RenderType {
		Singlepass = 0,
		Multipass = 1,
	};

	class Scene;
	class Prefab;

	//represents one element of the scene (could be lights, prefabs, cameras, etc)
	class BaseEntity
	{
	public:
		Scene* scene;
		std::string name;
		eEntityType entity_type;
		Matrix44 model;
		bool visible;
		BaseEntity() { entity_type = NONE; visible = true;}
		virtual ~BaseEntity() {}
		virtual void renderInMenu();
		virtual void configure(cJSON* json) {}
	};

	//represents one prefab in the scene
	class PrefabEntity : public GTR::BaseEntity
	{
	public:
		std::string filename;
		Prefab* prefab;
		
		PrefabEntity();
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
	};

	class LightEntity : public GTR::BaseEntity
	{
	public:

		//General features
		Vector3 color;
		float intensity;
		eLightType light_type;
		float max_distance;

		//Spot Light
		float cone_angle;
		float cone_exp;
		bool spot_shadow_tracker;// Tracks changes in spotlight properties that affect shadows for atlas rebuilding task.

		//Directional Light
		float area_size; 
		bool directional_shadow_tracker;// Tracks changes in spotlight properties that affect shadows for atlas rebuilding task.

		//Shadows
		bool cast_shadows;
		int shadow_index;
		float shadow_bias;
		Camera* light_camera;

		LightEntity();
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
	};

	//contains all entities of the scene
	class Scene
	{
	public:
		static Scene* instance;

		Vector3 background_color;
		Vector3 ambient_light;
		Camera main_camera;

		//Scene shadows
		FBO* fbo; //Frame Buffer Object
		Texture* shadow_atlas; //Shadow map of the lights of the scene

		//Scene properties
		bool alpha_sorting; //Whether we sort render calls or not.
		bool emissive_materials; //Whether we enable prefab's emissive texture or not.
		bool occlusion; //Whether we enable prefab's occlusion texture or not.
		bool specular_light; //Whether we enable prefab's roughness metallic texture or not.
		bool normal_mapping; //Whether we are redering with normal map or interpolated normals.
		int render_type; //Whether we are rendering with Single Pass or Multi Pass. By deafult we set the flag to Single Pass.
		bool shadow_sorting; //Whether we sort light by shadows or not.
		int num_shadows; //The number of shadows in the scene.

		//Shadow atlas
		int atlas_resolution_index; //The corresponding index in the array of shadow atlas resolutions
		int atlas_scope; //The current shadow scope in case that all shadow maps doesn't fit into the screen.
		bool show_atlas; //Enables or disables the display of the shadow atlas.

		//Scene trackers
		bool entity_tracker; //Tracks if an entity has changed his visibility or a visible entity has changed its model.
		bool shadow_visibility_tracker; //Tracks changes in shadow casting or light visibility for lights that cast shadows.
		bool shadow_resolution_tracker; //Tracks if shadow resolution has been changed.

		Scene();

		std::string filename;
		std::vector<BaseEntity*> entities;

		void clear();
		void addEntity(BaseEntity* entity);

		bool load(const char* filename);
		BaseEntity* createEntity(std::string type);
	};

};

#endif