
#include "../gl_driver.h"
#include "../egl_dispatch_table.h"

bool WrappedOpenGL::ReadExternalTexture(GLuint texture, byte* pixels, size_t size)
{
    while(GL.glGetError() != GL_NO_ERROR) { // dump any OpenGL errors
    }
    // get params of externaL texture
    GLuint curr_tex_ext = 0, width = 0, height = 0;
    GLenum internalFormat = eGL_NONE;
    GL.glGetIntegerv(eGL_TEXTURE_BINDING_EXTERNAL_OES, (GLint*)&curr_tex_ext);
    GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, texture);
    GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_WIDTH, (GLint*)&width);
    GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_HEIGHT,(GLint*)&height);
    GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_INTERNAL_FORMAT,
                           (GLint *)&internalFormat);
    GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, curr_tex_ext);
    if(!internalFormat) // c forma: tnot has no refLecton to GL format
      internalFormat = eGL_RGBA8; // wiLL be read as RGBAS
    size_t expectedSize = GetByteSize(width, height, 1, GetBaseFormat(internalFormat), eGL_UNSIGNED_BYTE);
    if(!pixels || size != expectedSize)
    {
      RDCERR("Invalid parameters(s)");
      return false;
    }
    // read pixels. ref

    GLuint curr_read_fb = 0, curr_pixpackbuf = 0, fb = 0;
    GL.glGetIntegerv(eGL_PIXEL_PACK_BUFFER_BINDING, (GLint*)&curr_pixpackbuf);
    GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, 0);
    GL.glGenFramebuffers(1, &fb);

    GL.glGetIntegerv(eGL_READ_FRAMEBUFFER_BINDING, (GLint*)&curr_read_fb); 
    GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, fb);
    GL.glFramebufferTexture2D(eGL_READ_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_TEXTURE_EXTERNAL_OES, texture, 0); 
    GLint curr_pixpackalign = 0;
    GL.glGetIntegerv(eGL_PACK_ALIGNMENT, &curr_pixpackalign); 
    GL.glPixelStorei(eGL_PACK_ALIGNMENT, 1);
    GL.glReadPixels(0, 0, width, height, GetBaseFormat(internalFormat), eGL_UNSIGNED_BYTE, pixels); 
    GL.glPixelStorei(eGL_PACK_ALIGNMENT, curr_pixpackalign); 
    GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, curr_read_fb);
    GL.glDeleteFramebuffers(1, &fb);
    GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, curr_pixpackbuf); 
    return GL.glGetError() == GL_NO_ERROR;
}

// static
bool WrappedOpenGL::WriteExternalTexture(const ExternalTextureResource* etr, const byte* pixels) 
{
#if defined(RENDERDOC_PLATFORM_ANDROID)
    RDCASSERT(etr->hw_buffer);
    AHardwareBuffer_Desc hw_buf_desc{};
    AHardwareBuffer_describe(etr->hw_buffer, &hw_buf_desc);

    uint32_t pixel_size;

    switch(hw_buf_desc.format)
    {
    case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
        pixel_size = 3;
        break;
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        pixel_size = 4;
        break;
    default:
        RDCERR("Unknown or unsupported hardware buffer format 0x%X", hw_buf_desc.format);
        return false;
    }

    byte* pwrite = nullptr;
    int res = AHardwareBuffer_lock(etr->hw_buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr, (void**)&pwrite);
    RDCASSERT(res == 0);
    if(hw_buf_desc.stride == hw_buf_desc.width) // copy at once
    {
        memcpy(pwrite, pixels, etr->width * etr->height * pixel_size);
    }
    else // copy row by row
    {
        hw_buf_desc.width *= pixel_size; // hw_buf_desc.width = src row size in bytes
        hw_buf_desc.stride *= pixel_size; // hw_buf_desc.stride = dst row size in bytes

        for(uint32_t h = 0; h < hw_buf_desc.height; ++h)
        {
            memcpy(pwrite, pixels, hw_buf_desc.width);
            pixels += hw_buf_desc.width;
            pwrite += hw_buf_desc.stride;
        }
     }
     res = AHardwareBuffer_unlock(etr->hw_buffer, nullptr);
     RDCASSERT(res == 0);
     return true;
#else
     return false;
#endif    // if defined(RENDERDOC PLATFORM ANDROID)
}

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID = nullptr;
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;

void WrappedOpenGL::ReleaseExternalTextureResources()
{
    for(auto &etr : m_ExternalTextureResources)
    {
        if(etr.image != EGL_NO_IMAGE_KHR)
        {
            eglDestroyImageKHR(EGL.GetCurrentDisplay(), etr.image);
            etr.image = EGL_NO_IMAGE_KHR;
        }
        etr.cl_buffer = nullptr;
#if defined(RENDERDOC_PLATFORM_ANDROID)
        if(etr.hw_buffer)
        {
            AHardwareBuffer_release(etr.hw_buffer);
            etr.hw_buffer = nullptr;
        }
#endif
    }
}

WrappedOpenGL::ExternalTextureResource* WrappedOpenGL::AddExternalTexture(ResourceId texture, GLuint width, GLuint height, GLenum format)
{
  RDCASSERT(width > 0 && height > 0 && format != eGL_NONE);
  {
    auto etr = FindExternalTexture(texture, width, height, format);
    if(etr)
    {
      return etr;
    }
  }

  if(!eglCreateImageKHR)
  {
    if(RenderDoc::Inst().IsReplayApp())
      if(!EGL.GetDisplay)
        if(!EGL.PopulateForReplay())
          return nullptr;

    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)EGL.GetProcAddress("eglCreateImageKHR");
    RDCASSERT(eglCreateImageKHR);
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)EGL.GetProcAddress("eglDestroyImageKHR");
    RDCASSERT(eglDestroyImageKHR);
  }

#if defined(RENDERDOC_PLATFORM_ANDROID)
  uint32_t hw_buf_format = 0;
  switch(format)
  {
    case eGL_RGB8:
      hw_buf_format = AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
      break;
    case eGL_RGBA8:
      hw_buf_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      break;
    default: RDCERR("Unknown or unsupported texture format 0x%X", format);
      return nullptr;
  }
  AHardwareBuffer_Desc hw_buf_desc{};
  hw_buf_desc.width = width;
  hw_buf_desc.height = height;
  hw_buf_desc.format = hw_buf_format;
  hw_buf_desc.layers = 1;
  hw_buf_desc.usage =
      AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
  // hw_buf_desc.stride = width;
  m_ExternalTextureResources.emplace_back(texture, (uint32_t)width, (uint32_t)height, format);
  auto etr = m_ExternalTextureResources.end() - 1;
  int res = AHardwareBuffer_allocate(&hw_buf_desc, &etr->hw_buffer);
  RDCASSERT(res == 0);
  if(!eglGetNativeClientBufferANDROID)
  {
    eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)EGL.GetProcAddress(
        "eglGetNativeClientBufferANDROID");
    RDCASSERT(eglGetNativeClientBufferANDROID);
  }
  etr->cl_buffer = eglGetNativeClientBufferANDROID(etr->hw_buffer);
  RDCASSERT(etr->cl_buffer);
  etr->image = eglCreateImageKHR(EGL.GetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                 etr->cl_buffer, nullptr);
  RDCASSERT(etr->image != EGL_NO_IMAGE_KHR);
  return &*etr;
#else
  return nullptr;
#endif    // if defined(RENDERDOC PLATFORM ANDROID)
}


WrappedOpenGL::ExternalTextureResource* WrappedOpenGL::FindExternalTexture(ResourceId texture,
GLuint width, GLuint height, GLenum format)
{
  for(auto &etr : m_ExternalTextureResources)
    if(etr/*.resourceId*/ == texture && etr.width == width && etr.height == height && etr.format == format)
      return &etr;
  return nullptr;
}
