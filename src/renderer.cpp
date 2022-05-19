#include "renderer.h"
#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "extra/hdre.h"
#include "application.h"
#include "fbo.h"
#include <algorithm>

constexpr int SHOW_ATLAS_RESOLUTION = 300;

using namespace GTR;
using namespace std;

bool sortRenderCall(const RenderCall* rc1, const RenderCall* rc2)
{
	eAlphaMode rc1_alpha = rc1->material->alpha_mode;
	eAlphaMode rc2_alpha = rc2->material->alpha_mode;
	if (rc1_alpha == eAlphaMode::BLEND && rc2_alpha != eAlphaMode::BLEND) return false;
	else if (rc1_alpha != eAlphaMode::BLEND && rc2_alpha == eAlphaMode::BLEND) return true;
	else if (rc1_alpha == eAlphaMode::BLEND && rc2_alpha == eAlphaMode::BLEND) return rc1->distance_to_camera > rc2->distance_to_camera;	
	else if (rc1_alpha != eAlphaMode::BLEND && rc2_alpha != eAlphaMode::BLEND) return rc1->distance_to_camera < rc2->distance_to_camera;	
	else return true;
}

bool sortLight(const LightEntity* l1, const LightEntity* l2) 
{
	bool l1_cast_shadows = l1->cast_shadows;
	bool l2_cast_shadows = l2->cast_shadows;
	if (l1_cast_shadows && !l2_cast_shadows) return true;
	else if (!l1_cast_shadows && l2_cast_shadows) return false;
	else return true;

}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{	
	//Set current scene and camera
	this->scene = scene;

	//Set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	//Clear the render calls vector and the lights vector
	render_calls.clear();
	lights.clear();

	//Generate render calls and fill lights vector
	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if(pent->prefab)
				processPrefab(ent->model, pent->prefab, camera);
		}

		if (ent->entity_type == LIGHT) {
			LightEntity* light = (LightEntity*)ent;
			lights.push_back(light);
		}
	}

	//If there aren't lights in the scene don't render nothing
	if (lights.empty()) return;

	//Now we sort the RenderCalls vector according to the boolean method sortRenderCall
	if (scene->alpha_sorting) std::sort(render_calls.begin(), render_calls.end(), sortRenderCall);

	//Now we sort the Light vector according to the boolean method sortLight
	if (scene->shadow_sorting) std::sort(lights.begin(), lights.end(), sortLight);

	//Set shadow resolution
	if (scene->shadow_resolution_trigger)
	{
		string shadow_resolution;
		shadow_resolution.assign(Application::instance->shadow_resolutions[scene->atlas_resolution_index]);
		shadow_map_resolution = strtoul(shadow_resolution.substr(0, shadow_resolution.find_first_of(" ")).c_str(), NULL, 10);
		cout << "Resolution successfully changed" << endl;
	}

	//Compute the number of shadows of the scene and the shadow index of each light
	if (scene->shadow_visibility_trigger)
	{
		scene->num_shadows = 0;
		int shadow_index = 0;
		for (int i = 0; i < lights.size(); i++)
		{
			if (lights[i]->cast_shadows)
			{
				//Assign a shadow slot for shadow atlas
				lights[i]->shadow_index = shadow_index;

				//Update
				scene->num_shadows++;
				shadow_index++;
			}
		}
	}

	//Create Shadow Atlas: We create a dynamic atlas to be resizable
	if (scene->shadow_visibility_trigger || scene->shadow_resolution_trigger) createShadowAtlas();

	//Compute Shadow Atlas
	if (scene->fbo || scene->shadow_atlas)
	{
		//Iterate over light vector
		for (int i = 0; i < lights.size(); i++)
		{
			//Current light
			LightEntity* light = lights[i];

			//Booleans
			bool compute_spot = light->light_type == SPOT && light->cast_shadows && (scene->entity_trigger || light->spot_shadow_trigger || scene->shadow_visibility_trigger || scene->shadow_resolution_trigger);
			bool compute_directional = light->light_type == DIRECTIONAL && light->cast_shadows && (scene->entity_trigger || light->directional_shadow_trigger || scene->shadow_visibility_trigger || scene->shadow_resolution_trigger || camera->camera_trigger);
			
			//Shadow Map
			if (compute_spot)
			{
				computeSpotShadowMap(light);
				light->spot_shadow_trigger = false;
			}
			if (compute_directional)
			{
				computeDirectionalShadowMap(light,camera);
				light->directional_shadow_trigger = false;
			}
		}
	}

	//Enable view camera after computing shadow maps
	camera->enable();

	//Final render
	for (int i = 0; i < render_calls.size(); i++)
	{
		RenderCall* rc = render_calls[i];
		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDrawCall(render_calls[i], camera); 	
		}
	}

	//Debug shadow maps
	if (scene->show_atlas) showShadowAtlas();

	//Reset triggers
	if (scene->entity_trigger) scene->entity_trigger = false;
	if (scene->shadow_visibility_trigger) scene->shadow_visibility_trigger = false;
	if (camera->camera_trigger) camera->camera_trigger = false;

}

//renders all the prefab
void Renderer::processPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	processNode(model, &prefab->root, camera); //For each prefab we render its nodes with the model matrix of the entity that we pass by parameter, which avoids having the same prefab in memory twice.
}

//renders a node of the prefab and its children
void Renderer::processNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model,node->mesh->box);

		//Create a render call for each node and push it back in the RenderCalls vector
		RenderCall* rc = new RenderCall();
		rc->mesh = node->mesh;
		rc->material = node->material;
		rc->model = node_model;
		rc->world_bounding_box = world_bounding;
		rc->distance_to_camera = world_bounding.center.distance(camera->center);
		render_calls.push_back(rc);

	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		processNode(prefab_model, node->children[i], camera);
}

//Render a draw call
void GTR::Renderer::renderDrawCall(RenderCall* rc, Camera* camera)
{
	//In case there is nothing to do
	if (!rc->mesh || !rc->mesh->getNumVertices() || !rc->material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//Define locals to simplify coding
	Shader* shader = NULL;

	//Textures
	Texture* color_texture = NULL;
	Texture* emissive_texture = NULL;
	Texture* omr_texture = NULL;
	Texture* normal_texture = NULL;
	//Texture* occlusion_texture = NULL;

	//Texture loading
	color_texture = rc->material->color_texture.texture;
	if(scene->emissive_materials) emissive_texture = rc->material->emissive_texture.texture;
	if(scene->specular_light || scene->occlusion) omr_texture = rc->material->metallic_roughness_texture.texture;
	if(scene->normal_mapping) normal_texture = rc->material->normal_texture.texture;
	//occlusion_texture = rc->material->occlusion_texture.texture;

	//Texture check
	if (color_texture == NULL)	color_texture = Texture::getWhiteTexture();
	if (scene->emissive_materials && emissive_texture == NULL) emissive_texture = Texture::getBlackTexture();
	if ((scene->specular_light || scene->occlusion) && omr_texture == NULL) omr_texture = Texture::getWhiteTexture();

	//Normal mapping
	int entity_has_normal_map;
	if (scene->normal_mapping) entity_has_normal_map = (normal_texture == NULL) ? 0 : 1;
	else entity_has_normal_map = 0;

	//Select the blending
	if (rc->material->alpha_mode == GTR::eAlphaMode::BLEND) glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	else glDisable(GL_BLEND);

	//Select whether to render both sides of the triangles
	if (rc->material->two_sided) glDisable(GL_CULL_FACE);
	else glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	switch (scene->render_type) {
	case(Singlepass):
		shader = Shader::Get("singlepass");
		break;
	case(Multipass):
		shader = Shader::Get("multipass");
		break;
	}
	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//Upload textures
	if(color_texture) shader->setTexture("u_color_texture", color_texture, 0);
	if (scene->emissive_materials) shader->setTexture("u_emissive_texture", emissive_texture, 1);
	if (scene->specular_light || scene->occlusion) shader->setTexture("u_omr_texture", omr_texture, 2);
	if (scene->normal_mapping && normal_texture) shader->setTexture("u_normal_texture", normal_texture, 3);
	//if(occlusion_texture) shader->setTexture("u_occlussion_texture", occlusion_texture, 4);

	//Upload scene uniforms
	shader->setUniform("u_model", rc->model);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_color", rc->material->color);
	shader->setUniform("u_time", getTime());
	shader->setUniform("u_alpha_cutoff", rc->material->alpha_mode == GTR::eAlphaMode::MASK ? rc->material->alpha_cutoff : 0); //this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_normal_mapping", entity_has_normal_map);
	shader->setUniform("u_occlusion", scene->occlusion);
	shader->setUniform("u_specular_light", scene->specular_light);

	switch (scene->render_type) {
	case(Singlepass):
		SinglePassLoop(rc->mesh, shader);
		break;
	case(Multipass):
		MultiPassLoop(rc->mesh, shader);
		break;
	}
}

//Render basic draw call
void GTR::Renderer::renderDepthMap(RenderCall* rc, Camera* light_camera)
{
	//In case there is nothing to do
	if (!rc->mesh || !rc->mesh->getNumVertices() || !rc->material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//Define locals to simplify coding
	Shader* shader = NULL;

	//Select whether to render both sides of the triangles
	if (rc->material->two_sided) glDisable(GL_CULL_FACE);
	else glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//Render the inner face of the triangles in order to reduce shadow acne
	/*glEnable(GL_CULL_FACE);
	glFrontFace(GL_CW);
	assert(glGetError() == GL_NO_ERROR);*/

	//chose a shader
	shader = Shader::Get("depth");
	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//Upload scene uniforms
	shader->setUniform("u_model", rc->model);
	shader->setUniform("u_viewprojection", light_camera->viewprojection_matrix);
	shader->setUniform("u_alpha_cutoff", rc->material->alpha_mode == GTR::eAlphaMode::MASK ? rc->material->alpha_cutoff : 0); //this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)

	//Disable blending
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	//do the draw call that renders the mesh into the screen
	rc->mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//Reset
	/*
	glDisable(GL_CULL_FACE);
	glFrontFace(GL_CCW);
	*/

}

//Singlepass lighting
void GTR::Renderer::SinglePassLoop(Mesh* mesh, Shader* shader)
{
	//Blending support
	glDepthFunc(GL_LEQUAL);

	//Loop variables
	int const lights_size = lights.size();
	int const max_num_lights = 5; //Single pass lighting accepts at most 5 lights
	int starting_light = 0;
	int final_light = min(max_num_lights - 1, lights_size - 1);

	//Lights properties vectors
	std::vector<Vector3> lights_position;
	std::vector<Vector3> lights_color;
	std::vector<float> lights_intensity;
	std::vector<float> lights_max_distance;
	std::vector<int> lights_type;

	//Spot lights vectors
	std::vector<Vector3> spots_direction;
	std::vector<Vector2> spots_cone;

	//Directional lights vectors
	std::vector<Vector3> directionals_front;

	//Shadows vectors
	std::vector<int> cast_shadows;
	std::vector<float> shadows_index;
	std::vector<float> shadows_bias;
	std::vector<Matrix44> shadows_vp;

	//Reserve memory
	lights_position.reserve(max_num_lights);
	lights_color.reserve(max_num_lights);
	lights_intensity.reserve(max_num_lights);
	lights_max_distance.reserve(max_num_lights);
	lights_type.reserve(max_num_lights);
	spots_direction.reserve(max_num_lights);
	spots_cone.reserve(max_num_lights);
	directionals_front.reserve(max_num_lights);
	cast_shadows.reserve(max_num_lights);
	shadows_index.reserve(max_num_lights);
	shadows_bias.reserve(max_num_lights);
	shadows_vp.reserve(max_num_lights);

	//Single pass lighting
	while (starting_light < lights_size)
	{
		if (starting_light == 5)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			shader->setUniform("u_ambient_light", Vector3());
		}
		if (final_light == lights_size - 1) shader->setUniform("u_last_iteration", 1);
		else shader->setUniform("u_last_iteration", 0);

		//Support variables
		int num_lights = final_light - starting_light + 1;
		int j = 0;

		for (int i = starting_light; i <= final_light; i++)
		{
			//Current Light
			LightEntity* light = lights[i];

			//General light properties
			lights_position[j] = light->model.getTranslation();
			lights_color[j] = light->color;
			lights_intensity[j] = light->intensity;
			lights_max_distance[j] = light->max_distance;

			//Specific light properties
			switch (light->light_type)
			{
				case(eLightType::POINT):
					lights_type[j] = light->light_type;
					break;
				case (eLightType::SPOT):
					if ((light->cone_angle < 2.0 && light->cone_angle > -2.0) || light->cone_angle < -90.0 || light->cone_angle > 90.0) lights_type[j] = eLightType::POINT;
					else
					{
						spots_direction[j] = light->model.rotateVector(Vector3(0, 0, -1));
						spots_cone[j] = Vector2(light->cone_exp, cos(light->cone_angle * DEG2RAD));
						lights_type[j] = light->light_type;
					}
					break;
				case (eLightType::DIRECTIONAL):
					directionals_front[j] = light->model.rotateVector(Vector3(0, 0, -1));
					lights_type[j] = light->light_type;
					break;
			}

			//Shadow properties
			if (scene->shadow_atlas && light->cast_shadows)
			{		
				cast_shadows[j] = 1;
				shadows_index[j] = (float) light->shadow_index;
				shadows_bias[j] = light->shadow_bias;
				shadows_vp[j] = light->light_camera->viewprojection_matrix;	
			}
			else
			{
				cast_shadows[j] = 0;
			}

			//Update iterator
			j++;		
		}

		//Upload light uniforms
		shader->setUniform3Array("u_lights_position", (float*)&lights_position[0], num_lights);
		shader->setUniform3Array("u_lights_color", (float*)&lights_color[0], num_lights);
		shader->setUniform1Array("u_lights_intensity", &lights_intensity[0], num_lights);
		shader->setUniform1Array("u_lights_max_distance", &lights_max_distance[0], num_lights);
		shader->setUniform1Array("u_lights_type", &lights_type[0], num_lights);
		shader->setUniform3Array("u_spots_direction", (float*)&spots_direction[0], num_lights);
		shader->setUniform2Array("u_spots_cone", (float*)&spots_cone[0], num_lights);
		shader->setUniform3Array("u_directionals_front", (float*)&directionals_front[0], num_lights);
		shader->setUniform("u_num_lights", num_lights);

		//Upload shadow uniforms
		shader->setUniform1Array("u_cast_shadows", &cast_shadows[0], num_lights);
		shader->setUniform1Array("u_shadows_index", &shadows_index[0], num_lights);
		shader->setUniform1Array("u_shadows_bias", &shadows_bias[0], num_lights);
		shader->setMatrix44Array("u_shadows_vp", &shadows_vp[0], num_lights);
		shader->setUniform("u_num_shadows", (float)scene->num_shadows);

		//Shadow Atlas
		if (scene->shadow_atlas) 
		{
			shader->setTexture("u_shadow_atlas", scene->shadow_atlas, 8);
			shader->setUniform("u_shadows", 1);
		}
		else
		{
			shader->setUniform("u_shadows", 0);
		}

		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);

		//Update variables
		starting_light = final_light + 1;
		final_light = min(max_num_lights + final_light, lights_size - 1);

	}
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

//Multipass lighting
void GTR::Renderer::MultiPassLoop(Mesh* mesh, Shader* shader)
{
	//Blending support
	glDepthFunc(GL_LEQUAL);

	//Multi pass lighting
	for (int i = 0; i < lights.size(); i++) {

		if (i == 0) shader->setUniform("u_last_iteration", 0);

		if (i == 1)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			shader->setUniform("u_ambient_light", Vector3());//reset the ambient light
		}
		if (i == lights.size() - 1) shader->setUniform("u_last_iteration", 1);

		//Current light
		LightEntity* light = lights[i];

		//Light uniforms
		shader->setUniform("u_light_position", light->model.getTranslation());
		shader->setUniform("u_light_color", light->color);
		shader->setUniform("u_light_intensity", light->intensity);
		shader->setUniform("u_light_max_distance", light->max_distance);

		//Specific light uniforms
		switch (light->light_type)
		{
		case(eLightType::POINT):
			shader->setUniform("u_light_type", 0);
			break;
		case (eLightType::SPOT):
			if ((light->cone_angle < 2.0 && light->cone_angle > -2.0) || light->cone_angle < -90.0 || light->cone_angle > 90.0) shader->setUniform("u_light_type", 0);
			else
			{
				shader->setVector3("u_spot_direction", light->model.rotateVector(Vector3(0, 0, -1)));
				shader->setUniform("u_spot_cone", Vector2(light->cone_exp, cos(light->cone_angle * DEG2RAD)));
				shader->setUniform("u_light_type", 1);
			}
			break;
		case (eLightType::DIRECTIONAL):
			shader->setVector3("u_directional_front", light->model.rotateVector(Vector3(0, 0, -1)));
			shader->setUniform("u_area_size", light->area_size);
			shader->setUniform("u_light_type", 2);
			break;
		}

		//Shadow uniforms
		if (scene->shadow_atlas && light->cast_shadows)
		{
			shader->setUniform("u_cast_shadows", 1);
			shader->setUniform("u_shadow_index", (float)light->shadow_index);
			shader->setUniform("u_shadow_bias", light->shadow_bias);
			shader->setMatrix44("u_shadow_vp", light->light_camera->viewprojection_matrix);
			shader->setTexture("u_shadow_atlas", scene->shadow_atlas, 8);
			shader->setUniform("u_num_shadows", (float)scene->num_shadows);
		}
		else
		{
			shader->setUniform("u_cast_shadows", 0);
		}
		
		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);
	}

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

//Create a shadow atlas
void GTR::Renderer::createShadowAtlas()
{
	//Delete the former atlas and continue
	if (scene->fbo)
	{
		delete scene->fbo;
		scene->fbo = NULL;
		scene->shadow_atlas = NULL;

		if (scene->num_shadows == 0)
			return;

	}

	//New shadow atlas
	scene->fbo = new FBO();
	scene->fbo->setDepthOnly(shadow_map_resolution * scene->num_shadows, shadow_map_resolution); //We create it wide in order to save memory space and improve shadow map management
	scene->shadow_atlas = scene->fbo->depth_texture;
}

//Compute spot shadow map into the shadow atlas
void GTR::Renderer::computeSpotShadowMap(LightEntity* light)
{
	//Speed boost
	glColorMask(false, false, false, false);

	//For the first render
	if (!light->light_camera) light->light_camera = new Camera();

	//Bind the fbo
	scene->fbo->bind();

	//Set the atlas region of the shadow map to work on
	Vector4 shadow_region(light->shadow_index * shadow_map_resolution, 0, shadow_map_resolution, shadow_map_resolution);

	//Activate flags on the shadow region
	glViewport(shadow_region.x, shadow_region.y, shadow_region.z, shadow_region.w);
	glScissor(shadow_region.x, shadow_region.y, shadow_region.z, shadow_region.w);
	glEnable(GL_SCISSOR_TEST);

	//Clear Depth Buffer on the shadow region
	glClear(GL_DEPTH_BUFFER_BIT);

	//Light camera
	Camera* light_camera = light->light_camera;

	//Camera properties
	float camera_fov = 2 * light->cone_angle;
	float camera_aspect = shadow_map_resolution / shadow_map_resolution;
	float camera_near = 0.1f;
	float camera_far = light->max_distance;
	Vector3 camera_position = light->model.getTranslation();
	Vector3 camera_front = light->model * Vector3(0, 0, -1);
	Vector3 camera_up = light->model.rotateVector(Vector3(0, 1, 0));

	//Set Perspective Matrix
	light_camera->setPerspective(camera_fov, camera_aspect, camera_near, camera_far);

	//Set View Matrix
	light_camera->lookAt(camera_position, camera_front, camera_up);

	//Enable camera
	light_camera->enable();

	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall* rc = render_calls[i];
		if (rc->material->alpha_mode == eAlphaMode::BLEND)
			continue;

		if (light_camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDepthMap(rc, light_camera);
		}
	}
	//Unbind the fbo
	scene->fbo->unbind();

	//Reset
	glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);
	glColorMask(true, true, true, true);
	glDisable(GL_SCISSOR_TEST);

}

//Compute directional shadow maps into the shadow atlas
void GTR::Renderer::computeDirectionalShadowMap(LightEntity* light, Camera* camera)
{
	//Speed boost
	glColorMask(false, false, false, false);

	//For the first render
	if (!light->light_camera) light->light_camera = new Camera();

	//Bind the fbo
	scene->fbo->bind();

	//Set the atlas region of the shadow map to work on
	Vector4 shadow_region(light->shadow_index * shadow_map_resolution, 0, shadow_map_resolution, shadow_map_resolution);

	//Activate flags on the shadow region
	glViewport(shadow_region.x, shadow_region.y, shadow_region.z, shadow_region.w);
	glScissor(shadow_region.x, shadow_region.y, shadow_region.z, shadow_region.w);
	glEnable(GL_SCISSOR_TEST);

	//Clear Depth Buffer on the shadow region
	glClear(GL_DEPTH_BUFFER_BIT);

	//Light camera
	Camera* light_camera = light->light_camera;

	//Camera properties
	float halfarea = light->area_size / 2;
	float camera_aspect = shadow_map_resolution / shadow_map_resolution;
	float camera_near = 0.1f;
	float camera_far = light->max_distance;
	float camera_factor = 1.0f;
	Vector3 camera_position = camera->center;
	Vector3 camera_front = camera->center - camera_factor*(camera->eye - camera->center);
	Vector3 camera_up = -1 * camera->up;

	//Orthographic Matrix
	light_camera->setOrthographic(-halfarea, halfarea, halfarea * camera_aspect, -halfarea * camera_aspect, camera_near, camera_far);

	//Set View Matrix
	light_camera->lookAt(camera_position, camera_front, camera_up);

	//Enable camera
	light_camera->enable();

	for (int i = 0; i < render_calls.size(); ++i)
	{
		RenderCall* rc = render_calls[i];
		if (rc->material->alpha_mode == eAlphaMode::BLEND)
			continue;

		if (light_camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDepthMap(rc, light_camera);
		}
	}
	//Unbind the fbo
	scene->fbo->unbind();

	//Reset
	glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);
	glColorMask(true, true, true, true);
	glDisable(GL_SCISSOR_TEST);

}

//Print shadow map in the screen
void GTR::Renderer::showShadowAtlas()
{
	//Just in case
	if (!scene->shadow_atlas || scene->num_shadows == 0)
		return;
	
	//Some variables
	int num_shadows = scene->num_shadows;
	int window_height = Application::instance->window_height;
	int window_width = Application::instance->window_width;
	int num_shadows_per_scope = min(num_shadows, window_width / SHOW_ATLAS_RESOLUTION); //Number of shadow maps that will be displayed at the same time (within a scope).
	int shadow_scope = clamp(scene->atlas_scope,0,ceil(num_shadows / (float)num_shadows_per_scope) - 1); //Current shadow scope.
	int num_shadows_in_scope = min(num_shadows - shadow_scope * num_shadows_per_scope, num_shadows_per_scope); //Number of shadows in the current scope
	int shadow_offset = (window_width - num_shadows_in_scope * SHOW_ATLAS_RESOLUTION)/2; //In order to place the shadow maps in the center of the horizontal axis.
	int starting_shadow = shadow_scope * num_shadows_per_scope;
	int final_shadow = starting_shadow + num_shadows_in_scope;

	for (int i = 0; i < lights.size(); ++i) 
	{
		if (lights[i]->cast_shadows) 
		{
			//Only render if lights are in the right scope
			if (starting_shadow <= lights[i]->shadow_index && lights[i]->shadow_index < final_shadow)
			{
				//Current Light
				LightEntity* light = lights[i];

				//Map shadow map into screen coordinates
				glViewport((light->shadow_index - starting_shadow) * SHOW_ATLAS_RESOLUTION + shadow_offset, 0, SHOW_ATLAS_RESOLUTION, SHOW_ATLAS_RESOLUTION);

				//Render the shadow map with the linearized shader
				Shader* shader = Shader::getDefaultShader("linearize");
				shader->enable();
				shader->setUniform("u_camera_nearfar", Vector2(light->light_camera->near_plane, light->light_camera->far_plane));
				shader->setUniform("u_shadow_index", (float)light->shadow_index);
				shader->setUniform("u_num_shadows", (float)num_shadows);
				scene->shadow_atlas->toViewport(shader);
				shader->disable();
			}
		}
	}

	//Update shadow scope
	scene->atlas_scope = shadow_scope;

	//Reset
	glViewport(0, 0, window_width, window_height);

}

Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = HDRE::Get(filename);
	if (!hdre)
		return NULL;

	Texture* texture = new Texture();
	if (hdre->getFacesf(0))
	{
		texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesf(0),
			hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT);
		for (int i = 1; i < hdre->levels; ++i)
			texture->uploadCubemap(texture->format, texture->type, false,
				(Uint8**)hdre->getFacesf(i), GL_RGBA32F, i);
	}
	else
		if (hdre->getFacesh(0))
		{
			texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesh(0),
				hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_HALF_FLOAT);
			for (int i = 1; i < hdre->levels; ++i)
				texture->uploadCubemap(texture->format, texture->type, false,
					(Uint8**)hdre->getFacesh(i), GL_RGBA16F, i);
		}
	return texture;
}
