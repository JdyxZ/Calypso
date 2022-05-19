#include "scene.h"
#include "utils.h"

#include "prefab.h"
#include "extra/cJSON.h"
#include <fstream> 

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
	render_type = Singlepass;
	shadow_sorting = false;
	num_shadows = 0;

	//Shadow atlas debugging
	atlas_resolution_index = 2; //Set by default 2048 x 2048 resolution
	show_atlas = false;
	atlas_scope = 0;

	//Scene triggers: We set them true just for the first iteration
	entity_trigger = true;
	shadow_visibility_trigger = true;
	
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

	//Parse JSON string 
	cJSON* json = cJSON_Parse(content.c_str());
	if (!json)
	{
		std::cout << "ERROR: Scene JSON has errors: " << filename << std::endl;
		return false;
	}

	//Read global properties
	background_color = readJSONVector3(json, "background_color", background_color);
	ambient_light = readJSONVector3(json, "ambient_light", ambient_light );
	Vector3 eye = readJSONVector3(json, "camera_position", main_camera->eye);
	Vector3 center = readJSONVector3(json, "camera_target", main_camera->center);
	float fov = readJSONNumber(json, "camera_fov", main_camera->fov);

	//Set the parameters of the main camera
	main_camera->lookAt(eye,center, Vector3(0, 1, 0));
	main_camera->fov = fov;

	//Entities
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

		//Entity name
		if (cJSON_GetObjectItem(entity_json, "name"))
		{
			ent->name = cJSON_GetObjectItem(entity_json, "name")->valuestring;
			stdlog(std::string(" + entity: ") + ent->name);
		}

		//Entity model
		if (cJSON_GetObjectItem(entity_json, "model"))
		{
			vector<float> model_array;
			readJSONVector(entity_json, "model", model_array);
			for(int i = 0; i < model_array.size(); ++i)	ent->model.m[i] = model_array[i];
		}

		ent->configure(entity_json);
	}

	//free memory
	cJSON_Delete(json);

	return true;
}

bool GTR::Scene::save()
{
	std::string content;

	if (!readFile(filename, content))
	{
		std::cout << "- ERROR: Scene file not found: " << filename << std::endl;
		return false;
	}

	//Parse JSON string
	cJSON* json = cJSON_Parse(content.c_str());
	if (!json)
	{
		std::cout << "ERROR: Scene JSON has errors: " << filename << std::endl;
		return false;
	}

	//Replace global properties
	replaceJSONVector3(json, "background_color", background_color);
	replaceJSONVector3(json, "ambient_light", ambient_light);
	replaceJSONVector3(json, "camera_position", main_camera->eye);
	replaceJSONVector3(json, "camera_target", main_camera->center);
	replaceJSONNumber(json, "camera_fov", 80);

	//Entities
	cJSON* entities_json = cJSON_GetObjectItemCaseSensitive(json, "entities");

	for (int i = 0; i < entities.size(); ++i)
	{
		BaseEntity* entity = entities[i];
		cJSON* entity_json = cJSON_GetArrayItem(entities_json, i);

		if (readJSONString(entity_json, "name", "") != entities[i]->name)
			continue;

		switch (entity->entity_type)
		{
			case (PREFAB):
			{
				PrefabEntity* prefab = (PrefabEntity*)entity;
				if (cJSON_GetObjectItem(entity_json, "model")) replaceJSONFloatVector(entity_json, "model", prefab->model.m, 16);
				break;
			}
			case (LIGHT):
			{
				LightEntity* light = (LightEntity*)entity;
				if (cJSON_GetObjectItem(entity_json, "color")) replaceJSONVector3(entity_json, "color", light->color);
				if (cJSON_GetObjectItem(entity_json, "intensity")) replaceJSONNumber(entity_json, "intensity", light->intensity);
				if (cJSON_GetObjectItem(entity_json, "max_dist")) replaceJSONNumber(entity_json, "max_dist", light->max_distance);
				if (cJSON_GetObjectItem(entity_json, "cone_angle")) replaceJSONNumber(entity_json, "cone_angle", light->cone_angle);
				if (cJSON_GetObjectItem(entity_json, "cone_exp")) replaceJSONNumber(entity_json, "cone_exp", light->cone_exp);
				if (cJSON_GetObjectItem(entity_json, "area_size")) replaceJSONNumber(entity_json, "area_size", light->area_size);
				if (cJSON_GetObjectItem(entity_json, "cast_shadows")) replaceJSONBoolean(entity_json, "cast_shadows", light->cast_shadows);
				if (cJSON_GetObjectItem(entity_json, "shadow_bias")) replaceJSONNumber(entity_json, "shadow_bias", light->shadow_bias);
				if (cJSON_GetObjectItem(entity_json, "model")) replaceJSONFloatVector(entity_json, "model", light->model.m, 16);
				break;
			}
		}

	}

	//JSON file
	ofstream json_file("data/scene.json", ofstream::binary);

	//Delete the old JSON
	json_file.clear();

	//Save the new JSON
	int json_size = 0;
	char* json_content = cJSON_Print(json,&json_size);
	json_file.write(json_content,json_size);

	//Notify the success
	cout << endl << "Scene successfully saved" << endl;

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
		if(light->cast_shadows)	scene->shadow_visibility_trigger = true;
	}
	if (this->entity_type == PREFAB && visibility_changed) scene->entity_trigger = true;
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
	spot_shadow_trigger = true;

	
	//Directional light
	area_size = 1000;
	directional_shadow_trigger = true;

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
			spot_shadow_trigger |= ImGui::DragFloat("Max distance", &max_distance, 1);
			spot_shadow_trigger |= ImGui::DragFloat("Cone angle", &cone_angle);
			ImGui::DragFloat("Cone exponent", &cone_exp);
			scene->shadow_visibility_trigger |= ImGui::Checkbox("Cast shadow", &cast_shadows);
			spot_shadow_trigger |= ImGui::DragFloat("Shadow bias", &shadow_bias, 0.001f);
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
			directional_shadow_trigger |= ImGui::DragFloat("Max distance", &max_distance, 1);
			directional_shadow_trigger |= ImGui::DragFloat("Area size", &area_size);
			scene->shadow_visibility_trigger |= ImGui::Checkbox("Cast shadow", &cast_shadows);
			directional_shadow_trigger |= ImGui::DragFloat("Shadow bias", &shadow_bias, 0.001f);
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