/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup VAMR
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <list>
#include <sstream>

#include "VAMR_capi.h"

#include "VAMR_IGraphicsBinding.h"
#include "VAMR_intern.h"
#include "VAMR_Context.h"
#include "VAMR_Exception.h"

#include "VAMR_Session.h"

struct OpenXRSessionData {
  XrSystemId system_id{XR_NULL_SYSTEM_ID};
  XrSession session{XR_NULL_HANDLE};
  XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};

  // Only stereo rendering supported now.
  const XrViewConfigurationType view_type{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  XrSpace reference_space;
  std::vector<XrView> views;
  std::vector<XrSwapchain> swapchains;
  std::map<XrSwapchain, std::vector<XrSwapchainImageBaseHeader *>> swapchain_images;
  int32_t swapchain_image_width, swapchain_image_height;
};

struct VAMR_DrawInfo {
  XrFrameState frame_state;

  /** Time at frame start to benchmark frame render durations. */
  std::chrono::high_resolution_clock::time_point frame_begin_time;
  /* Time previous frames took for rendering (in ms) */
  std::list<double> last_frame_times;
};

/* -------------------------------------------------------------------- */
/** \name Create, Initialize and Destruct
 *
 * \{ */

VAMR_Session::VAMR_Session(VAMR_Context *xr_context)
    : m_context(xr_context), m_oxr(new OpenXRSessionData())
{
}

VAMR_Session::~VAMR_Session()
{
  unbindGraphicsContext();

  for (XrSwapchain &swapchain : m_oxr->swapchains) {
    CHECK_XR_ASSERT(xrDestroySwapchain(swapchain));
  }
  m_oxr->swapchains.clear();
  if (m_oxr->reference_space != XR_NULL_HANDLE) {
    CHECK_XR_ASSERT(xrDestroySpace(m_oxr->reference_space));
  }
  if (m_oxr->session != XR_NULL_HANDLE) {
    CHECK_XR_ASSERT(xrDestroySession(m_oxr->session));
  }

  m_oxr->session = XR_NULL_HANDLE;
  m_oxr->session_state = XR_SESSION_STATE_UNKNOWN;
}

/**
 * A system in OpenXR the combination of some sort of HMD plus controllers and whatever other
 * devices are managed through OpenXR. So this attempts to init the HMD and the other devices.
 */
void VAMR_Session::initSystem()
{
  assert(m_context->getInstance() != XR_NULL_HANDLE);
  assert(m_oxr->system_id == XR_NULL_SYSTEM_ID);

  XrSystemGetInfo system_info{};
  system_info.type = XR_TYPE_SYSTEM_GET_INFO;
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  CHECK_XR(xrGetSystem(m_context->getInstance(), &system_info, &m_oxr->system_id),
           "Failed to get device information. Is a device plugged in?");
}

/** \} */ /* Create, Initialize and Destruct */

/* -------------------------------------------------------------------- */
/** \name State Management
 *
 * \{ */

static void create_reference_space(OpenXRSessionData *oxr, const VAMR_Pose *base_pose)
{
  XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  create_info.poseInReferenceSpace.orientation.w = 1.0f;

  create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
#if 0
/* Proper reference space set up is not supported yet. We simply hand OpenXR
 * the global space as reference space and apply its pose onto the active
 * camera matrix to get a basic viewing experience going. If there's no active
 * camera with stick to the world origin.
 *
 * Once we have proper reference space set up (i.e. a way to define origin, up-
 * direction and an initial view rotation perpendicular to the up-direction),
 * we can hand OpenXR a proper reference pose/space.
 */
  create_info.poseInReferenceSpace.position.x = base_pose->position[0];
  create_info.poseInReferenceSpace.position.y = base_pose->position[2];
  create_info.poseInReferenceSpace.position.z = -base_pose->position[1];
  create_info.poseInReferenceSpace.orientation.x = base_pose->orientation_quat[1];
  create_info.poseInReferenceSpace.orientation.y = base_pose->orientation_quat[3];
  create_info.poseInReferenceSpace.orientation.z = -base_pose->orientation_quat[2];
  create_info.poseInReferenceSpace.orientation.w = base_pose->orientation_quat[0];
#else
  (void)base_pose;
#endif

  CHECK_XR(xrCreateReferenceSpace(oxr->session, &create_info, &oxr->reference_space),
           "Failed to create reference space.");
}

void VAMR_Session::start(const VAMR_SessionBeginInfo *begin_info)
{
  assert(m_context->getInstance() != XR_NULL_HANDLE);
  assert(m_oxr->session == XR_NULL_HANDLE);
  if (m_context->getCustomFuncs()->gpu_ctx_bind_fn == nullptr) {
    THROW_XR(
        "Invalid API usage: No way to bind graphics context to the XR session. Call "
        "VAMR_GraphicsContextBindFuncs() with valid parameters before starting the "
        "session (through VAMR_SessionStart()).");
  }

  initSystem();

  bindGraphicsContext();
  if (m_gpu_ctx == nullptr) {
    THROW_XR(
        "Invalid API usage: No graphics context returned through the callback set with "
        "VAMR_GraphicsContextBindFuncs(). This is required for session starting (through "
        "VAMR_SessionStart()).");
  }

  std::string requirement_str;
  m_gpu_binding = VAMR_GraphicsBindingCreateFromType(m_context->getGraphicsBindingType());
  if (!m_gpu_binding->checkVersionRequirements(
          m_gpu_ctx, m_context->getInstance(), m_oxr->system_id, &requirement_str)) {
    std::ostringstream strstream;
    strstream << "Available graphics context version does not meet the following requirements: "
              << requirement_str;
    THROW_XR(strstream.str().c_str());
  }
  m_gpu_binding->initFromGhostContext(m_gpu_ctx);

  XrSessionCreateInfo create_info{};
  create_info.type = XR_TYPE_SESSION_CREATE_INFO;
  create_info.systemId = m_oxr->system_id;
  create_info.next = &m_gpu_binding->oxr_binding;

  CHECK_XR(xrCreateSession(m_context->getInstance(), &create_info, &m_oxr->session),
           "Failed to create VR session. The OpenXR runtime may have additional requirements for "
           "the graphics driver that are not met. Other causes are possible too however.\nTip: "
           "The --debug-xr command line option for Blender might allow the runtime to output "
           "detailed error information to the command line.");

  prepareDrawing();
  create_reference_space(m_oxr.get(), &begin_info->base_pose);
}

void VAMR_Session::requestEnd()
{
  xrRequestExitSession(m_oxr->session);
}

void VAMR_Session::end()
{
  assert(m_oxr->session != XR_NULL_HANDLE);

  CHECK_XR(xrEndSession(m_oxr->session), "Failed to cleanly end the VR session.");
  unbindGraphicsContext();
  m_draw_info = nullptr;
}

VAMR_Session::eLifeExpectancy VAMR_Session::handleStateChangeEvent(
    const XrEventDataSessionStateChanged *lifecycle)
{
  m_oxr->session_state = lifecycle->state;

  /* Runtime may send events for apparently destroyed session. Our handle should be NULL then. */
  assert((m_oxr->session == XR_NULL_HANDLE) || (m_oxr->session == lifecycle->session));

  switch (lifecycle->state) {
    case XR_SESSION_STATE_READY: {
      XrSessionBeginInfo begin_info{};

      begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
      begin_info.primaryViewConfigurationType = m_oxr->view_type;
      CHECK_XR(xrBeginSession(m_oxr->session, &begin_info),
               "Failed to cleanly begin the VR session.");
      break;
    }
    case XR_SESSION_STATE_STOPPING:
      /* Runtime will change state to STATE_EXITING, don't destruct session yet. */
      end();
      break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
      return SESSION_DESTROY;
    default:
      break;
  }

  return SESSION_KEEP_ALIVE;
}
/** \} */ /* State Management */

/* -------------------------------------------------------------------- */
/** \name Drawing
 *
 * \{ */

static std::vector<XrSwapchainImageBaseHeader *> swapchain_images_create(
    XrSwapchain swapchain, VAMR_IGraphicsBinding *gpu_binding)
{
  std::vector<XrSwapchainImageBaseHeader *> images;
  uint32_t image_count;

  CHECK_XR(xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr),
           "Failed to get count of swapchain images to create for the VR session.");
  images = gpu_binding->createSwapchainImages(image_count);
  CHECK_XR(xrEnumerateSwapchainImages(swapchain, images.size(), &image_count, images[0]),
           "Failed to create swapchain images for the VR session.");

  return images;
}

static unique_oxr_ptr<XrSwapchain> swapchain_create(const XrSession session,
                                                    VAMR_IGraphicsBinding *gpu_binding,
                                                    const XrViewConfigurationView *xr_view)
{
  XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  unique_oxr_ptr<XrSwapchain> swapchain(xrDestroySwapchain);
  uint32_t format_count = 0;
  int64_t chosen_format;

  CHECK_XR(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
           "Failed to get count of swapchain image formats.");
  std::vector<int64_t> swapchain_formats(format_count);
  CHECK_XR(xrEnumerateSwapchainFormats(
               session, swapchain_formats.size(), &format_count, swapchain_formats.data()),
           "Failed to get swapchain image formats.");
  assert(swapchain_formats.size() == format_count);

  if (!gpu_binding->chooseSwapchainFormat(swapchain_formats, &chosen_format)) {
    THROW_XR("Error: No format matching OpenXR runtime supported swapchain formats found.");
  }

  create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                           XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  create_info.format = chosen_format;
  create_info.sampleCount = xr_view->recommendedSwapchainSampleCount;
  create_info.width = xr_view->recommendedImageRectWidth;
  create_info.height = xr_view->recommendedImageRectHeight;
  create_info.faceCount = 1;
  create_info.arraySize = 1;
  create_info.mipCount = 1;
  CHECK_XR(swapchain.construct(xrCreateSwapchain, session, &create_info),
           "Failed to create OpenXR swapchain.");

  return swapchain;
}

void VAMR_Session::prepareDrawing()
{
  std::vector<XrViewConfigurationView> view_configs;
  uint32_t view_count;

  CHECK_XR(
      xrEnumerateViewConfigurationViews(
          m_context->getInstance(), m_oxr->system_id, m_oxr->view_type, 0, &view_count, nullptr),
      "Failed to get count of view configurations.");
  view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  CHECK_XR(xrEnumerateViewConfigurationViews(m_context->getInstance(),
                                             m_oxr->system_id,
                                             m_oxr->view_type,
                                             view_configs.size(),
                                             &view_count,
                                             view_configs.data()),
           "Failed to get count of view configurations.");

  for (const XrViewConfigurationView &view : view_configs) {
    unique_oxr_ptr<XrSwapchain> swapchain = swapchain_create(
        m_oxr->session, m_gpu_binding.get(), &view);
    auto images = swapchain_images_create(swapchain.get(), m_gpu_binding.get());

    m_oxr->swapchain_image_width = view.recommendedImageRectWidth;
    m_oxr->swapchain_image_height = view.recommendedImageRectHeight;
    m_oxr->swapchains.push_back(swapchain.get());
    m_oxr->swapchain_images.insert(std::make_pair(swapchain.get(), std::move(images)));

    swapchain.release();
  }

  m_oxr->views.resize(view_count, {XR_TYPE_VIEW});

  m_draw_info = std::unique_ptr<VAMR_DrawInfo>(new VAMR_DrawInfo());
}

void VAMR_Session::beginFrameDrawing()
{
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};

  // TODO Blocking call. Does this intefer with other drawing?
  CHECK_XR(xrWaitFrame(m_oxr->session, &wait_info, &frame_state),
           "Failed to synchronize frame rates between Blender and the device.");

  CHECK_XR(xrBeginFrame(m_oxr->session, &begin_info),
           "Failed to submit frame rendering start state.");

  m_draw_info->frame_state = frame_state;

  if (m_context->isDebugTimeMode()) {
    m_draw_info->frame_begin_time = std::chrono::high_resolution_clock::now();
  }
}

static void print_debug_timings(VAMR_DrawInfo *draw_info)
{
  /** Render time of last 8 frames (in ms) to calculate an average. */
  std::chrono::duration<double, std::milli> duration = std::chrono::high_resolution_clock::now() -
                                                       draw_info->frame_begin_time;
  const double duration_ms = duration.count();
  const int avg_frame_count = 8;
  double avg_ms_tot = 0.0;

  if (draw_info->last_frame_times.size() >= avg_frame_count) {
    draw_info->last_frame_times.pop_front();
    assert(draw_info->last_frame_times.size() == avg_frame_count - 1);
  }
  draw_info->last_frame_times.push_back(duration_ms);
  for (double ms_iter : draw_info->last_frame_times) {
    avg_ms_tot += ms_iter;
  }

  printf("VR frame render time: %.0fms - %.2f FPS (%.2f FPS 8 frames average)\n",
         duration_ms,
         1000.0 / duration_ms,
         1000.0 / (avg_ms_tot / draw_info->last_frame_times.size()));
}

void VAMR_Session::endFrameDrawing(std::vector<XrCompositionLayerBaseHeader *> *layers)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};

  end_info.displayTime = m_draw_info->frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = layers->size();
  end_info.layers = layers->data();

  CHECK_XR(xrEndFrame(m_oxr->session, &end_info), "Failed to submit rendered frame.");

  if (m_context->isDebugTimeMode()) {
    print_debug_timings(m_draw_info.get());
  }
}

void VAMR_Session::draw(void *draw_customdata)
{
  std::vector<XrCompositionLayerProjectionView>
      projection_layer_views;  // Keep alive until xrEndFrame() call!
  XrCompositionLayerProjection proj_layer;
  std::vector<XrCompositionLayerBaseHeader *> layers;

  beginFrameDrawing();

  if (m_draw_info->frame_state.shouldRender) {
    proj_layer = drawLayer(projection_layer_views, draw_customdata);
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&proj_layer));
  }

  endFrameDrawing(&layers);
}

static void vamr_draw_view_info_from_view(const XrView &view, VAMR_DrawViewInfo &r_info)
{
#if 0
  /* Set and convert to Blender coodinate space */
  r_info.pose.position[0] = view.pose.position.x;
  r_info.pose.position[1] = -view.pose.position.z;
  r_info.pose.position[2] = view.pose.position.y;
  r_info.pose.orientation_quat[0] = view.pose.orientation.w;
  r_info.pose.orientation_quat[1] = view.pose.orientation.x;
  r_info.pose.orientation_quat[2] = -view.pose.orientation.z;
  r_info.pose.orientation_quat[3] = view.pose.orientation.y;
#else
  r_info.pose.position[0] = view.pose.position.x;
  r_info.pose.position[1] = view.pose.position.y;
  r_info.pose.position[2] = view.pose.position.z;
  r_info.pose.orientation_quat[0] = view.pose.orientation.w;
  r_info.pose.orientation_quat[1] = view.pose.orientation.x;
  r_info.pose.orientation_quat[2] = view.pose.orientation.y;
  r_info.pose.orientation_quat[3] = view.pose.orientation.z;
#endif

  r_info.fov.angle_left = view.fov.angleLeft;
  r_info.fov.angle_right = view.fov.angleRight;
  r_info.fov.angle_up = view.fov.angleUp;
  r_info.fov.angle_down = view.fov.angleDown;
}

static bool vamr_draw_view_expects_srgb_buffer(const VAMR_Context *context)
{
  /* WMR seems to be faulty and doesn't do OETF transform correctly. So expect a SRGB buffer to
   * compensate. */
  return context->getOpenXRRuntimeID() == OPENXR_RUNTIME_WMR;
}

void VAMR_Session::drawView(XrSwapchain swapchain,
                            XrCompositionLayerProjectionView &proj_layer_view,
                            XrView &view,
                            void *draw_customdata)
{
  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XrSwapchainImageBaseHeader *swapchain_image;
  VAMR_DrawViewInfo draw_view_info{};
  uint32_t swapchain_idx;

  CHECK_XR(xrAcquireSwapchainImage(swapchain, &acquire_info, &swapchain_idx),
           "Failed to acquire swapchain image for the VR session.");
  wait_info.timeout = XR_INFINITE_DURATION;
  CHECK_XR(xrWaitSwapchainImage(swapchain, &wait_info),
           "Failed to acquire swapchain image for the VR session.");

  proj_layer_view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
  proj_layer_view.pose = view.pose;
  proj_layer_view.fov = view.fov;
  proj_layer_view.subImage.swapchain = swapchain;
  proj_layer_view.subImage.imageRect.offset = {0, 0};
  proj_layer_view.subImage.imageRect.extent = {m_oxr->swapchain_image_width,
                                               m_oxr->swapchain_image_height};

  swapchain_image = m_oxr->swapchain_images[swapchain][swapchain_idx];

  draw_view_info.expects_srgb_buffer = vamr_draw_view_expects_srgb_buffer(m_context);
  draw_view_info.ofsx = proj_layer_view.subImage.imageRect.offset.x;
  draw_view_info.ofsy = proj_layer_view.subImage.imageRect.offset.y;
  draw_view_info.width = proj_layer_view.subImage.imageRect.extent.width;
  draw_view_info.height = proj_layer_view.subImage.imageRect.extent.height;
  vamr_draw_view_info_from_view(view, draw_view_info);

  m_context->getCustomFuncs()->draw_view_fn(&draw_view_info, draw_customdata);
  m_gpu_binding->submitToSwapchain(swapchain_image, &draw_view_info);

  CHECK_XR(xrReleaseSwapchainImage(swapchain, &release_info),
           "Failed to release swapchain image used to submit VR session frame.");
}

XrCompositionLayerProjection VAMR_Session::drawLayer(
    std::vector<XrCompositionLayerProjectionView> &proj_layer_views, void *draw_customdata)
{
  XrViewLocateInfo viewloc_info{XR_TYPE_VIEW_LOCATE_INFO};
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  uint32_t view_count;

  viewloc_info.viewConfigurationType = m_oxr->view_type;
  viewloc_info.displayTime = m_draw_info->frame_state.predictedDisplayTime;
  viewloc_info.space = m_oxr->reference_space;

  CHECK_XR(xrLocateViews(m_oxr->session,
                         &viewloc_info,
                         &view_state,
                         m_oxr->views.size(),
                         &view_count,
                         m_oxr->views.data()),
           "Failed to query frame view and projection state.");
  assert(m_oxr->swapchains.size() == view_count);

  proj_layer_views.resize(view_count);

  for (uint32_t view_idx = 0; view_idx < view_count; view_idx++) {
    drawView(m_oxr->swapchains[view_idx],
             proj_layer_views[view_idx],
             m_oxr->views[view_idx],
             draw_customdata);
  }

  layer.space = m_oxr->reference_space;
  layer.viewCount = proj_layer_views.size();
  layer.views = proj_layer_views.data();

  return layer;
}

/** \} */ /* Drawing */

/* -------------------------------------------------------------------- */
/** \name State Queries
 *
 * \{ */

bool VAMR_Session::isRunning() const
{
  if (m_oxr->session == XR_NULL_HANDLE) {
    return false;
  }
  switch (m_oxr->session_state) {
    case XR_SESSION_STATE_READY:
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:
      return true;
    default:
      return false;
  }
}

/** \} */ /* State Queries */

/* -------------------------------------------------------------------- */
/** \name Graphics Context Injection
 *
 * Sessions need access to Ghost graphics context information. Additionally, this API allows
 * creating contexts on the fly (created on start, destructed on end). For this, callbacks to bind
 * (potentially create) and unbind (potentially destruct) a Ghost graphics context have to be set,
 * which will be called on session start and end respectively.
 *
 * \{ */

void VAMR_Session::bindGraphicsContext()
{
  const VAMR_CustomFuncs *custom_funcs = m_context->getCustomFuncs();
  assert(custom_funcs->gpu_ctx_bind_fn);
  m_gpu_ctx = static_cast<GHOST_Context *>(
      custom_funcs->gpu_ctx_bind_fn(m_context->getGraphicsBindingType()));
}
void VAMR_Session::unbindGraphicsContext()
{
  const VAMR_CustomFuncs *custom_funcs = m_context->getCustomFuncs();
  if (custom_funcs->gpu_ctx_unbind_fn) {
    custom_funcs->gpu_ctx_unbind_fn(m_context->getGraphicsBindingType(), m_gpu_ctx);
  }
  m_gpu_ctx = nullptr;
}

/** \} */ /* Graphics Context Injection */