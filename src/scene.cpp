#include "scene.h"
#include "utils.h"

#include "prefab.h"
#include "extra/cJSON.h"

GTR::Scene* GTR::Scene::instance = NULL;

using namespace std;

GTR::Scene::Scene()
{
	instance = this;

	//Shadow Atlas
	fbo = NULL;
	shadow_atlas = NULL;

	//Scene properties
	alpha_sorting = true;
	emissive_materials = true;
	occlusion = true;
	specular_light = true;
	normal_mapping = true;
	render_type = Multipass;
	shadow_sorting = false;
	num_shadows = 0;

	//Shadow atlas debugging
	atlas_resolution_index = 2; //Set by default 2048 x 2048 resolution
	show_atlas = false;
	atlas_scope = 0;

	//Scene trackers: We set them true just for the first iteration
	entity_tracker = true;
	shadow_visibility_tracker = true;
	
}

void GTR::Scene::clear()
{
	for (int i = 0; i < entities.size(); ++i)
	{
		BaseEntity* ent = entities[i];
		delete ent;
	}
	entities.resize(0);
}

void GTR::Scene::addEntity(BaseEntity* entity)
{
	entities.push_back(entity); entity->scene = this;
}

bool GTR::Scene::load(const char* filename)
{
	std::string content;

	this->filename = filename;
	std::cout << " + Reading scene JSON: " << filename << "..." << std::endl;

	if (!readFile(filename, content))
	{
		std::cout << "- ERROR: Scene file not found: " << filename << std::endl;
		return false;
	}

	//parse json string 
	cJSON* json = cJSON_Parse(content.c_str());
	if (!json)
	{
		std::cout << "ERROR: Scene JSON has errors: " << filename << std::endl;
		return false;
	}

	//read global properties
	background_color = readJSONVector3(json, "background_color", background_color);
	ambient_light = readJSONVector3(json, "ambient_light", ambient_light );
	main_camera.eye = readJSONVector3(json, "camera_position", main_camera.eye);
	main_camera.center = readJSONVector3(json, "camera_target", main_camera.center);
	main_camera.fov = readJSONNumber(json, "camera_fov", main_camera.fov);

	//entities
	cJSON* entities_json = cJSON_GetObjectItemCaseSensitive(json, "entities");
	cJSON* entity_json;
	cJSON_ArrayForEach(entity_json, entities_json)
	{
		std::string type_str = cJSON_GetObjectItem(entity_json, "type")->valuestring;
		BaseEntity* ent = createEntity(type_str);
		if (!ent)
		{
			std::cout << " - ENTITY TYPE UNKNOWN: " << type_str << std::endl;
			//continue;
			ent = new BaseEntity();
		}

		addEntity(ent);

		if (cJSON_GetObjectItem(entity_json, "name"))
		{
			ent->name = cJSON_GetObjectItem(entity_json, "name")->valuestring;
			stdlog(std::string(" + entity: ") + ent->name);
		}

		//read transform
		if (cJSON_GetObjectItem(entity_json, "position"))
		{
			ent->model.setIdentity();
			Vector3 position = readJSONVector3(entity_json, "position", Vector3());
			ent->model.translate(position.x, position.y, position.z);
		}

		if (cJSON_GetObjectItem(entity_json, "angle"))
		{
			float angle = cJSON_GetObjectItem(entity_json, "angle")->valuedouble;
			ent->model.rotate(angle * DEG2RAD, Vector3(0, 1, 0));
		}

		if (cJSON_GetObjectItem(entity_json, "rotation"))
		{
			Vector4 rotation = readJSONVector4(entity_json, "rotation");
			Quaternion q(rotation.x, rotation.y, rotation.z, rotation.w);
			Matrix44 R;
			q.toMatrix(R);
			ent->model = R * ent->model;
		}

		if (cJSON_GetObjectItem(entity_json, "target"))
		{
			Vector3 target = readJSONVector3(entity_json, "target", Vector3());
			Vector3 front = target - ent->model.getTranslation();
			ent->model.setFrontAndOrthonormalize(front);
		}

		if (cJSON_GetObjectItem(entity_json, "scale"))
		{
			Vector3 scale = readJSONVector3(entity_json, "scale", Vector3(1, 1, 1));
			ent->model.scale(scale.x, scale.y, scale.z);
		}
		if (cJSON_GetObjectItem(entity_json, "custom_rotation"))
		{
			//Hardcoded
			ent->model.rotate(-270 * DEG2RAD, Vector3(0, 1, 0));
			ent->model.rotate(90 * DEG2RAD, Vector3(1, 0, 1));
		}

		ent->configure(entity_json);
	}

	//free memory
	cJSON_Delete(json);

	return true;
}

GTR::BaseEntity* GTR::Scene::createEntity(std::string type)
{
	if (type == "PREFAB") return new GTR::PrefabEntity();
	else if (type == "LIGHT") return new GTR::LightEntity();
	else   return NULL;
}

void GTR::BaseEntity::renderInMenu()
{
#ifndef SKIP_IMGUI

	bool visibility_changed = 0;

	ImGui::Text("Name: %s", name.c_str()); // Edit 3 floats representing a color
	visibility_changed = ImGui::Checkbox("Visible", &visible); // Edit 3 floats representing a color
	//Model edit
	ImGuiMatrix44(model, "Model");

	if (this->entity_type == LIGHT && visibility_changed)
	{
		LightEntity* light = (LightEntity*)this;
		if(light->cast_shadows)	scene->shadow_visibility_tracker = true;
	}
	if (this->entity_type == PREFAB && visibility_changed) scene->entity_tracker = true;
#endif
}

GTR::PrefabEntity::PrefabEntity()
{
	entity_type = PREFAB;
	prefab = NULL;
}

void GTR::PrefabEntity::configure(cJSON* json)
{
	if (cJSON_GetObjectItem(json, "filename"))
	{
		filename = cJSON_GetObjectItem(json, "filename")->valuestring;
		prefab = GTR::Prefab::Get( (std::string("data/") + filename).c_str());
	}
}

void GTR::PrefabEntity::renderInMenu()
{
	BaseEntity::renderInMenu();

#ifndef SKIP_IMGUI
	ImGui::Text("filename: %s", filename.c_str()); // Edit 3 floats representing a color
	if (prefab && ImGui::TreeNode(prefab, "Prefab Info"))
	{
		prefab->root.renderInMenu();
		ImGui::TreePop();
	}
#endif
}

GTR::LightEntity::LightEntity()
{
	//General features
	entity_type = LIGHT;
	color.set(1.0f, 1.0f, 1.0f);
	intensity = 1;
	max_distance = 100;

	//Spot light
	cone_angle = 45;
	cone_exp = 30;
	spot_shadow_tracker = true;

	
	//Directional light
	area_size = 1000;
	directional_shadow_tracker = true;

	//Shadows
	cast_shadows = false;
	shadow_index = 0;
	shadow_bias = 0.001;
	light_camera = NULL;

}

void GTR::LightEntity::renderInMenu() 
{
	BaseEntity::renderInMenu();

#ifndef SKIP_IMGUI

	switch (light_type) 
	{
		case eLightType::SPOT: 
			ImGui::Text("Light type: %s", "Spot"); 
			ImGui::ColorEdit3("Color", color.v);
			ImGui::DragFloat("Intensity", &intensity, 0.1f);
			spot_shadow_tracker |= ImGui::DragFloat("Max distance", &max_distance, 1);
			spot_shadow_tracker |= ImGui::DragFloat("Cone angle", &cone_angle);
			ImGui::DragFloat("Cone exponent", &cone_exp);
			scene->shadow_visibility_tracker |= ImGui::Checkbox("Cast shadow", &cast_shadows);
			spot_shadow_tracker |= ImGui::DragFloat("Shadow bias", &shadow_bias, 0.001f);
			break;
		case eLightType::POINT:
			ImGui::Text("Light type: %s", "Point");
			ImGui::ColorEdit3("Color", color.v);
			ImGui::DragFloat("Intensity", &intensity, 0.1f);
			ImGui::DragFloat("Max distance", &max_distance, 1);
			break;
		case eLightType::DIRECTIONAL: 
			ImGui::Text("Light type: %s", "Directional");
			ImGui::ColorEdit3("Color", color.v);
			ImGui::DragFloat("Intensity", &intensity, 0.1f);
			directional_shadow_tracker |= ImGui::DragFloat("Max distance", &max_distance, 1);
			directional_shadow_tracker |= ImGui::DragFloat("Area size", &area_size);
			scene->shadow_visibility_tracker |= ImGui::Checkbox("Cast shadow", &cast_shadows);
			directional_shadow_tracker |= ImGui::DragFloat("Shadow bias", &shadow_bias, 0.001f);
			break;
	}

#endif
}

void GTR::LightEntity::configure(cJSON* json) {

	color = readJSONVector3(json, "color", color);
	intensity = readJSONNumber(json, "intensity", intensity);
	max_distance = readJSONNumber(json, "max_dist", max_distance);
	cast_shadows = readJSONBoolean(json, "cast_shadows", cast_shadows);
	shadow_bias = readJSONNumber(json, "shadow_bias", shadow_bias);
	std::string type_field = readJSONString(json, "light_type", "");

	if (type_field == "SPOT") {
		light_type = eLightType::SPOT;
		cone_angle = readJSONNumber(json, "cone_angle", cone_angle);
		cone_exp = readJSONNumber(json, "cone_exp", cone_exp);
	}
	else if (type_field == "POINT") {
		light_type = eLightType::POINT;
	}
	else if (type_field == "DIRECTIONAL") {
		light_type = eLightType::DIRECTIONAL;
		area_size = readJSONNumber(json, "area_size", area_size);
	}
}