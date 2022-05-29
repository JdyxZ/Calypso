#include "application.h"
#include "utils.h"
#include "mesh.h"
#include "texture.h"

#include "fbo.h"
#include "shader.h"
#include "input.h"
#include "includes.h"
#include "prefab.h"
#include "gltf_loader.h"
#include "renderer.h"

#include <cmath>
#include <string>
#include <cstdio>
#include <iostream>
#include <filesystem>

using namespace std;

Application* Application::instance = nullptr;

Camera* camera = nullptr;
GTR::Scene* scene = nullptr;
GTR::Prefab* prefab = nullptr;
GTR::Renderer* renderer = nullptr;
GTR::BaseEntity* selected_entity = nullptr;
FBO* fbo = nullptr;
Texture* texture = nullptr;

float cam_speed = 10;
bool scene_saved = false;

Application::Application(int window_width, int window_height, SDL_Window* window)
{
	this->window_width = window_width;
	this->window_height = window_height;
	this->window = window;
	instance = this;
	must_exit = false;
	render_debug = true;
	render_grid = false;
	render_gui = true;
	render_editor = false;
	render_wireframe = false;

	fps = 0;
	frame = 0;
	time = 0.0f;
	elapsed_time = 0.0f;
	mouse_locked = false;

	//Compute assets vector
	string prefabs_path = filesystem::current_path().string() + "\\data\\prefabs";
	for (const auto& entry : filesystem::directory_iterator(prefabs_path))
	{
		string str_path = entry.path().string();
		string asset = str_path.substr(prefabs_path.size() + 1, str_path.size());
		assets.push_back(asset);
	};

	//Entity creator
	current_entity_type = 0;
	if (assets.size()) current_asset = assets[0].c_str();
	else current_asset = NULL;
	current_light_type = 0;


	//loads and compiles several shaders from one single file
    //change to "data/shader_atlas_osx.txt" if you are in XCODE
#ifdef __APPLE__
    const char* shader_atlas_filename = "data/shader_atlas_osx.txt";
#else
    const char* shader_atlas_filename = "data/shader_atlas.txt";
#endif
	if(!Shader::LoadAtlas(shader_atlas_filename))
        exit(1);
    checkGLErrors();

	// Create camera
	camera = new Camera();
	camera->lookAt(Vector3(-150.f, 150.0f, 250.f), Vector3(0.f, 0.0f, 0.f), Vector3(0.f, 1.f, 0.f));
	camera->setPerspective( 45.f, window_width/(float)window_height, 1.0f, 10000.f);

	//Create the scene and bind the main camera
	scene = new GTR::Scene();
	scene->main_camera = camera;

	//Load the scene JSON
	if (!scene->load("data/scene.json"))
		exit(1);

	//This class will be the one in charge of rendering all 
	renderer = new GTR::Renderer(scene, camera); //here so we have opengl ready in constructor

	//hide the cursor
	SDL_ShowCursor(!mouse_locked); //hide or show the mouse
}

//what to do when the image has to be draw
void Application::render(void)
{
	//be sure no errors present in opengl before start
	checkGLErrors();

	//set the camera as default (used by some functions in the framework)
	camera->enable();

	//set default flags
	glDisable(GL_BLEND);
    
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	if(render_wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	else
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	//lets render something
	//Matrix44 model;
	//renderer->renderPrefab( model, prefab, camera );

	//Render the scene
	renderer->renderScene();

	//Draw the floor grid, helpful to have a reference point
	if(render_grid)
		drawGrid();

    glDisable(GL_DEPTH_TEST);
    //render anything in the gui after this

	//the swap buffers is done in the main loop after this function
}

void Application::update(double seconds_elapsed)
{
	float speed = seconds_elapsed * cam_speed; //the speed is defined by the seconds_elapsed so it goes constant
	float orbit_speed = seconds_elapsed * 0.5;

	//Boost speed
	if (Input::isKeyPressed(SDL_SCANCODE_LSHIFT)) speed *= 10; //move faster with left shift

	//mouse input to rotate the cam
	#ifndef SKIP_IMGUI
	if (!ImGuizmo::IsUsing())
	#endif
	{
		if (mouse_locked || Input::mouse_state & SDL_BUTTON(SDL_BUTTON_RIGHT)) //move in first person view
		{
			camera->rotate(-Input::mouse_delta.x * orbit_speed * 0.5, Vector3(0, 1, 0));
			Vector3 right = camera->getLocalVector(Vector3(1, 0, 0));
			camera->rotate(-Input::mouse_delta.y * orbit_speed * 0.5, right);
			camera->camera_trigger = true;
		}
		else //orbit around center
		{
			bool mouse_blocked = false;
			#ifndef SKIP_IMGUI
						mouse_blocked = ImGui::IsAnyWindowHovered() || ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
			#endif
			if (Input::mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT) && !mouse_blocked) //is left button pressed?
			{
				camera->orbit(-Input::mouse_delta.x * orbit_speed, Input::mouse_delta.y * orbit_speed);
				camera->camera_trigger = true;
			}
		}
	}

	//Move camera using WASD controls
	if (Input::isKeyPressed(SDL_SCANCODE_W)) camera->move(Vector3(0.0f, 0.0f, 1.0f) * speed), camera->camera_trigger = true;
	if (Input::isKeyPressed(SDL_SCANCODE_S) && !Input::isKeyPressed(SDL_SCANCODE_LCTRL)) camera->move(Vector3(0.0f, 0.0f, -1.0f) * speed), camera->camera_trigger = true;
	if (Input::isKeyPressed(SDL_SCANCODE_A)) camera->move(Vector3(1.0f, 0.0f, 0.0f) * speed), camera->camera_trigger = true;
	if (Input::isKeyPressed(SDL_SCANCODE_D)) camera->move(Vector3(-1.0f, 0.0f, 0.0f) * speed), camera->camera_trigger = true;
	
	//Move up or down the camera using Q and E
	if (Input::isKeyPressed(SDL_SCANCODE_Q)) camera->moveGlobal(Vector3(0.0f, -1.0f, 0.0f) * speed), camera->camera_trigger = true;
	if (Input::isKeyPressed(SDL_SCANCODE_E)) camera->moveGlobal(Vector3(0.0f, 1.0f, 0.0f) * speed), camera->camera_trigger = true;

	//to navigate with the mouse fixed in the middle
	SDL_ShowCursor(!mouse_locked);
	#ifndef SKIP_IMGUI
		ImGui::SetMouseCursor(mouse_locked ? ImGuiMouseCursor_None : ImGuiMouseCursor_Arrow);
	#endif
	if (mouse_locked)
	{
		Input::centerMouse();
		//ImGui::SetCursorPos(ImVec2(Input::mouse_position.x, Input::mouse_position.y));
	}

	if (Input::isKeyPressed(SDL_SCANCODE_LCTRL) && Input::isKeyPressed(SDL_SCANCODE_S))
	{
		if (!scene_saved)
		{
			scene->save();
			scene_saved = true;
		}
	}

}

void Application::renderDebugGizmo()
{
	if (!selected_entity || !render_debug)
		return;

	//example of matrix we want to edit, change this to the matrix of your entity
	Matrix44& matrix = selected_entity->model;

	#ifndef SKIP_IMGUI

	static ImGuizmo::OPERATION mCurrentGizmoOperation(ImGuizmo::TRANSLATE);
	static ImGuizmo::MODE mCurrentGizmoMode(ImGuizmo::WORLD);
	if (ImGui::IsKeyPressed(90))
		mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
	if (ImGui::IsKeyPressed(69))
		mCurrentGizmoOperation = ImGuizmo::ROTATE;
	if (ImGui::IsKeyPressed(82)) // r Key
		mCurrentGizmoOperation = ImGuizmo::SCALE;
	if (ImGui::RadioButton("Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE))
		mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
	ImGui::SameLine();
	if (ImGui::RadioButton("Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE))
		mCurrentGizmoOperation = ImGuizmo::ROTATE;
	ImGui::SameLine();
	if (ImGui::RadioButton("Scale", mCurrentGizmoOperation == ImGuizmo::SCALE))
		mCurrentGizmoOperation = ImGuizmo::SCALE;
	float matrixTranslation[3], matrixRotation[3], matrixScale[3];
	ImGuizmo::DecomposeMatrixToComponents(matrix.m, matrixTranslation, matrixRotation, matrixScale);
	ImGui::InputFloat3("Tr", matrixTranslation, 3);
	ImGui::InputFloat3("Rt", matrixRotation, 3);
	ImGui::InputFloat3("Sc", matrixScale, 3);
	ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, matrix.m);

	if (mCurrentGizmoOperation != ImGuizmo::SCALE)
	{
		if (ImGui::RadioButton("Local", mCurrentGizmoMode == ImGuizmo::LOCAL))
			mCurrentGizmoMode = ImGuizmo::LOCAL;
		ImGui::SameLine();
		if (ImGui::RadioButton("World", mCurrentGizmoMode == ImGuizmo::WORLD))
			mCurrentGizmoMode = ImGuizmo::WORLD;
	}
	static bool useSnap(false);
	if (ImGui::IsKeyPressed(83))
		useSnap = !useSnap;
	ImGui::Checkbox("", &useSnap);
	ImGui::SameLine();
	static Vector3 snap;
	switch (mCurrentGizmoOperation)
	{
	case ImGuizmo::TRANSLATE:
		//snap = config.mSnapTranslation;
		ImGui::InputFloat3("Snap", &snap.x);
		break;
	case ImGuizmo::ROTATE:
		//snap = config.mSnapRotation;
		ImGui::InputFloat("Angle Snap", &snap.x);
		break;
	case ImGuizmo::SCALE:
		//snap = config.mSnapScale;
		ImGui::InputFloat("Scale Snap", &snap.x);
		break;
	}
	ImGuiIO& io = ImGui::GetIO();
	ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

	if(selected_entity->visible)
		ImGuizmo::Manipulate(camera->view_matrix.m, camera->projection_matrix.m, mCurrentGizmoOperation, mCurrentGizmoMode, matrix.m, NULL, useSnap ? &snap.x : NULL, 0,0, &scene->entity_trigger);
	else
		ImGuizmo::Manipulate(camera->view_matrix.m, camera->projection_matrix.m, mCurrentGizmoOperation, mCurrentGizmoMode, matrix.m, NULL, useSnap ? &snap.x : NULL, 0, 0);

	#endif
}

//called to render the GUI from
void Application::renderDebugGUI(void)
{
#ifndef SKIP_IMGUI //to block this code from compiling if we want

	//System stats
	ImGui::Text(getGPUStats().c_str());					   // Display some text (you can use a format strings too)

	//Scene algorithms
	ImGui::Checkbox("Entity creator", &render_editor);
	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Grid", &render_grid);
	ImGui::Checkbox("Alpha sorting", &scene->alpha_sorting);
	ImGui::Checkbox("Emissive materials", &scene->emissive_materials);
	ImGui::Checkbox("Occlussion texture", &scene->occlusion);
	ImGui::Checkbox("Specular light", &scene->specular_light);
	ImGui::Checkbox("Normal map", &scene->normal_mapping);
	ImGui::Checkbox("Shadow atlas", &scene->show_atlas);
	ImGui::Checkbox("Shadow sorting", &scene->shadow_sorting);

	//Shadow resolution
	scene->shadow_resolution_trigger = ImGui::Combo("Shadow Resolution", &scene->atlas_resolution_index, shadow_resolutions, IM_ARRAYSIZE(shadow_resolutions));

	//Render type
	switch (scene->render_type) {
		case(GTR::Singlepass): ImGui::SliderInt("Render Type", &scene->render_type, GTR::Singlepass, GTR::Multipass, "SinglePass"); break;
		case(GTR::Multipass): ImGui::SliderInt("Render Type", &scene->render_type, GTR::Singlepass, GTR::Multipass, "Multipass"); break;
	}

	//Scene Color
	ImGui::ColorEdit3("Background color", scene->background_color.v);
	ImGui::ColorEdit3("Ambient Light", scene->ambient_light.v);

	//add info to the debug panel about the camera
	if (ImGui::TreeNode(camera, "Camera")) {
		camera->renderInMenu();
		ImGui::TreePop();
	}

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));

	//example to show prefab info: first param must be unique!
	for (int i = 0; i < scene->entities.size(); ++i)
	{
		GTR::BaseEntity* entity = scene->entities[i];

		if(selected_entity == entity)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.0f));

		if (ImGui::TreeNode(entity, entity->name.c_str()))
		{
			entity->renderInMenu();
			ImGui::TreePop();
		}

		if (selected_entity == entity)
			ImGui::PopStyleColor();

		if (ImGui::IsItemClicked(0))
			selected_entity = entity;
	}

	ImGui::PopStyleColor();
#endif
}

void Application::renderEntityEditor()
{
#ifndef SKIP_IMGUI //to block this code from compiling if we want

	//Select entity
	ImGui::Combo("Entity type", &current_entity_type, entity_types, IM_ARRAYSIZE(entity_types));
	
	//Entity features
	if (current_entity_type)
	{
		ImGui::Combo("Light type", &current_light_type, light_types, IM_ARRAYSIZE(light_types));
		bool create_light = ImGui::Button("Create");

		if (create_light)
		{
			GTR::LightEntity* new_light = new GTR::LightEntity(GTR::LightType(current_light_type));
			new_light->model.translate(camera->center.x, camera->center.y, camera->center.z);
			if (new_light->light_type == GTR::LightType::POINT) new_light->name = scene->nameEntity(string("point light"));
			else if (new_light->light_type == GTR::LightType::SPOT) new_light->name = scene->nameEntity(string("spotlight"));
			else if (new_light->light_type == GTR::LightType::DIRECTIONAL) new_light->name = scene->nameEntity(string("directional light"));
			scene->addEntity(new_light);
			scene->light_trigger = true;
		}
	}
	else
	{

		if (ImGui::BeginCombo("Assets", current_asset))
		{
			for (int i = 0; i < assets.size(); ++i)
			{
				bool is_selected = (current_asset == assets[i].c_str());
				if (ImGui::Selectable(assets[i].c_str(), is_selected))
					current_asset = assets[i].c_str();
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		bool create_prefab = ImGui::Button("Create");

		if (create_prefab && current_asset)
		{
			GTR::PrefabEntity* new_prefab = new GTR::PrefabEntity("prefabs/" + string(current_asset) + "/scene.gltf");
			new_prefab->model.translate(camera->center.x, camera->center.y, camera->center.z);
			new_prefab->name = scene->nameEntity(string(current_asset));
			scene->addEntity(new_prefab);
			scene->prefab_trigger = true;
		}
	}

#endif
}

//Keyboard event handler (sync input)
void Application::onKeyDown( SDL_KeyboardEvent event )
{
	switch(event.keysym.sym)
	{
		case SDLK_ESCAPE: must_exit = true; break; //ESC key, kill the app
		case SDLK_F1: render_debug = !render_debug; break;
		case SDLK_F2:  camera->center.set(0, 0, 0); camera->updateViewMatrix(); camera->camera_trigger = true; break;
		case SDLK_F5: Shader::ReloadAll(); break;
		case SDLK_F6:
			scene->clear();
			scene->load(scene->filename.c_str());
			camera->lookAt(Vector3(-150.f, 150.0f, 250.f), Vector3(0.f, 0.0f, 0.f), Vector3(0.f, 1.f, 0.f));
			camera->setPerspective(45.f, window_width / (float)window_height, 1.0f, 10000.f);
			camera->camera_trigger = true;
			break;
		case SDLK_LEFT:
			scene->atlas_scope--;
			break;
		case SDLK_RIGHT:
			scene->atlas_scope++;
			break;
		case SDLK_a:
			io->AddInputCharacter('a');
			break;
		case SDLK_b:
			io->AddInputCharacter('b');
			break;
		case SDLK_c:
			io->AddInputCharacter('c');
			break;
		case SDLK_d:
			io->AddInputCharacter('d');
			break;
		case SDLK_e:
			io->AddInputCharacter('e');
			break;
		case SDLK_f:
			io->AddInputCharacter('f');
			break;
		case SDLK_g:
			io->AddInputCharacter('g');
			break;
		case SDLK_h:
			io->AddInputCharacter('h');
			break;
		case SDLK_i:
			io->AddInputCharacter('i');
			break;
		case SDLK_j:
			io->AddInputCharacter('j');
			break;
		case SDLK_k:
			io->AddInputCharacter('k');
			break;
		case SDLK_l:
			io->AddInputCharacter('l');
			break;
		case SDLK_m:
			io->AddInputCharacter('m');
			break;
		case SDLK_n:
			io->AddInputCharacter('n');
			break;
		case SDLK_o:
			io->AddInputCharacter('o');
			break;
		case SDLK_p:
			io->AddInputCharacter('p');
			break;
		case SDLK_q:
			io->AddInputCharacter('q');
			break;
		case SDLK_r:
			io->AddInputCharacter('r');
			break;
		case SDLK_s:
			io->AddInputCharacter('s');
			break;
		case SDLK_t:
			io->AddInputCharacter('t');
			break;
		case SDLK_u:
			io->AddInputCharacter('u');
			break;
		case SDLK_v:
			io->AddInputCharacter('v');
			break;
		case SDLK_w:
			io->AddInputCharacter('w');
			break;
		case SDLK_x:
			io->AddInputCharacter('x');
			break;
		case SDLK_y:
			io->AddInputCharacter('y');
			break;
		case SDLK_z:
			io->AddInputCharacter('z');
			break;
		case SDLK_0:
			io->AddInputCharacter('0');
			break;
		case SDLK_1:
			io->AddInputCharacter('1');
			break;
		case SDLK_2:
			io->AddInputCharacter('2');
			break;
		case SDLK_3:
			io->AddInputCharacter('3');
			break;
		case SDLK_4:
			io->AddInputCharacter('4');
			break;
		case SDLK_5:
			io->AddInputCharacter('5');
			break;
		case SDLK_6:
			io->AddInputCharacter('6');
			break;
		case SDLK_7:
			io->AddInputCharacter('7');
			break;
		case SDLK_8:
			io->AddInputCharacter('8');
			break;
		case SDLK_9:
			io->AddInputCharacter('9');
			break;
		case SDLK_BACKSPACE:
			memset(scene->buffer, strlen(scene->buffer) - 1, strlen(scene->buffer));
			cout << strlen(scene->buffer) << endl;
			break;
	}
}

void Application::onKeyUp(SDL_KeyboardEvent event)
{
	switch (event.keysym.sym)
	{
	case SDLK_LCTRL:
		scene_saved = false;
	}
}

void Application::onGamepadButtonDown(SDL_JoyButtonEvent event)
{

}

void Application::onGamepadButtonUp(SDL_JoyButtonEvent event)
{

}

void Application::onMouseButtonDown( SDL_MouseButtonEvent event )
{
	if (event.button == SDL_BUTTON_MIDDLE) //middle mouse
	{
		//Input::centerMouse();
		mouse_locked = !mouse_locked;
		SDL_ShowCursor(!mouse_locked);
	}
}

void Application::onMouseButtonUp(SDL_MouseButtonEvent event)
{
}

void Application::onMouseWheel(SDL_MouseWheelEvent event)
{
	bool mouse_blocked = false;

	#ifndef SKIP_IMGUI
		ImGuiIO& io = ImGui::GetIO();
		if(!mouse_locked)
		switch (event.type)
		{
			case SDL_MOUSEWHEEL:
			{
				if (event.x > 0) io.MouseWheelH += 1;
				if (event.x < 0) io.MouseWheelH -= 1;
				if (event.y > 0) io.MouseWheel += 1;
				if (event.y < 0) io.MouseWheel -= 1;
			}
		}
		mouse_blocked = ImGui::IsAnyWindowHovered();
	#endif

	if (!mouse_blocked && event.y)
	{
		if (mouse_locked)
			cam_speed *= 1 + (event.y * 0.1);
		else
			camera->changeDistance(event.y * 0.5);
	}
}

void Application::onResize(int width, int height)
{
    std::cout << "window resized: " << width << "," << height << std::endl;
	glViewport( 0,0, width, height );
	camera->aspect =  width / (float)height;
	window_width = width;
	window_height = height;
}

