#pragma comment(lib,"openxr_loader-0_90.lib")
#pragma comment(lib,"D3D11.lib")
#pragma comment(lib,"D3dcompiler.lib") // for shader compile

// Tell OpenXR what platform code we'll be using
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

// Our hooks for specifying what API we're building for
#define APP_SWAPCHAINTYPE XrSwapchainImageD3D11KHR
#define APP_SWAPCHAIN_TYPE_ID XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR
#define APP_GRAPHICS_EXTENSION_NAME XR_KHR_D3D11_ENABLE_EXTENSION_NAME

#include <d3d11.h>
#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <thread> // sleep_for
#include <vector>

using namespace std;
using namespace DirectX; // Matrix math

///////////////////////////////////////////

struct swapchain_surfdata_t {
	ID3D11DepthStencilView *depth_view;
	ID3D11RenderTargetView *target_view;
};

struct swapchain_t {
	XrSwapchain handle;
	int32_t     width;
	int32_t     height;
	vector<APP_SWAPCHAINTYPE>    surface_images;
	vector<swapchain_surfdata_t> surface_data;
};

struct input_state_t {
	XrActionSet actionSet;
	XrAction    poseAction;
	XrAction    selectAction;
	XrPath   handSubactionPath[2];
	XrSpace  handSpace[2];
	XrPosef  handPose[2];
	XrBool32 renderHand[2];
	XrBool32 handSelect[2];
};

///////////////////////////////////////////

struct app_transform_buffer_t {
	XMFLOAT4X4 world;
	XMFLOAT4X4 viewproj;
};

XrFormFactor            app_config_form = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
XrViewConfigurationType app_config_view = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

ID3D11VertexShader *app_vshader;
ID3D11PixelShader  *app_pshader;
ID3D11InputLayout  *app_shader_layout;
ID3D11Buffer       *app_constant_buffer;
ID3D11Buffer       *app_vertex_buffer;
ID3D11Buffer       *app_index_buffer;

vector<XrPosef> app_cubes;

void app_init  ();
void app_draw  (XrCompositionLayerProjectionView &layerView);
void app_update();
void app_update_predicted();

///////////////////////////////////////////

const XrPosef  xr_pose_identity = { {0,0,0,1}, {0,0,0} };
XrInstance     xr_instance      = {};
XrSession      xr_session       = {};
XrSessionState xr_session_state = XR_SESSION_STATE_UNKNOWN;
XrSpace        xr_app_space     = {};
XrSystemId     xr_system_id     = XR_NULL_SYSTEM_ID;
input_state_t  xr_input         = { };
XrEnvironmentBlendMode xr_blend;

vector<XrView>                  xr_views;
vector<XrViewConfigurationView> xr_config_views;
vector<swapchain_t>             xr_swapchains;

bool openxr_init          (const char *app_name, XrBaseInStructure *gfx_binding, int64_t swapchain_format);
void openxr_make_actions  ();
void openxr_shutdown      ();
void openxr_poll_events   (bool &exit);
void openxr_poll_actions  ();
void openxr_poll_predicted(XrTime predicted_time);
void openxr_render_frame  ();
bool openxr_render_layer  (XrTime predictedTime, vector<XrCompositionLayerProjectionView> &projectionViews, XrCompositionLayerProjection &layer);

///////////////////////////////////////////

ID3D11Device             *d3d_device        = nullptr;
ID3D11DeviceContext      *d3d_context       = nullptr;
int64_t                   d3d_swapchain_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
XrGraphicsBindingD3D11KHR d3d_binding       = {};

XrBaseInStructure   *d3d_init             ();
void                 d3d_shutdown         ();
swapchain_surfdata_t d3d_make_surface_data(XrBaseInStructure &swapchainImage);
void                 d3d_render_layer     (XrCompositionLayerProjectionView &layerView, swapchain_surfdata_t &surface);
void                 d3d_swapchain_destroy(swapchain_t &swapchain);
XMMATRIX             d3d_xr_projection    (XrFovf fov, float clip_near, float clip_far);
ID3DBlob            *d3d_compile_shader   (const char* hlsl, const char* entrypoint, const char* target);

///////////////////////////////////////////

constexpr char app_shader_code[] = R"_(
cbuffer TransformBuffer : register(b0) {
	float4x4 world;
	float4x4 viewproj;
};
struct vsIn {
	float4 pos  : SV_POSITION;
	float3 norm : NORMAL;
};
struct psIn {
	float4 pos   : SV_POSITION;
	float3 color : COLOR0;
};

psIn vs(vsIn input) {
	psIn output;
	output.pos = mul(float4(input.pos.xyz, 1), world);
	output.pos = mul(output.pos, viewproj);

	float3 normal = normalize(mul(float4(input.norm, 0), world).xyz);

	output.color = saturate(dot(normal, float3(0,1,0))).xxx;
	return output;
}
float4 ps(psIn input) : SV_TARGET {
	return float4(input.color, 1);
})_";

float app_verts[] = {
	-1,-1,-1, -1,-1,-1, // Bottom verts
	 1,-1,-1,  1,-1,-1,
	 1, 1,-1,  1, 1,-1,
	-1, 1,-1, -1, 1,-1,
	-1,-1, 1, -1,-1, 1, // Top verts
	 1,-1, 1,  1,-1, 1,
	 1, 1, 1,  1, 1, 1,
	-1, 1, 1, -1, 1, 1, };

uint16_t app_inds[] = {
	1,2,0, 2,3,0, 4,6,5, 7,6,4,
	6,2,1, 5,6,1, 3,7,4, 0,3,4,
	4,5,1, 0,4,1, 2,7,3, 2,6,7, };

///////////////////////////////////////////
// Main                                  //
///////////////////////////////////////////

int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
	XrBaseInStructure *binding = d3d_init();
	if (!openxr_init("Single file OpenXR", binding, d3d_swapchain_fmt)) {
		d3d_shutdown();
		MessageBox(nullptr, "OpenXR initialization failed\n", "Error", 1);
		return 1;
	}
	openxr_make_actions();
	app_init();

	bool quit = false;
	while (!quit) {
		openxr_poll_events(quit);

		if (xr_session_state == XR_SESSION_STATE_VISIBLE || xr_session_state == XR_SESSION_STATE_FOCUSED || xr_session_state == XR_SESSION_STATE_RUNNING) {
			openxr_poll_actions();
			app_update();
			openxr_render_frame();
		} else {
			this_thread::sleep_for(chrono::milliseconds(250));
		}
	}

	openxr_shutdown();
	d3d_shutdown();
	return 0;
}

///////////////////////////////////////////
// OpenXR code                           //
///////////////////////////////////////////

bool openxr_init(const char *app_name, XrBaseInStructure *gfx_binding, int64_t swapchain_format) {
	const char          *extensions[] = { APP_GRAPHICS_EXTENSION_NAME };
	XrInstanceCreateInfo createInfo   = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.enabledExtensionCount      = _countof(extensions);
	createInfo.enabledExtensionNames      = extensions;
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	strcpy_s(createInfo.applicationInfo.applicationName, 128, app_name);
	xrCreateInstance(&createInfo, &xr_instance);

	// Check if OpenXR is on this system, if this is null here, the user needs to install an
	// OpenXR runtime and ensure it's active!
	if (xr_instance == nullptr)
		return false;

	// Request a form factor from the device (HMD, Handheld, etc.)
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = app_config_form;
	xrGetSystem(xr_instance, &systemInfo, &xr_system_id);

	// Check what blend mode is valid for this device (opaque vs transparent displays)
	// We'll just take the first one available!
	uint32_t                       blend_count = 0;
	vector<XrEnvironmentBlendMode> blend_modes;
	xrEnumerateEnvironmentBlendModes(xr_instance, xr_system_id, 0, &blend_count, nullptr);
	blend_modes.resize(blend_count);
	xrEnumerateEnvironmentBlendModes(xr_instance, xr_system_id, blend_count, &blend_count, blend_modes.data());
	for (size_t i = 0; i < blend_count; i++) {
		if (blend_modes[i] == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE || blend_modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
			xr_blend = blend_modes[i];
			break;
		}
	}

	// A session represents this application's desire to display things! This is where we hook up our graphics API.
	// This does not start the session, for that, you'll need a call to xrBeginSession, which we do in openxr_poll_events
	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next     = (const void*)gfx_binding;
	sessionInfo.systemId = xr_system_id;
	xrCreateSession(xr_instance, &sessionInfo, &xr_session);

	// Unable to start a session, may not have an MR device attached or ready
	if (xr_session == nullptr)
		return false;

	// OpenXR uses a couple different types of reference frames for positioning content, we need to choose one for
	// displaying our content! STAGE would be relative to the center of your guardian system's bounds, and LOCAL
	// would be relative to your device's starting location. HoloLens doesn't have a STAGE, so we'll use LOCAL.
	XrReferenceSpaceCreateInfo ref_space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	ref_space.poseInReferenceSpace = xr_pose_identity;
	ref_space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
	xrCreateReferenceSpace(xr_session, &ref_space, &xr_app_space);

	// Now we need to find all the viewpoints we need to take care of! For a stereo headset, this should be 2.
	// Similarly, for an AR phone, we'll need 1, and a VR cave could have 6, or even 12!
	uint32_t view_count = 0;
	xrEnumerateViewConfigurationViews(xr_instance, xr_system_id, app_config_view, 0, &view_count, nullptr);
	xr_config_views.resize(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	xr_views       .resize(view_count, { XR_TYPE_VIEW });
	xrEnumerateViewConfigurationViews(xr_instance, xr_system_id, app_config_view, view_count, &view_count, xr_config_views.data());
	for (uint32_t i = 0; i < view_count; i++) {
		// Create a swapchain for this viewpoint! A swapchain is a set of texture buffers used for displaying to screen,
		// typically this is a backbuffer and a front buffer, one for rendering data to, and one for displaying on-screen.
		XrViewConfigurationView &view           = xr_config_views[i];
		XrSwapchainCreateInfo    swapchain_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		XrSwapchain              handle;
		swapchain_info.arraySize   = 1;
		swapchain_info.mipCount    = 1;
		swapchain_info.faceCount   = 1;
		swapchain_info.format      = swapchain_format;
		swapchain_info.width       = view.recommendedImageRectWidth;
		swapchain_info.height      = view.recommendedImageRectHeight;
		swapchain_info.sampleCount = view.recommendedSwapchainSampleCount;
		swapchain_info.usageFlags  = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		xrCreateSwapchain(xr_session, &swapchain_info, &handle);

		// Find out how many textures were generated for the swapchain
		uint32_t surface_count = 0;
		xrEnumerateSwapchainImages(handle, 0, &surface_count, nullptr);

		// We'll want to track our own information about the swapchain, so we can draw stuff onto it! We'll also create
		// a depth buffer for each generated texture here as well with make_surfacedata.
		swapchain_t swapchain = {};
		swapchain.width  = swapchain_info.width;
		swapchain.height = swapchain_info.height;
		swapchain.handle = handle;
		swapchain.surface_images.resize(surface_count, { APP_SWAPCHAIN_TYPE_ID } );
		swapchain.surface_data  .resize(surface_count);
		xrEnumerateSwapchainImages(swapchain.handle, surface_count, nullptr, (XrSwapchainImageBaseHeader*)swapchain.surface_images.data());
		for (uint32_t i = 0; i < surface_count; i++) {
			swapchain.surface_data[i] = d3d_make_surface_data((XrBaseInStructure&)swapchain.surface_images[i]);
		}
		xr_swapchains.push_back(swapchain);
	}

	return true;
}

///////////////////////////////////////////

void openxr_make_actions() {
	XrActionSetCreateInfo actionset_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(actionset_info.actionSetName,          "gameplay");
	strcpy_s(actionset_info.localizedActionSetName, "Gameplay");
	xrCreateActionSet(xr_session, &actionset_info, &xr_input.actionSet);
	xrStringToPath(xr_instance, "/user/hand/left",  &xr_input.handSubactionPath[0]);
	xrStringToPath(xr_instance, "/user/hand/right", &xr_input.handSubactionPath[1]);

	// Create an action to track the position and orientation of the hands! This is
	// the controller location, or the center of the palms for actual hands.
	XrActionCreateInfo action_info = { XR_TYPE_ACTION_CREATE_INFO };
	action_info.countSubactionPaths = _countof(xr_input.handSubactionPath);
	action_info.subactionPaths      = xr_input.handSubactionPath;
	action_info.actionType          = XR_INPUT_ACTION_TYPE_POSE;
	strcpy_s(action_info.actionName,          "hand_pose");
	strcpy_s(action_info.localizedActionName, "Hand Pose");
	xrCreateAction(xr_input.actionSet, &action_info, &xr_input.poseAction);

	// Create an action for listening to the select action! This is primary trigger
	// on controllers, and an airtap on HoloLens
	action_info.actionType = XR_INPUT_ACTION_TYPE_BOOLEAN;
	strcpy_s(action_info.actionName,          "select");
	strcpy_s(action_info.localizedActionName, "Select");
	xrCreateAction(xr_input.actionSet, &action_info, &xr_input.selectAction);

	// Bind the actions we just created to specific locations on the Khronos simple_controller
	// definition! These are labeled as 'suggested' because they may be overridden by the runtime
	// preferences. For example, if the runtime allows you to remap buttons, or provides input
	// accessibility settings.
	XrPath profile_path;
	XrPath pose_path  [2];
	XrPath select_path[2];
	xrStringToPath(xr_instance, "/user/hand/left/input/palm/pose",     &pose_path[0]);
	xrStringToPath(xr_instance, "/user/hand/right/input/palm/pose",    &pose_path[1]);
	xrStringToPath(xr_instance, "/user/hand/left/input/select/click",  &select_path[0]);
	xrStringToPath(xr_instance, "/user/hand/right/input/select/click", &select_path[1]);
	xrStringToPath(xr_instance, "/interaction_profiles/khr/simple_controller", &profile_path);
	XrActionSuggestedBinding bindings[] = {
		{ xr_input.poseAction,   pose_path[0]   },
		{ xr_input.poseAction,   pose_path[1]   },
		{ xr_input.selectAction, select_path[0] },
		{ xr_input.selectAction, select_path[1] }, };
	XrInteractionProfileSuggestedBinding suggested_binds = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested_binds.interactionProfile     = profile_path;
	suggested_binds.suggestedBindings      = &bindings[0];
	suggested_binds.countSuggestedBindings = _countof(bindings);
	xrSetInteractionProfileSuggestedBindings(xr_session, &suggested_binds);

	// Create frames of reference for the pose actions
	for (int32_t i = 0; i < 2; i++) {
		XrActionSpaceCreateInfo action_space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		action_space_info.poseInActionSpace = xr_pose_identity;
		action_space_info.subactionPath     = xr_input.handSubactionPath[i];
		xrCreateActionSpace(xr_input.poseAction, &action_space_info, &xr_input.handSpace[i]);
	}
}

///////////////////////////////////////////

void openxr_shutdown() {
	// We used a graphics API to initialize the swapchain data, so we'll
	// give it a chance to release anythig here!
	for (int32_t i = 0; i < xr_swapchains.size(); i++) {
		d3d_swapchain_destroy(xr_swapchains[i]);
	}
	xr_swapchains.clear();
}

///////////////////////////////////////////

void openxr_poll_events(bool &exit) {
	exit = false;

	// This object is pretty large, making it static so we only need to create it once
	static XrEventDataBuffer event_buffer;
	// Only the data header needs cleared out each time
	event_buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	event_buffer.next = nullptr;

	while (xrPollEvent(xr_instance, &event_buffer) == XR_SUCCESS) {
		switch (event_buffer.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged*)&event_buffer;
			xr_session_state = changed->state;

			// Session state change is where we can begin and end sessions, as well as find quit messages!
			switch (xr_session_state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin_info = { XR_TYPE_SESSION_BEGIN_INFO };
				begin_info.primaryViewConfigurationType = app_config_view;
				xrBeginSession(xr_session, &begin_info);
			} break;
			case XR_SESSION_STATE_STOPPING:     xrEndSession(xr_session); break;
			case XR_SESSION_STATE_EXITING:      exit = true;              break;
			case XR_SESSION_STATE_LOSS_PENDING: exit = true;              break;
			}
		} break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: exit = true; return;
		}
	}
}

///////////////////////////////////////////

void openxr_poll_actions() {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	// Update our action set with up-to-date input data!
	XrActiveActionSet action_set = { XR_TYPE_ACTIVE_ACTION_SET };
	action_set.actionSet     = xr_input.actionSet;
	action_set.subactionPath = XR_NULL_PATH;
	xrSyncActionData(xr_session, 1, &action_set);

	// Now we'll get the current states of our actions, and store them for later use
	for (uint32_t hand = 0; hand < 2; hand++) {
		XrActionStatePose pose_state = { XR_TYPE_ACTION_STATE_POSE };
		xrGetActionStatePose(xr_input.poseAction, xr_input.handSubactionPath[hand], &pose_state);
		xr_input.renderHand[hand] = pose_state.isActive;

		// Events come with a timestamp
		XrActionStateBoolean select_state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		xrGetActionStateBoolean(xr_input.selectAction, 1, &xr_input.handSubactionPath[hand], &select_state);
		xr_input.handSelect[hand] = select_state.currentState && select_state.changedSinceLastSync;

		// If we have a select event, update the hand pose to match the event's timestamp
		if (xr_input.handSelect[hand]) {
			XrSpaceRelation spaceRelation = { XR_TYPE_SPACE_RELATION };
			XrResult        res = xrLocateSpace(xr_input.handSpace[hand], xr_app_space, select_state.lastChangeTime, &spaceRelation);
			if (XR_UNQUALIFIED_SUCCESS(res) &&
				(spaceRelation.relationFlags & XR_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
				(spaceRelation.relationFlags & XR_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
				xr_input.handPose[hand] = spaceRelation.pose;
			}
		}
	}
}

///////////////////////////////////////////

void openxr_poll_predicted(XrTime predicted_time) {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	// Update hand position based on the predicted time of when the frame will be rendered! This 
	// should result in a more accurate location, and reduce perceived lag.
	for (size_t i = 0; i < 2; i++) {
		if (!xr_input.renderHand[i])
			continue;
		XrSpaceRelation spaceRelation = { XR_TYPE_SPACE_RELATION };
		XrResult        res           = xrLocateSpace(xr_input.handSpace[i], xr_app_space, predicted_time, &spaceRelation);
		if (XR_UNQUALIFIED_SUCCESS(res) &&
			(spaceRelation.relationFlags & XR_SPACE_RELATION_POSITION_VALID_BIT   ) != 0 &&
			(spaceRelation.relationFlags & XR_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
			xr_input.handPose[i] = spaceRelation.pose;
		}
	}
}

///////////////////////////////////////////

void openxr_render_frame() {
	// Block until the previous frame is finished displaying, and is ready for another one.
	// Also returns a prediction of when the next frame will be displayed, for use with predicting
	// locations of controllers, viewpoints, etc.
	XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
	xrWaitFrame (xr_session, nullptr, &frame_state);
	// Must be called before any rendering is done! This can return some interesting flags, like 
	// XR_SESSION_VISIBILITY_UNAVAILABLE, which means we could skip rendering this frame and call
	// xrEndFrame right away.
	xrBeginFrame(xr_session, nullptr);

	// Execute any code that's dependant on the predicted time, such as updating the location of
	// controller models.
	openxr_poll_predicted(frame_state.predictedDisplayTime);
	app_update_predicted();

	// If the session is active, lets render our layer in the compositor!
	XrCompositionLayerBaseHeader            *layer      = nullptr;
	XrCompositionLayerProjection             layer_proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	vector<XrCompositionLayerProjectionView> views;
	bool session_active = xr_session_state == XR_SESSION_STATE_VISIBLE || xr_session_state == XR_SESSION_STATE_FOCUSED;
	if (session_active && openxr_render_layer(frame_state.predictedDisplayTime, views, layer_proj)) {
		layer = (XrCompositionLayerBaseHeader*)&layer_proj;
	}

	// We're finished with rendering our layer, so send it off for display!
	XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
	end_info.displayTime          = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = xr_blend;
	end_info.layerCount           = layer == nullptr ? 0 : 1;
	end_info.layers               = &layer;
	xrEndFrame(xr_session, &end_info);
}

///////////////////////////////////////////

bool openxr_render_layer(XrTime predictedTime, vector<XrCompositionLayerProjectionView> &views, XrCompositionLayerProjection &layer) {
	
	// Find the state and location of each viewpoint at the predicted time
	uint32_t         view_count  = 0;
	XrViewState      view_state  = { XR_TYPE_VIEW_STATE };
	XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
	locate_info.displayTime = predictedTime;
	locate_info.space       = xr_app_space;
	xrLocateViews(xr_session, &locate_info, &view_state, (uint32_t)xr_views.size(), &view_count, xr_views.data());
	views.resize(view_count);

	// And now we'll iterate through each viewpoint, and render it!
	for (uint32_t i = 0; i < view_count; i++) {

		// We need to ask which swapchain image to use for rendering! Which one will we get?
		// Who knows! It's up to the runtime to decide.
		uint32_t                    img_id;
		XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		xrAcquireSwapchainImage(xr_swapchains[i].handle, &acquire_info, &img_id);

		// Wait until the image is available to render to. The compositor could still be
		// reading from it.
		XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait_info.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(xr_swapchains[i].handle, &wait_info);

		// Set up our rendering information for the viewpoint we're using right now!
		views[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		views[i].pose = xr_views[i].pose;
		views[i].fov  = xr_views[i].fov;
		views[i].subImage.swapchain        = xr_swapchains[i].handle;
		views[i].subImage.imageRect.offset = { 0, 0 };
		views[i].subImage.imageRect.extent = { xr_swapchains[i].width, xr_swapchains[i].height };

		// Call the rendering callback with our view and swapchain info
		d3d_render_layer(views[i], xr_swapchains[i].surface_data[img_id]);

		// And tell OpenXR we're done with rendering to this one!
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xr_swapchains[i].handle, &release_info);
	}

	layer.space     = xr_app_space;
	layer.viewCount = (uint32_t)views.size();
	layer.views     = views.data();
	return true;
}

///////////////////////////////////////////
// DirectX code                          //
///////////////////////////////////////////

XrBaseInStructure *d3d_init() {
	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
	if (FAILED(D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, 0, 0, featureLevels, _countof(featureLevels), D3D11_SDK_VERSION, &d3d_device, nullptr, &d3d_context)))
		printf("Failed to init d3d!\n");

	// Create a binding for OpenXR to use during session creation
	d3d_binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	d3d_binding.device = d3d_device;
	return (XrBaseInStructure*)&d3d_binding;
}

///////////////////////////////////////////

void d3d_shutdown() {
	if (d3d_context) { d3d_context->Release(); d3d_context = nullptr; }
	if (d3d_device ) { d3d_device->Release();  d3d_device  = nullptr; }
}

///////////////////////////////////////////

swapchain_surfdata_t d3d_make_surface_data(XrBaseInStructure &swapchain_img) {
	assert(swapchain_img.type == APP_SWAPCHAIN_TYPE_ID);

	swapchain_surfdata_t result = {};

	// Get information about the swapchain image that OpenXR made for us!
	XrSwapchainImageD3D11KHR &d3d_swapchain_img = (XrSwapchainImageD3D11KHR &)swapchain_img;
	D3D11_TEXTURE2D_DESC      color_desc;
	d3d_swapchain_img.texture->GetDesc(&color_desc);

	// Create a view resource for the swapchain image target that we can use to set up rendering.
	D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
	target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	// NOTE: color_desc.Format comes back in a similar format, but not exactly the same! CreateRenderTargetView fails if
	// you pass it in directly
	target_desc.Format        = (DXGI_FORMAT)d3d_swapchain_fmt; 
	d3d_device->CreateRenderTargetView(d3d_swapchain_img.texture, &target_desc, &result.target_view);

	// Create a depth buffer that matches 
	ID3D11Texture2D     *depth_texture;
	D3D11_TEXTURE2D_DESC depth_desc = {};
	depth_desc.SampleDesc.Count = 1;
	depth_desc.MipLevels        = 1;
	depth_desc.Width            = color_desc.Width;
	depth_desc.Height           = color_desc.Height;
	depth_desc.ArraySize        = color_desc.ArraySize;
	depth_desc.Format           = DXGI_FORMAT_R32_TYPELESS;
	depth_desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	d3d_device->CreateTexture2D(&depth_desc, nullptr, &depth_texture);

	// And create a view resource for the depth buffer, so we can set that up for rendering to as well!
	D3D11_DEPTH_STENCIL_VIEW_DESC stencil_desc = {};
	stencil_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	stencil_desc.Format        = DXGI_FORMAT_D32_FLOAT;
	d3d_device->CreateDepthStencilView(depth_texture, &stencil_desc, &result.depth_view);

	// We don't need direct access to the ID3D11Texture2D object anymore, we only need the view
	depth_texture->Release();

	return result;
}

///////////////////////////////////////////

void d3d_render_layer(XrCompositionLayerProjectionView &view, swapchain_surfdata_t &surface) {
	// Set up where on the render target we want to draw, the view has a 
	XrRect2Di     &rect     = view.subImage.imageRect;
	D3D11_VIEWPORT viewport = CD3D11_VIEWPORT((float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height);
	d3d_context->RSSetViewports(1, &viewport);

	// Wipe our swapchain color and depth target clean, and then set them up for rendering!
	float clear[] = { 0, 0, 0, 1 };
	d3d_context->ClearRenderTargetView(surface.target_view, clear);
	d3d_context->ClearDepthStencilView(surface.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	d3d_context->OMSetRenderTargets(1, &surface.target_view, surface.depth_view);

	// And now that we're set up, pass on the rest of our rendering to the application
	app_draw(view);
}

///////////////////////////////////////////

void d3d_swapchain_destroy(swapchain_t &swapchain) {
	for (uint32_t i = 0; i < swapchain.surface_data.size(); i++) {
		swapchain.surface_data[i].depth_view ->Release();
		swapchain.surface_data[i].target_view->Release();
	}
}

///////////////////////////////////////////

XMMATRIX d3d_xr_projection(XrFovf fov, float clip_near, float clip_far) {
	// Mix of XMMatrixPerspectiveFovRH from DirectXMath and XrMatrix4x4f_CreateProjectionFov from xr_linear.h
	const float tanLeft        = tanf(fov.angleLeft);
	const float tanRight       = tanf(fov.angleRight);
	const float tanDown        = tanf(fov.angleDown);
	const float tanUp          = tanf(fov.angleUp);
	const float tanAngleWidth  = tanRight - tanLeft;
	const float tanAngleHeight = tanUp - tanDown;
	const float range          = clip_far / (clip_near - clip_far);

	// [row][column]
	float result[16] = { 0 };
	result[0]  = 2 / tanAngleWidth;                    // [0][0] Different, DX uses: Width (Height / AspectRatio);
	result[5]  = 2 / tanAngleHeight;                   // [1][1] Same as DX's: Height (CosFov / SinFov)
	result[8]  = (tanRight + tanLeft) / tanAngleWidth; // [2][0] Only present in xr's
	result[9]  = (tanUp + tanDown) / tanAngleHeight;   // [2][1] Only present in xr's
	result[10] = range;                               // [2][2] Same as xr's: -(farZ + offsetZ) / (farZ - nearZ)
	result[11] = -1;                                  // [2][3] Same
	result[14] = range * clip_near;                   // [3][2] Same as xr's: -(farZ * (nearZ + offsetZ)) / (farZ - nearZ);

	return XMLoadFloat4x4((XMFLOAT4X4*)&result);
}

///////////////////////////////////////////

ID3DBlob *d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target) {
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob *compiled, *errors;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
		printf("Error: D3DCompile failed %s", (char*)errors->GetBufferPointer());
	if (errors) errors->Release();

	return compiled;
}

///////////////////////////////////////////
// App                                   //
///////////////////////////////////////////

void app_init() {
	// Compile our shader code, and turn it into a shader resource!
	ID3DBlob *vert_shader_blob  = d3d_compile_shader(app_shader_code, "vs", "vs_5_0");
	ID3DBlob *pixel_shader_blob = d3d_compile_shader(app_shader_code, "ps", "ps_5_0");
	d3d_device->CreateVertexShader(vert_shader_blob->GetBufferPointer(), vert_shader_blob ->GetBufferSize(), nullptr, &app_vshader);
	d3d_device->CreatePixelShader(pixel_shader_blob->GetBufferPointer(), pixel_shader_blob->GetBufferSize(), nullptr, &app_pshader);

	// Describe how our mesh is laid out in memory
	D3D11_INPUT_ELEMENT_DESC vert_desc[] = {
		{"SV_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"NORMAL",      0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0}, };
	d3d_device->CreateInputLayout(vert_desc, (UINT)_countof(vert_desc), vert_shader_blob->GetBufferPointer(), vert_shader_blob->GetBufferSize(), &app_shader_layout);

	// Create GPU resources for our mesh's vertices and indices! Constant buffers are for passing transform
	// matrices into the shaders, so make a buffer for them too!
	D3D11_SUBRESOURCE_DATA vert_buff_data = { app_verts };
	D3D11_SUBRESOURCE_DATA ind_buff_data  = { app_inds };
	CD3D11_BUFFER_DESC     vert_buff_desc (sizeof(app_verts),              D3D11_BIND_VERTEX_BUFFER);
	CD3D11_BUFFER_DESC     ind_buff_desc  (sizeof(app_inds),               D3D11_BIND_INDEX_BUFFER);
	CD3D11_BUFFER_DESC     const_buff_desc(sizeof(app_transform_buffer_t), D3D11_BIND_CONSTANT_BUFFER);
	d3d_device->CreateBuffer(&vert_buff_desc, &vert_buff_data, &app_vertex_buffer);
	d3d_device->CreateBuffer(&ind_buff_desc,  &ind_buff_data,  &app_index_buffer);
	d3d_device->CreateBuffer(&const_buff_desc, nullptr,        &app_constant_buffer);
}

///////////////////////////////////////////

void app_draw(XrCompositionLayerProjectionView &view) {
	// Set up camera matrices based on OpenXR's predicted viewpoint information
	XMMATRIX mat_projection = d3d_xr_projection(view.fov, 0.05f, 100.0f);
	XMMATRIX mat_view       = XMMatrixInverse(nullptr, XMMatrixAffineTransformation(
		DirectX::g_XMOne, DirectX::g_XMZero,
		XMLoadFloat4((XMFLOAT4*)&view.pose.orientation),
		XMLoadFloat3((XMFLOAT3*)&view.pose.position)));

	// Set the active shaders and constant buffers.
	d3d_context->VSSetConstantBuffers(0, 1, &app_constant_buffer);
	d3d_context->VSSetShader(app_vshader, nullptr, 0);
	d3d_context->PSSetShader(app_pshader, nullptr, 0);

	// Set up the cube mesh's information
	UINT strides[] = { sizeof(float) * 6 };
	UINT offsets[] = { 0 };
	d3d_context->IASetVertexBuffers    (0, 1, &app_vertex_buffer, strides, offsets);
	d3d_context->IASetIndexBuffer      (app_index_buffer, DXGI_FORMAT_R16_UINT, 0);
	d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3d_context->IASetInputLayout      (app_shader_layout);

	// Put camera matrices into the shader's constant buffer
	app_transform_buffer_t transform_buffer;
	XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(mat_view * mat_projection));

	// Draw all the cubes we have in our list!
	for (size_t i = 0; i < app_cubes.size(); i++) {
		// Create a translate, rotate, scale matrix for the cube's world location
		XMMATRIX mat_model = XMMatrixAffineTransformation(
			DirectX::g_XMOne * 0.05f, DirectX::g_XMZero,
			XMLoadFloat4((XMFLOAT4*)&app_cubes[i].orientation),
			XMLoadFloat3((XMFLOAT3*)&app_cubes[i].position));

		// Update the shader's constant buffer with the transform matrix info, and then draw the mesh!
		XMStoreFloat4x4(&transform_buffer.world, XMMatrixTranspose(mat_model));
		d3d_context->UpdateSubresource(app_constant_buffer, 0, nullptr, &transform_buffer, 0, 0);
		d3d_context->DrawIndexed((UINT)_countof(app_inds), 0, 0);
	}
}

///////////////////////////////////////////

void app_update() {
	// If the user presses the select action, lets add a cube at that location!
	for (uint32_t i = 0; i < 2; i++) {
		if (xr_input.handSelect[i])
			app_cubes.push_back(xr_input.handPose[i]);
	}
}

///////////////////////////////////////////

void app_update_predicted() {
	// Update the location of the hand cubes. This is done after the inputs have been updated to 
	// use the predicted location, but during the render code, so we have the most up-to-date location.
	if (app_cubes.size() < 2)
		app_cubes.resize(2, xr_pose_identity);
	for (uint32_t i = 0; i < 2; i++) {
		app_cubes[i] = xr_input.renderHand[i] ? xr_input.handPose[i] : xr_pose_identity;
	}
}