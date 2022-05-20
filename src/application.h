/*  by Javi Agenjo 2013 UPF  javi.agenjo@gmail.com
	This class encapsulates the game, is in charge of creating the game, getting the user input, process the update and render.
*/

#ifndef APPLICATION_H
#define APPLICATION_H

#include "includes.h"
#include "camera.h"
#include "utils.h"


class Application
{
public:
	static Application* instance;

	//window
	SDL_Window* window;
	int window_width;
	int window_height;

	//some globals
	long frame;
    float time;
	float elapsed_time;
	int fps;
	bool must_exit;
	bool render_debug;
	bool render_grid;
	bool render_gui;
	bool render_editor;

	//some vars
	bool mouse_locked; //tells if the mouse is locked (blocked in the center and not visible)
	bool render_wireframe; //in case we want to render everything in wireframe mode
	const char* shadow_resolutions[4] = { "512 x 512","1024 x 1024", "2048 x 2048", "4096 x 4096" }; //Array of shadow resolutions

	//Entity creator
	int current_entity_type;
	const char* entity_types[2] = { "PREFAB","LIGHT" }; //Array of entity types for the entity creator

	//Prefab creation
	std::vector<std::string> assets;
	const char* current_asset;

	//Light creation
	int current_light_type;
	const char* light_types[3] = { "POINT","SPOT","DIRECTIONAL" };

	Application( int window_width, int window_height, SDL_Window* window );

	//main functions
	void render( void );
	void update( double dt );

	void renderDebugGUI(void);
	void renderEntityEditor();
	void renderDebugGizmo();

	//events
	void onKeyDown( SDL_KeyboardEvent event );
	void onKeyUp(SDL_KeyboardEvent event);
	void onMouseButtonDown( SDL_MouseButtonEvent event );
	void onMouseButtonUp(SDL_MouseButtonEvent event);
	void onMouseWheel(SDL_MouseWheelEvent event);
	void onGamepadButtonDown(SDL_JoyButtonEvent event);
	void onGamepadButtonUp(SDL_JoyButtonEvent event);
	void onResize(int width, int height);

};


#endif 