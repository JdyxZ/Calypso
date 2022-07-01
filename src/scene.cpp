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

	//Camera
	main_camera = NULL;

	//Scene algorithms
	light_status = true;
	alpha_sorting = true;
	emissive_materials = true;
	occlusion = true;
	specular_light = true;
	normal_mapping = true;

	//Render properties
	render_pipeline = Scene::Deferred;
	light_model = Scene::Phong;
	diffuse_reflection = Scene::Lambert;
	smith_aproximation = Scene::G1; 
	light_pass = Scene::Multipass;

	//Shadows
	shadow_atlas = NULL;
	atlas_resolution_index = 2; //Set by default 2048 x 2048 resolution
	atlas_scope = 0;
	show_atlas = false;
	num_shadows = 0;

	//Deferred buffers
	show_buffers = false;
	toggle_buffers = false;
	buffer_range = HDR;

	//Color correction algorithms
	gamma_correction = true;
	tone_mapper = true;

	//Scene triggers: We set some of them true just for the first iteration
	light_switch_trigger = false;
	resolution_trigger = true;
	entity_trigger = true;
	prefab_trigger = true;
	light_trigger = true;
	shadow_visibility_trigger = true;
	shadow_resolution_trigger = true;
	light_model_trigger = false;
	buffer_range_trigger = false;

	//SSAO debugging
	show_ssao = false;
	show_ssaop = false;
	SSAO_type = SSAOType::SSAO;

	//FX properties
	contrast = 1.0;
	saturation = 1.0;
	vigneting = 0.0;
	threshold = 0.9;
	debug1 = 1.0;
	debug2 = 1.0;
	
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

GTR::BaseEntity* GTR::Scene::createEntity(std::string type)
{
	if (type == "PREFAB") return new GTR::PrefabEntity();
	else if (type == "LIGHT") return new GTR::LightEntity();
	else   return NULL;
}

void GTR::Scene::addEntity(BaseEntity* entity)
{
	entities.push_back(entity); entity->scene = this;
}

void GTR::Scene::removeEntity(BaseEntity* entity) {

	for (auto it = entities.begin(); it != entities.end(); ++it) {
		if (*it == entity) entities.erase(it);
	}

}

std::string GTR::Scene::nameEntity(string default_name)
{

	bool simple_name = true;
	int name_index = 1;
	for (int i = 0; i < entities.size(); ++i)
	{
		if (entities[i]->name == default_name) simple_name = false;
		if (entities[i]->name == (default_name + " " + to_string(name_index))) name_index++;
	}
	if (simple_name) return default_name;
	else return default_name + " " + to_string(name_index);
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
	ambient_light = readJSONVector3(json, "ambient_light", ambient_light);
	color_scale = readJSONNumber(json, "color_scale", color_scale);
	avarage_lum = readJSONNumber(json, "avarage_lum", avarage_lum);
	white_lum = readJSONNumber(json, "white_lum", white_lum);
	Vector3 eye = readJSONVector3(json, "camera_position", main_camera->eye);
	Vector3 center = readJSONVector3(json, "camera_target", main_camera->center);
	float fov = readJSONNumber(json, "camera_fov", main_camera->fov);

	//Set the parameters of the main camera
	main_camera->lookAt(eye, center, Vector3(0, 1, 0));
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
			readJSONFloatVector(entity_json, "model", model_array);
			for (int i = 0; i < model_array.size(); ++i)	ent->model.m[i] = model_array[i];
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
		cout << "ERROR: Scene file not found: " << filename << endl;
		return false;
	}

	//Create JSON
	cJSON* scene_json = cJSON_CreateObject();

	//Add scene properties
	writeJSONString(scene_json, "environment", "night.hdre");
	writeJSONVector3(scene_json, "background_color", background_color);
	writeJSONVector3(scene_json, "ambient_light", ambient_light);
	writeJSONNumber(scene_json, "color_scale", color_scale);
	writeJSONNumber(scene_json, "avarage_lum", avarage_lum);
	writeJSONNumber(scene_json, "white_lum", white_lum);
	writeJSONVector3(scene_json, "camera_position", this->main_camera->eye);
	writeJSONVector3(scene_json, "camera_target", this->main_camera->center);
	writeJSONNumber(scene_json, "camera_fov", 80);

	//Entitiy JSON
	cJSON* entities_json = cJSON_AddArrayToObject(scene_json, "entities");

	for (int i = 0; i < entities.size(); ++i)
	{
		//Current entity
		BaseEntity* entity = entities[i];

		//Create a JSON object for the new entity
		cJSON* new_entity = cJSON_CreateObject();

		switch (entity->entity_type)
		{
			case (PREFAB):
			{
				PrefabEntity* prefab = (PrefabEntity*)entity;
				writeJSONString(new_entity, "name", prefab->name);
				writeJSONString(new_entity, "type", "PREFAB");
				writeJSONString(new_entity, "filename", prefab->filename);
				writeJSONFloatVector(new_entity, "model", prefab->model.m, 16);
				break;
			}
			case (LIGHT):
			{
				LightEntity* light = (LightEntity*)entity;
				writeJSONString(new_entity, "name", light->name);
				writeJSONString(new_entity, "type", "LIGHT");
				writeJSONVector3(new_entity, "color", light->color);
				writeJSONNumber(new_entity, "intensity", light->intensity);
				writeJSONNumber(new_entity, "max_dist", light->max_distance);
				switch (light->light_type) 
				{
				case(LightType::POINT):
					writeJSONString(new_entity, "light_type", "POINT");
					break;
				case(LightType::SPOT):
					writeJSONNumber(new_entity, "cone_angle", light->cone_angle);
					writeJSONNumber(new_entity, "cone_exp", light->cone_exp);
					writeJSONBoolean(new_entity, "cast_shadows", light->cast_shadows);
					writeJSONNumber(new_entity, "shadow_bias", light->shadow_bias);
					writeJSONString(new_entity, "light_type", "SPOT");
					break;
				case(LightType::DIRECTIONAL):
					writeJSONNumber(new_entity, "area_size", light->area_size);
					writeJSONBoolean(new_entity, "cast_shadows", light->cast_shadows);
					writeJSONString(new_entity, "light_type", "DIRECTIONAL");
					break;
				}
				writeJSONFloatVector(new_entity, "model", light->model.m, 16);

				break;
			}
		}
		
		//Add the new entity to the list of entities
		cJSON_AddItemToArray(entities_json, new_entity);

	}

	//JSON file
	ofstream json_file("data/scene.json", ofstream::binary);

	//Delete the old JSON
	json_file.clear();

	//Save the new JSON
	int json_size = 0;
	char* json_content = cJSON_Print(scene_json,&json_size);
	json_file.write(json_content,json_size);

	//Notify the success
	cout << endl << "Scene successfully saved" << endl;

	//Free memory
	cJSON_Delete(scene_json);

	return true;
}

void GTR::Scene::resetTriggers()
{
	this->resolution_trigger = false;
	this->entity_trigger = false;
	this->prefab_trigger = false;
	this->light_trigger = false;
	this->shadow_visibility_trigger = false;
	this->main_camera->camera_trigger = false;
}

void GTR::Scene::LightSwitch()
{
	for (auto it = this->entities.begin(); it != this->entities.end(); ++it)
	{
		//Current entity
		BaseEntity* ent = *it;

		//Check if it is a light
		if (ent->entity_type == LIGHT)
		{
			//Current light
			LightEntity* light = (LightEntity*)ent;

			//Update light visibility
			if (this->light_status) //Show light
				light->visible = true;
			else //Hide light
				light->visible = false;
		}
	}
}

void GTR::Scene::SwitchLightModel()
{
	float intensity_factor = 5.f;

	for (auto it = this->entities.begin(); it != this->entities.end(); ++it)
	{
		//Current entity
		BaseEntity* ent = *it;

		//Check if it is a light
		if (ent->entity_type == LIGHT)
		{
			//Current light
			LightEntity* light = (LightEntity*)ent;

			//Update light intensity
			if(this->light_model == BRDF)
				light->intensity *= intensity_factor;
			else if(this->light_model == Phong)
				light->intensity /= intensity_factor;
		}
	}
}

void GTR::BaseEntity::renderInMenu()
{
#ifndef SKIP_IMGUI

	//Support variables
	bool visibility_changed = 0;

	//Visibility
	visibility_changed = ImGui::Checkbox("Visible", &visible);

	//Delete entity
	bool delete_entity = false;
	delete_entity = ImGui::Button("Delete");
	if (delete_entity) scene->removeEntity(this);

	//Update triggers
	if (this->entity_type == EntityType::PREFAB) scene->prefab_trigger;
	if (this->entity_type == EntityType::LIGHT) scene->light_trigger;
	
	//Model
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

GTR::PrefabEntity::PrefabEntity(string filename)
{
	visible = true;
	model = Matrix44();
	entity_type = PREFAB;
	this->filename = filename;
	prefab = GTR::Prefab::Get((string("data/") + filename).c_str());
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

GTR::LightEntity::LightEntity(LightType light_type)
{
	//General features
	visible = true;
	model = Matrix44();
	entity_type = LIGHT;

	//Light features
	this->light_type = light_type;
	color.set(1.0f, 1.0f, 1.0f);
	intensity = 20;
	max_distance = 1000;

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
		case LightType::SPOT: 
			ImGui::Text("Light type: %s", "Spot"); 
			ImGui::ColorEdit3("Color", color.v);
			ImGui::DragFloat("Intensity", &intensity, 0.1f);
			spot_shadow_trigger |= ImGui::DragFloat("Max distance", &max_distance, 1);
			spot_shadow_trigger |= ImGui::DragFloat("Cone angle", &cone_angle);
			ImGui::DragFloat("Cone exponent", &cone_exp);
			scene->shadow_visibility_trigger |= ImGui::Checkbox("Cast shadow", &cast_shadows);
			spot_shadow_trigger |= ImGui::DragFloat("Shadow bias", &shadow_bias, 0.001f);
			break;
		case LightType::POINT:
			ImGui::Text("Light type: %s", "Point");
			ImGui::ColorEdit3("Color", color.v);
			ImGui::DragFloat("Intensity", &intensity, 0.1f);
			ImGui::DragFloat("Max distance", &max_distance, 1);
			break;
		case LightType::DIRECTIONAL: 
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
		light_type = LightType::SPOT;
		cone_angle = readJSONNumber(json, "cone_angle", cone_angle);
		cone_exp = readJSONNumber(json, "cone_exp", cone_exp);
	}
	else if (type_field == "POINT") {
		light_type = LightType::POINT;
	}
	else if (type_field == "DIRECTIONAL") {
		light_type = LightType::DIRECTIONAL;
		area_size = readJSONNumber(json, "area_size", area_size);
	}
}