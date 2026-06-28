/* RetroArch Mali fbdev context driver - with 90° CCW rotation for portrait displays
 * Based on the original mali_fbdev_ctx.c, modified for Denon Prime Go (800x1280 portrait -> landscape) */

#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <compat/strl.h>
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#ifdef HAVE_EGL
#include "../common/egl_common.h"
#endif
#include "../../frontend/frontend_driver.h"
#include "../../verbosity.h"
#include "../../configuration.h"
#include <streams/file_stream.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP 0x0005
#endif

typedef struct {
#ifdef HAVE_EGL
   egl_ctx_data_t egl;
   GLuint fbo, fbo_tex;
   unsigned fbo_w, fbo_h;
   /* Rotation blit shader */
   GLuint rot_prog, rot_vs, rot_fs;
   GLint rot_pos_loc, rot_uv_loc;
   /* Static VBO for fullscreen quad */
   GLuint rot_vbo;
#endif
   struct {
      unsigned short width, height;
   } native_window;
   unsigned width, height, fb_phys_w, fb_phys_h;
   float refresh_rate;
   bool resize;
} mali_ctx_data_t;

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

enum {
   GFX_CTX_MALI_FBDEV_FLAG_WAS_THREADED    = (1 << 0),
   GFX_CTX_MALI_FBDEV_FLAG_HW_CTX_TRIGGER   = (1 << 1),
   GFX_CTX_MALI_FBDEV_FLAG_RESTART_PENDING  = (1 << 2),
   GFX_CTX_MALI_FBDEV_FLAG_GLES3            = (1 << 3)
};

static mali_ctx_data_t *gfx_ctx_mali_fbdev_global = NULL;
static uint8_t mali_flags = 0;

/* GLES2 shader for rotating FBO texture 90° CCW */
static const char *rot_vs_src =
   "attribute vec2 a_pos;\n"
   "attribute vec2 a_uv;\n"
   "varying vec2 v_uv;\n"
   "void main() {\n"
   "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
   "  v_uv = a_uv;\n"
   "}\n";

static const char *rot_fs_src =
   "precision mediump float;\n"
   "varying vec2 v_uv;\n"
   "uniform sampler2D u_tex;\n"
   "void main() {\n"
   "  gl_FragColor = texture2D(u_tex, v_uv);\n"
   "}\n";

/* Fullscreen quad vertex data: position (2f) + uv (2f), interleaved */
/* Position in clip space (-1..1), UV in texture space (0..1) */
/* Quad covers full screen with FBO texture mapped 90° CCW */
/* FBO (1280x800 w>h) rotated 90° CCW into screen (800x1280 w<h) */
/* Rotated mapping: FBO.w(1280)→screen.h(1280), FBO.h(800)→screen.w(800) */
/* FBO bottom-left(0,0) → screen bottom-right, FBO top-right → screen top-left */
/* V-flipped + U-flipped: rotation with correct orientation */
static const float rot_quad_data[] = {
   /* pos(x,y)       uv(u,v)    */
   -1.0f, -1.0f,      0.0f, 1.0f,   /* screen BL → FBO TL */
   -1.0f,  1.0f,      1.0f, 1.0f,   /* screen TL → FBO TR */
    1.0f, -1.0f,      0.0f, 0.0f,   /* screen BR → FBO BL */
    1.0f,  1.0f,      1.0f, 0.0f,   /* screen TR → FBO BR */
};

static GLuint rot_compile_shader(const char *src, GLenum type) {
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char buf[512];
      glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
      RARCH_ERR("[Mali] Shader compile (%d): %s\n", type, buf);
   }
   return s;
}

static int gfx_ctx_mali_fbdev_get_vinfo(void *data) {
   struct fb_var_screeninfo vinfo;
   int fd = open("/dev/fb0", O_RDWR);
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   if (!mali || ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) goto error;
   if (vinfo.yoffset != 0) {
      vinfo.yoffset = 0;
      if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo))
         RARCH_ERR("[Mali] Error resetting yoffset.\n");
   }
   close(fd); fd = -1;

   /* Physical fb dimensions */
   mali->fb_phys_w = vinfo.xres;
   mali->fb_phys_h = vinfo.yres;

   /* Landscape: swap dimensions for RetroArch (reported as screen size) */
   mali->width  = vinfo.yres;
   mali->height = vinfo.xres;
   mali->fbo_w  = vinfo.yres;
   mali->fbo_h  = vinfo.xres;

   /* EGL window stays at physical fb dimensions */
   mali->native_window.width  = vinfo.xres;
   mali->native_window.height = vinfo.yres;

   if (vinfo.pixclock) {
      mali->refresh_rate = 1000000.0f / vinfo.pixclock * 1000000.0f /
         (vinfo.yres + vinfo.upper_margin + vinfo.lower_margin + vinfo.vsync_len) /
         (vinfo.xres + vinfo.left_margin  + vinfo.right_margin + vinfo.hsync_len);
   } else {
      mali->refresh_rate = 60.0f;
   }
   return 0;
error:
   if (fd >= 0) close(fd);
   return 1;
}

static void gfx_ctx_mali_fbdev_clear_screen(void) {
   struct fb_var_screeninfo vinfo;
   int fd = open("/dev/fb0", O_RDWR);
   if (fd < 0) return;
   if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) { close(fd); return; }
   long sz = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
   void *buf = calloc(1, sz);
   if (buf) { write(fd, buf, sz); free(buf); }
   close(fd);
   if (!system(NULL) && !system("which setterm > /dev/null 2>&1")) {
      int fd2 = open("/dev/tty", O_RDWR);
      ioctl(fd2, VT_ACTIVATE, 5);
      ioctl(fd2, VT_ACTIVATE, 1);
      close(fd2);
      system("setterm -cursor on");
   }
}

static void gfx_ctx_mali_fbdev_destroy_really(void) {
   if (gfx_ctx_mali_fbdev_global) {
      mali_ctx_data_t *m = gfx_ctx_mali_fbdev_global;
#ifdef HAVE_EGL
      if (m->rot_prog)   glDeleteProgram(m->rot_prog);
      if (m->rot_vs)     glDeleteShader(m->rot_vs);
      if (m->rot_fs)     glDeleteShader(m->rot_fs);
      if (m->rot_vbo)    glDeleteBuffers(1, &m->rot_vbo);
      if (m->fbo)        glDeleteFramebuffers(1, &m->fbo);
      if (m->fbo_tex)    glDeleteTextures(1, &m->fbo_tex);
      egl_destroy(&m->egl);
#endif
      m->resize = false;
      free(m);
      gfx_ctx_mali_fbdev_global = NULL;
   }
}

static void gfx_ctx_mali_fbdev_maybe_restart(void) {
   if (!(runloop_get_flags() & RUNLOOP_FLAG_SHUTDOWN_INITIATED))
      frontend_driver_set_fork(FRONTEND_FORK_RESTART);
}

static void gfx_ctx_mali_fbdev_destroy(void *data) {
   if (runloop_get_flags() & RUNLOOP_FLAG_SHUTDOWN_INITIATED) {
      if (!(mali_flags & GFX_CTX_MALI_FBDEV_FLAG_RESTART_PENDING)) {
         gfx_ctx_mali_fbdev_destroy_really();
         gfx_ctx_mali_fbdev_clear_screen();
      }
   } else {
      if ((mali_flags & GFX_CTX_MALI_FBDEV_FLAG_HW_CTX_TRIGGER) ||
          (bool)(mali_flags & GFX_CTX_MALI_FBDEV_FLAG_WAS_THREADED) !=
          *video_driver_get_threaded()) {
         gfx_ctx_mali_fbdev_destroy_really();
         mali_flags |= GFX_CTX_MALI_FBDEV_FLAG_RESTART_PENDING;
         if (!(mali_flags & GFX_CTX_MALI_FBDEV_FLAG_HW_CTX_TRIGGER))
            gfx_ctx_mali_fbdev_maybe_restart();
      }
   }
}

static void gfx_ctx_mali_fbdev_get_video_size(void *data, unsigned *w, unsigned *h) {
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   *w = mali->width;
   *h = mali->height;
}

static void *gfx_ctx_mali_fbdev_init(void *video_driver) {
   if (gfx_ctx_mali_fbdev_global)
      return gfx_ctx_mali_fbdev_global;

   mali_ctx_data_t *mali = NULL;
   if (!(mali = (mali_ctx_data_t *)calloc(1, sizeof(*mali))))
      return NULL;

   if (gfx_ctx_mali_fbdev_get_vinfo(mali))
      goto error;

#ifdef HAVE_EGL
   EGLint n, major, minor;
   EGLint attribs_init[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE
   };
   EGLint attribs_create[] = {
      EGL_CONTEXT_CLIENT_VERSION, (mali_flags & GFX_CTX_MALI_FBDEV_FLAG_GLES3) ? 3 : 2,
      EGL_NONE
   };
   if (mali_flags & GFX_CTX_MALI_FBDEV_FLAG_GLES3)
      attribs_init[1] = EGL_OPENGL_ES3_BIT;
   RARCH_LOG("[Mali] GLES version = %d.\n", (mali_flags & GFX_CTX_MALI_FBDEV_FLAG_GLES3) ? 3 : 2);

   frontend_driver_install_signal_handler();
   mali->egl.use_hw_ctx = true;
   if (!egl_init_context(&mali->egl, EGL_NONE, EGL_DEFAULT_DISPLAY,
            &major, &minor, &n, attribs_init, NULL) ||
       !egl_create_context(&mali->egl, attribs_create) ||
       !egl_create_surface(&mali->egl, &mali->native_window))
      goto error;

   /* Make context current so we can create FBO */
   if (eglMakeCurrent(mali->egl.dpy, mali->egl.surf, mali->egl.surf, mali->egl.ctx) != EGL_TRUE)
      goto error;

   /* Create offscreen FBO for landscape rendering */
   glGenFramebuffers(1, &mali->fbo);
   glGenTextures(1, &mali->fbo_tex);
   glBindTexture(GL_TEXTURE_2D, mali->fbo_tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mali->fbo_w, mali->fbo_h, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindFramebuffer(GL_FRAMEBUFFER, mali->fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mali->fbo_tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RARCH_ERR("[Mali] FBO creation failed!\n");
      goto error;
   }
   RARCH_LOG("[Mali] FBO %dx%d created\n", mali->fbo_w, mali->fbo_h);

   /* Create rotation blit shader */
   mali->rot_vs = rot_compile_shader(rot_vs_src, GL_VERTEX_SHADER);
   mali->rot_fs = rot_compile_shader(rot_fs_src, GL_FRAGMENT_SHADER);
   mali->rot_prog = glCreateProgram();
   glAttachShader(mali->rot_prog, mali->rot_vs);
   glAttachShader(mali->rot_prog, mali->rot_fs);
   glLinkProgram(mali->rot_prog);
   GLint ok;
   glGetProgramiv(mali->rot_prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      char buf[512];
      glGetProgramInfoLog(mali->rot_prog, sizeof(buf), NULL, buf);
      RARCH_ERR("[Mali] Shader link: %s\n", buf);
      goto error;
   }
   mali->rot_pos_loc = glGetAttribLocation(mali->rot_prog, "a_pos");
   mali->rot_uv_loc  = glGetAttribLocation(mali->rot_prog, "a_uv");
   RARCH_LOG("[Mali] Rotation shader ready\n");

   /* Create quad VBO */
   glGenBuffers(1, &mali->rot_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, mali->rot_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(rot_quad_data), rot_quad_data, GL_STATIC_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);

   /* Bind FBO as default render target */
   glBindFramebuffer(GL_FRAMEBUFFER, mali->fbo);
#endif

   gfx_ctx_mali_fbdev_global = mali;
   if (*video_driver_get_threaded())
      mali_flags |= GFX_CTX_MALI_FBDEV_FLAG_WAS_THREADED;
   else
      mali_flags &= ~GFX_CTX_MALI_FBDEV_FLAG_WAS_THREADED;
   return mali;

error:
   egl_report_error();
   gfx_ctx_mali_fbdev_destroy(mali);
   return NULL;
}

static void gfx_ctx_mali_fbdev_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height) {
   unsigned new_w, new_h;
   gfx_ctx_mali_fbdev_get_video_size(data, &new_w, &new_h);
   if (new_w != *width || new_h != *height) {
      *width = new_w; *height = new_h; *resize = true;
   }
   *quit = (bool)frontend_driver_get_signal_handler_state();
   if (mali_flags & GFX_CTX_MALI_FBDEV_FLAG_RESTART_PENDING)
      gfx_ctx_mali_fbdev_maybe_restart();
}

static bool gfx_ctx_mali_fbdev_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen) {
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   if (video_driver_is_hw_context())
      mali_flags |= GFX_CTX_MALI_FBDEV_FLAG_HW_CTX_TRIGGER;
   if (gfx_ctx_mali_fbdev_get_vinfo(mali)) {
      gfx_ctx_mali_fbdev_destroy(data);
      return false;
   }
   width  = mali->width;
   height = mali->height;
   return true;
}

static void gfx_ctx_mali_fbdev_input_driver(void *data, const char *name,
      input_driver_t **input, void **input_data) {
   *input = NULL; *input_data = NULL;
}

static enum gfx_ctx_api gfx_ctx_mali_fbdev_get_api(void *data) {
   return GFX_CTX_OPENGL_ES_API;
}

static bool gfx_ctx_mali_fbdev_bind_api(void *data, enum gfx_ctx_api api,
      unsigned major, unsigned minor) {
   unsigned version = major * 100 + minor;
   if (version >= 300) mali_flags |= GFX_CTX_MALI_FBDEV_FLAG_GLES3;
   return (api == GFX_CTX_OPENGL_ES_API);
}

static bool gfx_ctx_mali_fbdev_has_focus(void *data) { return true; }
static bool gfx_ctx_mali_fbdev_suppress_screensaver(void *data, bool enable) { return false; }

static void gfx_ctx_mali_fbdev_set_swap_interval(void *data, int interval) {
#ifdef HAVE_EGL
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   egl_set_swap_interval(&mali->egl, interval);
#endif
}

static void gfx_ctx_mali_fbdev_swap_buffers(void *data) {
#ifdef HAVE_EGL
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;

   /* Unbind FBO, switch to default framebuffer (EGL surface) */
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   unsigned pw = mali->fb_phys_w;  /* 800 */
   unsigned ph = mali->fb_phys_h;  /* 1280 */

   glViewport(0, 0, pw, ph);
   glClear(GL_COLOR_BUFFER_BIT);

   /* Use rotation blit shader */
   glUseProgram(mali->rot_prog);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, mali->fbo_tex);
   glUniform1i(glGetUniformLocation(mali->rot_prog, "u_tex"), 0);

   glBindBuffer(GL_ARRAY_BUFFER, mali->rot_vbo);
   glEnableVertexAttribArray(mali->rot_pos_loc);
   glEnableVertexAttribArray(mali->rot_uv_loc);
   glVertexAttribPointer(mali->rot_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
   glVertexAttribPointer(mali->rot_uv_loc,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glUseProgram(0);

   /* Swap EGL buffers (present to framebuffer) */
   egl_swap_buffers(&mali->egl);

   /* Rebind FBO for next frame */
   glBindFramebuffer(GL_FRAMEBUFFER, mali->fbo);
#endif
}

static void gfx_ctx_mali_fbdev_bind_hw_render(void *data, bool enable) {
#ifdef HAVE_EGL
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   egl_bind_hw_render(&mali->egl, enable);
#endif
}

static uint32_t gfx_ctx_mali_fbdev_get_flags(void *data) {
   uint32_t flags = 0;
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);
   return flags;
}

static void gfx_ctx_mali_fbdev_set_flags(void *data, uint32_t flags) {}

static float gfx_ctx_mali_fbdev_get_refresh_rate(void *data) {
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   return mali->refresh_rate;
}

static bool gfx_ctx_mali_create_surface(void *data) {
#ifdef HAVE_EGL
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   return egl_create_surface(&mali->egl, &mali->native_window);
#else
   return false;
#endif
}

static bool gfx_ctx_mali_destroy_surface(void *data) {
#ifdef HAVE_EGL
   mali_ctx_data_t *mali = (mali_ctx_data_t *)data;
   return egl_destroy_surface(&mali->egl);
#else
   return false;
#endif
}

const gfx_ctx_driver_t gfx_ctx_mali_fbdev = {
   gfx_ctx_mali_fbdev_init,
   gfx_ctx_mali_fbdev_destroy,
   gfx_ctx_mali_fbdev_get_api,
   gfx_ctx_mali_fbdev_bind_api,
   gfx_ctx_mali_fbdev_set_swap_interval,
   gfx_ctx_mali_fbdev_set_video_mode,
   gfx_ctx_mali_fbdev_get_video_size,
   gfx_ctx_mali_fbdev_get_refresh_rate,
   NULL, NULL, NULL, NULL, NULL,
   NULL,
   gfx_ctx_mali_fbdev_check_window,
   NULL,
   gfx_ctx_mali_fbdev_has_focus,
   gfx_ctx_mali_fbdev_suppress_screensaver,
   false,
   gfx_ctx_mali_fbdev_swap_buffers,
   gfx_ctx_mali_fbdev_input_driver,
#ifdef HAVE_EGL
   egl_get_proc_address,
#else
   NULL,
#endif
   NULL, NULL, NULL,
   "fbdev_mali",
   gfx_ctx_mali_fbdev_get_flags,
   gfx_ctx_mali_fbdev_set_flags,
   gfx_ctx_mali_fbdev_bind_hw_render,
   NULL, NULL,
   gfx_ctx_mali_create_surface,
   gfx_ctx_mali_destroy_surface
};
