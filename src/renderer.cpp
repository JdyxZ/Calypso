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

//<--------------------------------------------------- Support methods --------------------------------------------------->

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

//<--------------------------------------------------- Constructor --------------------------------------------------->

Renderer::Renderer(Scene* scene, Camera* camera, int window_width, int window_height)
{
	//Set current scene and camera
	this->scene = scene;
	this->camera = camera;

	//FBOs
	shadow_fbo = NULL;
	gbuffers_fbo = NULL;
	illumination_fbo = NULL;

	//Windows size
	window_size = Vector2(window_width, window_height);
}

//< --------------------------------------------------- Scene render --------------------------------------------------->

void Renderer::renderScene()
{	
	//Set the render calls and lights of the renderer
	processScene();

	//If there aren't lights in the scene render a scene without lights
	if (lights.empty())
	{
		renderWithoutLights();
		return;
	}

	//Update Shadow Atlas: We create a dynamic atlas to be resizable
	updateShadowAtlas();

	//Compute shadow atlas
	computeShadowAtlas();

	//Enable view camera after computing shadow maps
	camera->enable();

	//Choose a rendering pipeline
	switch (scene->render_pipeline)
	{
	case(Scene::Forward):
		renderForward();
		break;
	case(Scene::Deferred):
		renderDeferred();
		break;
	}
	
	//Debug shadow maps
	if (scene->show_atlas) showShadowAtlas();
}

//< --------------------------------------------------- Scene elements --------------------------------------------------->

//Create or update render calls vector
void Renderer::processScene()
{
	//Clear renderer vectors
	lights.clear();
	render_calls.clear();
	transparent_objects.clear();

	//Generate render calls and fill the light vector
	for (auto it = scene->entities.begin(); it != scene->entities.end(); ++it)
	{
		BaseEntity* ent = *it;
		if (!ent->visible)
			continue;

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab)
				processPrefab(ent->model, pent->prefab, camera);
		}

		//is a light!
		if (ent->entity_type == LIGHT) {
			LightEntity* light = (LightEntity*)ent;
			lights.push_back(light);
		}
	}
	

	//Now we sort the RenderCalls vector according to the boolean method sortRenderCall
	if (scene->alpha_sorting)
		std::sort(render_calls.begin(), render_calls.end(), sortRenderCall);
	
}

//Renders all the prefab
void Renderer::processPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	processNode(model, &prefab->root, camera); //For each prefab we render its nodes with the model matrix of the entity that we pass by parameter, which avoids having the same prefab in memory twice.
}

//Renders a node of the prefab and its children
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
		BoundingBox world_bounding_box = transformBoundingBox(node_model,node->mesh->box);

		//Create a render call for each node and push it back in the RenderCalls vector
		RenderCall* rc = new RenderCall();
		rc->mesh = node->mesh;
		rc->material = node->material;
		rc->model = node_model;
		rc->world_bounding_box = world_bounding_box;
		rc->distance_to_camera = world_bounding_box.center.distance(camera->center);
		render_calls.push_back(rc);

	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		processNode(prefab_model, node->children[i], camera);
}

//< --------------------------------------------------- Pipeline globals --------------------------------------------------->

//Render without lights
void GTR::Renderer::renderWithoutLights()
{
	//Set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	assert(checkGLErrors() && "An error has been produced cleaning the background");

	//Enable the camera to upload camera uniforms
	camera->enable();

	//Create a new shader
	Shader* shader = Shader::Get("nolights");
	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;

	//Enable the shader
	shader->enable();

	//Upload scene uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_emissive_materials", scene->emissive_materials);
	shader->setUniform("u_time", (float)getTime());

	//Send render calls to the GPU
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		//If bounding box is inside the camera frustum then the prefab is probably visible
		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			//In case there is nothing to do
			if (!rc->mesh || !rc->mesh->getNumVertices() || !rc->material)
				return;

			assert(glGetError() == GL_NO_ERROR);

			//Only render color texture and emissive materials
			Texture* color_texture = NULL;
			Texture* emissive_texture = NULL;

			//Get textures
			color_texture = rc->material->color_texture.texture;
			emissive_texture = rc->material->emissive_texture.texture;

			//Just in case		
			if (color_texture == NULL) color_texture = Texture::getWhiteTexture(); 
			if (emissive_texture == NULL) emissive_texture = Texture::getBlackTexture();

			//Upload prefab uniforms
			shader->setMatrix44("u_model", rc->model);
			shader->setUniform("u_color", rc->material->color);
			shader->setUniform("u_alpha_cutoff", rc->material->alpha_mode == GTR::eAlphaMode::MASK ? rc->material->alpha_cutoff : 0); //this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
			shader->setUniform("u_emissive_factor", rc->material->emissive_factor);

			//Upload textures
			if(color_texture) shader->setTexture("u_color_texture", color_texture, 0);
			if (emissive_texture && scene->emissive_materials) shader->setTexture("u_emissive_texture", emissive_texture, 1);

			//Enable depth test
			glEnable(GL_DEPTH_TEST);

			//Select the blending
			if (rc->material->alpha_mode == GTR::eAlphaMode::BLEND)
			{
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDepthFunc(GL_LEQUAL);
			}
			else
			{
				glDisable(GL_BLEND);
				glDepthFunc(GL_LESS);
			}

			//Select if render both sides of the triangles
			if (rc->material->two_sided)
				glDisable(GL_CULL_FACE);
			else
				glEnable(GL_CULL_FACE);
			assert(glGetError() == GL_NO_ERROR);

			//Do the draw call that renders the mesh into the screen
			rc->mesh->render(GL_TRIANGLES);
		}
	}

	//Disable the shader
	shader->disable();

	//Set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

//Singlepass lighting
void GTR::Renderer::SinglePassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector)
{
	//Loop variables
	int const lights_size = lights_vector.size();
	int const max_num_lights = 5; //Single pass lighting accepts at most 5 lights
	int starting_light = 0;
	int final_light = min(max_num_lights - 1, lights_size - 1);

	//Lights properties vectors
	vector<Vector3> lights_position;
	vector<Vector3> lights_color;
	vector<float> lights_intensity;
	vector<float> lights_max_distance;
	vector<int> lights_type;

	//Spot lights vectors
	vector<Vector3> spots_direction;
	vector<Vector2> spots_cone;

	//Directional lights vectors
	vector<Vector3> directionals_front;

	//Shadows vectors
	vector<int> cast_shadows;
	vector<float> shadows_index;
	vector<float> shadows_bias;
	vector<Matrix44> shadows_vp;

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
			if (scene->render_pipeline == Scene::Forward)
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			else if (scene->render_pipeline == Scene::Deferred)
				glBlendFunc(GL_ONE, GL_ONE);

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
			LightEntity* light = lights_vector[i];

			//General light properties
			lights_position[j] = light->model.getTranslation();
			if (scene->gamma_correction) lights_color[j] = degamma(light->color); //Send light color in linear space
			else lights_color[j] = light->color; //Send light color in gamma space
			lights_intensity[j] = light->intensity;
			lights_max_distance[j] = light->max_distance;

			//Specific light properties
			switch (light->light_type)
			{
			case(LightType::POINT):
				lights_type[j] = light->light_type;
				break;
			case (LightType::SPOT):
				spots_direction[j] = light->model.rotateVector(Vector3(0, 0, -1));
				spots_cone[j] = Vector2(light->cone_exp, cos(light->cone_angle * DEG2RAD));
				lights_type[j] = light->light_type;
				break;
			case (LightType::DIRECTIONAL):
				directionals_front[j] = light->model.rotateVector(Vector3(0, 0, -1));
				lights_type[j] = light->light_type;
				break;
			}

			//Shadow properties
			if (scene->shadow_atlas && light->cast_shadows)
			{
				cast_shadows[j] = 1;
				shadows_index[j] = (float)light->shadow_index;
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

		//Shadow Atlas
		if (scene->shadow_atlas)
			shader->setUniform("u_shadows", 1);
		else
			shader->setUniform("u_shadows", 0);

		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);

		//Update variables
		starting_light = final_light + 1;
		final_light = min(max_num_lights + final_light, lights_size - 1);

	}

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
	glFrontFace(GL_CCW);
	glDepthMask(true);
}

//Multipass lighting
void GTR::Renderer::MultiPassLoop(Shader* shader, Mesh* mesh, std::vector<LightEntity*>& lights_vector)
{
	//Determine the mesh type
	bool sphere_projection = mesh->name == "data/meshes/sphere.obj";

	//Multi pass lighting
	for (int i = 0; i < lights_vector.size(); i++) {

		if (i == 0) shader->setUniform("u_last_iteration", 0);

		if (i == 1)
		{
			glEnable(GL_BLEND);
			if (scene->render_pipeline == Scene::Forward)
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			else if (scene->render_pipeline == Scene::Deferred)
				glBlendFunc(GL_ONE, GL_ONE);

			shader->setUniform("u_ambient_light", Vector3());//reset the ambient light
		}
		if (i == lights_vector.size() - 1) shader->setUniform("u_last_iteration", 1);

		//Current light
		LightEntity* light = lights_vector[i];

		//Light model
		if (sphere_projection)
		{
			Matrix44 light_model;
			Vector3 light_position = light->model.getTranslation();
			light_model.setTranslation(light_position.x, light_position.y, light_position.z);
			light_model.scale(light->max_distance, light->max_distance, light->max_distance);
			shader->setMatrix44("u_model", light_model);

		}

		//Light uniforms
		shader->setUniform("u_light_position", light->model.getTranslation());
		if(scene->gamma_correction)	shader->setUniform("u_light_color", degamma(light->color)); //Send light color in linear space
		else shader->setUniform("u_light_color", light->color); //Send light color in gamma space
		shader->setUniform("u_light_intensity", light->intensity);
		shader->setUniform("u_light_max_distance", light->max_distance);

		//Specific light uniforms
		switch (light->light_type)
		{
		case(LightType::POINT):
			shader->setUniform("u_light_type", 0);
			break;
		case (LightType::SPOT):
			if ((light->cone_angle < 2.0 && light->cone_angle > -2.0) || light->cone_angle < -90.0 || light->cone_angle > 90.0) shader->setUniform("u_light_type", 0);
			else
			{
				shader->setVector3("u_spot_direction", light->model.rotateVector(Vector3(0, 0, -1)));
				shader->setUniform("u_spot_cone", Vector2(light->cone_exp, cos(light->cone_angle * DEG2RAD)));
				shader->setUniform("u_light_type", 1);
			}
			break;
		case (LightType::DIRECTIONAL):
			shader->setVector3("u_directional_front", light->model.rotateVector(Vector3(0, 0, -1)));
			shader->setUniform("u_area_size", light->area_size);
			shader->setUniform("u_light_type", 2);
			break;
		}

		//Shadow uniforms
		if (scene->shadow_atlas && light->cast_shadows)
		{
			shader->setUniform("u_cast_shadow", 1);
			shader->setUniform("u_shadow_index", (float)light->shadow_index);
			shader->setUniform("u_shadow_bias", light->shadow_bias);
			shader->setMatrix44("u_shadow_vp", light->light_camera->viewprojection_matrix);
		}
		else
		{
			shader->setUniform("u_cast_shadow", 0);
		}

		//Render only the backfacing triangles of the sphere
		if (sphere_projection)
			glFrontFace(GL_CW);

		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);
	}

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
	glFrontFace(GL_CCW);
	glDepthMask(true);
}

//< --------------------------------------------------- Forward pipeline --------------------------------------------------->

//Forward pipeline
void GTR::Renderer::renderForward()
{
	//Render depth map
	renderMainCameraShadowMap();

	//Set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	assert(checkGLErrors() && "An error has been produced cleaning the background");
	
	//Create a new shader
	Shader* shader = Shader::Get("forward");

	//no shader? then nothing to render
	if (!shader)
		return;

	//Enable the shader
	shader->enable();

	//Upload scene uniforms
	setForwardSceneUniforms(shader);

	//Send render calls to the GPU
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		//Set ambient light
		shader->setUniform("u_ambient_light", scene->ambient_light);

		//If bounding box is inside the camera frustum then the prefab is probably visible
		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderMesh(shader, rc, camera);
		}
	}

	//Disable the shader
	shader->disable();
}

//Upload foward scene uniforms to the shader
void GTR::Renderer::setForwardSceneUniforms(Shader* shader)
{
	//Scene uniforms
	shader->setUniform("u_light_model", scene->light_model);
	shader->setUniform("u_diffuse_reflection", scene->diffuse_reflection);
	shader->setUniform("u_geometry_shadowing", scene->smith_aproximation);
	shader->setUniform("u_light_pass", scene->light_pass);
	shader->setUniform("u_gamma_correction", scene->gamma_correction);
	shader->setUniform("u_tone_mapper", scene->tone_mapper.working);
	shader->setUniform("u_color_scale", scene->tone_mapper.color_scale);
	shader->setUniform("u_average_illumination", scene->tone_mapper.avarage_illumination);
	shader->setUniform("u_white_illumination", scene->tone_mapper.white_illumination);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_time", (float)getTime());
	shader->setUniform("u_occlusion", scene->occlusion);
	shader->setUniform("u_specular_light", scene->specular_light);
	shader->setTexture("u_shadow_atlas", scene->shadow_atlas, 8);
	shader->setUniform("u_num_shadows", (float)scene->num_shadows);
	shader->setUniform("u_screen_width", window_size.x);
	shader->setUniform("u_screen_height", window_size.y);
	shader->setUniform("u_znear", camera->near_plane);
	shader->setUniform("u_zfar", camera->far_plane);
	shader->setUniform("u_fov", camera->fov);
	shader->setUniform("u_aspect_ratio", camera->aspect);

	if (main_camera_fbo->depth_texture)
		shader->setTexture("u_depth_texture", main_camera_fbo->depth_texture, 5);

}

//Render a mesh with its materials and lights
void GTR::Renderer::renderMesh(Shader* shader, RenderCall* rc, Camera* camera)
{
	//In case there is nothing to do
	if (!rc->mesh || !rc->mesh->getNumVertices() || !rc->material)
		return;
	assert(glGetError() == GL_NO_ERROR);

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

	//Upload textures
	if(color_texture) shader->setTexture("u_color_texture", color_texture, 0);
	if (scene->emissive_materials) shader->setTexture("u_emissive_texture", emissive_texture, 1);
	if (scene->specular_light || scene->occlusion) shader->setTexture("u_omr_texture", omr_texture, 2);
	if (scene->normal_mapping && normal_texture) shader->setTexture("u_normal_texture", normal_texture, 3);
	//if(occlusion_texture) shader->setTexture("u_occlussion_texture", occlusion_texture, 4);

	//Upload prefab uniforms
	shader->setMatrix44("u_model", rc->model);
	shader->setUniform("u_color", rc->material->color);
	shader->setUniform("u_alpha_cutoff", rc->material->alpha_mode == GTR::eAlphaMode::MASK ? rc->material->alpha_cutoff : 0); //this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_normal_mapping", entity_has_normal_map);
	shader->setUniform("u_emissive_factor", rc->material->emissive_factor);

	//Enable depth test
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);

	//Select the blending
	if (rc->material->alpha_mode == GTR::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		glDisable(GL_BLEND);
	}

	//Select whether to render both sides of the triangles
	if (rc->material->two_sided) glDisable(GL_CULL_FACE);
	else glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//Select render tpye
	switch (scene->light_pass) {
	case(Scene::Singlepass):
		SinglePassLoop(shader, rc->mesh, lights);
		break;
	case(Scene::Multipass):
		MultiPassLoop(shader, rc->mesh, lights);
		break;
	}
}

//Render main camera shadow map
void GTR::Renderer::renderMainCameraShadowMap()
{
	//Crete the main camera fbo if they don't exist yet or resolution have changed
	if (!main_camera_fbo || scene->resolution_trigger)
	{
		if (main_camera_fbo)
		{
			delete main_camera_fbo;
			main_camera_fbo = NULL;
		}

		//Create a new FBO
		main_camera_fbo = new FBO();

		//Create only depth map texture
		main_camera_fbo->setDepthOnly(window_size.x, window_size.y); 
	}

	//Speed boost
	glColorMask(false, false, false, false);

	//Bind the fbo
	main_camera_fbo->bind();

	//Clear Depth Buffer on the shadow region
	glClear(GL_DEPTH_BUFFER_BIT);

	//Enable camera
	camera->enable();

	//Iterate over render calls vector
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		if (rc->material->alpha_mode == eAlphaMode::BLEND)
			continue;

		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDepthMap(rc, camera);
		}
	}
	//Unbind the fbo
	main_camera_fbo->unbind();

	//Reset
	glViewport(0, 0, window_size.x, window_size.y);
	glColorMask(true, true, true, true);
}

// <--------------------------------------------------- Deferred pipeline --------------------------------------------------->

//Deferred pipeline
void GTR::Renderer::renderDeferred()
{
	//Set buffer range
	if (scene->buffer_range == Scene::SDR) buffer_range = GL_UNSIGNED_BYTE;
	else if (scene->buffer_range == Scene::HDR) buffer_range = GL_FLOAT;

	//Create and render the GBuffers
	GBuffers();
	//Illumination and transparencies
	IlluminationNTransparencies();

	//Show Buffers or render the final frame
	if (scene->show_buffers)
		showBuffers();
	else
	{
		//Reset viewport
		glViewport(0, 0, window_size.x, window_size.y);

		//Final frame
		illumination_fbo->color_textures[0]->toViewport();
	}
	
}

//Upload Deferred scene uniforms to the shader
void GTR::Renderer::setDeferredSceneUniforms(Shader* shader)
{
	//Scene support variables
	Vector2 i_Res = Vector2(1.0 / window_size.x, 1.0 / window_size.y);
	Matrix44 inv_camera_vp = camera->viewprojection_matrix;
	inv_camera_vp.inverse(); //Pass the inverse projection of the camera to reconstruct world pos.

	//Scene uniforms
	shader->setUniform("u_light_model", scene->light_model);
	shader->setUniform("u_diffuse_reflection", scene->diffuse_reflection);
	shader->setUniform("u_geometry_shadowing", scene->smith_aproximation);
	shader->setUniform("u_light_pass", scene->light_pass);
	shader->setUniform("u_gamma_correction", scene->gamma_correction);
	shader->setUniform("u_tone_mapper", scene->tone_mapper.working);
	shader->setUniform("u_color_scale", scene->tone_mapper.color_scale);
	shader->setUniform("u_average_lum", scene->tone_mapper.avarage_illumination);
	shader->setUniform("u_white_lum", scene->tone_mapper.white_illumination);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_emissive_materials", scene->emissive_materials);
	shader->setMatrix44("u_viewprojection", camera->viewprojection_matrix);
	shader->setMatrix44("u_inverse_viewprojection", inv_camera_vp);
	shader->setUniform("u_iRes", i_Res); //Pass the inverse window resolution, this may be useful
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_time", (float)getTime());
	shader->setUniform("u_occlusion", scene->occlusion);
	shader->setUniform("u_specular_light", scene->specular_light);
	shader->setTexture("u_shadow_atlas", scene->shadow_atlas, 8);
	shader->setUniform("u_num_shadows", (float)scene->num_shadows);
	shader->setUniform("u_screen_width", window_size.x);
	shader->setUniform("u_screen_height", window_size.y);
	shader->setUniform("u_znear", camera->near_plane);
	shader->setUniform("u_zfar", camera->far_plane);
	shader->setUniform("u_fov", camera->fov);
	shader->setUniform("u_aspect_ratio", camera->aspect);

	//Upload textures
	shader->setTexture("u_gb0_texture", gbuffers_fbo->color_textures[0], 0);
	shader->setTexture("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setTexture("u_gb2_texture", gbuffers_fbo->color_textures[2], 2);
	shader->setTexture("u_depth_texture", gbuffers_fbo->depth_texture, 3);
}

//GBuffers
void GTR::Renderer::GBuffers()
{
	//Crete the gbuffers fbo if they don't exist yet
	if (!gbuffers_fbo || scene->resolution_trigger || scene->buffer_range_trigger)
	{
		if (gbuffers_fbo)
		{
			delete gbuffers_fbo;
			gbuffers_fbo = NULL;
		}

		//Create a new FBO
		gbuffers_fbo = new FBO();

		//Create three textures of four components
		gbuffers_fbo->create(window_size.x, window_size.y,
			3, 					//three textures
			GL_RGBA, 			//four channels
			buffer_range,		//SDR or HDR
			true);				//add depth_texture

	}

	//Get the gbuffers shader
	Shader* shader = Shader::Get("gbuffers");

	//no shader? then nothing to render
	if (!shader)
		return;

	//Start rendering inside the gbuffers fbo
	gbuffers_fbo->bind();

	//Clear all buffers
	clearGBuffers();

	//Enable the shader
	shader->enable();

	//Upload Scene Uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_time", (float)getTime());

	//Send render calls to the GPU
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		//If bounding box is inside the camera frustum then the prefab is probably visible
		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			if (rc->material->alpha_mode == BLEND)
			{
				//Add transparent objects to render them at the end of the pipeline
				transparent_objects.push_back(rc);
				continue;
			}

			renderGBuffers(shader, rc, camera);
		}
	}

	//Disable the shader
	shader->disable();

	//Stop rendering to the gbuffers fbo
	gbuffers_fbo->unbind();
}

//We clear in several passes so we can control the clear color independently for every gbuffer
void GTR::Renderer::clearGBuffers()
{
	//Set clear color
	glClearColor(0.0, 0.0, 0.0, 1.0);

	//Disable all but the GB0 and depth and clear them
	gbuffers_fbo->enableSingleBuffer(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//Now enable the second GB to clear it
	gbuffers_fbo->enableSingleBuffer(1);
	glClear(GL_COLOR_BUFFER_BIT);

	//Now enable the third GB to clear it
	gbuffers_fbo->enableSingleBuffer(2);
	glClear(GL_COLOR_BUFFER_BIT);

	//Enable all buffers back
	gbuffers_fbo->enableAllBuffers();
}

//Fill the GBuffers
void GTR::Renderer::renderGBuffers(Shader* shader, RenderCall* rc, Camera* camera)
{
	//In case there is nothing to do
	if (!rc->mesh || !rc->mesh->getNumVertices() || !rc->material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//Textures
	Texture* color_texture = NULL;
	Texture* emissive_texture = NULL;
	Texture* omr_texture = NULL;
	Texture* normal_texture = NULL;
	//Texture* occlusion_texture = NULL;

	//Texture loading
	color_texture = rc->material->color_texture.texture;
	emissive_texture = rc->material->emissive_texture.texture;
	omr_texture = rc->material->metallic_roughness_texture.texture;
	normal_texture = rc->material->normal_texture.texture;
	//occlusion_texture = rc->material->occlusion_texture.texture;

	//Texture check
	if (color_texture == NULL)	color_texture = Texture::getWhiteTexture();
	if (emissive_texture == NULL) emissive_texture = Texture::getBlackTexture();
	if (omr_texture == NULL) omr_texture = Texture::getWhiteTexture();

	//Normal mapping
	int entity_has_normal_map;
	if (scene->normal_mapping) entity_has_normal_map = (normal_texture == NULL) ? 0 : 1;
	else entity_has_normal_map = 0;

	//Select whether to render both sides of the triangles
	if (rc->material->two_sided) glDisable(GL_CULL_FACE);
	else glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//Upload textures
	shader->setTexture("u_color_texture", color_texture, 0);
	shader->setTexture("u_emissive_texture", emissive_texture, 1);
	shader->setTexture("u_omr_texture", omr_texture, 2);
	if (normal_texture) shader->setTexture("u_normal_texture", normal_texture, 3);

	//Upload prefab uniforms
	shader->setMatrix44("u_model", rc->model);
	shader->setUniform("u_color", rc->material->color);
	shader->setUniform("u_alpha_cutoff", rc->material->alpha_mode == GTR::eAlphaMode::MASK ? rc->material->alpha_cutoff : 0); //this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha).
	shader->setUniform("u_normal_mapping", entity_has_normal_map);
	shader->setUniform("u_emissive_factor", rc->material->emissive_factor);

	//Enable depth test
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);

	//Select whether to render both sides of the triangles
	if (rc->material->two_sided) glDisable(GL_CULL_FACE);
	else glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//Do the draw call that renders the mesh into the screen
	rc->mesh->render(GL_TRIANGLES);

	//Set the render state as it was before to avoid problems with future renders
	glDisable(GL_CULL_FACE);

}

//Computes illumination and transparent objects
void GTR::Renderer::IlluminationNTransparencies()
{
	//Crete the illumination fbo if they don't exist yet
	if (!illumination_fbo || scene->resolution_trigger)
	{
		if (illumination_fbo)
		{
			delete illumination_fbo;
			illumination_fbo = NULL;
		}

		//Create a new FBO
		illumination_fbo = new FBO();

		//Create one texture with RGB components
		illumination_fbo->create(window_size.x, window_size.y,
			2, 					//two texture
			GL_RGB, 			//three channels
			GL_UNSIGNED_BYTE,		//SDR or HDR
			true);				//add depth_texture
	}

	//Render camera depth map
	renderMainCameraShadowMap();

	//Start rendering inside the illumination fbo
	illumination_fbo->bind();

	//Clear all buffers
	clearIlluminationBuffers();

	//Copy the gbuffers depth texture to enable depth testing in illumination fbo
	gbuffers_fbo->depth_texture->copyTo(NULL);

	//Render lights into the ilumination fbo
	renderDeferredIllumination();

	//Render objects with blending in forward pipeline
	renderTransparentObjects();

	//Stop rendering to the illumination fbo
	illumination_fbo->unbind();
}

//We clear in several passes so we can control the clear color independently for every illumination buffer
void GTR::Renderer::clearIlluminationBuffers()
{
	//Set clear color
	glClearColor(0.0, 0.0, 0.0, 1.0);

	//Disable all but the GB0 and depth and clear them
	illumination_fbo->enableSingleBuffer(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//Now enable the second GB to clear it
	illumination_fbo->enableSingleBuffer(1);
	glClear(GL_COLOR_BUFFER_BIT);

	//Enable all buffers back
	illumination_fbo->enableAllBuffers();
}

//Render the deferred illumination
void GTR::Renderer::renderDeferredIllumination()
{
	switch (scene->light_pass)
	{
	case(Scene::Singlepass):
		renderQuadIllumination();
		break;
	case(Scene::Multipass):
		renderSphereIllumination();
		break;
	}
}

//Singlepass quad render
void GTR::Renderer::renderQuadIllumination()
{
	//Get shaders
	Shader* quad_shader = Shader::Get("deferred_illumination_quad");

	//no shader? then nothing to render
	if (!quad_shader)
		return;

	//Get mesh
	Mesh* quad = Mesh::getQuad();

	//Enable the shader
	quad_shader->enable();

	//Scene uniforms
	setDeferredSceneUniforms(quad_shader);

	//Disable blending for the first iteration
	glDisable(GL_BLEND);

	//Disable depth test as we are not going to test geometry
	glDisable(GL_DEPTH_TEST);

	//Block writing to the ZBuffer so we do not modify it with our geometry
	glDepthMask(false);

	//Compute lights
	SinglePassLoop(quad_shader, quad, lights);

	//Disable the shader
	quad_shader->disable();
}

//Multipass light volume render for point lights and spotlights and quad render for directional lights
void GTR::Renderer::renderSphereIllumination()
{
	//Create two lights vectors: One for point and spot lights and another one for directional lights
	vector<LightEntity*> points_n_spots;
	vector<LightEntity*> directionals;

	//Loop over the lights vector and assign lights
	int j = 0;
	int k = 0;
	for (auto it = lights.begin(); it != lights.end(); ++it)
	{
		//Current light
		LightEntity* light = *it;

		if (light->light_type == DIRECTIONAL)
		{
			directionals.push_back(light);
			j++;
		}
		else
		{
			points_n_spots.push_back(light);
			k++;
		}
	}

	/*
		SPHERE
	*/

	//Get Sphere shader
	Shader* sphere_shader = Shader::Get("deferred_illumination_sphere");

	//no shader? then nothing to render
	if (!sphere_shader)
		return;

	//Get meshes
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false);

	//Enable the shader
	sphere_shader->enable();

	//Scene uniforms
	setDeferredSceneUniforms(sphere_shader);

	//Don't add the emissive lights and the ambient light now
	sphere_shader->setUniform("u_ambient_light", Vector3());
	sphere_shader->setUniform("u_emissive_materials", 0);

	//Disable blending for the first iteration
	glDisable(GL_BLEND);

	//Enable depth test
	glEnable(GL_DEPTH_TEST);

	//Draw the inner part of the sphere
	glDepthFunc(GL_GREATER);

	//Block writing to the ZBuffer so we do not modify it with our geometry
	glDepthMask(false);

	//Compute lights
	MultiPassLoop(sphere_shader, sphere, points_n_spots);

	//Disable the shader
	sphere_shader->disable();

	/*
		QUAD
	*/

	//Get quad shader
	Shader* quad_shader = Shader::Get("deferred_illumination_quad");

	//no shader? then nothing to render
	if (!quad_shader)
		return;

	//Get quad
	Mesh* quad = Mesh::getQuad();

	//Enable the shader
	quad_shader->enable();

	//Scene uniforms
	setDeferredSceneUniforms(quad_shader);

	//Enable blending
	glEnable(GL_BLEND);

	//Blending function
	glBlendFunc(GL_ONE, GL_ONE);

	//Enable depth test
	glDisable(GL_DEPTH_TEST);

	//Block writing to the ZBuffer so we do not modify it with our geometry
	glDepthMask(false);

	//Compute lights
	MultiPassLoop(quad_shader, quad, directionals);

	//Disable the shader
	quad_shader->disable();

}

//Render objects with transparencies
void GTR::Renderer::renderTransparentObjects()
{

	//Get forward shader
	Shader* shader = Shader::Get("forward");

	//no shader? then nothing to render
	if (!shader)
		return;

	//Enable shader
	shader->enable();

	//Upload scene uniforms
	setForwardSceneUniforms(shader);

	//Enable depth test
	glEnable(GL_DEPTH_TEST);

	//Render transparent objects with the forward pipeline
	for (auto it = transparent_objects.begin(); it != transparent_objects.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		//If bounding box is inside the camera frustum then the prefab is probably visible
		if (camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderMesh(shader, rc, camera);
		}
	}

	//Disable shader
	shader->disable();

}

//Show the gbuffers and the illumination buffers
void GTR::Renderer::showBuffers()
{
	if (scene->show_buffers && gbuffers_fbo && gbuffers_fbo->num_color_textures > 0)
	{
		if (!scene->toggle_buffers)
		{
			//Regions
			Vector4 albedo_region = Vector4(0, window_size.y * 0.5, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 specular_region = Vector4(window_size.x * 0.5, window_size.y * 0.5, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 normal_region = Vector4(0, 0, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 result_region = Vector4(window_size.x * 0.5, 0, window_size.x * 0.5, window_size.y * 0.5);

			//Albedo screen
			glViewport(albedo_region.x, albedo_region.y, albedo_region.z, albedo_region.w);
			gbuffers_fbo->color_textures[0]->toViewport();

			//Normal screen
			glViewport(normal_region.x, normal_region.y, normal_region.z, normal_region.w);
			gbuffers_fbo->color_textures[1]->toViewport();

			//Specular screen
			glViewport(specular_region.x, specular_region.y, specular_region.z, specular_region.w);
			gbuffers_fbo->color_textures[2]->toViewport();

			//Result screen
			glViewport(result_region.x, result_region.y, result_region.z, result_region.w);
			illumination_fbo->color_textures[0]->toViewport();
		}
		else
		{
			//Regions
			Vector4 illumination_region = Vector4(0, window_size.y * 0.5, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 depth_region = Vector4(window_size.x * 0.5, window_size.y * 0.5, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 emissive_region = Vector4(0, 0, window_size.x * 0.5, window_size.y * 0.5);
			Vector4 result_region = Vector4(window_size.x * 0.5, 0, window_size.x * 0.5, window_size.y * 0.5);

			//Illumination region
			glViewport(illumination_region.x, illumination_region.y, illumination_region.z, illumination_region.w);
			illumination_fbo->color_textures[1]->toViewport();

			//Depth region
			Shader* shader = Shader::Get("linearize");
			if (shader)
			{
				shader->enable();
				shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
				glViewport(depth_region.x, depth_region.y, depth_region.z, depth_region.w);
				gbuffers_fbo->depth_texture->toViewport(shader);
				shader->disable();
			}

			//Emissive region
			glViewport(emissive_region.x, emissive_region.y, emissive_region.z, emissive_region.w);
			viewportEmissive();		

			//Result screen
			glViewport(result_region.x, result_region.y, result_region.z, result_region.w);
			illumination_fbo->color_textures[0]->toViewport();
		}

	}
}

//Viewport emissive texture from color, normal and omr textures
void GTR::Renderer::viewportEmissive()
{
	//Get the shader
	Shader* shader = Shader::Get("emissive");
	if (!shader)
		return;
	
	//Create a quead
	Mesh* quad = Mesh::getQuad();

	//Enable the shader
	shader->enable();
	assert(glGetError() == GL_NO_ERROR);

	//Upload uniforms
	shader->setTexture("u_color_texture", gbuffers_fbo->color_textures[0], 0);
	shader->setTexture("u_normal_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setTexture("u_omr_texture", gbuffers_fbo->color_textures[2], 2);

	//Disable OpenGL flags
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	//Render the quead
	quad->render(GL_TRIANGLES);
	assert(glGetError() == GL_NO_ERROR);

	//Disable the shader
	shader->disable();
}

// <--------------------------------------------------- Gamma correction --------------------------------------------------->

//Convert from gamma space to linear space
Vector3 GTR::Renderer::degamma(Vector3 color)
{
	return Vector3(pow(color.x, 2.2), pow(color.y, 2.2), pow(color.z, 2.2));
}

//Convert from linear space to gamma space
Vector3 GTR::Renderer::gamma(Vector3 color)
{
	float gamma_factor = 1.0 / 2.2;
	return Vector3(pow(color.x, gamma_factor), pow(color.y, gamma_factor), pow(color.z, gamma_factor));
}

// <--------------------------------------------------- Shadows --------------------------------------------------->

//Create or update a shadow atlas
void GTR::Renderer::updateShadowAtlas()
{
	//Compute the number of shadows of the scene and the shadow index of each light in the scene
	if (scene->shadow_visibility_trigger || scene->light_trigger)
	{
		scene->num_shadows = 0;
		int shadow_index = 0;

		//Iterate over light vector
		for (auto it = lights.begin(); it != lights.end(); ++it)
		{
			//Current light
			LightEntity* light = *it;

			if (light->cast_shadows)
			{
				//Assign a shadow slot for shadow atlas
				light->shadow_index = shadow_index;

				//Update
				scene->num_shadows++;
				shadow_index++;
			}
		}
	}

	//Set or update shadow resolution
	if (scene->shadow_resolution_trigger)
	{
		string shadow_resolution;
		shadow_resolution.assign(scene->shadow_resolutions[scene->atlas_resolution_index]);
		shadow_map_resolution = strtoul(shadow_resolution.substr(0, shadow_resolution.find_first_of(" ")).c_str(), NULL, 10);
		cout << "Resolution successfully changed" << endl;
	}

	//Create or update the shadow atlas
	if (scene->shadow_visibility_trigger || scene->shadow_resolution_trigger || scene->light_trigger)
	{
		//Delete the former atlas and continue
		if (shadow_fbo)
		{
			delete shadow_fbo;
			shadow_fbo = NULL;
			scene->shadow_atlas = NULL;

			if (scene->num_shadows == 0)
				return;

		}

		//New shadow atlas
		shadow_fbo = new FBO();
		shadow_fbo->setDepthOnly(shadow_map_resolution * scene->num_shadows, shadow_map_resolution); //We create it wide in order to save memory space and improve shadow map management
		scene->shadow_atlas = shadow_fbo->depth_texture;
	}

}

//Compute the shadow atlas for each light of the scene
void GTR::Renderer::computeShadowAtlas()
{
	if (shadow_fbo && scene->shadow_atlas)
	{
		//Iterate over light vector
		for (auto it = lights.begin(); it != lights.end(); ++it)
		{
			//Current light
			LightEntity* light = *it;

			//Booleans
			bool compute_spot = light->light_type == SPOT && light->cast_shadows && (scene->prefab_trigger || scene->light_trigger || scene->entity_trigger || light->spot_shadow_trigger || scene->shadow_visibility_trigger || scene->shadow_resolution_trigger);
			bool compute_directional = light->light_type == DIRECTIONAL && light->cast_shadows && (scene->prefab_trigger || scene->light_trigger || scene->entity_trigger || light->directional_shadow_trigger || scene->shadow_visibility_trigger || scene->shadow_resolution_trigger || camera->camera_trigger);

			//Shadow Maps
			if (compute_spot)
			{
				computeSpotShadowMap(light);
				light->spot_shadow_trigger = false;
			}
			if (compute_directional)
			{
				computeDirectionalShadowMap(light, camera);
				light->directional_shadow_trigger = false;
			}
		}
	}
}

//Compute spot shadow maps into the shadow atlas
void GTR::Renderer::computeSpotShadowMap(LightEntity* light)
{
	//Speed boost
	glColorMask(false, false, false, false);

	//For the first render
	if (!light->light_camera) light->light_camera = new Camera();

	//Bind the fbo
	shadow_fbo->bind();

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

	//Iterate over render calls vector
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		if (rc->material->alpha_mode == eAlphaMode::BLEND)
			continue;

		if (light_camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDepthMap(rc, light_camera);
		}
	}
	//Unbind the fbo
	shadow_fbo->unbind();

	//Reset
	glViewport(0, 0, window_size.x, window_size.y);
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
	shadow_fbo->bind();

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
	//Vector3 light_front = light->model.rotateVector(Vector3(0, 0, 1));
	float halfarea = light->area_size / 2;
	float camera_aspect = shadow_map_resolution / shadow_map_resolution;
	float camera_near = 0.1f;
	float camera_far = light->max_distance;
	float camera_factor = 0.5f;
	Vector3 camera_eye = camera->eye;
	Vector3 camera_center = camera->center - camera_factor * (camera->eye - camera->center);
	Vector3 camera_up = -1 * camera->up;

	//Orthographic Matrix
	light_camera->setOrthographic(-halfarea, halfarea, halfarea * camera_aspect, -halfarea * camera_aspect, camera_near, camera_far);

	//Set View Matrix
	light_camera->lookAt(camera_eye, camera_center, camera_up);

	//Enable camera
	light_camera->enable();

	//Iterate over render calls vector
	for (auto it = render_calls.begin(); it != render_calls.end(); ++it)
	{
		//Current render call
		RenderCall* rc = *it;

		if (rc->material->alpha_mode == eAlphaMode::BLEND)
			continue;

		if (light_camera->testBoxInFrustum(rc->world_bounding_box.center, rc->world_bounding_box.halfsize))
		{
			renderDepthMap(rc, light_camera);
		}
	}
	//Unbind the fbo
	shadow_fbo->unbind();

	//Reset
	glViewport(0, 0, window_size.x, window_size.y);
	glColorMask(true, true, true, true);
	glDisable(GL_SCISSOR_TEST);

}

//Render teh depth map
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
	shader->setMatrix44("u_model", rc->model);
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

//Print shadow map in the screen
void GTR::Renderer::showShadowAtlas()
{
	//Just in case
	if (!scene->shadow_atlas || scene->num_shadows == 0)
		return;
	
	//Some variables
	int num_shadows = scene->num_shadows;
	int num_shadows_per_scope = min(num_shadows, window_size.x / SHOW_ATLAS_RESOLUTION); //Number of shadow maps that will be displayed at the same time (within a scope).
	int shadow_scope = clamp(scene->atlas_scope,0,ceil(num_shadows / (float)num_shadows_per_scope) - 1); //Current shadow scope.
	int num_shadows_in_scope = min(num_shadows - shadow_scope * num_shadows_per_scope, num_shadows_per_scope); //Number of shadows in the current scope
	int shadow_offset = (window_size.x - num_shadows_in_scope * SHOW_ATLAS_RESOLUTION)/2; //In order to place the shadow maps in the center of the horizontal axis.
	int starting_shadow = shadow_scope * num_shadows_per_scope;
	int final_shadow = starting_shadow + num_shadows_in_scope;

	//Iterate over light vector
	for (auto it = lights.begin(); it != lights.end(); ++it)
	{
		//Current light
		LightEntity* light = *it;

		if (light->cast_shadows)
		{
			//Only render if lights are in the right scope
			if (starting_shadow <= light->shadow_index && light->shadow_index < final_shadow)
			{
				//Map shadow map into screen coordinates
				glViewport((light->shadow_index - starting_shadow) * SHOW_ATLAS_RESOLUTION + shadow_offset, 0, SHOW_ATLAS_RESOLUTION, SHOW_ATLAS_RESOLUTION);

				//Render the shadow map with the linearized shader
				Shader* shader = Shader::getDefaultShader("linearize_atlas");
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
	glViewport(0, 0, window_size.x, window_size.y);

}

// <--------------------------------------------------- Cubemap --------------------------------------------------->

//Cubemap texture
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
